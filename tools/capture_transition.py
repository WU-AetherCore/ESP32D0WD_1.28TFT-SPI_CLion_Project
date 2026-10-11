#!/usr/bin/env python3
"""Capture one mid-transition frame to check that old-page digits are cleared."""

from __future__ import annotations

import time
from pathlib import Path

import serial

from capture_tft import FRAME_BYTES, FRAME_MARKER, read_exact, write_rgb565_png


def main() -> int:
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", nargs="?", default="COM13")
    args = parser.parse_args()
    port = serial.Serial(port=None, baudrate=115200, timeout=0.25)
    port.port = args.port
    port.dtr = False
    port.rts = False
    with port:
        port.reset_input_buffer()
        port.write(b"DEMO,0\n")
        port.flush()
        time.sleep(0.12)
        port.write(b"NEXT\n")
        port.flush()
        time.sleep(0.12)
        port.write(b"CAPTURE\n")
        port.flush()
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            line = port.readline().strip()
            if line == FRAME_MARKER:
                frame = read_exact(port, FRAME_BYTES, timeout=20)
                path = Path(__file__).resolve().parents[1] / "ui_captures" / "transition_mid.png"
                write_rgb565_png(path, frame)
                print(f"Saved mid-transition frame: {path}")
                return 0
    raise TimeoutError("firmware did not return the transition frame")


if __name__ == "__main__":
    raise SystemExit(main())
