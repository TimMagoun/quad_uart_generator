#include "quad_uart_controller.h"

#include "command_parser.h"
#include "packet_builder.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {
constexpr uint16_t kMinPps = 1;
constexpr uint16_t kMaxPps = 1000;
constexpr const char kHelpText[] =
    "OK\n"
    "Commands:\n"
    "  HELP\n"
    "  STATUS\n"
    "  ALL ENABLE\n"
    "  ALL DISABLE\n"
    "  PORT <id> SHOW\n"
    "  PORT <id> ENABLE\n"
    "  PORT <id> DISABLE\n"
    "  PORT <id> CFG BAUD <1..921600>\n"
    "  PORT <id> CFG FORMAT <5N1..8O2>\n"
    "  PORT <id> CFG LEN <10..128>\n"
    "  PORT <id> CFG PPS <1..1000>\n"
    "Notes:\n"
    "  - Port id range: 0..3\n"
    "  - Disable all outputs before any CFG change\n"
    "  - FORMAT examples: 8N1 7E1 8O2";

bool parse_u32(const char* s, uint32_t* out) {
    if (out == nullptr || s == nullptr || s[0] == '\0') {
        return false;
    }

    uint32_t v = 0;
    for (size_t i = 0; s[i] != '\0'; i++) {
        const char c = s[i];
        if (c < '0' || c > '9') {
            return false;
        }
        const uint32_t digit = static_cast<uint32_t>(c - '0');
        if (v > (std::numeric_limits<uint32_t>::max() - digit) / 10U) {
            return false;
        }
        v = v * 10U + digit;
    }

    *out = v;
    return true;
}

char parity_char(ParityMode p) {
    if (p == ParityMode::Even) {
        return 'E';
    }
    if (p == ParityMode::Odd) {
        return 'O';
    }
    return 'N';
}

bool time_reached(uint32_t now_us, uint32_t deadline_us) {
    return static_cast<int32_t>(now_us - deadline_us) >= 0;
}

size_t bounded_strlen(const char* s, size_t max_len_plus_one) {
    size_t n = 0;
    while (n < max_len_plus_one && s[n] != '\0') {
        n++;
    }
    return n;
}
}  // namespace

uint16_t compute_max_pps(uint32_t baud, uint8_t data_bits, ParityMode parity, uint8_t stop_bits, uint8_t payload_len) {
    const uint8_t parity_bits = (parity == ParityMode::None) ? 0 : 1;
    const uint16_t bits_per_char = static_cast<uint16_t>(1 + data_bits + parity_bits + stop_bits);
    const uint32_t denom = static_cast<uint32_t>(bits_per_char) * payload_len;
    if (denom == 0) {
        return 0;
    }
    return static_cast<uint16_t>(baud / denom);
}

QuadUartController::QuadUartController(UartTxWriter* writer) : writer_(writer) {
    for (uint8_t i = 0; i < kPortCount; i++) {
        PortConfig cfg{};
        cfg.baud = 115200;
        cfg.data_bits = 8;
        cfg.parity = ParityMode::None;
        cfg.stop_bits = 1;
        cfg.payload_len = 16;
        cfg.pps = 10;
        configs_[i] = cfg;

        PortRuntime rt{};
        rt.enabled = false;
        rt.seq = 0;
        rt.next_deadline_us = 0;
        rt.tx_in_progress = false;
        rt.tx_write_index = 0;
        rt.tx_packet_len = 0;
        for (uint8_t b = 0; b < kPayloadMaxLen; b++) {
            rt.tx_packet_buf[b] = 0;
        }
        runtimes_[i] = rt;
    }

    resp_buf_[0] = '\0';
}

// cppcheck-suppress unusedFunction
const PortConfig& QuadUartController::config(uint8_t port_id) const {
    return configs_[port_id];
}

// cppcheck-suppress unusedFunction
const PortRuntime& QuadUartController::runtime(uint8_t port_id) const {
    return runtimes_[port_id];
}

bool QuadUartController::any_enabled() const {
    for (uint8_t i = 0; i < kPortCount; i++) {
        if (runtimes_[i].enabled) {
            return true;
        }
    }
    return false;
}

