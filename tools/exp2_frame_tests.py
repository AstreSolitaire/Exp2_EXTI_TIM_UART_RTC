#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
exp2_frame_tests.py —— 串口帧格式测试用例
=========================================
逐条发送设计好的数据帧，检查回执，输出 PASS/FAIL。
运行前请关闭串口监测 GUI（串口独占）。

用法：
    python exp2_frame_tests.py            # 自动选口
    python exp2_frame_tests.py -p COM12
"""

import argparse
import os
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    p = r"D:\Lib\site-packages"
    if os.path.isdir(p):
        sys.path.append(p)
    import serial
    from serial.tools import list_ports

# (用例名, 发送内容, 期望: "ok"=RTC更新 / "ignored"=被拒 / "silent"=无任何回执)
CASES = [
    ("① 基本合法：2025-11-07 14:01:01",   "$0,1,251107,140101*",  "ok"),
    ("② 年末边界：2025-12-31 23:59:59",   "$0,1,251231,235959*",  "ok"),
    ("③ 年初零点：2025-01-01 00:00:00",   "$0,1,250101,000000*",  "ok"),
    ("④ 闰日：2024-02-29 12:00:00",       "$0,1,240229,120000*",  "ok"),
    ("⑤ 空格作分隔符",                    "$0 1 251113 093000*",  "ok"),
    ("⑥ 地址非 0（地址不校验）",          "$7,1,251113,093001*",  "ok"),
    ("⑦ 帧前带杂字符",                    "abc$0,1,251107,140105*", "ok"),
    ("⑧ 非法：秒=99",                     "$0,1,251107,999999*",  "ignored"),
    ("⑨ 非法：月=13",                     "$0,1,251331,120000*",  "ignored"),
    ("⑩ 非法：日=32",                     "$0,1,251132,120000*",  "ignored"),
    ("⑪ 非法：时=24",                     "$0,1,251107,246000*",  "ignored"),
    ("⑫ 非法：命令字=2",                  "$0,2,251107,120000*",  "ignored"),
    ("⑬ 缺帧头 $",                        "0,1,251107,140101*",   "silent"),
    ("⑭ 缺帧尾 *",                        "$0,1,251107,140101",   "silent"),
    ("⑮ 帧外纯文本 hello",                "hello",                "silent"),
    ("⑯ 已知限制：2025-02-30（只查范围）", "$0,1,250230,120000*",  "ok"),
]


def pick_port(want):
    if want:
        return want
    cand = [p for p in list_ports.comports()
            if "ST-Link" not in p.description and "STLink" not in p.description]
    if not cand:
        sys.exit("未找到 USB 转 TTL 串口")
    return cand[0].device


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port")
    args = ap.parse_args()
    ser = serial.Serial(pick_port(args.port), 9600, timeout=0.1)
    ser.reset_input_buffer()
    print(f"串口 {ser.port} @ 9600 8N1\n")

    ok_count = 0
    for name, frame, expect in CASES:
        ser.reset_input_buffer()
        ser.write(frame.encode("ascii", "replace"))
        time.sleep(0.9)
        buf = b""
        end = time.time() + 0.9
        while time.time() < end:
            buf += ser.read(256)
        text = buf.decode("ascii", "replace")

        got_upd = "[RTC] updated" in text
        got_ign = "[RX] ignored" in text
        if expect == "ok":
            good = got_upd
        elif expect == "ignored":
            good = got_ign and not got_upd
        else:
            good = not got_upd and not got_ign

        detail = "updated" if got_upd else ("ignored" if got_ign else "无回执")
        print(f"[{'PASS' if good else 'FAIL'}] {name}")
        print(f"        发送: {frame!r}   回执: {detail}")
        ok_count += 1 if good else 0

    ser.close()
    print(f"\n{ok_count}/{len(CASES)} 用例通过")
    return 0 if ok_count == len(CASES) else 1


if __name__ == "__main__":
    sys.exit(main())
