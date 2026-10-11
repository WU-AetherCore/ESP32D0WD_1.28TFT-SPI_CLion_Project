#!/usr/bin/env python3
"""Capture the firmware's mirrored RGB565 framebuffer over the CH343 UART."""

from __future__ import annotations

import argparse
import struct
import time
import zlib
from pathlib import Path

import serial


WIDTH = 240
HEIGHT = 240
FRAME_BYTES = WIDTH * HEIGHT * 2
FRAME_MARKER = b"FRAME_BEGIN_RGB565LE_240x240"


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    body = kind + payload
    return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def write_rgb565_png(path: Path, frame: bytes) -> None:
    if len(frame) != FRAME_BYTES:
        raise ValueError(f"expected {FRAME_BYTES} frame bytes, received {len(frame)}")

    rows = bytearray()
    for y in range(HEIGHT):
        rows.append(0)  # PNG filter: None
        start = y * WIDTH * 2
        for x in range(WIDTH):
            offset = start + x * 2
            pixel = frame[offset] | (frame[offset + 1] << 8)
            red = (pixel >> 11) & 0x1F
            green = (pixel >> 5) & 0x3F
            blue = pixel & 0x1F
            rows.extend(((red * 255 + 15) // 31,
                         (green * 255 + 31) // 63,
                         (blue * 255 + 15) // 31))

    image = b"\x89PNG\r\n\x1a\n"
    image += png_chunk(b"IHDR", struct.pack(">IIBBBBB", WIDTH, HEIGHT, 8, 2, 0, 0, 0))
    image += png_chunk(b"IDAT", zlib.compress(bytes(rows), level=6))
    image += png_chunk(b"IEND", b"")
    path.write_bytes(image)


def read_exact(port: serial.Serial, count: int, timeout: float) -> bytes:
    deadline = time.monotonic() + timeout
    data = bytearray()
    while len(data) < count and time.monotonic() < deadline:
        chunk = port.read(count - len(data))
        if chunk:
            data.extend(chunk)
    if len(data) != count:
        raise TimeoutError(f"frame ended early: {len(data)} of {count} bytes received")
    return bytes(data)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="board serial port, for example COM13")
    parser.add_argument(
        "output",
        nargs="?",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "tft_capture.png",
        help="PNG output path (default: project folder/tft_capture.png)",
    )
    args = parser.parse_args()

    port = serial.Serial(port=None, baudrate=115200, timeout=0.25)
    port.port = args.port
    # Avoid asserting the CH343 auto-download lines just by opening the port.
    port.dtr = False
    port.rts = False
    try:
        port.open()
        port.reset_input_buffer()
        port.write(b"CAPTURE\n")
        port.flush()

        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            line = port.readline().strip()
            if line == FRAME_MARKER:
                frame = read_exact(port, FRAME_BYTES, timeout=20)
                break
            if line.startswith(b"FRAME_ERROR:"):
                raise RuntimeError(line.decode("ascii", errors="replace"))
        else:
            raise TimeoutError("firmware did not return a frame; check the COM port and firmware")
    finally:
        port.close()

    args.output.parent.mkdir(parents=True, exist_ok=True)
    write_rgb565_png(args.output, frame)
    print(f"Saved {WIDTH}x{HEIGHT} firmware framebuffer to: {args.output.resolve()}")
    print("This is the image sent by firmware; the SPI panel itself has no wired readback path.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
