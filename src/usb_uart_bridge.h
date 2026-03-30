#pragma once

#include <Arduino.h>
#include <SerialPIO.h>
#include <Adafruit_TinyUSB.h>

namespace uub {

uint16_t lineCodingToUartConfig(uint8_t dataBits, uint8_t parity, uint8_t stopBits);

class UsbUartBridge {
 public:
  void init(const char *description,
            const char *usbLabel,
            Adafruit_USBD_CDC *usbInterface,
            SerialUART *uart);
  void init(const char *description,
            const char *usbLabel,
            Adafruit_USBD_CDC *usbInterface,
            SerialPIO *uart);

  void queueLineCoding(uint32_t baud, uint16_t config);
  bool takeReadyLineCoding(uint32_t nowMs, uint32_t *newBaud, uint16_t *newConfig);

  void applyConfig(uint32_t requestedBaud, uint16_t requestedConfig);
  void end();
  void transmit();

  bool isPioBridge() const;
  uint32_t baud() const;
  uint16_t uartConfig() const;
  const char *description() const;

 private:
  const char *description_ = "";
  uint32_t baud_ = 0;
  uint16_t uartConfig_ = SERIAL_8N1;

  SerialUART *hardwareUart_ = nullptr;
  SerialPIO *pioUart_ = nullptr;
  Adafruit_USBD_CDC *usb_ = nullptr;

  volatile bool reconfigPending_ = false;
  volatile uint32_t pendingSinceMs_ = 0;
  volatile uint32_t pendingBaud_ = 0;
  volatile uint16_t pendingConfig_ = SERIAL_8N1;

  void initCommon(const char *description,
                  const char *usbLabel,
                  Adafruit_USBD_CDC *usbInterface);

  int uartAvailable() const;
  int uartAvailableForWrite() const;
  int uartReadByte();
  size_t uartWriteByte(uint8_t byte);
  void uartEndInternal();
  bool uartBeginWithConfig(uint32_t baud, uint16_t config);
};

}  // namespace uub
