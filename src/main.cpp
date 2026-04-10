#include <Arduino.h>
#include <SoftwareSerial.h>

#include "quad_uart_controller.h"

namespace {

uint16_t serial_data_bits_cfg(uint8_t bits) {
    if (bits == 5) {
        return SERIAL_DATA_5;
    }
    if (bits == 6) {
        return SERIAL_DATA_6;
    }
    if (bits == 7) {
        return SERIAL_DATA_7;
    }
    return SERIAL_DATA_8;
}

uint16_t serial_parity_cfg(ParityMode parity) {
    if (parity == ParityMode::Even) {
        return SERIAL_PARITY_EVEN;
    }
    if (parity == ParityMode::Odd) {
        return SERIAL_PARITY_ODD;
    }
    return SERIAL_PARITY_NONE;
}

uint16_t serial_stop_cfg(uint8_t stop_bits) {
    if (stop_bits == 1) {
        return SERIAL_STOP_BIT_1;
    }
    return SERIAL_STOP_BIT_2;
}

uint16_t serial_cfg_from_port(const PortConfig& cfg) {
    return static_cast<uint16_t>(
        serial_data_bits_cfg(cfg.data_bits) |
        serial_parity_cfg(cfg.parity) |
        serial_stop_cfg(cfg.stop_bits));
}

class PioUartWriter : public UartTxWriter {
public:
    PioUartWriter()
        : uart0_(NOPIN, 2),
          uart1_(NOPIN, 3),
          uart2_(NOPIN, 4),
          uart3_(NOPIN, 5) {
    }

    // cppcheck-suppress unusedFunction
    bool begin_port(uint8_t port_id, const PortConfig& config) override {
        SoftwareSerial* uart = serial_for(port_id);
        if (uart == nullptr) {
            return false;
        }
        uart->begin(config.baud, serial_cfg_from_port(config));
        return true;
    }

    // cppcheck-suppress unusedFunction
    int available_for_write(uint8_t port_id) override {
        SoftwareSerial* uart = serial_for(port_id);
        if (uart == nullptr) {
            return 0;
        }
        return uart->availableForWrite();
    }

    // cppcheck-suppress unusedFunction
    size_t write_byte(uint8_t port_id, uint8_t byte) override {
        SoftwareSerial* uart = serial_for(port_id);
        if (uart == nullptr) {
            return 0;
        }
        return uart->write(byte);
    }

private:
    SoftwareSerial* serial_for(uint8_t port_id) {
        if (port_id == 0) {
            return &uart0_;
        }
        if (port_id == 1) {
            return &uart1_;
        }
        if (port_id == 2) {
            return &uart2_;
        }
        if (port_id == 3) {
            return &uart3_;
        }
        return nullptr;
    }

    SoftwareSerial uart0_;
    SoftwareSerial uart1_;
    SoftwareSerial uart2_;
    SoftwareSerial uart3_;
};

PioUartWriter g_writer;
QuadUartController g_controller(&g_writer);
constexpr size_t kLineMax = 255;
char g_line[kLineMax + 1] = {0};
size_t g_line_len = 0;
bool g_line_overflow = false;

void handle_control_serial() {
    while (Serial.available() > 0) {
        const char c = static_cast<char>(Serial.read());
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            if (g_line_overflow) {
                Serial.println("ERR BAD_CMD line-too-long");
            } else if (g_line_len > 0) {
                g_line[g_line_len] = '\0';
                const char* resp = g_controller.handle_command(g_line, micros());
                Serial.println(resp);
            }
            g_line_len = 0;
            g_line_overflow = false;
            continue;
        }
        if (g_line_len < kLineMax) {
            g_line[g_line_len++] = c;
        } else {
            g_line_overflow = true;
        }
    }
}

}  // namespace

// cppcheck-suppress unusedFunction
void setup() {
    Serial.begin(115200);
}

// cppcheck-suppress unusedFunction
void loop() {
    handle_control_serial();
    g_controller.service_tx_nonblocking(micros());
}
