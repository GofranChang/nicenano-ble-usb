/* SPDX-License-Identifier: MIT
 * Standalone Zephyr implementation of BLE HOGP -> USB HID report-map mirroring.
 * The keyboard remains an ordinary BLE peripheral; this is not a ZMK split central.
 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/uart.h>
#include <stdio.h>
#include <stdlib.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/class/usb_hid.h>
#include <zephyr/logging/log.h>
#include <zephyr/fatal.h>
#include "hid_map.h"
#include "fallback_map.h"

LOG_MODULE_REGISTER(bridge, LOG_LEVEL_INF);

/* Keep a small crash record across a software reset; never persist key data. */
static __noinit struct {
    uint32_t magic, reason, pc, lr, cfsr, hfsr;
} last_crash;

void k_sys_fatal_error_handler(unsigned int reason, const z_arch_esf_t *esf)
{
    last_crash.reason=reason;
    last_crash.pc=esf ? esf->basic.pc : 0;
    last_crash.lr=esf ? esf->basic.lr : 0;
    last_crash.cfsr=SCB->CFSR;
    last_crash.hfsr=SCB->HFSR;
    last_crash.magic=0x42524348;
    sys_reboot(SYS_REBOOT_COLD);
    CODE_UNREACHABLE;
}

#define CHARS_MAX 24
#define REPORTS_MAX 16
#define OP_TIMEOUT K_SECONDS(10)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static const struct device *usb;
static struct bt_conn *peer;
static bt_addr_le_t bonded_addr;
static bool has_bond, usb_started;
static atomic_t link_up, secure, ready, usb_mounted, release_needed, queue_fault;
static atomic_t selecting;
static atomic_t ble_reports, usb_reports, usb_failures, last_usb_status;
static atomic_t last_report_time;
static atomic_t advertisements, target_hits;
static atomic_t connected_at;
static atomic_t stale_bond;
static bool paused, clear_pending, resume_after_connect;
static int64_t pairing_deadline, scan_deadline;
struct seen_device { bt_addr_le_t addr; char name[33]; int8_t rssi; bool used; };
static struct seen_device seen[12];
static struct k_spinlock seen_lock;
static K_SEM_DEFINE(link_event, 0, 1);
static K_SEM_DEFINE(op_done, 0, 1);
static K_SEM_DEFINE(usb_done, 0, 1);
static int op_result;
static const char *operation="idle";

static uint8_t report_map[BRIDGE_MAP_MAX];
static size_t map_len;
static struct hid_map_info map_info;
static struct bt_gatt_discover_params discover;
static struct bt_gatt_read_params read_params;
static struct bt_gatt_write_params write_params;
static struct bt_gatt_exchange_params mtu_params;
static uint16_t service_start, service_end;
struct characteristic { uint16_t decl, value, uuid; uint8_t props; };
static struct characteristic chars[CHARS_MAX];
static unsigned char_count;
struct report {
    uint16_t value, ccc, ref;
    uint8_t id, type, props;
    struct bt_gatt_subscribe_params sub;
};
static struct report reports[REPORTS_MAX];
static unsigned report_count;
static struct report *descriptor_target;
static uint8_t *read_dest;
static size_t read_size, read_capacity;

struct packet { uint8_t len; uint8_t data[BRIDGE_PACKET_MAX]; };
K_MSGQ_DEFINE(input_queue, sizeof(struct packet), 64, 4);
K_MSGQ_DEFINE(output_queue, sizeof(struct packet), 16, 4);
K_MSGQ_DEFINE(candidate_queue, sizeof(bt_addr_le_t), 1, 4);

static int load_setting(const char *name, size_t len, settings_read_cb cb, void *arg)
{
    if (strcmp(name, "map")) return -ENOENT;
    if (!len || len > sizeof(report_map)) return -EINVAL;
    int n = cb(arg, report_map, len);
    if (n != len || hid_map_parse(report_map, len, &map_info)) return -EINVAL;
    map_len = len;
    return 0;
}
SETTINGS_STATIC_HANDLER_DEFINE(bridge_settings, "bridge", NULL, load_setting, NULL, NULL);

static void bond_found(const struct bt_bond_info *info, void *user)
{
    ARG_UNUSED(user);
    bonded_addr = info->addr;
    has_bond = true;
}

static int wait_operation(void)
{
    if (k_sem_take(&op_done, OP_TIMEOUT)) {
        /* Never reuse an outstanding async operation's parameters after timeout. */
        LOG_ERR("GATT timeout at %s; restarting without clearing the bond", operation);
        k_sleep(K_MSEC(300));
        sys_reboot(SYS_REBOOT_COLD);
        return -ETIMEDOUT;
    }
    return atomic_get(&link_up) ? op_result : -ENOTCONN;
}

static void begin_operation(void)
{
    k_sem_reset(&op_done);
    op_result = 0;
}

