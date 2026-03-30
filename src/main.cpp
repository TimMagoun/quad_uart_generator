#include <Arduino.h>
#include <SerialPIO.h>
#include <Adafruit_TinyUSB.h>
#include <tusb.h>

#include "uub_config.h"
#include "usb_uart_bridge.h"
#include "bridge_debug.h"

namespace {

#define UUB_EXPECTED_CDC_PORTS (UUB_BRIDGE_COUNT + (UUB_ENABLE_DEBUG_CDC ? 1 : 0))
#if CFG_TUD_CDC < UUB_EXPECTED_CDC_PORTS
#error Must increase CFG_TUD_CDC for enabled bridge+debug CDC ports.
#endif

struct BridgeInitSpec {
  const char *description;
  const char *usbLabel;
  Adafruit_USBD_CDC *usb;
  SerialUART *hardwareUart;
  SerialPIO *pioUart;
};

// UART bridge map (tx, rx)
// serial1, hardware Serial1: GP0/1
SerialPIO serial2Pio(2, 3, uub::kReadBufferSize);    // GP2/3
SerialPIO serial3Pio(5, 6, uub::kReadBufferSize);    // GP5/6
// serial4, hardware Serial2: GP8/9
SerialPIO serial5Pio(21, 23, uub::kReadBufferSize);  // GP21/23
SerialPIO serial6Pio(26, 27, uub::kReadBufferSize);  // GP26/27

Adafruit_USBD_CDC usbInterfaces[uub::kBridgeCount];
uub::UsbUartBridge bridges[uub::kBridgeCount];

void initBridges() {
  const BridgeInitSpec specs[uub::kBridgeCount] = {
    {"B0 (GP0/1)", "UART0", &usbInterfaces[0], &Serial1, nullptr},
    {"B1 (GP2/3)", "UART1", &usbInterfaces[1], nullptr, &serial2Pio},
    {"B2 (GP5/6)", "UART2", &usbInterfaces[2], nullptr, &serial3Pio},
    {"B3 (GP8/9)", "UART3", &usbInterfaces[3], &Serial2, nullptr},
    {"B4 (GP21/23)", "UART4", &usbInterfaces[4], nullptr, &serial5Pio},
    {"B5 (GP26/27)", "UART5", &usbInterfaces[5], nullptr, &serial6Pio},
  };

  for(size_t i = 0; i < uub::kBridgeCount; i++) {
    if(specs[i].hardwareUart != nullptr) {
      bridges[i].init(specs[i].description, specs[i].usbLabel, specs[i].usb, specs[i].hardwareUart);
    } else {
      bridges[i].init(specs[i].description, specs[i].usbLabel, specs[i].usb, specs[i].pioUart);
    }
  }
}

}  // namespace

extern "C" void tud_cdc_line_coding_cb(uint8_t itf,
                                       cdc_line_coding_t const *lineCoding) {
  const uint8_t bridgeItfBase = UUB_ENABLE_DEBUG_CDC ? 1 : 0;
  if(lineCoding == nullptr || itf < bridgeItfBase || itf >= (bridgeItfBase + uub::kBridgeCount)) {
    uub::debug::logLineCodingSkip(itf);
    return;
  }

  const uint8_t bridgeIndex = static_cast<uint8_t>(itf - bridgeItfBase);
  const uint16_t config =
    uub::lineCodingToUartConfig(lineCoding->data_bits, lineCoding->parity, lineCoding->stop_bits);

  bridges[bridgeIndex].queueLineCoding(lineCoding->bit_rate, config);
  uub::debug::logLineCodingApply(itf, bridgeIndex, bridges[bridgeIndex].description(), lineCoding);
}

void setup() {
  uub::debug::begin();

  Serial1.setTX(0);
  Serial1.setRX(1);
  Serial2.setTX(8);
  Serial2.setRX(9);

  initBridges();
}

void loop() {
  const uint32_t nowMs = millis();
  uint32_t pioTargetBaud[uub::kBridgeCount];
  uint16_t pioTargetConfig[uub::kBridgeCount];
  bool pioRestartNeeded = false;

  for(size_t i = 0; i < uub::kBridgeCount; i++) {
    pioTargetBaud[i] = bridges[i].baud();
    pioTargetConfig[i] = bridges[i].uartConfig();
  }

  for(size_t i = 0; i < uub::kBridgeCount; i++) {
    uint32_t newBaud = 0;
    uint16_t newConfig = SERIAL_8N1;

    if(bridges[i].takeReadyLineCoding(nowMs, &newBaud, &newConfig) == false) {
      continue;
    }

    if(bridges[i].isPioBridge()) {
      pioTargetBaud[i] = newBaud;
      pioTargetConfig[i] = newConfig;
      if(newBaud != bridges[i].baud() || newConfig != bridges[i].uartConfig()) {
        pioRestartNeeded = true;
      }
      continue;
    }

    if(newBaud != bridges[i].baud() || newConfig != bridges[i].uartConfig()) {
      bridges[i].applyConfig(newBaud, newConfig);
    }
  }

  if(pioRestartNeeded) {
    uub::debug::logPioRestartStop();
    for(size_t i = 0; i < uub::kBridgeCount; i++) {
      if(bridges[i].isPioBridge()) {
        bridges[i].end();
      }
    }

    uub::debug::logPioRestartStart();
    for(size_t i = 0; i < uub::kBridgeCount; i++) {
      if(bridges[i].isPioBridge()) {
        bridges[i].applyConfig(pioTargetBaud[i], pioTargetConfig[i]);
      }
    }
  }

  for(size_t i = 0; i < uub::kBridgeCount; i++) {
    bridges[i].transmit();
  }
}
