# Quad UART Traffic Generator Design

## Objective
Build RP2040 firmware that provides 4 independent UART transmit-only outputs for serial stress/integration testing. Each port must support commonly used serial formats and configurable packet generation behavior via an ASCII host command interface.

## Scope
In scope:
- 4 independent TX-only UART outputs
- Per-port configuration: baud, serial format, payload length, packet rate
- Packet format with port identification, sequence counter, and CRC-16
- USB CDC ASCII command interface for control and status
- Guardrail that configuration changes are only allowed while outputs are disabled

Out of scope (v1):
- UART RX path
- Binary host protocol
- Persistent storage of settings across reboot
- PIO/DMA advanced acceleration

## Platform Context
- Board: SparkFun Pro Micro RP2040
- Build system: PlatformIO
- Framework: Arduino-Pico
- Current config source: `platformio.ini`

## Functional Requirements

### 1) Port Count and Independence
- Exactly 4 TX ports, identified as `0..3`.
- Ports are behaviorally independent:
  - Different baud per port
  - Different serial format per port
  - Different payload length per port
  - Different packet rate per port
- Each port maintains its own sequence counter.

### 2) Supported Serial Formats
Per port support:
- Baud: up to `921600`
- Data bits: `5, 6, 7, 8`
- Parity: `N, E, O`
- Stop bits: `1, 2`

### 3) Payload Controls
Per port support:
- Payload length: `1..128` bytes user-configurable (effective minimum for protocol payload is constrained by frame format, see packet section)
- Packet rate: `1..1000` packets/sec

### 4) Configuration Safety Rule
- Host-side configuration changes are permitted only when outputs are disabled.
- If any output is enabled and a mutating config command is issued, firmware returns `ERR BUSY`.

### 5) Packet Integrity Metadata
Each generated payload must include:
- Port identifier
- Sequence number
- CRC-16 checksum

## Architecture

### UART Implementation Choice
- Use RP2040 PIO UART transmit paths (`SerialPIO`/PIO-backed TX) for all 4 output ports.
- Rationale: RP2040 has only 2 hardware UART peripherals; 4 independent ports require PIO-backed UART TX.

### Control Plane
- USB CDC serial endpoint accepts newline-delimited ASCII commands.
- Lightweight parser converts commands to operations on per-port config/state.
- Responses are one-line ASCII replies for simple host scripting.

### Data Plane
- Per-port configuration struct:
  - `baud`, `data_bits`, `parity`, `stop_bits`, `len`, `pps`
- Per-port runtime state:
  - `enabled`, `seq`, `next_deadline_us`
  - `tx_packet_buf[128]`, `tx_packet_len`, `tx_write_index`, `tx_in_progress`
- Scheduler loop checks all 4 ports and emits packets when per-port deadlines are reached.
- TX servicing is non-blocking: each scheduler pass writes only while UART has room, then yields.

### Timing Model
- Packet period computed per port from `pps`.
- `next_deadline_us` advanced by period for deterministic periodic behavior.
- Catch-up policy (if delayed): transmit once and rebase to now + period (KISS, avoids burst storms).

### Throughput Guardrail (Dynamic Rate Cap)
- Requested `pps` is validated against per-port maximum throughput:
  - `max_pps = floor(baud / (bits_per_char * len))`
  - `bits_per_char = 1(start) + data_bits + parity_bits(0 or 1) + stop_bits`
- If requested `pps > max_pps`, command is rejected with `ERR BAD_VALUE`.
- This prevents oversubscription and prevents scheduler stalls at low baud/high payload settings (e.g., 9600 baud).

## Host ASCII Command Interface

## Command Rules
- Commands are case-insensitive.
- Arguments are whitespace-separated.
- One command per line.
- Generic responses:
  - Success: `OK`
  - Error: `ERR <CODE> <MSG>`

Error codes:
- `BAD_CMD`
- `BAD_PORT`
- `BAD_VALUE`
- `BUSY`
- `UNSUPPORTED`