static uint8_t discover_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           struct bt_gatt_discover_params *params)
{
    ARG_UNUSED(conn);
    if (!attr) { k_sem_give(&op_done); return BT_GATT_ITER_STOP; }
    if (params->type == BT_GATT_DISCOVER_PRIMARY) {
        struct bt_gatt_service_val *s = attr->user_data;
        service_start = attr->handle + 1;
        service_end = s->end_handle;
        k_sem_give(&op_done);
        return BT_GATT_ITER_STOP;
    }
    if (params->type == BT_GATT_DISCOVER_CHARACTERISTIC) {
        struct bt_gatt_chrc *c = attr->user_data;
        if (!c || !c->uuid) {
            op_result=-EINVAL; k_sem_give(&op_done); return BT_GATT_ITER_STOP;
        }
        LOG_INF("Characteristic %04x, UUID type=%u", attr->handle, c->uuid->type);
        if (char_count == CHARS_MAX) {
            op_result = -E2BIG; k_sem_give(&op_done); return BT_GATT_ITER_STOP;
        }
        chars[char_count++] = (struct characteristic){
            .decl=attr->handle, .value=c->value_handle, .props=c->properties,
            .uuid=c->uuid->type == BT_UUID_TYPE_16 ? BT_UUID_16(c->uuid)->val : 0,
        };
        /* The last value already occupies the end of this service. There
         * cannot be another declaration or descriptor after it in range. */
        if (c->value_handle>=params->end_handle) {
            k_sem_give(&op_done);
            return BT_GATT_ITER_STOP;
        }
    } else if (params->type == BT_GATT_DISCOVER_DESCRIPTOR) {
        if (!bt_uuid_cmp(attr->uuid, BT_UUID_GATT_CCC)) descriptor_target->ccc=attr->handle;
        if (!bt_uuid_cmp(attr->uuid, BT_UUID_DECLARE_16(0x2908))) descriptor_target->ref=attr->handle;
        if (attr->handle>=params->end_handle) {
            k_sem_give(&op_done);
            return BT_GATT_ITER_STOP;
        }
    }
    return BT_GATT_ITER_CONTINUE;
}

static int discover_range(uint8_t type, uint16_t start, uint16_t end,
                          const struct bt_uuid *uuid)
{
    if (start > end) return 0;
    begin_operation();
    operation=type==BT_GATT_DISCOVER_PRIMARY ? "primary discovery" :
              type==BT_GATT_DISCOVER_CHARACTERISTIC ? "characteristic discovery" : "descriptor discovery";
    LOG_INF("GATT %s: %04x-%04x", operation, start, end);
    memset(&discover, 0, sizeof(discover));
    discover.type=type; discover.start_handle=start; discover.end_handle=end;
    discover.uuid=uuid; discover.func=discover_cb;
    int err=bt_gatt_discover(peer, &discover);
    return err ? err : wait_operation();
}

static uint8_t read_cb(struct bt_conn *conn, uint8_t err,
                      struct bt_gatt_read_params *params, const void *data, uint16_t len)
{
    ARG_UNUSED(conn); ARG_UNUSED(params);
    if (err || !data) {
        op_result=err ? -EIO : 0;
        k_sem_give(&op_done); return BT_GATT_ITER_STOP;
    }
    if (read_size+len > read_capacity) {
        op_result=-EMSGSIZE; k_sem_give(&op_done); return BT_GATT_ITER_STOP;
    }
    memcpy(read_dest+read_size, data, len); read_size+=len;
    return BT_GATT_ITER_CONTINUE;
}

static int read_value(uint16_t handle, uint8_t *dest, size_t capacity)
{
    operation="read";
    LOG_INF("GATT read: handle=%04x", handle);
    begin_operation(); read_dest=dest; read_capacity=capacity; read_size=0;
    memset(&read_params, 0, sizeof(read_params));
    read_params.func=read_cb; read_params.handle_count=1;
    read_params.single.handle=handle;
    int err=bt_gatt_read(peer, &read_params);
    if (!err) err=wait_operation();
    return err ? err : (int)read_size;
}

static void write_cb(struct bt_conn *conn, uint8_t err, struct bt_gatt_write_params *p)
{
    ARG_UNUSED(conn); ARG_UNUSED(p);
    op_result=err ? -EIO : 0; k_sem_give(&op_done);
}

static int write_value(uint16_t handle, uint8_t props, const uint8_t *data, size_t len)
{
    if (!atomic_get(&link_up)) return -ENOTCONN;
    if (props & BT_GATT_CHRC_WRITE_WITHOUT_RESP)
        return bt_gatt_write_without_response(peer, handle, data, len, false);
    if (!(props & BT_GATT_CHRC_WRITE)) return -ENOTSUP;
    begin_operation();
    memset(&write_params, 0, sizeof(write_params));
    operation="write";
    LOG_INF("GATT write: handle=%04x", handle);
    write_params.handle=handle; write_params.data=data;
    write_params.length=len; write_params.func=write_cb;
    int err=bt_gatt_write(peer, &write_params);
    return err ? err : wait_operation();
}

