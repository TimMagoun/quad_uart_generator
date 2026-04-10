#pragma once

#include <stddef.h>
#include <stdint.h>

#include <array>

enum class ParityMode : uint8_t { None, Even, Odd };

constexpr uint8_t kPortCount = 4;
constexpr uint8_t kPayloadMinLen = 10;
constexpr uint8_t kPayloadMaxLen = 128;

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
    uint8_t tx_packet_buf[kPayloadMaxLen];
};

uint16_t compute_max_pps(uint32_t baud, uint8_t data_bits, ParityMode parity, uint8_t stop_bits, uint8_t payload_len);

class UartTxWriter {
public:
    virtual ~UartTxWriter() = default;
    virtual bool begin_port(uint8_t port_id, const PortConfig& config) = 0;
    virtual int available_for_write(uint8_t port_id) = 0;
    virtual size_t write_byte(uint8_t port_id, uint8_t byte) = 0;
};

class QuadUartController {
public:
    explicit QuadUartController(UartTxWriter* writer);

    const char* handle_command(const char* line, uint32_t now_us = 0);
    void service_tx_nonblocking(uint32_t now_us);

    const PortConfig& config(uint8_t port_id) const;
    const PortRuntime& runtime(uint8_t port_id) const;

private:
    static constexpr size_t kCmdLineMax = 255;
    static constexpr size_t kMaxTokens = 8;
    static constexpr size_t kPortShowMax = 80;
    static constexpr size_t kPortShowWorstCaseLen =
        sizeof("PORT=255 BAUD=4294967295 FORMAT=8N2 LEN=255 PPS=65535 EN=1 SEQ=4294967295") - 1;
    static_assert(kPortShowWorstCaseLen <= kPortShowMax, "kPortShowMax too small");
    static constexpr size_t kRespMax = (kPortCount * kPortShowMax) + ((kPortCount - 1) * 3) + 1;

    bool any_enabled() const;
    bool valid_port(uint8_t port_id) const;
    bool validate_config(const PortConfig& cfg) const;
    const char* enable_port(uint8_t port_id, uint32_t now_us);
    const char* disable_port(uint8_t port_id);
    const char* port_show(uint8_t port_id);
    const char* set_ports_show(const uint8_t* port_ids, size_t port_count);
    const char* set_error(const char* msg);
    const char* set_port_show(uint8_t port_id);
    const char* set_status_all();
    size_t tokenize_upper(char* line, char* tokens[kMaxTokens]) const;
    void drain_tx(uint8_t port_id);

    std::array<PortConfig, kPortCount> configs_{};
    std::array<PortRuntime, kPortCount> runtimes_{};
    char resp_buf_[kRespMax]{};
    UartTxWriter* writer_;
};
