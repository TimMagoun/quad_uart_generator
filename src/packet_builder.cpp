#include "packet_builder.h"

uint16_t crc16_ccitt_false(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int b = 0; b < 8; ++b) {
            if (crc & 0x8000) {
                crc = static_cast<uint16_t>((crc << 1) ^ 0x1021);
            } else {
                crc = static_cast<uint16_t>(crc << 1);
            }
        }
    }
    return crc;
}

size_t build_packet(uint8_t port_id, uint32_t seq, uint8_t payload_len, uint8_t* out, size_t out_cap) {
    if (!out || payload_len < 10 || payload_len > 128 || out_cap < payload_len) {
        return 0;
    }

    out[0] = 0x51;
    out[1] = 0x55;
    out[2] = port_id;
    out[3] = static_cast<uint8_t>(seq & 0xFF);
    out[4] = static_cast<uint8_t>((seq >> 8) & 0xFF);
    out[5] = static_cast<uint8_t>((seq >> 16) & 0xFF);
    out[6] = static_cast<uint8_t>((seq >> 24) & 0xFF);
    out[7] = payload_len;

    for (uint8_t i = 8; i < static_cast<uint8_t>(payload_len - 2); ++i) {
        out[i] = static_cast<uint8_t>(port_id ^ static_cast<uint8_t>(seq) ^ i);
    }

    const uint16_t crc = crc16_ccitt_false(out, payload_len - 2);
    out[payload_len - 2] = static_cast<uint8_t>(crc & 0xFF);
    out[payload_len - 1] = static_cast<uint8_t>((crc >> 8) & 0xFF);
    return payload_len;
}