static void usb_status(enum usb_dc_status_code status, const uint8_t *param)
{
    atomic_set(&last_usb_status, status);
    LOG_INF("USB status %u", status);
    if (status == USB_DC_CONFIGURED) {
        bool configured=param && *param != 0;
        atomic_set(&usb_mounted, configured);
        if (configured) atomic_set(&release_needed, 1);
    } else if (status == USB_DC_DISCONNECTED || status == USB_DC_RESET ||
               status == USB_DC_SUSPEND) {
        atomic_clear(&usb_mounted);
    } else if (status == USB_DC_RESUME) {
        atomic_set(&usb_mounted, 1);
        atomic_set(&release_needed, 1);
    }
}

static void input_sent(const struct device *dev)
{
    ARG_UNUSED(dev); k_sem_give(&usb_done);
}

static int queue_output(const uint8_t *data, size_t len)
{
    if (!atomic_get(&ready) || !len || len > BRIDGE_PACKET_MAX) return -EINVAL;
    uint8_t id=map_info.uses_ids ? data[0] : 0;
    size_t offset=map_info.uses_ids ? 1 : 0;
    if (!map_info.output_len[id] || len != map_info.output_len[id]+offset) return -EINVAL;
    struct packet p={.len=len}; memcpy(p.data, data, len);
    return k_msgq_put(&output_queue, &p, K_NO_WAIT);
}

static void output_received(const struct device *dev)
{
    struct packet p; uint32_t len=0;
    if (!hid_int_ep_read(dev, p.data, sizeof(p.data), &len)) queue_output(p.data, len);
}

static int set_report(const struct device *dev, struct usb_setup_packet *setup,
                       int32_t *len, uint8_t **data)
{
    ARG_UNUSED(dev);
    uint16_t value=sys_le16_to_cpu(setup->wValue);
    if ((value >> 8) != 2 || *len <= 0) return -ENOTSUP;
    if (map_info.uses_ids && (*data)[0] != (value & 0xff)) return -EINVAL;
    return queue_output(*data, *len);
}

static const struct hid_ops usb_ops={
    .set_report=set_report, .int_in_ready=input_sent, .int_out_ready=output_received,
};

static int start_usb(void)
{
    if (usb_started) return 0;
    usb=device_get_binding("HID_0");
    if (!usb) return -ENODEV;
    usb_hid_register_device(usb, report_map, map_len, &usb_ops);
    int err=usb_hid_init(usb);
    if (!err) err=usb_enable(usb_status);
    if (!err) usb_started=true;
    return err;
}

static int send_packet(const uint8_t *data, size_t len)
{
    if (!atomic_get(&usb_mounted)) return -ENOTCONN;
    k_sem_reset(&usb_done);
    int err=hid_int_ep_write(usb, data, len, NULL);
    if (err) { atomic_inc(&usb_failures); return err; }
    if (k_sem_take(&usb_done, K_MSEC(500))) {
        /* An abandoned in-flight transfer must not race the next report. */
        LOG_ERR("USB IN stalled; restarting to release host state");
        sys_reboot(SYS_REBOOT_COLD);
        return -ETIMEDOUT;
    }
    atomic_inc(&usb_reports);
    return 0;
}

static void send_releases(void)
{
    uint8_t zeros[BRIDGE_PACKET_MAX]={0};
    k_msgq_purge(&input_queue);
    for (unsigned id=0; id<256; id++) {
        if (!map_info.input_len[id]) continue;
        if (map_info.uses_ids) zeros[0]=id;
        if (send_packet(zeros, map_info.input_len[id]+map_info.uses_ids)) {
            atomic_set(&release_needed, 1);
            k_sleep(K_MSEC(10));
            return;
        }
    }
}

static void input_worker(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    struct packet p;
    while (true) {
        if (!atomic_get(&usb_mounted)) { k_sleep(K_MSEC(10)); continue; }
        if (atomic_cas(&release_needed, 1, 0)) { send_releases(); continue; }
        if (k_msgq_get(&input_queue, &p, K_MSEC(20))) continue;
        if (send_packet(p.data, p.len)) {
            /* Losing a key-up could leave a stuck key. Fail closed and reset state. */
            atomic_set(&release_needed, 1); atomic_set(&queue_fault, 1);
        }
    }
}
K_THREAD_DEFINE(input_thread, 2048, input_worker, NULL, NULL, NULL, 7, 0, 0);

