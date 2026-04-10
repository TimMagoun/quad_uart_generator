#include "command_parser.h"

#include <ctype.h>
#include <stddef.h>

bool parse_format_token(const char* token, ParsedFormat* out) {
    if (token == nullptr || out == nullptr) {
        return false;
    }

    if (token[0] == '\0' || token[1] == '\0' || token[2] == '\0') {
        return false;
    }
    if (token[3] == '\0') {
        // exact length 3
    } else {
        return false;
    }

    const char d = token[0];
    const char p = static_cast<char>(toupper(static_cast<unsigned char>(token[1])));
    const char s = token[2];

    if (d < '5' || d > '8') {
        return false;
    }

    const bool parity_ok = (p == 'N') || (p == 'E') || (p == 'O');
    if (parity_ok == false) {
        return false;
    }

    const bool stop_ok = (s == '1') || (s == '2');
    if (stop_ok == false) {
        return false;
    }

    out->data_bits = static_cast<uint8_t>(d - '0');
    out->parity = p;
    out->stop_bits = static_cast<uint8_t>(s - '0');
    return true;
}
