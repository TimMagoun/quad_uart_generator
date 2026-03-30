#!/usr/bin/env python3
"""Hardware validation for UART self-loopback on the RP2040 adapter."""

from __future__ import annotations

import argparse
import re
import secrets
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


@dataclass
class PortInfo:
    device: str
    vid: int | None = None
    pid: int | None = None
    serial_number: str | None = None
    description: str = ""


@dataclass(frozen=True)
class SerialConfig:
    baudrate: int
    data_bits: int
    parity: str  # N/E/O
    stop_bits: int

    @property
    def label(self) -> str:
        return f"{self.baudrate}-{self.data_bits}{self.parity}{self.stop_bits}"


class Logger:
    def __init__(self, verbose: bool, stream=None):
        self.verbose = verbose
        self.stream = stream if stream is not None else sys.stdout

    def info(self, message: str) -> None:
        if self.verbose:
            print(message, file=self.stream, flush=True)

    def always(self, message: str) -> None:
        print(message, file=self.stream, flush=True)


def parse_ports_arg(ports_arg: str | None) -> list[str]:
    if not ports_arg:
        return []
    return [p.strip() for p in ports_arg.split(",") if p.strip()]


def parse_serial_configs(configs_arg: str) -> list[SerialConfig]:
    configs: list[SerialConfig] = []
    pattern = re.compile(r"^(\d+)-([5678])([NEO])([12])$", re.IGNORECASE)

    for token in [t.strip().upper() for t in configs_arg.split(",") if t.strip()]:
        match = pattern.match(token)
        if match is None:
            raise ValueError(
                f"Invalid config '{token}'. Use BAUD-DPS format such as 115200-8N1 or 57600-7E1."
            )

        baudrate = int(match.group(1))
        data_bits = int(match.group(2))
        parity = match.group(3)
        stop_bits = int(match.group(4))
        configs.append(SerialConfig(baudrate, data_bits, parity, stop_bits))

    if not configs:
        raise ValueError("At least one serial config must be provided.")

    return configs


def parse_line_endings(line_endings_arg: str) -> list[tuple[str, bytes]]:
    mapping = {
        "lf": b"\n",
        "cr": b"\r",
        "crlf": b"\r\n",
    }

    endings: list[tuple[str, bytes]] = []
    for token in [t.strip().lower() for t in line_endings_arg.split(",") if t.strip()]:
        if token not in mapping:
            raise ValueError(
                f"Invalid line ending '{token}'. Use comma-separated values from: lf, cr, crlf."
            )
        endings.append((token, mapping[token]))

    if not endings:
        raise ValueError("At least one line ending must be provided.")

    return endings


def run_command(command: list[str], cwd: Path, logger: Logger) -> int:
    if command and command[0] == "pio" and shutil.which("pio") is None:
        local_pio = cwd / ".venv" / "bin" / "pio"
        if local_pio.exists():
            command = [str(local_pio), *command[1:]]

    logger.info(f"Running command: {' '.join(command)}")
    try:
        completed = subprocess.run(command, cwd=str(cwd), check=False)
    except FileNotFoundError as exc:
        logger.always(f"Unable to run command: {exc}")
        return 127
    return int(completed.returncode)


def _load_pyserial(logger: Logger):
    try:
        import serial  # type: ignore
        import serial.tools.list_ports  # type: ignore
    except ImportError:
        logger.always(
            "pyserial is required. Install in the project venv: uv pip install pyserial"
        )
        return None, None
    return serial, serial.tools.list_ports


def discover_ports(
    logger: Logger, vid: int | None = None, pid: int | None = None
) -> list[PortInfo]:
    serial_mod, list_ports_mod = _load_pyserial(logger)
    if serial_mod is None:
        return []

    all_ports = []
    for p in list_ports_mod.comports():
        all_ports.append(
            PortInfo(
                device=p.device,
                vid=getattr(p, "vid", None),
                pid=getattr(p, "pid", None),
                serial_number=getattr(p, "serial_number", None),
                description=getattr(p, "description", ""),
            )
        )

    logger.info(f"Discovered {len(all_ports)} serial device(s) before filtering.")

    # Ignore platform ttyS* ports that are not USB serial adapters.
    all_ports = [
        p
        for p in all_ports
        if p.vid is not None or "/ttyACM" in p.device or "/ttyUSB" in p.device
    ]
    logger.info(f"USB-like serial candidates after baseline filter: {len(all_ports)}")
    if vid is not None:
        all_ports = [p for p in all_ports if p.vid == vid]
        logger.info(f"Filtered by VID 0x{vid:04x}: {len(all_ports)} device(s).")
    if pid is not None:
        all_ports = [p for p in all_ports if p.pid == pid]
        logger.info(f"Filtered by PID 0x{pid:04x}: {len(all_ports)} device(s).")

    all_ports.sort(key=lambda p: p.device)
    for p in all_ports:
        logger.info(
            f"  candidate: {p.device} vid={_hex_or_none(p.vid)} pid={_hex_or_none(p.pid)} "
            f"serial={p.serial_number or '-'} desc={p.description or '-'}"
        )
    return all_ports


