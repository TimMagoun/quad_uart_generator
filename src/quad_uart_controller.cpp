#include "quad_uart_controller.h"

#include "command_parser.h"
#include "packet_builder.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <vector>

namespace {
constexpr uint16_t kMinPps = 1;
constexpr uint16_t kMaxPps = 1000;

std::string trim_copy(const std::string& in) {
    size_t start = 0;
    while (start < in.size() && std::isspace(static_cast<unsigned char>(in[start])) != 0) {
        start++;
    }
    size_t end = in.size();
    while (end > start && std::isspace(static_cast<unsigned char>(in[end - 1])) != 0) {
        end--;
    }
    return in.substr(start, end - start);
}

std::vector<std::string> split_ws_upper(const std::string& in) {
    std::istringstream iss(in);
    std::vector<std::string> tokens;
    std::string tok;
    while (iss >> tok) {
        std::transform(tok.begin(), tok.end(), tok.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
        tokens.push_back(tok);
    }
    return tokens;
}

bool parse_u32(const std::string& s, uint32_t* out) {
    if (out == nullptr || s.empty()) {
        return false;
    }
    uint32_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
        v = v * 10 + static_cast<uint32_t>(c - '0');
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
}

const PortConfig& QuadUartController::config(uint8_t port_id) const {
    return configs_[port_id];
}

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

std::string QuadUartController::port_show(uint8_t port_id) const {
    const PortConfig& cfg = configs_[port_id];
    const PortRuntime& rt = runtimes_[port_id];
    std::ostringstream oss;
    oss << "PORT=" << static_cast<int>(port_id) << " BAUD=" << cfg.baud << " FORMAT="
        << static_cast<int>(cfg.data_bits) << parity_char(cfg.parity) << static_cast<int>(cfg.stop_bits)
        << " LEN=" << static_cast<int>(cfg.payload_len) << " PPS=" << cfg.pps
        << " EN=" << static_cast<int>(rt.enabled ? 1 : 0) << " SEQ=" << rt.seq;
    return oss.str();
}

std::string QuadUartController::enable_port(uint8_t port_id, uint32_t now_us) {
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

std::string QuadUartController::disable_port(uint8_t port_id) {
    PortRuntime& rt = runtimes_[port_id];
    rt.enabled = false;
    rt.tx_in_progress = false;
    rt.tx_write_index = 0;
    rt.tx_packet_len = 0;
    return "OK";
}

std::string QuadUartController::handle_command(const std::string& line, uint32_t now_us) {
    const std::string t = trim_copy(line);
    if (t.empty()) {
        return "ERR BAD_CMD empty";
    }

    const std::vector<std::string> tokens = split_ws_upper(t);
    if (tokens.empty()) {
        return "ERR BAD_CMD empty";
    }

    if (tokens[0] == "HELP") {
        return "OK HELP STATUS PORT <id> SHOW PORT <id> CFG BAUD|FORMAT|LEN|PPS <v> PORT <id> ENABLE|DISABLE ALL ENABLE|DISABLE";
    }

    if (tokens[0] == "STATUS") {
        std::ostringstream oss;
        for (uint8_t i = 0; i < kPortCount; i++) {
            if (i > 0) {
                oss << " | ";
            }
            oss << port_show(i);
        }
        return oss.str();
    }

    if (tokens[0] == "ALL") {
        if (tokens.size() != 2) {
            return "ERR BAD_CMD all-args";
        }
        if (tokens[1] == "ENABLE") {
            for (uint8_t i = 0; i < kPortCount; i++) {
                const std::string r = enable_port(i, now_us);
                if (r != "OK") {
                    return r;
                }
            }
            return "OK";
        }
        if (tokens[1] == "DISABLE") {
            for (uint8_t i = 0; i < kPortCount; i++) {
                disable_port(i);
            }
            return "OK";
        }
        return "ERR BAD_CMD all-op";
    }

    if (tokens[0] != "PORT" || tokens.size() < 3) {
        return "ERR BAD_CMD syntax";
    }

    uint32_t port_u32 = 0;
    if (parse_u32(tokens[1], &port_u32) == false || port_u32 > 255U) {
        return "ERR BAD_PORT value";
    }
    const uint8_t port_id = static_cast<uint8_t>(port_u32);
    if (valid_port(port_id) == false) {
        return "ERR BAD_PORT range";
    }

    if (tokens[2] == "SHOW") {
        if (tokens.size() != 3) {
            return "ERR BAD_CMD show-args";
        }
        return port_show(port_id);
    }

    if (tokens[2] == "ENABLE") {
        if (tokens.size() != 3) {
            return "ERR BAD_CMD enable-args";
        }
        return enable_port(port_id, now_us);
    }

    if (tokens[2] == "DISABLE") {
        if (tokens.size() != 3) {
            return "ERR BAD_CMD disable-args";
        }
        return disable_port(port_id);
    }

    if (tokens[2] != "CFG" || tokens.size() != 5) {
        return "ERR BAD_CMD cfg-syntax";
    }

    if (any_enabled()) {
        return "ERR BUSY outputs-enabled";
    }

    PortConfig trial = configs_[port_id];

    if (tokens[3] == "BAUD") {
        uint32_t v = 0;
        if (parse_u32(tokens[4], &v) == false) {
            return "ERR BAD_VALUE baud";
        }
        trial.baud = v;
    } else if (tokens[3] == "FORMAT") {
        ParsedFormat pf{};
        if (parse_format_token(tokens[4].c_str(), &pf) == false) {
            return "ERR BAD_VALUE format";
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
    } else if (tokens[3] == "LEN") {
        uint32_t v = 0;
        if (parse_u32(tokens[4], &v) == false || v > 255U) {
            return "ERR BAD_VALUE len";
        }
        trial.payload_len = static_cast<uint8_t>(v);
    } else if (tokens[3] == "PPS") {
        uint32_t v = 0;
        if (parse_u32(tokens[4], &v) == false || v > 65535U) {
            return "ERR BAD_VALUE pps";
        }
        trial.pps = static_cast<uint16_t>(v);
    } else {
        return "ERR BAD_CMD cfg-key";
    }

    if (validate_config(trial) == false) {
        return "ERR BAD_VALUE cfg";
    }

    configs_[port_id] = trial;
    return "OK";
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

        if (now_us < rt.next_deadline_us) {
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
