#pragma once

#include <stddef.h>
#include <stdint.h>

#include <array>
#include <string>

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
    uint8_t tx_packet_buf[128];
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

    std::string handle_command(const std::string& line, uint32_t now_us = 0);
    void service_tx_nonblocking(uint32_t now_us);

    const PortConfig& config(uint8_t port_id) const;
    const PortRuntime& runtime(uint8_t port_id) const;

private:
    bool any_enabled() const;
    bool valid_port(uint8_t port_id) const;
    bool validate_config(const PortConfig& cfg) const;
    std::string enable_port(uint8_t port_id, uint32_t now_us);
    std::string disable_port(uint8_t port_id);
    std::string port_show(uint8_t port_id) const;
    void drain_tx(uint8_t port_id);

    std::array<PortConfig, kPortCount> configs_{};
    std::array<PortRuntime, kPortCount> runtimes_{};
    UartTxWriter* writer_;
};
