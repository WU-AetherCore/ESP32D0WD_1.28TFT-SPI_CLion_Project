#!/usr/bin/env python3
"""Broadcast accurate Windows health samples to the ESP32 over the LAN."""

from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes
import ipaddress
import os
import shutil
import socket
import subprocess
import sys
import time

import psutil

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")


class FILETIME(ctypes.Structure):
    _fields_ = [("low", ctypes.wintypes.DWORD), ("high", ctypes.wintypes.DWORD)]


class MEMORYSTATUSEX(ctypes.Structure):
    _fields_ = [
        ("dwLength", ctypes.wintypes.DWORD), ("dwMemoryLoad", ctypes.wintypes.DWORD),
        ("ullTotalPhys", ctypes.c_ulonglong), ("ullAvailPhys", ctypes.c_ulonglong),
        ("ullTotalPageFile", ctypes.c_ulonglong), ("ullAvailPageFile", ctypes.c_ulonglong),
        ("ullTotalVirtual", ctypes.c_ulonglong), ("ullAvailVirtual", ctypes.c_ulonglong),
        ("ullAvailExtendedVirtual", ctypes.c_ulonglong),
    ]


def _filetime(value: FILETIME) -> int:
    return (value.high << 32) | value.low


def _system_times() -> tuple[int, int]:
    idle, kernel, user = FILETIME(), FILETIME(), FILETIME()
    if not ctypes.windll.kernel32.GetSystemTimes(
        ctypes.byref(idle), ctypes.byref(kernel), ctypes.byref(user)
    ):
        raise ctypes.WinError()
    return _filetime(idle), _filetime(kernel) + _filetime(user)


def _memory() -> tuple[int, int, int]:
    status = MEMORYSTATUSEX()
    status.dwLength = ctypes.sizeof(status)
    if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
        raise ctypes.WinError()
    total = status.ullTotalPhys
    used = total - status.ullAvailPhys
    return round(used * 1000 / total), used // (1024 * 1024), total // (1024 * 1024)


def _cpu_package_temperature() -> float | None:
    """Read only a named CPU package/core sensor from an active monitor provider."""
    command = r"""
$sensor = $null
foreach ($namespace in @('root/LibreHardwareMonitor','root/OpenHardwareMonitor')) {
  try {
    $sensor = Get-CimInstance -Namespace $namespace -ClassName Sensor -ErrorAction Stop |
      Where-Object { $_.SensorType -eq 'Temperature' -and $_.Name -match 'CPU Package|CPU Core|Core #|Tctl/Tdie' } |
      Sort-Object @{Expression={ if ($_.Name -match 'Package|Tctl/Tdie') { 0 } else { 1 } }} |
      Select-Object -First 1
  } catch { $sensor = $null }
  if ($sensor) { break }
}
if ($sensor) { [math]::Round([double]$sensor.Value, 1) }
"""
    try:
        result = subprocess.run(
            ["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", command],
            capture_output=True, text=True, timeout=5,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0), check=False,
        )
        temperature = float(result.stdout.strip())
        return temperature if 0 <= temperature <= 125 else None
    except (OSError, ValueError, subprocess.TimeoutExpired):
        return None


def _lan_interfaces() -> list[tuple[str, str]]:
    """Return (adapter, directed broadcast) pairs for active physical LANs."""
    virtual_markers = (
        "virtual", "vmware", "vbox", "hyper-v", "vEthernet", "tailscale",
        "tap-", "tun", "loopback", "nano", "wsl", "docker", "zerotier",
    )
    results: list[tuple[str, str]] = []
    for name, addresses in psutil.net_if_addrs().items():
        stats = psutil.net_if_stats().get(name)
        if not stats or not stats.isup or any(marker in name.casefold() for marker in virtual_markers):
            continue
        for address in addresses:
            if address.family != socket.AF_INET or not address.address or not address.netmask:
                continue
            try:
                interface = ipaddress.IPv4Interface(f"{address.address}/{address.netmask}")
            except ValueError:
                continue
            if (interface.ip.is_loopback or interface.ip.is_link_local
                    or interface.ip.is_unspecified or interface.network.prefixlen >= 31):
                continue
            pair = (name, str(interface.network.broadcast_address))
            if pair not in results:
                results.append(pair)
    return results