static uint8_t input_notify(struct bt_conn *conn, struct bt_gatt_subscribe_params *sub,
                           const void *data, uint16_t len)
{
    ARG_UNUSED(conn);
    struct report *r=CONTAINER_OF(sub, struct report, sub);
    if (!data) {
        sub->value_handle=0;
        if (atomic_get(&ready)) {
            atomic_set(&queue_fault, 1); atomic_set(&release_needed, 1);
        }
        return BT_GATT_ITER_STOP;
    }
    atomic_inc(&ble_reports);
    atomic_set(&last_report_time, k_uptime_get_32());
    if (!atomic_get(&ready) || !atomic_get(&usb_mounted)) return BT_GATT_ITER_CONTINUE;
    struct packet p;
    int n=hid_packet(&map_info, r->id, data, len, p.data);
    if (n < 0) { atomic_set(&queue_fault, 1); return BT_GATT_ITER_CONTINUE; }
    p.len=n;
    if (k_msgq_put(&input_queue, &p, K_NO_WAIT)) {
        atomic_set(&queue_fault, 1); atomic_set(&release_needed, 1);
    }
    return BT_GATT_ITER_CONTINUE;
}

static void subscribed(struct bt_conn *conn, uint8_t err, struct bt_gatt_subscribe_params *sub)
{
    ARG_UNUSED(conn); ARG_UNUSED(sub);
    op_result=err ? -EIO : 0; k_sem_give(&op_done);
}

static void exchanged(struct bt_conn *conn, uint8_t err, struct bt_gatt_exchange_params *p)
{
    ARG_UNUSED(conn); ARG_UNUSED(p);
    /* Default ATT MTU is still usable if the exchange is not supported. */
    op_result=0; k_sem_give(&op_done);
}

static int setup_hid(void)
{
    LOG_INF("HID initialization starting");
    service_start=service_end=0; char_count=report_count=0;
    memset(reports, 0, sizeof(reports));
    operation="MTU exchange";
    LOG_INF("GATT MTU exchange starting");
    begin_operation(); mtu_params.func=exchanged;
    int err=bt_gatt_exchange_mtu(peer, &mtu_params);
    if (!err && (err=wait_operation())) return err;
    LOG_INF("ATT MTU=%u", bt_gatt_get_mtu(peer));
    err=discover_range(BT_GATT_DISCOVER_PRIMARY, 1, 0xffff, BT_UUID_HIDS);
    if (err || !service_start) return err ? err : -ENOENT;
    err=discover_range(BT_GATT_DISCOVER_CHARACTERISTIC, service_start, service_end, NULL);
    if (err) return err;
    LOG_INF("HID service: %04x-%04x, %u characteristics", service_start, service_end, char_count);
    uint16_t map_handle=0, protocol_handle=0; uint8_t protocol_props=0;
    for (unsigned i=0; i<char_count; i++) {
        if (chars[i].uuid == 0x2a4b) map_handle=chars[i].value;
        if (chars[i].uuid == 0x2a4e) {
            protocol_handle=chars[i].value; protocol_props=chars[i].props;
        }
        if (chars[i].uuid != 0x2a4d) continue;
        if (report_count == REPORTS_MAX) return -E2BIG;
        struct report *r=&reports[report_count++];
        r->value=chars[i].value; r->props=chars[i].props;
        descriptor_target=r;
        err=discover_range(BT_GATT_DISCOVER_DESCRIPTOR, r->value+1,
                          i+1<char_count ? chars[i+1].decl-1 : service_end, NULL);
        if (err || !r->ref) return err ? err : -EINVAL;
        uint8_t ref[2];
        err=read_value(r->ref, ref, sizeof(ref));
        if (err != 2) return err < 0 ? err : -EINVAL;
        r->id=ref[0]; r->type=ref[1];
        LOG_INF("Report ID=%u type=%u value=%04x CCC=%04x", r->id, r->type, r->value, r->ccc);
    }
    if (!map_handle) return -ENOENT;
    static uint8_t new_map[BRIDGE_MAP_MAX];
    int n=read_value(map_handle, new_map, sizeof(new_map));
    if (n <= 0) return n ? n : -EINVAL;
    static struct hid_map_info parsed;
    err=hid_map_parse(new_map, n, &parsed);
    if (err) return err;
    if (map_len != n || memcmp(report_map, new_map, n)) {
        err=settings_save_one("bridge/map", new_map, n);
        if (err) return err;
        if (usb_started) {
            LOG_INF("Report map changed; saved and restarting for USB enumeration");
            /* A first pairing may still have a deferred Bluetooth key save.
             * Do not reboot with only the keyboard's half of the bond stored. */
            k_sleep(K_MSEC(1500));
            sys_reboot(SYS_REBOOT_COLD);
        }
        memcpy(report_map, new_map, n); map_len=n; map_info=parsed;
    }
    /* Refuse incomplete mappings instead of dropping keys or LEDs silently. */
    for (unsigned id=0; id<256; id++) {
        bool found=false;
        if (!map_info.input_len[id]) continue;
        if (map_info.input_len[id] > bt_gatt_get_mtu(peer)-3) return -EMSGSIZE;
        for (unsigned i=0; i<report_count; i++) {
            if (reports[i].type==1 && reports[i].id==id && reports[i].ccc &&
                (reports[i].props & BT_GATT_CHRC_NOTIFY)) found=true;
        }
        if (!found) return -EINVAL;
    }
    for (unsigned id=0; id<256; id++) {
        bool found=false;
        if (!map_info.output_len[id]) continue;
        for (unsigned i=0; i<report_count; i++) {
            if (reports[i].type==2 && reports[i].id==id &&
                (reports[i].props & (BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP)))
                found=true;
        }
        if (!found) return -EINVAL;
    }
    if (protocol_handle) {
        const uint8_t report_protocol=1;
        err=write_value(protocol_handle, protocol_props, &report_protocol, 1);
        if (err) return err;
    }
    err=start_usb();
    if (err) return err;
    for (unsigned i=0; i<report_count; i++) {
        struct report *r=&reports[i];
        if (r->type != 1) continue;
        r->sub.notify=input_notify; r->sub.subscribe=subscribed;
        r->sub.value=BT_GATT_CCC_NOTIFY; r->sub.value_handle=r->value;
        r->sub.ccc_handle=r->ccc; r->sub.min_security=BT_SECURITY_L2;
        atomic_set_bit(r->sub.flags, BT_GATT_SUBSCRIBE_FLAG_VOLATILE);
        begin_operation();
        operation="CCC subscription";
        LOG_INF("Subscribe report ID=%u", r->id);
        err=bt_gatt_subscribe(peer, &r->sub);
        if (!err) err=wait_operation();
        if (err) return err;
    }
    atomic_set(&ready, 1);
    if(resume_after_connect) { paused=false;resume_after_connect=false; }
    LOG_INF("HID ready: %u reports, %u-byte map", report_count, (unsigned)map_len);
    return 0;
}