bool QuadUartController::valid_port(uint8_t port_id) const {
    return port_id < kPortCount;
}

bool QuadUartController::validate_config(const PortConfig& cfg) const {
    if (cfg.baud == 0 || cfg.baud > 921600) {
        return false;
    }
    if (cfg.data_bits < 5 || cfg.data_bits > 8) {
        return false;
    }
    if (cfg.stop_bits < 1 || cfg.stop_bits > 2) {
        return false;
    }
    if (cfg.payload_len < kPayloadMinLen || cfg.payload_len > kPayloadMaxLen) {
        return false;
    }
    if (cfg.pps < kMinPps || cfg.pps > kMaxPps) {
        return false;
    }

    const uint16_t max_pps =
        compute_max_pps(cfg.baud, cfg.data_bits, cfg.parity, cfg.stop_bits, cfg.payload_len);
    if (max_pps == 0) {
        return false;
    }
    if (cfg.pps > max_pps) {
        return false;
    }
    return true;
}

const char* QuadUartController::set_error(const char* msg) {
    if (msg == nullptr) {
        resp_buf_[0] = '\0';
        return resp_buf_;
    }
    const int n = snprintf(resp_buf_, sizeof(resp_buf_), "%s", msg);
    if (n < 0) {
        resp_buf_[0] = '\0';
    }
    return resp_buf_;
}

const char* QuadUartController::set_port_show(uint8_t port_id) {
    const PortConfig& cfg = configs_[port_id];
    const PortRuntime& rt = runtimes_[port_id];
    const int n = snprintf(
        resp_buf_,
        sizeof(resp_buf_),
        "PORT=%u BAUD=%lu FORMAT=%u%c%u LEN=%u PPS=%u EN=%u SEQ=%lu",
        static_cast<unsigned>(port_id),
        static_cast<unsigned long>(cfg.baud),
        static_cast<unsigned>(cfg.data_bits),
        parity_char(cfg.parity),
        static_cast<unsigned>(cfg.stop_bits),
        static_cast<unsigned>(cfg.payload_len),
        static_cast<unsigned>(cfg.pps),
        rt.enabled ? 1U : 0U,
        static_cast<unsigned long>(rt.seq));
    if (n < 0) {
        return set_error("ERR INTERNAL fmt");
    }
    return resp_buf_;
}

const char* QuadUartController::set_status_all() {
    resp_buf_[0] = '\0';
    size_t used = 0;
    for (uint8_t i = 0; i < kPortCount; i++) {
        const PortConfig& cfg = configs_[i];
        const PortRuntime& rt = runtimes_[i];
        const char* sep = (i > 0) ? "\n\n" : "";
        const int n = snprintf(
            resp_buf_ + used,
            sizeof(resp_buf_) - used,
            "%sPORT=%u BAUD=%lu FORMAT=%u%c%u LEN=%u PPS=%u EN=%u SEQ=%lu",
            sep,
            static_cast<unsigned>(i),
            static_cast<unsigned long>(cfg.baud),
            static_cast<unsigned>(cfg.data_bits),
            parity_char(cfg.parity),
            static_cast<unsigned>(cfg.stop_bits),
            static_cast<unsigned>(cfg.payload_len),
            static_cast<unsigned>(cfg.pps),
            rt.enabled ? 1U : 0U,
            static_cast<unsigned long>(rt.seq));
        if (n < 0) {
            return set_error("ERR INTERNAL fmt");
        }
        if (static_cast<size_t>(n) >= (sizeof(resp_buf_) - used)) {
            used = sizeof(resp_buf_) - 1;
            resp_buf_[used] = '\0';
            break;
        }
        used += static_cast<size_t>(n);
    }
    return resp_buf_;
}

