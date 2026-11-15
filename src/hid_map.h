#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BRIDGE_PACKET_MAX 64
#define BRIDGE_MAP_MAX 1536

struct hid_map_info {
    bool uses_ids;
    uint8_t input_len[256];  /* Payload length, without report ID. */
    uint8_t output_len[256];
};
/* Reject malformed maps, unnumbered/numbered mixtures and oversized reports. */
int hid_map_parse(const uint8_t *map, size_t len, struct hid_map_info *info);
int hid_packet(const struct hid_map_info *info, uint8_t id,
               const uint8_t *data, size_t len, uint8_t *out);