static void connected(struct bt_conn *conn, uint8_t err)
{
    ARG_UNUSED(conn);
    atomic_set(&connected_at, k_uptime_get_32());
    atomic_set(&link_up, err == 0); k_sem_give(&link_event);
    LOG_INF("BLE connection result %u", err);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    ARG_UNUSED(conn);
    atomic_clear(&ready); atomic_clear(&link_up); atomic_clear(&secure);
    atomic_set(&release_needed, 1);
    k_msgq_purge(&output_queue);
    op_result=-ENOTCONN; k_sem_give(&op_done); k_sem_give(&link_event);
    LOG_INF("BLE disconnected: %u", reason);
}

static void security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
    ARG_UNUSED(conn);
    LOG_INF("Security level %u, result %u", level, err);
    if (!err && level >= BT_SECURITY_L2) atomic_set(&secure, 1);
    else if (err) {
        if (err==BT_SECURITY_ERR_PIN_OR_KEY_MISSING) {
            atomic_set(&stale_bond, 1);
        }
        LOG_ERR("BLE security error %u; inspect the disconnect reason", err);
        bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
    }
    k_sem_give(&link_event);
}
BT_CONN_CB_DEFINE(connection_callbacks)={
    .connected=connected, .disconnected=disconnected, .security_changed=security_changed,
};

struct advertisement { bool hid, name; char label[33]; };
static bool parse_ad(struct bt_data *ad, void *user)
{
    struct advertisement *a=user;
    if (ad->type==BT_DATA_NAME_COMPLETE || ad->type==BT_DATA_NAME_SHORTENED) {
        unsigned n=MIN(ad->data_len,32);
        for(unsigned i=0;i<n;i++) {
            unsigned char c=ad->data[i];
            a->label[i]=(c>=32 && c<127 && c!='"' && c!='\\') ? c : '?';
        }
        a->label[n]=0;
    }
    if (ad->type==BT_DATA_UUID16_ALL || ad->type==BT_DATA_UUID16_SOME) {
        for (unsigned i=0; i+1<ad->data_len; i+=2)
            if (sys_get_le16(ad->data+i)==0x1812) a->hid=true;
    }
    if ((ad->type==BT_DATA_NAME_COMPLETE || ad->type==BT_DATA_NAME_SHORTENED) &&
        strlen(CONFIG_BRIDGE_PEER_NAME)==ad->data_len &&
        !memcmp(ad->data, CONFIG_BRIDGE_PEER_NAME, ad->data_len)) a->name=true;
    return true;
}

