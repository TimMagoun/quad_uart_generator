#pragma once

#include <stdint.h>
#include <stddef.h>

enum class ParityMode : uint8_t { None, Even, Odd };

struct PortConfig {
    uint32_t baud;
    uint8_t data_bits;
    ParityMode parity;
    uint8_t stop_bits;
    uint8_t payload_len;
    uint16_t pps;
};

struct PortRuntime {
    bool enabled;
    uint32_t seq;
    uint32_t next_deadline_us;
    bool tx_in_progress;
    uint8_t tx_write_index;
    uint8_t tx_packet_len;
    uint8_t tx_packet_buf[128];
};