def _disk() -> tuple[int, int]:
    root = os.environ.get("SystemDrive", "C:") + "\\"
    usage = shutil.disk_usage(root)
    return usage.used // (1024 * 1024), usage.total // (1024 * 1024)


def _uptime_seconds() -> int:
    return int(ctypes.windll.kernel32.GetTickCount64() / 1000)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--broadcast", default="auto", help="LAN broadcast address (default: detect each physical LAN)")
    parser.add_argument("--port", type=int, default=4210, help="ESP32 UDP port")
    parser.add_argument("--interval", type=float, default=1.0, help="sample period in seconds")
    parser.add_argument("--once", action="store_true", help="send one packet and exit")
    args = parser.parse_args()
    if args.interval < 0.25:
        parser.error("--interval must be at least 0.25 seconds")

    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    tx.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    interfaces = _lan_interfaces()
    if args.broadcast == "auto":
        targets = interfaces or [("通用广播", "255.255.255.255")]
    else:
        targets = [("手动指定", args.broadcast)]
    iface = interfaces[0][0] if interfaces else None
    counters = psutil.net_io_counters(pernic=True)
    previous_idle, previous_total = _system_times()
    previous_net = counters.get(iface) if iface else None
    previous_at = time.monotonic()
    last_temp_at = 0.0
    temperature: float | None = None
    sequence = 0
    print(f"正在通过 {', '.join(name for name, _ in targets)} 发送局域网数据")
    print(f"广播地址：{', '.join(address for _, address in targets)}，UDP 端口：{args.port}")
    print("ESP32 和电脑必须加入同一个局域网。停止发送请按 Ctrl+C。")
    try:
        while True:
            tick = time.monotonic()
            time.sleep(0.12)
            idle, total = _system_times()
            dt = max(1, total - previous_total)
            cpu10 = round(max(0, min(1000, 1000 * (dt - (idle - previous_idle)) / dt)))
            previous_idle, previous_total = idle, total

            ram10, ram_used, ram_total = _memory()
            disk_used, disk_total = _disk()
            uptime = _uptime_seconds()
            counters = psutil.net_io_counters(pernic=True)
            current_net = counters.get(iface) if iface else None
            elapsed = max(0.05, time.monotonic() - previous_at)
            if current_net and previous_net:
                down_bps = max(0, int((current_net.bytes_recv - previous_net.bytes_recv) / elapsed))
                up_bps = max(0, int((current_net.bytes_sent - previous_net.bytes_sent) / elapsed))
            else:
                down_bps = up_bps = 0
            previous_net = current_net
            previous_at = time.monotonic()

            now = time.monotonic()
            if now - last_temp_at >= 15:
                temperature = _cpu_package_temperature()
                last_temp_at = time.monotonic()
            temp10 = -9999 if temperature is None else round(temperature * 10)
            sequence += 1
            packet = (
                f"PC2,{sequence},{cpu10},{ram10},{ram_used},{ram_total},"
                f"{down_bps},{up_bps},{disk_used},{disk_total},{uptime},{temp10}"
            ).encode("ascii")
            for _, broadcast in targets:
                tx.sendto(packet, (broadcast, args.port))
            temp_text = "不可用" if temperature is None else f"{temperature:.1f}°C"
            print(
                f"CPU {cpu10 / 10:.1f}% | 内存 {ram10 / 10:.1f}% "
                f"({ram_used}/{ram_total} MB) | 下行 {down_bps / 1048576:.2f} MB/s "
                f"| 系统盘 {disk_used}/{disk_total} MB | CPU 温度 {temp_text}",
                flush=True,
            )
            if args.once:
                break
            time.sleep(max(0, args.interval - (time.monotonic() - tick)))
    except KeyboardInterrupt:
        print("\n已停止局域网数据发送。")
    except OSError as exc:
        print(f"网络发送失败：{exc}")
        return 2
    finally:
        tx.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
