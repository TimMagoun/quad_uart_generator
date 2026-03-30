#include <Arduino.h>
#include <SerialPIO.h>
#include <Adafruit_TinyUSB.h>
#include <tusb.h>

#define UUB_MAX 6
#define READBUFSIZE 256
#define DEFAULT_UART_BAUD 115200
#define UUB_UART_TO_USB_BATCH 28
#define UUB_USB_TO_UART_BUDGET 28
#define UUB_DEBUG_STATS_INTERVAL_MS 1000
#define UUB_LINECODING_SETTLE_MS 25

#ifndef UUB_ENABLE_DEBUG_CDC
#define UUB_ENABLE_DEBUG_CDC 1
#endif

#ifndef UUB_SWAP_B1_B3_FOR_HW_DEBUG
#define UUB_SWAP_B1_B3_FOR_HW_DEBUG 1
#endif

#define UUB_EXPECTED_CDC_PORTS (UUB_MAX + (UUB_ENABLE_DEBUG_CDC ? 1 : 0))

#if CFG_TUD_CDC < UUB_EXPECTED_CDC_PORTS
#error Must increase CFG_TUD_CDC for enabled bridge+debug CDC ports.
#endif
/*
tusb_config_rt2040.h
#define CFG_TUD_CDC 7 <-
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
    volatile uint32_t pending_since_ms;
    volatile uint32_t pending_baud;
    volatile uint16_t pending_config;
    volatile uint32_t stat_usb_to_uart_bytes;
    volatile uint32_t stat_uart_to_usb_bytes;
    volatile uint32_t stat_uart_overflow;
    volatile uint32_t stat_uart_write_blocked;
    volatile uint32_t stat_usb_short_write;

    void _init(const char *descr, const char *usb_label, Adafruit_USBD_CDC *usbi);
    void _applyConfig(uint32_t baud, uint16_t config);

  public:
    void init(const char *descr, const char *usb_label, Adafruit_USBD_CDC *usbi, SerialUART *uart);
    void init(const char *descr, const char *usb_label, Adafruit_USBD_CDC *usbi, SerialPIO *uart);
    void queueLineCoding(uint32_t baud, uint16_t config);
    bool takeReadyLineCoding(uint32_t now_ms, uint32_t *new_baud, uint16_t *new_config);
    bool hasPendingLineCoding(void);
    void applyConfigIfChanged(uint32_t baud, uint16_t config);
    void forceApplyConfig(uint32_t baud, uint16_t config);
    void stopUart(void);
    void startUart(uint32_t baud, uint16_t config);
    void transmit(void);
    void snapshotAndResetStats(
      uint32_t *usb_to_uart_bytes,
      uint32_t *uart_to_usb_bytes,
      uint32_t *uart_overflow,
      uint32_t *uart_write_blocked,
      uint32_t *usb_short_write
    );
    const char *getDescr(void);
    bool isPioBridge(void) const;
    uint32_t getBaud(void) const;
    uint16_t getUartConfig(void) const;
};

void UsbUartBridge::_init(const char *descr, const char *usb_label, Adafruit_USBD_CDC *usbi) {
  this->descr = descr;
  this->baud = DEFAULT_UART_BAUD;
  this->uart_config = SERIAL_8N1;
  this->uarts = NULL;
  this->uartp = NULL;
  this->usbi = usbi;

  this->reconfig_pending = false;
  this->pending_since_ms = 0;
  this->pending_baud = DEFAULT_UART_BAUD;
  this->pending_config = SERIAL_8N1;
  this->stat_usb_to_uart_bytes = 0;
  this->stat_uart_to_usb_bytes = 0;
  this->stat_uart_overflow = 0;
  this->stat_uart_write_blocked = 0;
  this->stat_usb_short_write = 0;

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

  uint32_t old_baud = this->baud;
  uint16_t old_config = this->uart_config;
  bool applied = false;

  if(uarts) {
    uarts->end();
    uarts->begin(baud, config);
    applied = true;
  } else if(uartp) {
    uartp->end();
    uartp->begin(baud, config);
    if(static_cast<bool>(*uartp)) {
      applied = true;
    } else {
      // Keep the previous running configuration if reconfigure failed.
      uartp->end();
      uartp->begin(old_baud, old_config);
    }
  }

  if(applied) {
    this->baud = baud;
    this->uart_config = config;
  }

#if UUB_ENABLE_DEBUG_CDC
  Serial.print("[uub] cfg ");
  Serial.print(descr);
  Serial.print(" baud=");
  Serial.print(applied ? baud : old_baud);
  Serial.print(" cfg=0x");
  Serial.print(applied ? config : old_config, HEX);
  if(!applied) {
    Serial.print(" fail-restore=1");
  }
  Serial.print(" run=");
  if(uarts) {
    Serial.print(1);
    Serial.print(" afw=");
    Serial.println(uarts->availableForWrite());
  } else if(uartp) {
    Serial.print(static_cast<int>(static_cast<bool>(*uartp)));
    Serial.print(" afw=");
    Serial.println(uartp->availableForWrite());
  } else {
    Serial.println("0 afw=0");
  }
#endif
}

void UsbUartBridge::queueLineCoding(uint32_t baud, uint16_t config) {
  uint32_t now_ms = millis();
  noInterrupts();
  pending_since_ms = now_ms;
  pending_baud = baud;
  pending_config = config;
  reconfig_pending = true;
  interrupts();
}

bool UsbUartBridge::takeReadyLineCoding(uint32_t now_ms, uint32_t *new_baud, uint16_t *new_config) {
  bool ready = false;
  noInterrupts();
  if(reconfig_pending) {
    uint32_t elapsed_ms = now_ms - pending_since_ms;
    if(elapsed_ms >= UUB_LINECODING_SETTLE_MS) {
      *new_baud = pending_baud;
      *new_config = pending_config;
      reconfig_pending = false;
      ready = true;
    }
  }
  interrupts();
  return ready;
}

bool UsbUartBridge::hasPendingLineCoding(void) {
  bool pending = false;
  noInterrupts();
  pending = reconfig_pending;
  interrupts();
  return pending;
}

void UsbUartBridge::applyConfigIfChanged(uint32_t new_baud, uint16_t new_config) {
  if(new_baud != baud || new_config != uart_config) {
    _applyConfig(new_baud, new_config);
  }
}

void UsbUartBridge::forceApplyConfig(uint32_t new_baud, uint16_t new_config) {
  _applyConfig(new_baud, new_config);
}

void UsbUartBridge::stopUart(void) {
  if(uarts) {
    uarts->end();
  } else if(uartp) {
    uartp->end();
  }
}

void UsbUartBridge::startUart(uint32_t new_baud, uint16_t new_config) {
  if(new_baud == 0) {
    new_baud = DEFAULT_UART_BAUD;
  }

  uint32_t old_baud = baud;
  uint16_t old_config = uart_config;
  bool applied = false;

  if(uarts) {
    uarts->begin(new_baud, new_config);
    applied = true;
  } else if(uartp) {
    uartp->begin(new_baud, new_config);
    if(static_cast<bool>(*uartp)) {
      applied = true;
    } else {
      // Preserve previous config if the new begin fails.
      uartp->end();
      uartp->begin(old_baud, old_config);
    }
  }

  if(applied) {
    baud = new_baud;
    uart_config = new_config;
  }

#if UUB_ENABLE_DEBUG_CDC
  Serial.print("[uub] cfg ");
  Serial.print(descr);
  Serial.print(" baud=");
  Serial.print(applied ? new_baud : old_baud);
  Serial.print(" cfg=0x");
  Serial.print(applied ? new_config : old_config, HEX);
  if(!applied) {
    Serial.print(" fail-restore=1");
  }
  Serial.print(" run=");
  if(uarts) {
    Serial.print(1);
    Serial.print(" afw=");
    Serial.println(uarts->availableForWrite());
  } else if(uartp) {
    Serial.print(static_cast<int>(static_cast<bool>(*uartp)));
    Serial.print(" afw=");
    Serial.println(uartp->availableForWrite());
  } else {
    Serial.println("0 afw=0");
  }
#endif
}

bool UsbUartBridge::isPioBridge(void) const {
  return uartp != NULL;
}

uint32_t UsbUartBridge::getBaud(void) const {
  return baud;
}

uint16_t UsbUartBridge::getUartConfig(void) const {
  return uart_config;
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
  int usb_flush_needed;
  bool linecoding_pending = hasPendingLineCoding();

  auto drain_uart_to_usb_once = [&]() {
    uint8_t batch[UUB_UART_TO_USB_BATCH];
    int uart_avail = UUB_UART_AVAILABLE(this);
    int batch_len = 0;

    while(uart_avail-- > 0 && batch_len < UUB_UART_TO_USB_BATCH) {
      c = UUB_UART_READ(this);
      if(c >= 0) {
        batch[batch_len++] = static_cast<uint8_t>(c);
      }
    }

    if(batch_len > 0) {
      size_t wrote = usbi->write(batch, static_cast<size_t>(batch_len));
      if(wrote > 0) {
        stat_uart_to_usb_bytes += static_cast<uint32_t>(wrote);
        usb_flush_needed = 1;
      }
      if(wrote < static_cast<size_t>(batch_len)) {
        stat_usb_short_write++;
      }
    }
  };

  usb_flush_needed = 0;
  drain_uart_to_usb_once();

  // While a line-coding change is pending, do not forward USB->UART bytes yet.
  // This avoids sending the first payload with stale UART settings.
  if(!linecoding_pending) {
    // Keep each bridge turn short and avoid blocking work.
    // This prevents one busy bridge from starving others.
    len = usbi->available();
    budget = UUB_USB_TO_UART_BUDGET;
    while(len-- > 0 && budget-- > 0) {
      if(UUB_UART_AVAILABLE_FOR_WRITE(this) <= 0) {
        stat_uart_write_blocked++;
        break;
      }

      c = usbi->read();
      if(c >= 0) {
        UUB_UART_WRITE(this, static_cast<uint8_t>(c));
        stat_usb_to_uart_bytes++;
      }
    }
  }

  drain_uart_to_usb_once();
  if(usb_flush_needed) {
    usbi->flush();
  }

  if(uartp && uartp->overflow()) {
    stat_uart_overflow++;
  }
}

void UsbUartBridge::snapshotAndResetStats(
  uint32_t *usb_to_uart_bytes,
  uint32_t *uart_to_usb_bytes,
  uint32_t *uart_overflow,
  uint32_t *uart_write_blocked,
  uint32_t *usb_short_write
) {
  noInterrupts();
  *usb_to_uart_bytes = stat_usb_to_uart_bytes;
  *uart_to_usb_bytes = stat_uart_to_usb_bytes;
  *uart_overflow = stat_uart_overflow;
  *uart_write_blocked = stat_uart_write_blocked;
  *usb_short_write = stat_usb_short_write;
  stat_usb_to_uart_bytes = 0;
  stat_uart_to_usb_bytes = 0;
  stat_uart_overflow = 0;
  stat_uart_write_blocked = 0;
  stat_usb_short_write = 0;
  interrupts();
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
  const uint8_t bridge_itf_base = UUB_ENABLE_DEBUG_CDC ? 1 : 0;
  if(p_line_coding == NULL || itf < bridge_itf_base || itf >= (bridge_itf_base + UUB_MAX)) {
#if UUB_ENABLE_DEBUG_CDC
    Serial.print("[uub] linecoding skip itf=");
    Serial.println(itf);
#endif
    return;
  }
  uint8_t bridge_idx = static_cast<uint8_t>(itf - bridge_itf_base);

  uint16_t config = lineCodingToUartConfig(
    p_line_coding->data_bits,
    p_line_coding->parity,
    p_line_coding->stop_bits
  );

  UUB[bridge_idx].queueLineCoding(p_line_coding->bit_rate, config);
#if UUB_ENABLE_DEBUG_CDC
  Serial.print("[uub] linecoding itf=");
  Serial.print(itf);
  Serial.print(" idx=");
  Serial.print(bridge_idx);
  Serial.print(" -> ");
  Serial.print(UUB[bridge_idx].getDescr());
  Serial.print(" baud=");
  Serial.print(p_line_coding->bit_rate);
  Serial.print(" data=");
  Serial.print(p_line_coding->data_bits);
  Serial.print(" parity=");
  Serial.print(p_line_coding->parity);
  Serial.print(" stop=");
  Serial.println(p_line_coding->stop_bits);
#endif
}

void setup() {
#if UUB_ENABLE_DEBUG_CDC
  Serial.begin(115200);
#endif

  Serial1.setTX(0);
  Serial1.setRX(1);
  Serial2.setTX(8);
  Serial2.setRX(9);

  UUB[0].init("B0 (GP0/1)", "UART0", &USB1, &Serial1);
#if UUB_SWAP_B1_B3_FOR_HW_DEBUG
  // Debug mode: put /dev/ttyACM1 on HW UART and move GP2/3 PIO to index 3.
  UUB[1].init("B1 (HW GP8/9)", "UART1", &USB2, &Serial2);
  UUB[2].init("B2 (GP5/6)", "UART2", &USB3, &Serial3PIO);
  UUB[3].init("B3 (PIO GP2/3)", "UART3", &USB4, &Serial2PIO);
#else
  UUB[1].init("B1 (GP2/3)", "UART1", &USB2, &Serial2PIO);
  UUB[2].init("B2 (GP5/6)", "UART2", &USB3, &Serial3PIO);
  UUB[3].init("B3 (GP8/9)", "UART3", &USB4, &Serial2);
#endif
  UUB[4].init("B4 (GP21/23)", "UART4", &USB5, &Serial5PIO);
  UUB[5].init("B5 (GP26/27)", "UART5", &USB6, &Serial6PIO);
}

void loop() {
  uint32_t now_ms = millis();
  uint32_t pio_target_baud[UUB_MAX];
  uint16_t pio_target_cfg[UUB_MAX];
  bool pio_restart_needed = false;

  for(int i = 0; i < UUB_MAX; i++) {
    pio_target_baud[i] = UUB[i].getBaud();
    pio_target_cfg[i] = UUB[i].getUartConfig();
  }

  for(int i = 0; i < UUB_MAX; i++) {
    uint32_t new_baud = 0;
    uint16_t new_config = SERIAL_8N1;
    if(!UUB[i].takeReadyLineCoding(now_ms, &new_baud, &new_config)) {
      continue;
    }

    if(UUB[i].isPioBridge()) {
      pio_target_baud[i] = new_baud;
      pio_target_cfg[i] = new_config;
      if(new_baud != UUB[i].getBaud() || new_config != UUB[i].getUartConfig()) {
        pio_restart_needed = true;
      }
      continue;
    }

    UUB[i].applyConfigIfChanged(new_baud, new_config);
  }

  if(pio_restart_needed) {
#if UUB_ENABLE_DEBUG_CDC
    Serial.println("[uub] coordinated PIO restart (stop)");
#endif
    for(int i = 0; i < UUB_MAX; i++) {
      if(UUB[i].isPioBridge()) {
        UUB[i].stopUart();
      }
    }
#if UUB_ENABLE_DEBUG_CDC
    Serial.println("[uub] coordinated PIO restart (start)");
#endif
    for(int i = 0; i < UUB_MAX; i++) {
      if(UUB[i].isPioBridge()) {
        UUB[i].startUart(pio_target_baud[i], pio_target_cfg[i]);
      }
    }
  }

  for(int i = 0; i < UUB_MAX; i++) {
    UUB[i].transmit();
  }

#if UUB_ENABLE_DEBUG_CDC
  static uint32_t last_debug_ms = 0;
  uint32_t dbg_now_ms = millis();
  if((uint32_t)(dbg_now_ms - last_debug_ms) >= UUB_DEBUG_STATS_INTERVAL_MS) {
    last_debug_ms = dbg_now_ms;

    Serial.println("[uub] bridge stats (last 1s):");
    for(int i = 0; i < UUB_MAX; i++) {
      uint32_t usb_to_uart = 0;
      uint32_t uart_to_usb = 0;
      uint32_t overflow = 0;
      uint32_t write_blocked = 0;
      uint32_t usb_short_write = 0;

      UUB[i].snapshotAndResetStats(
        &usb_to_uart,
        &uart_to_usb,
        &overflow,
        &write_blocked,
        &usb_short_write
      );

      Serial.print("  [");
      Serial.print(i);
      Serial.print("] ");
      Serial.print(UUB[i].getDescr());
      Serial.print(" u2u=");
      Serial.print(usb_to_uart);
      Serial.print(" u2usb=");
      Serial.print(uart_to_usb);
      Serial.print(" ovf=");
      Serial.print(overflow);
      Serial.print(" wblk=");
      Serial.print(write_blocked);
      Serial.print(" usbshort=");
      Serial.println(usb_short_write);
    }
  }
#endif
}
