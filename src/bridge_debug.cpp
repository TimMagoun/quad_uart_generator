#include "bridge_debug.h"

#include <Arduino.h>

#include "uub_config.h"
#include "usb_uart_bridge.h"

namespace uub::debug {

#if UUB_ENABLE_DEBUG_CDC

void begin() {
  Serial.begin(115200);
}

void logConfigResult(const char *description,
                     bool applied,
                     uint32_t requestedBaud,
                     uint16_t requestedConfig,
                     uint32_t activeBaud,
                     uint16_t activeConfig) {
  Serial.print("[uub] cfg ");
  Serial.print(description);
  Serial.print(" ");
  Serial.print(applied ? "ok" : "restore");
  Serial.print(" baud=");
  Serial.print(applied ? requestedBaud : activeBaud);
  Serial.print(" cfg=0x");
  Serial.println(applied ? requestedConfig : activeConfig, HEX);
}

void logLineCodingSkip(uint8_t itf) {
  Serial.print("[uub] linecoding skip itf=");
  Serial.println(itf);
}

void logLineCodingApply(uint8_t itf,
                        uint8_t bridgeIndex,
                        const char *description,
                        cdc_line_coding_t const *lineCoding) {
  Serial.print("[uub] linecoding itf=");
  Serial.print(itf);
  Serial.print(" idx=");
  Serial.print(bridgeIndex);
  Serial.print(" ");
  Serial.print(description);
  Serial.print(" baud=");
  Serial.print(lineCoding->bit_rate);
  Serial.print(" cfg=0x");
  Serial.println(lineCodingToUartConfig(lineCoding->data_bits, lineCoding->parity, lineCoding->stop_bits), HEX);
}

void logPioRestartStop() {
  Serial.println("[uub] pio restart stop");
}

void logPioRestartStart() {
  Serial.println("[uub] pio restart start");
}

#else

void begin() {}

void logConfigResult(const char *,
                     bool,
                     uint32_t,
                     uint16_t,
                     uint32_t,
                     uint16_t) {}

void logLineCodingSkip(uint8_t) {}

void logLineCodingApply(uint8_t, uint8_t, const char *, cdc_line_coding_t const *) {}

void logPioRestartStop() {}

void logPioRestartStart() {}

#endif

}  // namespace uub::debug
