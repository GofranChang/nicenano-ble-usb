#include "hid_map.h"
#include <errno.h>
#include <string.h>

struct globals { uint32_t size, count; uint8_t id; };

int hid_map_parse(const uint8_t *map, size_t len, struct hid_map_info *info)
{
    struct globals g = {0}, stack[8];
    unsigned depth = 0;
    uint32_t bits[2][256] = {{0}};
    bool has_input = false;
    memset(info, 0, sizeof(*info));
    if (!len || len > BRIDGE_MAP_MAX) return -EINVAL;
    for (size_t pos = 0; pos < len;) {
        uint8_t prefix = map[pos++];
        if (prefix == 0xfe) return -ENOTSUP; /* HID long items are not handled. */
        unsigned n = prefix & 3;
        if (n == 3) n = 4;
        if (pos + n > len) return -EINVAL;
        uint32_t value = 0;
        for (unsigned j = 0; j < n; j++) value |= (uint32_t)map[pos+j] << (8*j);
        pos += n;
        unsigned type = (prefix >> 2) & 3, tag = prefix >> 4;
        if (type == 1) {
            switch (tag) {
            case 7: g.size = value; break;
            case 8:
                if (!value || value > 255) return -EINVAL;
                g.id = value; info->uses_ids = true; break;
            case 9: g.count = value; break;
            case 10:
                if (depth == 8) return -E2BIG;
                stack[depth++] = g; break;
            case 11:
                if (!depth) return -EINVAL;
                g = stack[--depth]; break;
            }
        } else if (type == 0 && (tag == 8 || tag == 9)) {
            unsigned k = tag == 8 ? 0 : 1;
            if (g.size > 512 || g.count > 512 || g.size * g.count > 512)
                return -EMSGSIZE;
            bits[k][g.id] += g.size * g.count;
            if (bits[k][g.id] > 512) return -EMSGSIZE;
            if (k == 0) has_input = true;
        }
    }
    if (depth || !has_input || (info->uses_ids && (bits[0][0] || bits[1][0])))
        return -EINVAL;
    for (unsigned id = 0; id < 256; id++) {
        unsigned in = (bits[0][id]+7)/8, out = (bits[1][id]+7)/8;
        if (in + info->uses_ids > BRIDGE_PACKET_MAX ||
            out + info->uses_ids > BRIDGE_PACKET_MAX) return -EMSGSIZE;
        info->input_len[id] = in;
        info->output_len[id] = out;
    }
    return 0;
}

int hid_packet(const struct hid_map_info *info, uint8_t id,
               const uint8_t *data, size_t len, uint8_t *out)
{
    if (!info->input_len[id] || len != info->input_len[id] ||
        (!info->uses_ids && id != 0)) return -EINVAL;
    unsigned offset = info->uses_ids ? 1 : 0;
    if (offset) out[0] = id;
    memcpy(out+offset, data, len);
    return len+offset;
}
