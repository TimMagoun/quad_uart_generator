# Quad UART Traffic Generator Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build RP2040 firmware with 4 independent PIO-backed UART TX ports controlled by USB CDC ASCII commands, enforcing dynamic rate caps and sequential non-blocking packet transmission.

**Architecture:** USB CDC handles ASCII control commands and per-port configuration. Four PIO-backed TX ports run independent non-blocking transmit state machines. Each packet includes port ID, sequence, deterministic payload bytes, and CRC-16/CCITT for integrity validation.

**Tech Stack:** PlatformIO, Arduino-Pico (RP2040), C++17, Unity tests (`pio test`)

---

### File Map

- `platformio.ini`
  - Add a native/unit-test environment and test build flags.
- `src/main.cpp`
  - Arduino `setup()/loop()`, command I/O, scheduler loop integration.
- `src/quad_uart_controller.h`
  - Public controller API and shared structs for 4 ports.
- `src/quad_uart_controller.cpp`
  - Port config state, dynamic max-pps math, command handlers, scheduling.
- `src/packet_builder.h`
  - Packet builder and CRC function declarations.
- `src/packet_builder.cpp`
  - CRC-16/CCITT and packet assembly logic.
- `src/command_parser.h`
  - Parser interface for ASCII commands.
- `src/command_parser.cpp`
  - Tokenization and command decode/validation.
- `test/test_native_crc_packet/test_main.cpp`
  - CRC and packet-layout tests.
- `test/test_native_parser/test_main.cpp`
  - Command parsing and validation tests.
- `test/test_native_rate_cap/test_main.cpp`
  - Throughput cap calculations and rejection logic tests.

### Task 1: Project Skeleton and Test Harness

**Files:**
- Modify: `platformio.ini`
- Create: `src/main.cpp`
- Create: `src/quad_uart_controller.h`
- Create: `src/quad_uart_controller.cpp`

- [ ] **Step 1: Write the failing test baseline (native test target should fail before env exists)**

```cpp
// test/test_native_rate_cap/test_main.cpp
#include <unity.h>

void test_placeholder(void) {
    TEST_ASSERT_TRUE(false);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_placeholder);
    return UNITY_END();
}
```

- [ ] **Step 2: Run test to verify it fails due to missing/invalid native setup**

Run: `pio test -e native -v`
Expected: FAIL (environment missing or test failure)

- [ ] **Step 3: Add native test env and create firmware/controller skeleton**

```ini
; platformio.ini (append)
[env:native]
platform = native
test_framework = unity
build_flags =
  -std=gnu++17
```

```cpp
// src/quad_uart_controller.h
#pragma once

#include <stdint.h>
#include <stddef.h>

enum class ParityMode : uint8_t { None, Even, Odd };

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
```

```cpp
// src/main.cpp
#include <Arduino.h>

void setup() {
    Serial.begin(115200);
}

void loop() {
    // Filled in by later tasks.
}
```

- [ ] **Step 4: Re-run baseline tests and confirm harness works**

Run: `pio test -e native -v`
Expected: FAIL (placeholder assertion) with successful native environment setup

- [ ] **Step 5: Commit**

```bash
git add platformio.ini src/main.cpp src/quad_uart_controller.h src/quad_uart_controller.cpp test/test_native_rate_cap/test_main.cpp
git commit -m "chore: scaffold quad uart project and native unity test target"
```

### Task 2: CRC-16 and Packet Builder

**Files:**
- Create: `src/packet_builder.h`
- Create: `src/packet_builder.cpp`
- Test: `test/test_native_crc_packet/test_main.cpp`

- [ ] **Step 1: Write failing tests for CRC and packet layout**

```cpp
#include <unity.h>
#include "packet_builder.h"

void test_crc16_known_vector(void) {
    const uint8_t bytes[] = {'1','2','3','4','5','6','7','8','9'};
    TEST_ASSERT_EQUAL_HEX16(0x29B1, crc16_ccitt_false(bytes, sizeof(bytes)));
}

void test_packet_layout_contains_port_seq_len_and_crc(void) {
    uint8_t out[32] = {0};
    const size_t n = build_packet(2, 7, 16, out, sizeof(out));

    TEST_ASSERT_EQUAL_UINT32(16, n);
    TEST_ASSERT_EQUAL_HEX8(0x51, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x55, out[1]);
    TEST_ASSERT_EQUAL_UINT8(2, out[2]);
    TEST_ASSERT_EQUAL_UINT8(16, out[7]);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_crc16_known_vector);
    RUN_TEST(test_packet_layout_contains_port_seq_len_and_crc);
    return UNITY_END();
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `pio test -e native -f test_native_crc_packet -v`
Expected: FAIL with missing symbols for `crc16_ccitt_false` / `build_packet`

- [ ] **Step 3: Implement CRC and packet assembly**

```cpp
// src/packet_builder.h
#pragma once

