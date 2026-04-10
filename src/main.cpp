#include <Arduino.h>
#include <SoftwareSerial.h>

#include <string>

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

    bool begin_port(uint8_t port_id, const PortConfig& config) override {
        SoftwareSerial* uart = serial_for(port_id);
        if (uart == nullptr) {
            return false;
        }
        uart->begin(config.baud, serial_cfg_from_port(config));
        return true;
    }

    int available_for_write(uint8_t port_id) override {
        SoftwareSerial* uart = serial_for(port_id);
        if (uart == nullptr) {
            return 0;
        }
        return uart->availableForWrite();
    }

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
String g_line;

void handle_control_serial() {
    while (Serial.available() > 0) {
        const char c = static_cast<char>(Serial.read());
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            if (g_line.length() > 0) {
                const std::string cmd(g_line.c_str());
                const std::string resp = g_controller.handle_command(cmd, micros());
                Serial.println(resp.c_str());
                g_line = "";
            }
            continue;
        }
        if (g_line.length() < 255) {
            g_line += c;
        }
    }
}

}  // namespace

void setup() {
    Serial.begin(115200);
}

void loop() {
    handle_control_serial();
    g_controller.service_tx_nonblocking(micros());
}
