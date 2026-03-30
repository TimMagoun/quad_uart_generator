#include <Arduino.h>
#include <SerialPIO.h>
#include <Adafruit_TinyUSB.h>
#include <tusb.h>

#define UUB_MAX 6
#define READBUFSIZE 64
#define DEFAULT_UART_BAUD 115200

#if CFG_TUD_CDC < UUB_MAX
#error Must change CFG_TUD_CDC >= 6 and Adafruit_USBD_Device._desc_cfg_buffer >= 1024
#endif
/*
tusb_config_rt2040.h
#define CFG_TUD_CDC 6 <-
&
Adafruit_USBD_Device.h
class Adafruit_USBD_Device {
  ...
  uint8_t _desc_cfg_buffer[1024]; <- 256 is not enough
*/

static uint16_t lineCodingToUartConfig(uint8_t data_bits, uint8_t parity, uint8_t stop_bits) {
  uint16_t data_cfg = SERIAL_DATA_8;
  switch(data_bits) {
    case 5:
      data_cfg = SERIAL_DATA_5;
      break;
    case 6:
      data_cfg = SERIAL_DATA_6;
      break;
    case 7:
      data_cfg = SERIAL_DATA_7;
      break;
    case 8:
    default:
      data_cfg = SERIAL_DATA_8;
      break;
  }

  uint16_t parity_cfg = SERIAL_PARITY_NONE;
  switch(parity) {
    case 1: // odd
      parity_cfg = SERIAL_PARITY_ODD;
      break;
    case 2: // even
      parity_cfg = SERIAL_PARITY_EVEN;
      break;
    default:
      parity_cfg = SERIAL_PARITY_NONE;
      break;
  }

  uint16_t stop_cfg = SERIAL_STOP_BIT_1;
  switch(stop_bits) {
    case 1: // 1.5, not supported by RP2040 UART API, map to 2
    case 2: // 2
      stop_cfg = SERIAL_STOP_BIT_2;
      break;
    default:
      stop_cfg = SERIAL_STOP_BIT_1;
      break;
  }

  return data_cfg | parity_cfg | stop_cfg;
}

class UsbUartBridge {
  private:
    const char *descr;
    uint32_t baud;
    uint16_t uart_config;

    SerialUART *uarts;
    SerialPIO *uartp;
    Adafruit_USBD_CDC *usbi;

    volatile bool reconfig_pending;
    volatile uint32_t pending_baud;
    volatile uint16_t pending_config;

    void _init(const char *descr, const char *usb_label, Adafruit_USBD_CDC *usbi);
    void _applyConfig(uint32_t baud, uint16_t config);

  public:
    void init(const char *descr, const char *usb_label, Adafruit_USBD_CDC *usbi, SerialUART *uart);
    void init(const char *descr, const char *usb_label, Adafruit_USBD_CDC *usbi, SerialPIO *uart);
    void queueLineCoding(uint32_t baud, uint16_t config);
    void applyPendingLineCoding(void);
    void transmit(void);
    const char *getDescr(void);
};

void UsbUartBridge::_init(const char *descr, const char *usb_label, Adafruit_USBD_CDC *usbi) {
  this->descr = descr;
  this->baud = DEFAULT_UART_BAUD;
  this->uart_config = SERIAL_8N1;
  this->uarts = NULL;
  this->uartp = NULL;
  this->usbi = usbi;

  this->reconfig_pending = false;
  this->pending_baud = DEFAULT_UART_BAUD;
  this->pending_config = SERIAL_8N1;

  usbi->setStringDescriptor(usb_label);
  usbi->begin(DEFAULT_UART_BAUD);
}

void UsbUartBridge::init(const char *descr, const char *usb_label, Adafruit_USBD_CDC *usbi, SerialUART *uart) {
  _init(descr, usb_label, usbi);
  uarts = uart;
  _applyConfig(DEFAULT_UART_BAUD, SERIAL_8N1);
}

void UsbUartBridge::init(const char *descr, const char *usb_label, Adafruit_USBD_CDC *usbi, SerialPIO *uart) {
  _init(descr, usb_label, usbi);
  uartp = uart;
  _applyConfig(DEFAULT_UART_BAUD, SERIAL_8N1);
}

void UsbUartBridge::_applyConfig(uint32_t baud, uint16_t config) {
  if(baud == 0) {
    baud = DEFAULT_UART_BAUD;
  }

  if(uarts) {
    uarts->end();
    uarts->begin(baud, config);
  } else if(uartp) {
    uartp->end();
    uartp->begin(baud, config);
  }

  this->baud = baud;
  this->uart_config = config;
}