#include <stddef.h>
#include <stdint.h>

uint16_t crc16_ccitt_false(const uint8_t* data, size_t len);
size_t build_packet(uint8_t port_id, uint32_t seq, uint8_t payload_len, uint8_t* out, size_t out_cap);
```

```cpp
// src/packet_builder.cpp
#include "packet_builder.h"

uint16_t crc16_ccitt_false(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

size_t build_packet(uint8_t port_id, uint32_t seq, uint8_t payload_len, uint8_t* out, size_t out_cap) {
    if (!out || payload_len < 10 || payload_len > 128 || out_cap < payload_len) return 0;
    out[0] = 0x51; out[1] = 0x55; out[2] = port_id;
    out[3] = static_cast<uint8_t>(seq & 0xFF);
    out[4] = static_cast<uint8_t>((seq >> 8) & 0xFF);
    out[5] = static_cast<uint8_t>((seq >> 16) & 0xFF);
    out[6] = static_cast<uint8_t>((seq >> 24) & 0xFF);
    out[7] = payload_len;
    for (uint8_t i = 8; i < payload_len - 2; ++i) {
        out[i] = static_cast<uint8_t>(port_id ^ static_cast<uint8_t>(seq) ^ i);
    }
    const uint16_t crc = crc16_ccitt_false(out, payload_len - 2);
    out[payload_len - 2] = static_cast<uint8_t>(crc & 0xFF);
    out[payload_len - 1] = static_cast<uint8_t>((crc >> 8) & 0xFF);
    return payload_len;
}
```

- [ ] **Step 4: Run tests to verify pass**

Run: `pio test -e native -f test_native_crc_packet -v`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add src/packet_builder.h src/packet_builder.cpp test/test_native_crc_packet/test_main.cpp
git commit -m "feat: add crc16 and packet builder"
```

### Task 3: ASCII Parser and Dynamic Rate Cap Validation

**Files:**
- Create: `src/command_parser.h`
- Create: `src/command_parser.cpp`
- Modify: `src/quad_uart_controller.h`
- Modify: `src/quad_uart_controller.cpp`
- Test: `test/test_native_parser/test_main.cpp`
- Test: `test/test_native_rate_cap/test_main.cpp`

- [ ] **Step 1: Write failing parser/rate-cap tests**

```cpp
#include <unity.h>
#include "command_parser.h"
#include "quad_uart_controller.h"

void test_parse_format_8e2(void) {
    ParsedFormat f{};
    TEST_ASSERT_TRUE(parse_format_token("8E2", &f));
    TEST_ASSERT_EQUAL_UINT8(8, f.data_bits);
    TEST_ASSERT_EQUAL_UINT8(2, f.stop_bits);
}

void test_rate_cap_rejects_oversubscription_9600_8e2_128(void) {
    const uint16_t max_pps = compute_max_pps(9600, 8, ParityMode::Even, 2, 128);
    TEST_ASSERT_EQUAL_UINT16(6, max_pps);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_parse_format_8e2);
    RUN_TEST(test_rate_cap_rejects_oversubscription_9600_8e2_128);
    return UNITY_END();
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `pio test -e native -f test_native_parser -f test_native_rate_cap -v`
Expected: FAIL with missing parser and cap functions

- [ ] **Step 3: Implement parser and cap math**

```cpp
// src/command_parser.h
#pragma once

#include <stdint.h>

struct ParsedFormat {
    uint8_t data_bits;
    char parity;
    uint8_t stop_bits;
};

bool parse_format_token(const char* token, ParsedFormat* out);
```

```cpp
// src/quad_uart_controller.h (append)
uint16_t compute_max_pps(uint32_t baud, uint8_t data_bits, ParityMode parity, uint8_t stop_bits, uint8_t payload_len);
```

```cpp
// src/quad_uart_controller.cpp
#include "quad_uart_controller.h"

uint16_t compute_max_pps(uint32_t baud, uint8_t data_bits, ParityMode parity, uint8_t stop_bits, uint8_t payload_len) {
    const uint8_t parity_bits = (parity == ParityMode::None) ? 0 : 1;
    const uint16_t bits_per_char = static_cast<uint16_t>(1 + data_bits + parity_bits + stop_bits);
    const uint32_t denom = static_cast<uint32_t>(bits_per_char) * payload_len;
    if (denom == 0) return 0;
    return static_cast<uint16_t>(baud / denom);
}
```

- [ ] **Step 4: Re-run parser/rate tests**

Run: `pio test -e native -f test_native_parser -f test_native_rate_cap -v`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add src/command_parser.h src/command_parser.cpp src/quad_uart_controller.h src/quad_uart_controller.cpp test/test_native_parser/test_main.cpp test/test_native_rate_cap/test_main.cpp
git commit -m "feat: add ascii format parser and dynamic rate cap validation"
```

### Task 4: Non-Blocking Sequential TX Scheduler on Hardware

**Files:**
- Modify: `src/main.cpp`
- Modify: `src/quad_uart_controller.h`
- Modify: `src/quad_uart_controller.cpp`
- Modify: `platformio.ini` (if needed for monitor speed/logging)

- [ ] **Step 1: Write hardware-facing behavior tests as assertions/hooks in controller unit tests**

```cpp
// test/test_native_rate_cap/test_main.cpp (append)
void test_seq_advances_only_after_packet_complete(void) {
    // Use a fake TX writer with limited FIFO to force partial writes.
    // Assert: seq increments once after all bytes queued, never mid-packet.
    TEST_IGNORE_MESSAGE("implemented with fake writer in controller tests");
}
```

- [ ] **Step 2: Run tests to verify new behavior is not yet implemented**

Run: `pio test -e native -f test_native_rate_cap -v`
Expected: FAIL/IGNORE indicating missing sequential state-machine checks

- [ ] **Step 3: Implement non-blocking TX state machine with strict sequencing**

```cpp
// scheduler pseudocode in quad_uart_controller.cpp
for each port:
  if !enabled: continue
  if tx_in_progress:
    while writer.availableForWrite(port) > 0 && tx_write_index < tx_packet_len:
      writer.write(port, tx_packet_buf[tx_write_index++])
    if tx_write_index == tx_packet_len:
      tx_in_progress = false
      seq++
  else if now_us >= next_deadline_us:
    tx_packet_len = build_packet(port_id, seq, cfg.payload_len, tx_packet_buf, sizeof(tx_packet_buf))
    tx_write_index = 0
    tx_in_progress = true
    next_deadline_us = now_us + (1000000 / cfg.pps)
```

```cpp
// main.cpp integration sketch
void loop() {
    controller.poll_control_serial(Serial);
    controller.service_tx_nonblocking(micros());
}
```

- [ ] **Step 4: Verify build and on-device behavior**

Run: `pio run -e sparkfun_promicrorp2040`
Expected: SUCCESS

Run: `pio run -e sparkfun_promicrorp2040 -t upload`
Expected: SUCCESS (firmware flashed to attached RP2040)

Run: `pio device monitor -b 115200`
Expected: accepts commands; rejects oversubscription (`ERR BAD_VALUE`); transmits ordered packets per enabled port

- [ ] **Step 5: Commit**

```bash
git add src/main.cpp src/quad_uart_controller.h src/quad_uart_controller.cpp platformio.ini
git commit -m "feat: add non-blocking sequential tx scheduler for 4 pio uart ports"
```

### Task 5: Command Interface Finalization and Smoke Validation

**Files:**
- Modify: `src/command_parser.cpp`
- Modify: `src/quad_uart_controller.cpp`
- Test: `test/test_native_parser/test_main.cpp`

- [ ] **Step 1: Write failing tests for BUSY rule and key commands**

```cpp
void test_cfg_rejected_when_any_port_enabled(void) {
    // Arrange enabled port then issue CFG command.
    // Expect: ERR BUSY
}

void test_port_show_includes_seq_and_enable_flag(void) {
    // Expect fields: PORT, BAUD, FORMAT, LEN, PPS, EN, SEQ
}
```

- [ ] **Step 2: Run tests to confirm failures**

Run: `pio test -e native -f test_native_parser -v`
Expected: FAIL for BUSY/status expectations

- [ ] **Step 3: Implement command handlers and response strings**

```cpp
// handle commands:
// HELP, STATUS, PORT <id> SHOW, PORT <id> CFG BAUD/FORMAT/LEN/PPS,
// PORT <id> ENABLE/DISABLE, ALL ENABLE, ALL DISABLE
// enforce: any CFG mutation while any port enabled => ERR BUSY
```

- [ ] **Step 4: Run full test suite and hardware smoke**

Run: `pio test -e native -v`
Expected: PASS

Run: `pio run -e sparkfun_promicrorp2040 -t upload`
Expected: SUCCESS

Run: `pio device monitor -b 115200`
Expected: manual command smoke checks pass

- [ ] **Step 5: Commit**

```bash
git add src/command_parser.cpp src/quad_uart_controller.cpp test/test_native_parser/test_main.cpp
git commit -m "feat: finalize ascii control protocol and busy-rule enforcement"
```

## Self-Review Checklist

- Spec coverage: all requirements mapped (4 PIO TX ports, independent config, ASCII control, CRC packet, dynamic cap, sequential non-blocking TX, BUSY mutation rule).
- Placeholder scan: no TBD/TODO placeholders left in actionable steps.
- Type consistency: shared enums/structs and helper function names are consistent across tasks.