### Commands
- `HELP`
- `STATUS`
- `PORT <id> SHOW`
- `PORT <id> CFG BAUD <n>`
- `PORT <id> CFG FORMAT <data><parity><stop>` (examples: `8N1`, `7E1`, `8O2`)
- `PORT <id> CFG LEN <n>`
- `PORT <id> CFG PPS <n>`
- `PORT <id> ENABLE`
- `PORT <id> DISABLE`
- `ALL ENABLE`
- `ALL DISABLE`

### Example Query Response
`PORT=2 BAUD=115200 FORMAT=8N1 LEN=64 PPS=50 EN=0 SEQ=1234`

## Packet Format
For configured port payload length `LEN`, byte layout:

1. `MAGIC` (2 bytes): `0x51 0x55`
2. `PORT_ID` (1 byte): `0..3`
3. `SEQ` (4 bytes, little-endian)
4. `PAYLOAD_LEN` (1 byte): configured `LEN`
5. `DATA` (`LEN - 10` bytes): deterministic pattern
6. `CRC16` (2 bytes, little-endian): CRC-16/CCITT over bytes `0..LEN-3`

Derived constraints:
- Protocol minimum frame size is `10` bytes.
- If host sets `LEN < 10`, command rejected with `ERR BAD_VALUE`.

DATA fill pattern:
- For each data byte index `i` in `DATA`: `(port_id ^ (seq & 0xFF) ^ i) & 0xFF`
- Deterministic and cheap to compute for debugging and integrity checks.

### Sequential Transmission Rule
- Per port, packets are strictly in-order and non-overlapping.
- `SEQ` increments only after the full current packet has been queued for TX.
- A new packet is not started for a port while `tx_in_progress` is true.
- This guarantees sequential packet emission even under non-blocking partial-write scheduling.

## Error Handling
- Invalid syntax or unknown tokens: `ERR BAD_CMD`
- Invalid port index: `ERR BAD_PORT`
- Value out of range / malformed format: `ERR BAD_VALUE`
  - Includes `pps` above dynamic throughput cap for current baud/format/len
- Unsupported serial mode by backend implementation: `ERR UNSUPPORTED`
- Config mutation while enabled: `ERR BUSY`

## Testing Strategy

### Unit-Level (host-side or embedded where feasible)
- Parser acceptance/rejection for all commands
- Format parser for `5/6/7/8`, `N/E/O`, `1/2`
- Payload generator layout and field correctness
- CRC-16 known-vector validation
- Sequence increment behavior

### Integration-Level (on hardware)
- Configure all 4 ports differently and verify concurrent transmit
- Validate effective baud/format combinations on external analyzer/receiver
- Verify packet rate control for representative values (1, 10, 100, 1000 pps)
- Verify dynamic cap enforcement using low-baud/high-len cases (e.g., 9600 baud, len 128)
- Verify non-blocking scheduler behavior under saturated throughput (no control-plane starvation)
- Verify strict per-port sequence ordering with no skipped/interleaved packet starts
- Confirm `ERR BUSY` rule while any output is enabled

## Open Choices Finalized
- Command interface type: ASCII
- Reconfiguration policy: only when outputs disabled
- Packet rate model: fixed packets/sec
- Checksum: CRC-16
- Design principle: KISS
- UART expansion mechanism: 4x PIO TX UART

## Risks and Mitigations
- High combined throughput can stress software UART timing.
  - Mitigation: keep scheduler simple, avoid catch-up bursts, validate high-rate scenarios early.
- Some serial format combinations may be unsupported by underlying UART implementation.
  - Mitigation: detect and return `ERR UNSUPPORTED`.

## Success Criteria
- All 4 ports can transmit simultaneously with independent config.
- Each port supports baud up to `921600` for supported hardware UART mode.
- Commands reliably configure/query each port.
- Every emitted packet includes verifiable `PORT_ID`, monotonic `SEQ`, and valid CRC-16.
- Configuration changes are blocked while outputs are enabled.
