#pragma once

#include <stdint.h>

struct ParsedFormat {
    uint8_t data_bits;
    char parity;
    uint8_t stop_bits;
};

bool parse_format_token(const char* token, ParsedFormat* out);
