#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
exp2_selftest.py —— 实验二串口功能自检
======================================
依次验证：
  1) RTC 秒中断上传（每秒一行时间）
  2) 串口时间帧解析 + RTC 更新（合法帧）
  3) 非法帧被拒绝
  4) 帧外杂散字符被忽略

用法：
  python exp2_selftest.py            # 自动选非 ST-Link 的串口
  python exp2_selftest.py -p COM12
注意：运行前请先关闭串口监测 GUI（COM 口独占）。
"""

import argparse
import os
import re
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    for p in (r"D:\Lib\site-packages",):
        if os.path.isdir(p) and p not in sys.path:
            sys.path.append(p)
    try:
        import serial
        from serial.tools import list_ports
    except ImportError:
        sys.exit("缺少 pyserial：pip install pyserial")

TIME_RE = re.compile(r"20\d{2}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}")
results = []


def pick_port(want):
    ports = list(list_ports.comports())
    if want:
        return want
    cand = [p for p in ports if "ST-Link" not in p.description and "STLink" not in p.description]
    if not cand:
        sys.exit("没找到 USB 转 TTL 串口（只有 ST-Link 虚拟口，它没接 PA2/PA3）")
    return cand[0].device


def read_for(ser, seconds):
    buf = b""
    end = time.time() + seconds
    while time.time() < end:
        try:
            chunk = ser.read(256)
        except serial.SerialException:
            break
        if chunk:
            buf += chunk
        else:
            time.sleep(0.02)
    return buf.decode("ascii", "replace")


def check(name, ok, detail):
    results.append((name, ok, detail))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}  {detail}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port")
    args = ap.parse_args()
    port = pick_port(args.port)
    print(f"串口 {port} @ 9600 8N1\n")

    try:
        ser = serial.Serial(port, 9600, bytesize=8, parity="N", stopbits=1, timeout=0.1)
    except serial.SerialException as e:
        sys.exit(f"打不开 {port}：{e}\n（串口监测 GUI 是否还占着端口？先关掉它）")

    ser.reset_input_buffer()

    # ---------- 1. RTC 秒中断上传 ----------
    txt = read_for(ser, 4.0)
    times = TIME_RE.findall(txt)
    check("1) RTC 每秒上传", len(times) >= 2,
          f"4 秒内收到 {len(times)} 行时间，最后一行 {times[-1] if times else '-'}")

    # ---------- 2. 合法时间帧 ----------
    frame = "$0,1,251107,151530*"
    print(f"\n发送帧: {frame}")
    ser.write(frame.encode())
    txt = read_for(ser, 3.0)
    got_rx = f"[RX] {frame}" in txt
    got_upd = "[RTC] updated -> 2025-11-07 15:15:30" in txt
    new_time = [t for t in TIME_RE.findall(txt) if t.startswith("2025-11-07 15:15:3")]
    check("2a) 帧被接收并回显", got_rx, "收到 [RX] 回显" if got_rx else "未见 [RX] 回显")
    check("2b) RTC 被改写", got_upd, "收到 [RTC] updated 提示" if got_upd else "未见更新提示")
    check("2c) 后续秒上传为新时间", bool(new_time),
          f"新时间行 {new_time[:2]}" if new_time else "未见 15:15:3x 的时间行")

    # ---------- 3. 非法帧 ----------
    bad = "$0,1,251107,999999*"
    print(f"\n发送非法帧: {bad}")
    ser.write(bad.encode())
    txt = read_for(ser, 2.0)
    check("3) 非法帧被拒绝", "[RX] ignored" in txt,
          "收到 ignored 提示" if "[RX] ignored" in txt else "未被拒绝")

    # ---------- 4. 帧外杂散字符 ----------
    print("\n发送帧外字符: hello")
    ser.write(b"hello")
    txt = read_for(ser, 1.5)
    check("4) 帧外字符被忽略", "[RX]" not in txt, "未触发帧解析" if "[RX]" not in txt else "误触发")

    ser.close()

    print("\n===== 汇总 =====")
    ok = sum(1 for _, o, _ in results if o)
    for n, o, d in results:
        print(f"  {'✓' if o else '✗'} {n}")
    print(f"{ok}/{len(results)} 项通过")
    return 0 if ok == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