void UsbUartBridge::queueLineCoding(uint32_t baud, uint16_t config) {
  noInterrupts();
  pending_baud = baud;
  pending_config = config;
  reconfig_pending = true;
  interrupts();
}

void UsbUartBridge::applyPendingLineCoding(void) {
  uint32_t new_baud = 0;
  uint16_t new_config = SERIAL_8N1;
  bool should_apply = false;

  noInterrupts();
  if(reconfig_pending) {
    new_baud = pending_baud;
    new_config = pending_config;
    reconfig_pending = false;
    should_apply = true;
  }
  interrupts();

  if(should_apply && (new_baud != baud || new_config != uart_config)) {
    _applyConfig(new_baud, new_config);
  }
}

const char *UsbUartBridge::getDescr(void) {
  return descr;
}

#define UUB_UART_AVAILABLE(o) ((o)->uarts ? (o)->uarts->available() : (o)->uartp->available())
#define UUB_UART_AVAILABLE_FOR_WRITE(o) ((o)->uarts ? (o)->uarts->availableForWrite() : (o)->uartp->availableForWrite())
#define UUB_UART_READ(o) ((o)->uarts ? (o)->uarts->read() : (o)->uartp->read())
#define UUB_UART_WRITE(o, c) ((o)->uarts ? (o)->uarts->write(c) : (o)->uartp->write(c))
#define UUB_UART_FLUSH(o) ((o)->uarts ? (o)->uarts->flush() : (o)->uartp->flush())

void UsbUartBridge::transmit(void) {
  int len;
  int c;
  int budget;

  auto drain_uart_to_usb_once = [&]() {
    int uart_avail = UUB_UART_AVAILABLE(this);
    int wrote_usb = 0;

    while(uart_avail-- > 0) {
      c = UUB_UART_READ(this);
      if(c >= 0) {
        usbi->write(static_cast<uint8_t>(c));
        wrote_usb = 1;
      }
    }

    if(wrote_usb) {
      usbi->flush();
    }
  };

  drain_uart_to_usb_once();

  // Keep each bridge turn short and avoid blocking TX flushes.
  // This prevents one busy PIO channel from starving others.
  len = usbi->available();
  budget = 16;
  while(len-- > 0 && budget-- > 0) {
    if(UUB_UART_AVAILABLE_FOR_WRITE(this) <= 0) {
      break;
    }

    c = usbi->read();
    if(c >= 0) {
      UUB_UART_WRITE(this, static_cast<uint8_t>(c));
      drain_uart_to_usb_once();
    }
  }

  drain_uart_to_usb_once();
}

// tx, rx
// Serial1: GP0/1
SerialPIO Serial2PIO(2, 3, READBUFSIZE);   // GP2/3
SerialPIO Serial3PIO(5, 6, READBUFSIZE);   // GP5/6
SerialPIO Serial5PIO(21, 23, READBUFSIZE); // GP21/23
SerialPIO Serial6PIO(26, 27, READBUFSIZE); // GP26/27
Adafruit_USBD_CDC USB1;
Adafruit_USBD_CDC USB2;
Adafruit_USBD_CDC USB3;
Adafruit_USBD_CDC USB4;
Adafruit_USBD_CDC USB5;
Adafruit_USBD_CDC USB6;

UsbUartBridge UUB[UUB_MAX];

extern "C" void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const *p_line_coding) {
  if(p_line_coding == NULL || itf >= UUB_MAX) {
    return;
  }

  uint16_t config = lineCodingToUartConfig(
    p_line_coding->data_bits,
    p_line_coding->parity,
    p_line_coding->stop_bits
  );

  UUB[itf].queueLineCoding(p_line_coding->bit_rate, config);
}

void setup() {
  // Remove TinyUSB's default CDC console so only 6 bridge CDC ports enumerate.
  Serial.end();

  Serial1.setTX(0);
  Serial1.setRX(1);
  Serial2.setTX(8);
  Serial2.setRX(9);

  UUB[0].init("B0 (GP0/1)", "UART0", &USB1, &Serial1);
  UUB[1].init("B1 (GP2/3)", "UART1", &USB2, &Serial2PIO);
  UUB[2].init("B2 (GP5/6)", "UART2", &USB3, &Serial3PIO);
  UUB[3].init("B3 (GP8/9)", "UART3", &USB4, &Serial2);
  UUB[4].init("B4 (GP21/23)", "UART4", &USB5, &Serial5PIO);
  UUB[5].init("B5 (GP26/27)", "UART5", &USB6, &Serial6PIO);
}

void loop() {
  for(int i = 0; i < UUB_MAX; i++) {
    UUB[i].applyPendingLineCoding();
    UUB[i].transmit();
  }
}
