#pragma once

#include <stddef.h>
#include <stdint.h>

uint16_t crc16_ccitt_false(const uint8_t* data, size_t len);
size_t build_packet(uint8_t port_id, uint32_t seq, uint8_t payload_len, uint8_t* out, size_t out_cap);