size_t QuadUartController::tokenize_upper(char* line, char* tokens[kMaxTokens]) const {
    for (size_t i = 0; line[i] != '\0'; i++) {
        line[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(line[i])));
    }

    size_t count = 0;
    char* p = line;
    while (*p != '\0') {
        while (*p != '\0' && std::isspace(static_cast<unsigned char>(*p)) != 0) {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        if (count >= kMaxTokens) {
            return kMaxTokens + 1;
        }
        tokens[count++] = p;
        while (*p != '\0' && std::isspace(static_cast<unsigned char>(*p)) == 0) {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        *p = '\0';
        p++;
    }
    return count;
}

const char* QuadUartController::enable_port(uint8_t port_id, uint32_t now_us) {
    if (writer_ == nullptr) {
        return "ERR UNSUPPORTED no-writer";
    }
    if (writer_->begin_port(port_id, configs_[port_id]) == false) {
        return "ERR UNSUPPORTED uart-begin";
    }
    PortRuntime& rt = runtimes_[port_id];
    rt.enabled = true;
    rt.tx_in_progress = false;
    rt.tx_write_index = 0;
    rt.tx_packet_len = 0;
    rt.next_deadline_us = now_us;
    return "OK";
}

const char* QuadUartController::disable_port(uint8_t port_id) {
    PortRuntime& rt = runtimes_[port_id];
    rt.enabled = false;
    rt.tx_in_progress = false;
    rt.tx_write_index = 0;
    rt.tx_packet_len = 0;
    return "OK";
}

const char* QuadUartController::port_show(uint8_t port_id) {
    return set_port_show(port_id);
}

// cppcheck-suppress unusedFunction
const char* QuadUartController::handle_command(const char* line, uint32_t now_us) {
    if (line == nullptr) {
        return set_error("ERR BAD_CMD empty");
    }

    const size_t n = bounded_strlen(line, kCmdLineMax + 1);
    if (n == 0) {
        return set_error("ERR BAD_CMD empty");
    }
    if (n > kCmdLineMax) {
        return set_error("ERR BAD_CMD line-too-long");
    }

    char line_buf[kCmdLineMax + 1];
    memcpy(line_buf, line, n);
    line_buf[n] = '\0';

    size_t start = 0;
    while (line_buf[start] != '\0' && std::isspace(static_cast<unsigned char>(line_buf[start])) != 0) {
        start++;
    }
    if (line_buf[start] == '\0') {
        return set_error("ERR BAD_CMD empty");
    }
    size_t end = n;
    while (end > start && std::isspace(static_cast<unsigned char>(line_buf[end - 1])) != 0) {
        end--;
    }
    line_buf[end] = '\0';

    char* tokens[kMaxTokens] = {nullptr};
    const size_t token_count = tokenize_upper(line_buf + start, tokens);
    if (token_count == 0) {
        return set_error("ERR BAD_CMD empty");
    }
    if (token_count > kMaxTokens) {
        return set_error("ERR BAD_CMD too-many-tokens");
    }

    if (strcmp(tokens[0], "HELP") == 0) {
        return kHelpText;
    }

    if (strcmp(tokens[0], "STATUS") == 0) {
        return set_status_all();
    }

    if (strcmp(tokens[0], "ALL") == 0) {
        if (token_count != 2) {
            return set_error("ERR BAD_CMD all-args");
        }
        if (strcmp(tokens[1], "ENABLE") == 0) {
            for (uint8_t i = 0; i < kPortCount; i++) {
                const char* r = enable_port(i, now_us);
                if (strcmp(r, "OK") != 0) {
                    return set_error(r);
                }
            }
            return set_error("OK");
        }
        if (strcmp(tokens[1], "DISABLE") == 0) {
            for (uint8_t i = 0; i < kPortCount; i++) {
                disable_port(i);
            }
            return set_error("OK");
        }
        return set_error("ERR BAD_CMD all-op");
    }

    if (strcmp(tokens[0], "PORT") != 0 || token_count < 3) {
        return set_error("ERR BAD_CMD syntax");
    }

    uint32_t port_u32 = 0;
    if (parse_u32(tokens[1], &port_u32) == false || port_u32 > 255U) {
        return set_error("ERR BAD_PORT value");
    }
    const uint8_t port_id = static_cast<uint8_t>(port_u32);
    if (valid_port(port_id) == false) {
        return set_error("ERR BAD_PORT range");
    }

    if (strcmp(tokens[2], "SHOW") == 0) {
        if (token_count != 3) {
            return set_error("ERR BAD_CMD show-args");
        }
        return port_show(port_id);
    }

    if (strcmp(tokens[2], "ENABLE") == 0) {
        if (token_count != 3) {
            return set_error("ERR BAD_CMD enable-args");
        }
        return set_error(enable_port(port_id, now_us));
    }

    if (strcmp(tokens[2], "DISABLE") == 0) {
        if (token_count != 3) {
            return set_error("ERR BAD_CMD disable-args");
        }
        return set_error(disable_port(port_id));
    }

    if (strcmp(tokens[2], "CFG") != 0 || token_count != 5) {
        return set_error("ERR BAD_CMD cfg-syntax");
    }

    if (any_enabled()) {
        return set_error("ERR BUSY outputs-enabled");
    }

    PortConfig trial = configs_[port_id];

    if (strcmp(tokens[3], "BAUD") == 0) {
        uint32_t v = 0;
        if (parse_u32(tokens[4], &v) == false) {
            return set_error("ERR BAD_VALUE baud");
        }
        trial.baud = v;
    } else if (strcmp(tokens[3], "FORMAT") == 0) {
        ParsedFormat pf{};
        if (parse_format_token(tokens[4], &pf) == false) {
            return set_error("ERR BAD_VALUE format");
        }
        trial.data_bits = pf.data_bits;
        trial.stop_bits = pf.stop_bits;
        if (pf.parity == 'E') {
            trial.parity = ParityMode::Even;
        } else if (pf.parity == 'O') {
            trial.parity = ParityMode::Odd;
        } else {
            trial.parity = ParityMode::None;
        }
    } else if (strcmp(tokens[3], "LEN") == 0) {
        uint32_t v = 0;
        if (parse_u32(tokens[4], &v) == false || v > 255U) {
            return set_error("ERR BAD_VALUE len");
        }
        trial.payload_len = static_cast<uint8_t>(v);
    } else if (strcmp(tokens[3], "PPS") == 0) {
        uint32_t v = 0;
        if (parse_u32(tokens[4], &v) == false || v > 65535U) {
            return set_error("ERR BAD_VALUE pps");
        }
        trial.pps = static_cast<uint16_t>(v);
    } else {
        return set_error("ERR BAD_CMD cfg-key");
    }

    if (validate_config(trial) == false) {
        return set_error("ERR BAD_VALUE cfg");
    }

    configs_[port_id] = trial;
    return set_error("OK");
}

void QuadUartController::drain_tx(uint8_t port_id) {
    if (writer_ == nullptr) {
        return;
    }
    PortRuntime& rt = runtimes_[port_id];
    while (rt.tx_write_index < rt.tx_packet_len && writer_->available_for_write(port_id) > 0) {
        const uint8_t b = rt.tx_packet_buf[rt.tx_write_index];
        const size_t wrote = writer_->write_byte(port_id, b);
        if (wrote == 0) {
            break;
        }
        rt.tx_write_index++;
    }
    if (rt.tx_write_index == rt.tx_packet_len) {
        rt.tx_in_progress = false;
        rt.seq++;
    }
}

// cppcheck-suppress unusedFunction
void QuadUartController::service_tx_nonblocking(uint32_t now_us) {
    for (uint8_t port_id = 0; port_id < kPortCount; port_id++) {
        PortRuntime& rt = runtimes_[port_id];
        const PortConfig& cfg = configs_[port_id];

        if (rt.enabled == false) {
            continue;
        }

        if (rt.tx_in_progress) {
            drain_tx(port_id);
            continue;
        }

        if (!time_reached(now_us, rt.next_deadline_us)) {
            continue;
        }

        const size_t n = build_packet(
            port_id, rt.seq, cfg.payload_len, rt.tx_packet_buf, sizeof(rt.tx_packet_buf));
        if (n == 0) {
            continue;
        }

        rt.tx_packet_len = static_cast<uint8_t>(n);
        rt.tx_write_index = 0;
        rt.tx_in_progress = true;

        uint32_t period_us = 1000000U / cfg.pps;
        if (period_us == 0) {
            period_us = 1;
        }
        rt.next_deadline_us = now_us + period_us;

        drain_tx(port_id);
    }
}
