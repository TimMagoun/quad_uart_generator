#include "usb_uart_bridge.h"

#include "uub_config.h"
#include "bridge_debug.h"

namespace uub {

uint16_t lineCodingToUartConfig(uint8_t dataBits, uint8_t parity, uint8_t stopBits) {
  uint16_t dataConfig = SERIAL_DATA_8;
  switch(dataBits) {
    case 5:
      dataConfig = SERIAL_DATA_5;
      break;
    case 6:
      dataConfig = SERIAL_DATA_6;
      break;
    case 7:
      dataConfig = SERIAL_DATA_7;
      break;
    case 8:
    default:
      dataConfig = SERIAL_DATA_8;
      break;
  }

  uint16_t parityConfig = SERIAL_PARITY_NONE;
  switch(parity) {
    case 1:
      parityConfig = SERIAL_PARITY_ODD;
      break;
    case 2:
      parityConfig = SERIAL_PARITY_EVEN;
      break;
    default:
      parityConfig = SERIAL_PARITY_NONE;
      break;
  }

  uint16_t stopConfig = SERIAL_STOP_BIT_1;
  switch(stopBits) {
    case 1:  // 1.5 stop bits unsupported, map to 2 stop bits.
    case 2:
      stopConfig = SERIAL_STOP_BIT_2;
      break;
    default:
      stopConfig = SERIAL_STOP_BIT_1;
      break;
  }

  return dataConfig | parityConfig | stopConfig;
}

void UsbUartBridge::initCommon(const char *description,
                               const char *usbLabel,
                               Adafruit_USBD_CDC *usbInterface) {
  description_ = description;
  baud_ = kDefaultUartBaud;
  uartConfig_ = SERIAL_8N1;
  hardwareUart_ = nullptr;
  pioUart_ = nullptr;
  usb_ = usbInterface;

  reconfigPending_ = false;
  pendingSinceMs_ = 0;
  pendingBaud_ = kDefaultUartBaud;
  pendingConfig_ = SERIAL_8N1;

  usb_->setStringDescriptor(usbLabel);
  usb_->begin(kDefaultUartBaud);
}

void UsbUartBridge::init(const char *description,
                         const char *usbLabel,
                         Adafruit_USBD_CDC *usbInterface,
                         SerialUART *uart) {
  initCommon(description, usbLabel, usbInterface);
  hardwareUart_ = uart;
  applyConfig(kDefaultUartBaud, SERIAL_8N1);
}

void UsbUartBridge::init(const char *description,
                         const char *usbLabel,
                         Adafruit_USBD_CDC *usbInterface,
                         SerialPIO *uart) {
  initCommon(description, usbLabel, usbInterface);
  pioUart_ = uart;
  applyConfig(kDefaultUartBaud, SERIAL_8N1);
}

int UsbUartBridge::uartAvailable() const {
  if(hardwareUart_ != nullptr) {
    return hardwareUart_->available();
  }
  return (pioUart_ != nullptr) ? pioUart_->available() : 0;
}

int UsbUartBridge::uartAvailableForWrite() const {
  if(hardwareUart_ != nullptr) {
    return hardwareUart_->availableForWrite();
  }
  return (pioUart_ != nullptr) ? pioUart_->availableForWrite() : 0;
}

int UsbUartBridge::uartReadByte() {
  if(hardwareUart_ != nullptr) {
    return hardwareUart_->read();
  }
  return (pioUart_ != nullptr) ? pioUart_->read() : -1;
}

size_t UsbUartBridge::uartWriteByte(uint8_t byte) {
  if(hardwareUart_ != nullptr) {
    return hardwareUart_->write(byte);
  }
  return (pioUart_ != nullptr) ? pioUart_->write(byte) : 0;
}

void UsbUartBridge::uartEndInternal() {
  if(hardwareUart_ != nullptr) {
    hardwareUart_->end();
  } else if(pioUart_ != nullptr) {
    pioUart_->end();
  }
}

bool UsbUartBridge::uartBeginWithConfig(uint32_t baud, uint16_t config) {
  if(hardwareUart_ != nullptr) {
    hardwareUart_->begin(baud, config);
    return true;
  }

  if(pioUart_ != nullptr) {
    pioUart_->begin(baud, config);
    return static_cast<bool>(*pioUart_);
  }

  return false;
}

void UsbUartBridge::applyConfig(uint32_t requestedBaud, uint16_t requestedConfig) {
  if(requestedBaud == 0) {
    requestedBaud = kDefaultUartBaud;
  }

  const uint32_t oldBaud = baud_;
  const uint16_t oldConfig = uartConfig_;

  uartEndInternal();
  bool applied = uartBeginWithConfig(requestedBaud, requestedConfig);

  if(!applied) {
    // Preserve known-good runtime config when reconfigure fails.
    uartEndInternal();
    applied = uartBeginWithConfig(oldBaud, oldConfig);
  }

  if(applied) {
    baud_ = requestedBaud;
    uartConfig_ = requestedConfig;
  }

  debug::logConfigResult(description_,
                         applied,
                         requestedBaud,
                         requestedConfig,
                         oldBaud,
                         oldConfig);
}

void UsbUartBridge::end() {
  uartEndInternal();
}

void UsbUartBridge::queueLineCoding(uint32_t baud, uint16_t config) {
  noInterrupts();
  pendingSinceMs_ = millis();
  pendingBaud_ = baud;
  pendingConfig_ = config;
  reconfigPending_ = true;
  interrupts();
}

bool UsbUartBridge::takeReadyLineCoding(uint32_t nowMs,
                                        uint32_t *newBaud,
                                        uint16_t *newConfig) {
  bool ready = false;
  noInterrupts();
  if(reconfigPending_) {
    const uint32_t elapsed = nowMs - pendingSinceMs_;
    if(elapsed >= kLineCodingSettleMs) {
      *newBaud = pendingBaud_;
      *newConfig = pendingConfig_;
      reconfigPending_ = false;
      ready = true;
    }
  }
  interrupts();
  return ready;
}

bool UsbUartBridge::isPioBridge() const {
  return pioUart_ != nullptr;
}

uint32_t UsbUartBridge::baud() const {
  return baud_;
}

uint16_t UsbUartBridge::uartConfig() const {
  return uartConfig_;
}

const char *UsbUartBridge::description() const {
  return description_;
}

void UsbUartBridge::transmit() {
  bool flushNeeded = false;

  auto drainUartToUsb = [&]() {
    uint8_t batch[kUartToUsbBatch];
    int batchLen = 0;

    int available = uartAvailable();
    while(available-- > 0 && batchLen < kUartToUsbBatch) {
      const int value = uartReadByte();
      if(value >= 0) {
        batch[batchLen++] = static_cast<uint8_t>(value);
      }
    }

    if(batchLen <= 0) {
      return;
    }

    const size_t wrote = usb_->write(batch, static_cast<size_t>(batchLen));
    if(wrote > 0) {
      flushNeeded = true;
    }
  };

  drainUartToUsb();

  if(!reconfigPending_) {
    int available = usb_->available();
    int budget = kUsbToUartBudget;
    while(available-- > 0 && budget-- > 0) {
      if(uartAvailableForWrite() <= 0) {
        break;
      }

      const int value = usb_->read();
      if(value >= 0) {
        uartWriteByte(static_cast<uint8_t>(value));
      }
    }
  }

  drainUartToUsb();

  if(flushNeeded) {
    usb_->flush();
  }
}

}  // namespace uub