static void scan_found(const struct bt_le_scan_recv_info *info, struct net_buf_simple *ad)
{
    atomic_inc(&advertisements);
    struct advertisement observed={0};
    bt_data_parse(ad, parse_ad, &observed);
    if (observed.hid || observed.label[0]) {
        k_spinlock_key_t key=k_spin_lock(&seen_lock);
        int slot=-1;
        for(unsigned i=0;i<ARRAY_SIZE(seen);i++) {
            if (seen[i].used && !bt_addr_le_cmp(&seen[i].addr,info->addr)) { slot=i; break; }
            if (!seen[i].used && slot<0) slot=i;
        }
        if(slot>=0 && (observed.hid || seen[slot].used)) {
            seen[slot].used=true;seen[slot].addr=*info->addr;seen[slot].rssi=info->rssi;
            if(observed.label[0]) memcpy(seen[slot].name,observed.label,sizeof(observed.label));
        }
        k_spin_unlock(&seen_lock,key);
    }
    if (paused) return;
    if (atomic_get(&selecting) || !(info->adv_props & (BT_GAP_ADV_PROP_CONNECTABLE |
                                   BT_GAP_ADV_PROP_SCAN_RESPONSE))) return;
    if (has_bond) {
        if (bt_addr_le_cmp(info->addr, &bonded_addr)) return;
    } else {
        struct advertisement a={0};
        bt_data_parse(ad, parse_ad, &a);
        /* ZMK puts the name in the scan response; appearance/service may be
         * in a separate advertising packet. An exact name is sufficient here. */
        if (strlen(CONFIG_BRIDGE_PEER_NAME) ? !a.name : !a.hid) return;
    }
    atomic_inc(&target_hits);
    LOG_INF("Selected peer advertisement, RSSI %d", info->rssi);
    if (!atomic_cas(&selecting, 0, 1)) return;
    if (k_msgq_put(&candidate_queue, info->addr, K_NO_WAIT)) atomic_clear(&selecting);
    k_sem_give(&link_event);
}
static struct bt_le_scan_cb scan_callbacks={.recv=scan_found};

static void clear_pairing(void)
{
    LOG_INF("Clearing bridge bond and descriptor; keyboard's other profiles unaffected");
    atomic_clear(&ready);
    atomic_set(&release_needed, 1);
    k_sleep(K_MSEC(100));
    int err=bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);
    if (!err) err=settings_delete("bridge/map");
    if (err) LOG_ERR("Clear failed: %d", err);
    sys_reboot(SYS_REBOOT_COLD);
}


static void control_status(void)
{
    printk("@bridge {\"event\":\"status\",\"protocol\":1,\"version\":\"v8-web\",\"ble\":%d,\"usb\":%d,\"secure\":%d,\"bonded\":%d,\"paused\":%d,\"rx\":%ld,\"tx\":%ld,\"errors\":%ld}\n",
           (int)atomic_get(&ready),(int)atomic_get(&usb_mounted),
           (int)atomic_get(&secure),has_bond,paused,
           (long)atomic_get(&ble_reports),(long)atomic_get(&usb_reports),(long)atomic_get(&usb_failures));
}

static void control_devices(void)
{
    struct seen_device snapshot[12];
    k_spinlock_key_t key=k_spin_lock(&seen_lock);
    memcpy(snapshot,seen,sizeof(snapshot));
    k_spin_unlock(&seen_lock,key);
    printk("@bridge {\"event\":\"devices_begin\"}\n");
    for(unsigned i=0;i<ARRAY_SIZE(snapshot);i++) {
        if(!snapshot[i].used) continue;
        char addr[BT_ADDR_LE_STR_LEN];bt_addr_le_to_str(&snapshot[i].addr,addr,sizeof(addr));
        printk("@bridge {\"event\":\"device\",\"index\":%u,\"name\":\"%s\",\"address\":\"%s\",\"rssi\":%d}\n",
               i,snapshot[i].name,addr,snapshot[i].rssi);
    }
    printk("@bridge {\"event\":\"devices_end\"}\n");
}

static void control_error(const char *message)
{
    printk("@bridge {\"event\":\"error\",\"message\":\"%s\"}\n",message);
}

