#!/usr/bin/env python3
"""Capture all six dashboard pages from the mirrored ESP32 framebuffer."""

from __future__ import annotations

import time
from pathlib import Path

import serial

from capture_tft import FRAME_BYTES, FRAME_MARKER, read_exact, write_rgb565_png


PAGES = ("cpu", "memory", "temperature", "network", "disk", "uptime")
ROOT = Path(__file__).resolve().parents[1]


def capture(port: serial.Serial, index: int, name: str) -> None:
    port.reset_input_buffer()
    port.write(f"DEMO,{index}\n".encode("ascii"))
    port.flush()
    time.sleep(0.8)
    port.write(b"CAPTURE\n")
    port.flush()
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        line = port.readline().strip()
        if line == FRAME_MARKER:
            frame = read_exact(port, FRAME_BYTES, timeout=20)
            output = ROOT / "ui_captures" / f"{index + 1:02d}_{name}.png"
            output.parent.mkdir(parents=True, exist_ok=True)
            write_rgb565_png(output, frame)
            print(f"已保存：{output}")
            return
        if line.startswith(b"FRAME_ERROR:"):
            raise RuntimeError(line.decode("ascii", errors="replace"))
    raise TimeoutError(f"第 {index + 1} 页没有返回截图")


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
        for index, name in enumerate(PAGES):
            capture(port, index, name)
    print("六页截图已生成。截图使用模拟数据，仅用于检查排版和中文字体。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
