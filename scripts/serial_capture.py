"""Capture ESP32-S3 USB-Serial/JTAG logs across USB re-enumeration."""

from __future__ import annotations

import argparse
import sys
import time

import serial
from serial import SerialException


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("port")
    parser.add_argument("--seconds", type=float, default=15.0)
    parser.add_argument("--no-reset", action="store_true")
    args = parser.parse_args()

    deadline = time.monotonic() + args.seconds
    connection: serial.Serial | None = None
    did_reset = args.no_reset

    while time.monotonic() < deadline:
        if connection is None:
            try:
                connection = serial.Serial(args.port, 115200, timeout=0.1)
                if not did_reset:
                    connection.dtr = False
                    connection.rts = True
                    time.sleep(0.1)
                    connection.rts = False
                    did_reset = True
            except (SerialException, OSError):
                time.sleep(0.2)
                continue
        try:
            data = connection.read(connection.in_waiting or 1)
            if data:
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()
        except (SerialException, OSError):
            try:
                connection.close()
            except (SerialException, OSError):
                pass
            connection = None
            time.sleep(0.2)

    if connection is not None:
        connection.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