static void control_command(char *line)
{
    if(!strcmp(line,"@status")) { control_status();return; }
    if(!strcmp(line,"@devices")) { control_devices();return; }
    if(!strcmp(line,"@scan")) {
        paused=true;scan_deadline=k_uptime_get()+15000;
        int err=bt_le_scan_start(BT_LE_SCAN_ACTIVE,NULL);
        if(err && err!=-EALREADY) { control_error("scan_failed");return; }
        printk("@bridge {\"event\":\"scanning\",\"seconds\":15}\n");return;
    }
    if(!strcmp(line,"@disconnect") || !strcmp(line,"@clear")) {
        paused=true;resume_after_connect=false;scan_deadline=0;bt_le_scan_stop();
        k_msgq_purge(&candidate_queue);
        if(!strcmp(line,"@clear")) clear_pending=true;
        if(peer) bt_conn_disconnect(peer,BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        else atomic_clear(&selecting);
        printk("@bridge {\"event\":\"accepted\"}\n");return;
    }
    if(!strcmp(line,"@resume")) {
        if(clear_pending) { control_error("busy");return; }
        paused=false;scan_deadline=0;pairing_deadline=k_uptime_get()+120000;
        printk("@bridge {\"event\":\"resuming\"}\n");return;
    }
    if(!strncmp(line,"@connect ",9)) {
        char *end;long index=strtol(line+9,&end,10);
        if(end==line+9 || *end || index<0 || index>=ARRAY_SIZE(seen)) { control_error("invalid_index");return; }
        if(peer || clear_pending) { control_error("disconnect_first");return; }
        struct seen_device target;
        k_spinlock_key_t key=k_spin_lock(&seen_lock);target=seen[index];k_spin_unlock(&seen_lock,key);
        if(!target.used) { control_error("device_not_found");return; }
        if(has_bond && bt_addr_le_cmp(&target.addr,&bonded_addr)) { control_error("clear_binding_first");return; }
        paused=true;resume_after_connect=true;scan_deadline=0;bt_le_scan_stop();
        k_msgq_purge(&candidate_queue);atomic_set(&selecting,1);
        if(k_msgq_put(&candidate_queue,&target.addr,K_NO_WAIT)) { atomic_clear(&selecting);control_error("busy");return; }
        printk("@bridge {\"event\":\"connecting\"}\n");return;
    }
    control_error("unknown_command");
}

static void control_poll(void)
{
#if DT_HAS_CHOSEN(zephyr_console)
    const struct device *uart=DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
    static char line[96];static unsigned used;static bool overflow;
    unsigned char c;
    for(unsigned n=0;n<128 && !uart_poll_in(uart,&c);n++) {
        if(c=='\r') continue;
        if(c=='\n') {
            if(overflow) control_error("command_too_long");
            else if(used) { line[used]=0;control_command(line); }
            used=0;overflow=false;
        } else if(used<sizeof(line)-1 && !overflow) line[used++]=c;
        else overflow=true;
    }
    if(scan_deadline && k_uptime_get()>scan_deadline) { scan_deadline=0;bt_le_scan_stop(); }
    if(clear_pending && !peer) {
        int err=bt_unpair(BT_ID_DEFAULT,BT_ADDR_LE_ANY);
        has_bond=false;bt_foreach_bond(BT_ID_DEFAULT,bond_found,NULL);
        atomic_clear(&stale_bond);atomic_clear(&selecting);clear_pending=false;
        if(err) control_error("clear_failed");
        else printk("@bridge {\"event\":\"cleared\"}\n");
    }
#endif
}

static void poll_button_led(void)
{
    static int64_t pressed_at=-1;
    int64_t now=k_uptime_get();
    if (gpio_pin_get_dt(&button)>0) {
        if (pressed_at<0) pressed_at=now;
        if (now-pressed_at >= 3000) clear_pairing();
    } else pressed_at=-1;
    bool on;
    if (atomic_get(&ready) && atomic_get(&usb_mounted)) {
        /* Brief blink on incoming BLE reports, otherwise solid end-to-end ready. */
        on=!atomic_get(&ble_reports) ||
           (uint32_t)(k_uptime_get_32()-atomic_get(&last_report_time))>=100;
    } else if (atomic_get(&ready)) {
        /* BLE is ready but USB has NOT enumerated: two pulses every 2 seconds. */
        unsigned phase=now%2000;
        on=phase<120 || (phase>=260 && phase<380);
    } else {
        on=(now/(has_bond ? 600 : 150))&1;
    }
    gpio_pin_set_dt(&led, on);
    static int64_t logged_at;
    if (now-logged_at>=5000) {
        logged_at=now;
        LOG_INF("status BLE=%ld USB=%ld event=%ld RX=%ld TX=%ld ERR=%ld ADV=%ld MATCH=%ld",
                (long)atomic_get(&ready), (long)atomic_get(&usb_mounted),
                (long)atomic_get(&last_usb_status), (long)atomic_get(&ble_reports),
                (long)atomic_get(&usb_reports), (long)atomic_get(&usb_failures),
                (long)atomic_get(&advertisements), (long)atomic_get(&target_hits));
    }
}

int main(void)
{
    if (!gpio_is_ready_dt(&led) || !gpio_is_ready_dt(&button)) return 0;
    gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&button, GPIO_INPUT);
    /* Enumerate USB independently of radio/pairing so diagnostics stay available
     * even when the peer cannot be found or the LF clock cannot start Bluetooth. */
    int err=settings_subsys_init();
    if (err) { LOG_ERR("Settings init %d", err); return 0; }
    err=settings_load_subtree("bridge");
    if (err) LOG_WRN("Descriptor cache load %d", err);
    if (!map_len) {
        memcpy(report_map, fallback_map, sizeof(fallback_map));
        map_len=sizeof(fallback_map);
        if (hid_map_parse(report_map, map_len, &map_info)) return 0;
    }
    if ((err=start_usb())) { LOG_ERR("USB init %d", err); return 0; }
    LOG_INF("Bridge v8-web starting; expected peer name '%s'", CONFIG_BRIDGE_PEER_NAME);
    uint32_t reset_cause=0;
    if (!hwinfo_get_reset_cause(&reset_cause)) {
        LOG_INF("Reset cause mask=%08x", reset_cause);
        hwinfo_clear_reset_cause();
    }
    if (last_crash.magic==0x42524348) {
        LOG_ERR("Previous crash: reason=%u PC=%08x LR=%08x CFSR=%08x HFSR=%08x",
                last_crash.reason, last_crash.pc, last_crash.lr, last_crash.cfsr, last_crash.hfsr);
        last_crash.magic=0;
        /* Allow a connected diagnostic console to receive this startup record. */
        k_sleep(K_SECONDS(3));
        LOG_ERR("Crash replay: PC=%08x LR=%08x CFSR=%08x reason=%u",
                last_crash.pc, last_crash.lr, last_crash.cfsr, last_crash.reason);
    }
    LOG_INF("LF clock source: %s", IS_ENABLED(CONFIG_CLOCK_CONTROL_NRF_K32SRC_XTAL) ? "XTAL" : "RC");
    err=bt_enable(NULL);
    if (err) { LOG_ERR("BT init %d", err); return 0; }
    err=settings_load_subtree("bt");
    if (err) LOG_WRN("Settings load %d", err);
    bt_foreach_bond(BT_ID_DEFAULT, bond_found, NULL);
    bt_le_scan_cb_register(&scan_callbacks);
    LOG_INF("Bluetooth initialized; stored bond=%u", has_bond);
    pairing_deadline=k_uptime_get()+120000;
    while (true) {
        poll_button_led();
        control_poll();
        if (!peer) {
            bt_addr_le_t candidate;
            if (!k_msgq_get(&candidate_queue, &candidate, K_NO_WAIT)) {
                bt_le_scan_stop();
                err=bt_conn_le_create(&candidate, BT_CONN_LE_CREATE_CONN,
                                     BT_LE_CONN_PARAM_DEFAULT, &peer);
                if (err) { LOG_ERR("Create connection: %d", err); atomic_clear(&selecting); }
                continue;
            }
            if(paused) { k_sleep(K_MSEC(20));continue; }
            if (!has_bond && k_uptime_get()>pairing_deadline) {
                /* Replug for another first-pairing window; avoid indefinite discovery. */
                bt_le_scan_stop(); k_sleep(K_MSEC(50)); continue;
            }
            k_sem_reset(&link_event);
            err=bt_le_scan_start(BT_LE_SCAN_ACTIVE, NULL);
            if (err && err!=-EALREADY) LOG_WRN("Scan start %d", err);
            k_sem_take(&link_event, K_MSEC(50));
            continue;
        }
        if (!atomic_get(&link_up)) {
            /* bt_conn_le_create may still be pending; wait for its callback. */
            struct bt_conn_info info;
            if (!bt_conn_get_info(peer, &info) && info.state!=BT_CONN_STATE_DISCONNECTED) {
                k_sleep(K_MSEC(50)); continue;
            }
            if (atomic_cas(&stale_bond, 1, 0)) {
                /* Only an explicit missing-key error allows this recovery.
                 * Retain the USB descriptor; never clear unrelated identities. */
                int unpair_err=bt_unpair(BT_ID_DEFAULT, bt_conn_get_dst(peer));
                LOG_INF("Removed stale peer bond: %d", unpair_err);
                pairing_deadline=k_uptime_get()+120000;
            }
            bt_conn_unref(peer); peer=NULL;
            has_bond=false; bt_foreach_bond(BT_ID_DEFAULT, bond_found, NULL);
            atomic_clear(&selecting);
            k_sleep(K_MSEC(500)); continue;
        }
        if (!atomic_get(&secure)) {
            /* Establish the radio link before initiating SMP. Start with
             * Zephyr's zero-latency connection parameters, not a power-saving
             * peripheral latency of 30 on the very first connection events. */
            if ((uint32_t)(k_uptime_get_32()-atomic_get(&connected_at))<300) {
                k_sleep(K_MSEC(20)); continue;
            }
            err=bt_conn_set_security(peer, BT_SECURITY_L2);
            if (err) { LOG_ERR("Security request %d", err); bt_conn_disconnect(peer, BT_HCI_ERR_AUTH_FAIL); }
            k_sem_take(&link_event, K_MSEC(50));
            continue;
        }
        if (!atomic_get(&ready)) {
            err=setup_hid();
            if (err) {
                LOG_ERR("HID setup %d", err);
                bt_conn_disconnect(peer, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
                /* Let all GATT cancellation callbacks drain before reusing params. */
                while (atomic_get(&link_up)) k_sleep(K_MSEC(20));
                k_sleep(K_MSEC(100));
            }
            continue;
        }
        if (atomic_cas(&queue_fault, 1, 0)) {
            LOG_ERR("Report stream lost; releasing keys and reconnecting");
            bt_conn_disconnect(peer, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
            continue;
        }
        struct packet p;
        if (!k_msgq_get(&output_queue, &p, K_MSEC(20))) {
            unsigned offset=map_info.uses_ids ? 1 : 0;
            uint8_t id=offset ? p.data[0] : 0;
            for (unsigned i=0; i<report_count; i++) {
                if (reports[i].type!=2 || reports[i].id!=id) continue;
                err=write_value(reports[i].value, reports[i].props, p.data+offset, p.len-offset);
                if (err) LOG_WRN("Output report %u failed: %d", id, err);
                break;
            }
        }
    }
    return 0;
}
