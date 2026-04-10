# Quad UART TX Traffic Generator (RP2040)

Firmware for RP2040 that generates deterministic UART transmit traffic on 4 independent TX-only ports.

## Features

- 4 independent UART TX outputs
- Per-port config: baud, serial format, payload length, packets/sec
- ASCII command interface over USB serial
- Non-blocking TX scheduler with sequential packets per port
- Dynamic throughput guard (`pps` rejected if link cannot carry requested load)
- Packet integrity fields: port ID, sequence number, CRC-16/CCITT-FALSE

## Hardware / Pin Map

Target environment:
- `sparkfun_promicrorp2040` (PlatformIO)

TX pin assignment:
- Port 0 TX: GPIO 2
- Port 1 TX: GPIO 3
- Port 2 TX: GPIO 4
- Port 3 TX: GPIO 5

USB CDC serial (for commands):
- 115200 baud

## Build and Flash

From repo root:

```bash
.venv/bin/pio run -e sparkfun_promicrorp2040
.venv/bin/pio run -e sparkfun_promicrorp2040 -t upload
```

Open serial monitor:

```bash
.venv/bin/pio device monitor -b 115200
```

## Command Interface

Commands are ASCII text lines terminated by `\n`.
Parsing is case-insensitive.

### Quick Start

```text
PORT 0 CFG BAUD 115200
PORT 0 CFG FORMAT 8N1
PORT 0 CFG LEN 32
PORT 0 CFG PPS 50
PORT 0 ENABLE
STATUS
PORT 0 DISABLE
```

### Commands

- `HELP`
- `STATUS`
- `ALL ENABLE`
- `ALL DISABLE`
- `PORT <id[,id...]> SHOW`
- `PORT <id[,id...]> ENABLE`
- `PORT <id[,id...]> DISABLE`
- `PORT <id[,id...]> CFG BAUD <value>`
- `PORT <id[,id...]> CFG FORMAT <value>` where format is `<data><parity><stop>` (for example `8N1`, `7E1`, `8O2`)
- `PORT <id[,id...]> CFG LEN <value>`
- `PORT <id[,id...]> CFG PPS <value>`

Port ID range:
- `0..3`
- List syntax example: `PORT 1,2,3 CFG BAUD 921600`

Configuration rules:
- Any `CFG` command is rejected while any port is enabled: `ERR BUSY outputs-enabled`
- Disable outputs before changing config

## Limits

- Baud: `1..921600`
- Data bits: `5..8`
- Parity: `N`, `E`, `O`
- Stop bits: `1..2`
- Payload length: `10..128` bytes
- PPS: `1..1000` (hard ceiling)
- Effective PPS must also satisfy dynamic link cap:
  - `max_pps = floor(baud / (bits_per_char * payload_len))`
  - where `bits_per_char = 1 + data_bits + parity_bits + stop_bits`

Example:
- At `9600`, `8E2`, `LEN=128`, max is `6 pps`

## Packet Format

Each transmitted packet has length `LEN` (10..128) with this layout:

- Byte 0: `0x51`
- Byte 1: `0x55`
- Byte 2: `PORT_ID` (`0..3`)
- Bytes 3..6: `SEQ` (uint32 little-endian)
- Byte 7: `LEN`
- Bytes 8..LEN-3: deterministic payload bytes
- Bytes LEN-2..LEN-1: CRC-16/CCITT-FALSE (little-endian CRC value)

CRC parameters:
- Polynomial: `0x1021`
- Init: `0xFFFF`
- No reflection
- No final xor

Sequence behavior:
- Sequence increments only after a full packet is successfully queued out
- Packets remain strictly sequential per port

## Responses and Errors

Success:
- `OK`
- Or status lines like `PORT=0 BAUD=115200 FORMAT=8N1 LEN=32 PPS=50 EN=1 SEQ=123`

Common errors:
- `ERR BAD_CMD ...` (syntax/shape)
- `ERR BAD_PORT ...` (invalid port)
- `ERR BAD_VALUE ...` (value/format/range/throughput invalid)
- `ERR BUSY outputs-enabled` (configuration attempted while enabled)
- `ERR UNSUPPORTED ...` (backend could not start UART mode)

If an input command line exceeds 255 chars:
- `ERR BAD_CMD line-too-long`

## Validation

Run host-side tests:

```bash
.venv/bin/pio test -e native
```

Optional static analysis:

```bash
.venv/bin/pio check -e sparkfun_promicrorp2040 --skip-packages
```

### Hardware test results:
![Logic Analyzer](docs/images/logic_analyzer_1.png)
Logic analyzer view of all ports at 921600 baud, 8N1, 16 byte payload, 1000 pps

![Logic Analyzer](docs/images/logic_analyzer_2.png)
Logic analyzer view of all ports at 921600 baud, 8N1, 64 byte payload, 1000 pps
