#ifndef RESIDENT_TEXT_H
#define RESIDENT_TEXT_H

#include "resident_hardware.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void resident_text_format_ipv4(const uint8_t value[4], char *text, size_t text_size);
bool resident_text_ipv4_is_unusable_reserved(const uint8_t addr[4]);
bool resident_text_parse_mac(const uint8_t *text, uint16_t text_len, uint8_t value[6]);
bool resident_text_parse_ipv4(const uint8_t *text, uint16_t text_len, uint8_t value[4]);
bool resident_text_parse_u32(const uint8_t *text, uint16_t text_len, uint32_t *value);
bool resident_text_parse_rail_mode(const uint8_t *text, uint16_t text_len, ResidentHardwareRailMode *mode);
bool resident_text_parse_net_dhcp(const uint8_t *text, uint16_t text_len, uint8_t *enabled_out);
void resident_text_bytes_to_hex(const uint8_t *bytes, uint16_t byte_len, char *hex_out, size_t hex_cap);

#endif /* RESIDENT_TEXT_H */