def _hex_or_none(value: int | None) -> str:
    return f"0x{value:04x}" if value is not None else "None"


def select_test_ports(candidates: list[PortInfo], expected_ports: int) -> list[PortInfo]:
    if len(candidates) != expected_ports:
        raise ValueError(
            f"Expected exactly {expected_ports} candidate ports, found {len(candidates)}."
        )
    return candidates


def _configure_serial_port(ser, serial_mod, cfg: SerialConfig) -> None:
    bytesize_map = {
        5: serial_mod.FIVEBITS,
        6: serial_mod.SIXBITS,
        7: serial_mod.SEVENBITS,
        8: serial_mod.EIGHTBITS,
    }
    parity_map = {
        "N": serial_mod.PARITY_NONE,
        "E": serial_mod.PARITY_EVEN,
        "O": serial_mod.PARITY_ODD,
    }
    stop_map = {
        1: serial_mod.STOPBITS_ONE,
        2: serial_mod.STOPBITS_TWO,
    }

    ser.baudrate = cfg.baudrate
    ser.bytesize = bytesize_map[cfg.data_bits]
    ser.parity = parity_map[cfg.parity]
    ser.stopbits = stop_map[cfg.stop_bits]


def open_serial_ports(
    ports: list[PortInfo],
    baudrate: int,
    timeout_seconds: float,
    logger: Logger,
):
    serial_mod, _ = _load_pyserial(logger)
    if serial_mod is None:
        return None, None

    handles = []
    try:
        for p in ports:
            logger.info(f"Opening {p.device} at initial {baudrate} baud")
            s = serial_mod.Serial(
                p.device,
                baudrate=baudrate,
                timeout=timeout_seconds,
                write_timeout=timeout_seconds,
            )
            time.sleep(0.05)
            s.reset_input_buffer()
            s.reset_output_buffer()
            handles.append(s)
        return handles, serial_mod
    except Exception as exc:
        logger.always(f"Failed opening serial ports: {exc}")
        for h in handles:
            h.close()
        return None, None


def _read_exact(serial_handle, expected_len: int, timeout_seconds: float) -> bytes:
    deadline = time.monotonic() + timeout_seconds
    data = bytearray()
    while len(data) < expected_len and time.monotonic() < deadline:
        try:
            chunk = serial_handle.read(expected_len - len(data))
        except Exception:
            break
        if chunk:
            data.extend(chunk)
    return bytes(data)


