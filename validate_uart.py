#!/usr/bin/env python3
"""Validate UART loopback on serial devices."""

from __future__ import annotations

import argparse
import secrets
import sys
import time
from dataclasses import dataclass
from typing import Sequence


def load_pyserial():
    try:
        import serial  # type: ignore
        import serial.tools.list_ports  # type: ignore
    except ImportError:
        print("pyserial is required. Install with: uv pip install pyserial", flush=True)
        return None, None
    return serial, serial.tools.list_ports


@dataclass(frozen=True)
class SerialConfig:
    baudrate: int
    data_bits: int
    parity: str
    stop_bits: int

    @property
    def label(self) -> str:
        return f"{self.baudrate}-{self.data_bits}{self.parity}{self.stop_bits}"


def build_sweep_configs() -> list[SerialConfig]:
    baudrates = [5400, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600]
    data_bits = [7, 8]
    parity_values = ["N", "E", "O"]
    stop_bits = [1, 2]

    configs: list[SerialConfig] = []
    for baudrate in baudrates:
        for data_bit in data_bits:
            for parity in parity_values:
                for stop_bit in stop_bits:
                    if data_bit == 7 and stop_bit == 2:
                        continue
                    configs.append(
                        SerialConfig(
                            baudrate=baudrate,
                            data_bits=data_bit,
                            parity=parity,
                            stop_bits=stop_bit,
                        )
                    )
    return configs


def parse_ports_arg(ports_arg: str | None) -> list[str]:
    if not ports_arg:
        return []
    return [port.strip() for port in ports_arg.split(",") if port.strip()]


def list_serial_devices() -> list[str]:
    _serial, list_ports = load_pyserial()
    if list_ports is None:
        return []
    return sorted(port.device for port in list_ports.comports())


def resolve_ports(ports_arg: str | None) -> list[str]:
    explicit_ports = parse_ports_arg(ports_arg)
    if explicit_ports:
        return explicit_ports
    return list_serial_devices()


def read_exact(serial_handle, size: int, timeout: float) -> bytes:
    deadline = time.monotonic() + timeout
    data = bytearray()

    while len(data) < size and time.monotonic() < deadline:
        chunk = serial_handle.read(size - len(data))
        if chunk:
            data.extend(chunk)

    return bytes(data)


def build_payload(port_name: str, round_index: int, payload_size: int) -> bytes:
    header = f"{port_name}-r{round_index:02d}:".encode("ascii", errors="ignore")
    minimum_size = len(header) + 1
    if payload_size < minimum_size:
        payload_size = minimum_size

    body_size = payload_size - len(header) - 1
    body = secrets.token_hex((body_size + 1) // 2)[:body_size].encode("ascii")
    return header + body + b"\n"


def configure_serial(serial_mod, serial_handle, config: SerialConfig) -> None:
    bytesize_map = {
        7: serial_mod.SEVENBITS,
        8: serial_mod.EIGHTBITS,
    }
    parity_map = {
        "N": serial_mod.PARITY_NONE,
        "E": serial_mod.PARITY_EVEN,
        "O": serial_mod.PARITY_ODD,
    }
    stopbits_map = {
        1: serial_mod.STOPBITS_ONE,
        2: serial_mod.STOPBITS_TWO,
    }
    serial_handle.baudrate = config.baudrate
    serial_handle.bytesize = bytesize_map[config.data_bits]
    serial_handle.parity = parity_map[config.parity]
    serial_handle.stopbits = stopbits_map[config.stop_bits]


def validate_port_loopback(
    serial_mod,
    port_name: str,
    configs: list[SerialConfig],
    rounds: int,
    payload_size: int,
    timeout: float,
    settle_time: float,
    quiet: bool,
) -> bool:
    serial_handle = None

    try:
        serial_handle = serial_mod.Serial(
            port_name,
            baudrate=115200,
            timeout=timeout,
            write_timeout=timeout,
        )
        time.sleep(settle_time)

        for config in configs:
            configure_serial(serial_mod, serial_handle, config)
            for round_index in range(rounds):
                payload = build_payload(port_name, round_index, payload_size)
                serial_handle.reset_input_buffer()
                serial_handle.reset_output_buffer()
                serial_handle.write(payload)
                serial_handle.flush()
                echoed = read_exact(serial_handle, len(payload), timeout)

                if echoed != payload:
                    print(
                        f"FAIL {port_name} [{config.label}] round {round_index + 1}/{rounds}: "
                        f"expected {len(payload)} bytes, got {len(echoed)}",
                        flush=True,
                    )
                    return False

                if not quiet:
                    print(
                        f"PASS {port_name} [{config.label}] round {round_index + 1}/{rounds} "
                        f"({len(payload)} bytes)",
                        flush=True,
                    )

        return True
    except Exception as exc:
        print(f"FAIL {port_name}: {exc}", flush=True)
        return False
    finally:
        if serial_handle is not None:
            try:
                serial_handle.close()
            except Exception:
                pass


def validate_uart(args) -> int:
    serial_mod, _ = load_pyserial()
    if serial_mod is None:
        return 2

    ports = resolve_ports(args.ports)
    if not ports:
        print("No serial devices found.", flush=True)
        return 2

    configs = build_sweep_configs()
    print(f"Validating {len(ports)} serial device(s): {', '.join(ports)}", flush=True)
    print(f"Running config sweep across {len(configs)} serial settings.", flush=True)
    all_passed = True

    for port_name in ports:
        port_passed = validate_port_loopback(
            serial_mod=serial_mod,
            port_name=port_name,
            configs=configs,
            rounds=args.rounds,
            payload_size=args.payload_size,
            timeout=args.timeout,
            settle_time=args.settle_time,
            quiet=args.quiet,
        )
        all_passed = all_passed and port_passed

    if all_passed:
        print("UART validation PASSED.", flush=True)
        return 0

    print("UART validation FAILED.", flush=True)
    return 1


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Validate UART loopback on all serial devices (or --ports override)."
    )
    parser.add_argument(
        "--ports",
        default=None,
        help="Optional comma-separated serial ports to validate.",
    )
    parser.add_argument("--rounds", type=int, default=3, help="Loopback rounds per port.")
    parser.add_argument(
        "--payload-size",
        type=int,
        default=64,
        help="Payload bytes written per round.",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=1.0,
        help="Read and write timeout in seconds.",
    )
    parser.add_argument(
        "--settle-time",
        type=float,
        default=0.05,
        help="Delay after opening each serial port.",
    )
    parser.add_argument("--quiet", action="store_true", help="Only print failures and summary.")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_arg_parser()
    args = parser.parse_args(argv)
    return validate_uart(args)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