def _build_payload(
    port_idx: int,
    round_idx: int,
    payload_size: int,
    line_ending: bytes,
) -> bytes:
    header = f"P{port_idx:02d}R{round_idx:02d}-".encode("ascii")

    min_size = len(header) + len(line_ending)
    if payload_size < min_size:
        payload_size = min_size

    body_len = payload_size - len(header) - len(line_ending)
    body = secrets.token_hex((body_len + 1) // 2)[:body_len].encode("ascii")

    return header + body + line_ending


def run_loopback_test(
    serial_mod,
    serials: list,
    ports: list[PortInfo],
    rounds: int,
    payload_size: int,
    timeout_seconds: float,
    logger: Logger,
    configs: list[SerialConfig],
    line_endings: list[tuple[str, bytes]],
    attempts: int = 3,
    inter_attempt_delay: float = 0.03,
) -> bool:
    all_ok = True

    for cfg in configs:
        logger.always(f"Testing serial config: {cfg.label}")
        for ending_name, ending_bytes in line_endings:
            logger.always(f"  Line ending: {ending_name}")
            for round_idx in range(rounds):
                logger.info(f"  Round {round_idx + 1}/{rounds}")
                for idx, (port, ser) in enumerate(zip(ports, serials)):
                    _configure_serial_port(ser, serial_mod, cfg)
                    payload = _build_payload(idx, round_idx, payload_size, ending_bytes)
                    logger.info(
                        f"    [{port.device}] TX {len(payload)} bytes @ {cfg.label} ending={ending_name}"
                    )

                    pass_on_attempt = False
                    last_received = b""
                    for attempt in range(1, max(1, attempts) + 1):
                        logger.info(
                            f"    [{port.device}] attempt {attempt}/{max(1, attempts)}"
                        )
                        try:
                            ser.reset_input_buffer()
                            ser.reset_output_buffer()
                            ser.write(payload)
                            ser.flush()
                        except Exception as exc:
                            logger.info(f"    [{port.device}] write/read setup failed: {exc}")
                            last_received = b""
                            continue
                        time.sleep(inter_attempt_delay)

                        received = _read_exact(ser, len(payload), timeout_seconds)
                        if received == payload:
                            pass_on_attempt = True
                            logger.info(
                                f"    [{port.device}] PASS ({len(received)} bytes echoed)"
                            )
                            break
                        last_received = received

                    if not pass_on_attempt:
                        all_ok = False
                        logger.always(
                            f"    [{port.device}] FAIL cfg={cfg.label} ending={ending_name} "
                            f"expected {len(payload)} bytes, received {len(last_received)}"
                        )
    return all_ok


def close_serial_ports(serials: Iterable) -> None:
    for ser in serials:
        try:
            ser.close()
        except Exception:
            pass


def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="Validate UART self-loopback on all bridge ports.")
    p.add_argument("--project-dir", default=".", help="Project directory (default: .)")
    p.add_argument("--flash", action="store_true", help="Flash firmware before loopback test.")
    p.add_argument(
        "--ports",
        default=None,
        help="Comma-separated explicit serial ports (overrides auto-discovery).",
    )
    p.add_argument("--vid", type=lambda x: int(x, 0), default=None, help="USB VID filter.")
    p.add_argument("--pid", type=lambda x: int(x, 0), default=None, help="USB PID filter.")
    p.add_argument("--baudrate", type=int, default=115200, help="Initial open baudrate.")
    p.add_argument("--rounds", type=int, default=2)
    p.add_argument("--payload-size", type=int, default=28)
    p.add_argument("--timeout", type=float, default=1.5, help="Per-port read timeout seconds.")
    p.add_argument("--attempts", type=int, default=3, help="Retries per port per round.")
    p.add_argument(
        "--inter-attempt-delay",
        type=float,
        default=0.03,
        help="Delay between write and read (seconds).",
    )
    p.add_argument("--expected-ports", type=int, default=6)
    p.add_argument(
        "--configs",
        default="9600-8N1,57600-7E1,115200-8O1,230400-8N2",
        help="Comma-separated serial configs in BAUD-DPS format.",
    )
    p.add_argument(
        "--line-endings",
        default="lf,cr,crlf",
        help="Comma-separated line endings to test: lf,cr,crlf.",
    )
    p.add_argument("--quiet", action="store_true", help="Reduce log verbosity.")
    return p


def validate_uart(args) -> int:
    logger = Logger(verbose=not args.quiet)
    project_dir = Path(args.project_dir).resolve()

    logger.always("Starting UART self-loopback validation")
    logger.info(f"Project dir: {project_dir}")
    logger.info("Expected pin pairs: 0-1, 2-3, 5-6, 8-9, 21-23, 26-27")

    try:
        configs = parse_serial_configs(args.configs)
        line_endings = parse_line_endings(args.line_endings)
    except ValueError as exc:
        logger.always(str(exc))
        return 2

    if args.flash:
        logger.always("Flashing firmware before test...")
        rc = run_command(["pio", "run", "-t", "upload"], cwd=project_dir, logger=logger)
        if rc != 0:
            logger.always(f"Flash failed with code {rc}")
            return rc

    explicit_ports = parse_ports_arg(args.ports)
    if explicit_ports:
        candidates = [PortInfo(device=p) for p in explicit_ports]
        logger.always(f"Using explicit ports ({len(candidates)}): {', '.join(explicit_ports)}")
    else:
        logger.always("Auto-discovering ports by USB metadata...")
        candidates = discover_ports(logger, vid=args.vid, pid=args.pid)
        if not candidates:
            return 2

    try:
        test_ports = select_test_ports(candidates, expected_ports=args.expected_ports)
    except ValueError as exc:
        logger.always(str(exc))
        return 2

    logger.always(f"Selected {len(test_ports)} test port(s):")
    for i, p in enumerate(test_ports):
        logger.always(f"  {i}: {p.device}")

    serials, serial_mod = open_serial_ports(
        ports=test_ports,
        baudrate=args.baudrate,
        timeout_seconds=args.timeout,
        logger=logger,
    )
    if serials is None or serial_mod is None:
        return 2

    try:
        logger.always("Running loopback tests across serial settings...")
        ok = run_loopback_test(
            serial_mod=serial_mod,
            serials=serials,
            ports=test_ports,
            rounds=args.rounds,
            payload_size=args.payload_size,
            timeout_seconds=args.timeout,
            logger=logger,
            configs=configs,
            line_endings=line_endings,
            attempts=args.attempts,
            inter_attempt_delay=args.inter_attempt_delay,
        )
    finally:
        close_serial_ports(serials)

    if ok:
        logger.always("UART validation PASSED for all ports/configurations.")
        return 0
    logger.always("UART validation FAILED.")
    return 1


def main(argv: list[str] | None = None) -> int:
    parser = build_arg_parser()
    args = parser.parse_args(argv)
    return validate_uart(args)


if __name__ == "__main__":
    raise SystemExit(main())
