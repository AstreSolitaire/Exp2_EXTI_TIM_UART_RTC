#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
serial_monitor_gui.py —— Exp1 Serial Monitor GUI
=================================================
Tkinter-based serial monitor for STM32F407G-DISC1 (USART2).

Features
  * COM port / baud / data bits / parity / stop bits / flow control,
    all selectable, matching the experiment config by default
    (9600, 8 data bits, no parity, 1 stop bit, no flow control)
  * Real-time colored RX display (KEY lines yellow, RTC lines cyan,
    error lines red), with per-line local timestamps
  * TX box + Send, Enter sends; raw bytes echoed by the board show up
    immediately, so the UART loopback test is one click away
  * Statistics: RX bytes / TX bytes / lines; live status bar
  * Auto log to logs\gui_YYYYMMDD_HHMMSS.log

Usage:
    python serial_monitor_gui.py            # auto-pick a port, GUI up
    python serial_monitor_gui.py -p COM11   # preselect port
"""

import argparse
import os
import queue
import sys
import threading
import time
from datetime import datetime

# --- pyserial import with portable-python fallback -------------------------
try:
    import serial
    from serial.tools import list_ports
except ImportError:
    _sp = r"D:\Lib\site-packages"
    if os.path.isdir(_sp) and _sp not in sys.path:
        sys.path.append(_sp)
    try:
        import serial
        from serial.tools import list_ports
    except ImportError:
        print("pyserial missing: pip install pyserial")
        sys.exit(1)

import tkinter as tk
from tkinter import ttk, messagebox, scrolledtext

BAUDS = [1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600]
PARITIES = [("None (N)", serial.PARITY_NONE), ("Even (E)", serial.PARITY_EVEN),
            ("Odd (O)", serial.PARITY_ODD), ("Mark (M)", serial.PARITY_MARK),
            ("Space (S)", serial.PARITY_SPACE)]
DATABITS = [("8", serial.EIGHTBITS), ("7", serial.SEVENBITS),
            ("6", serial.SIXBITS), ("5", serial.FIVEBITS)]
STOPBITS = [("1", serial.STOPBITS_ONE), ("1.5", serial.STOPBITS_ONE_POINT_FIVE),
            ("2", serial.STOPBITS_TWO)]
FLOW = [("None", None), ("XON/XOFF", "xon"), ("RTS/CTS", "rtscts"), ("DSR/DTR", "dsrdtr")]

APP_TITLE = "Exp1 Serial Monitor - STM32F407G-DISC1 (USART2 PA2/PA3)"


class SerialMonitorGUI(tk.Tk):
    def __init__(self, pre_port=None):
        super().__init__()
        self.title(APP_TITLE)
        self.geometry("1100x680")
        self.minsize(860, 500)

        self.ser = None
        self.rx_thread = None
        self.rx_queue = queue.Queue()
        self.running = False
        self.rx_bytes = 0
        self.tx_bytes = 0
        self.lines = 0
        self.logf = None
        self.pre_port = pre_port

        self._build_ui()
        self.refresh_ports()
        self.after(50, self._poll_queue)
        self.protocol("WM_DELETE_WINDOW", self._on_close)

    def report_callback_exception(self, exc, val, tb):
        """Tk 回调里出现异常时只在窗口内提示，不退出程序"""
        try:
            self._append(f"[ERR] {val}\n", "err")
        except Exception:
            pass

    # ------------------------------------------------------------------ UI
    def _build_ui(self):
        frm_top = ttk.LabelFrame(self, text=" Serial Settings (Serial Port Settings)")
        frm_top.pack(fill="x", padx=8, pady=(8, 4))

        # row 0: Port + Refresh + Baud + Parity
        ttk.Label(frm_top, text="Port (COM Port):").grid(row=0, column=0, padx=4, pady=6, sticky="e")
        self.cmb_port = ttk.Combobox(frm_top, width=28, state="readonly")
        self.cmb_port.grid(row=0, column=1, padx=2)
        ttk.Button(frm_top, text="Refresh", command=self.refresh_ports,
                   width=8).grid(row=0, column=2, padx=4)

        ttk.Label(frm_top, text="Baud (Baud Rate):").grid(row=0, column=3, padx=(12, 4), sticky="e")
        self.cmb_baud = ttk.Combobox(frm_top, width=9, state="readonly", values=BAUDS)
        self.cmb_baud.set(9600)
        self.cmb_baud.grid(row=0, column=4, padx=2)

        ttk.Label(frm_top, text="Parity (Parity Check):").grid(row=0, column=5, padx=(12, 4), sticky="e")
        self.cmb_par = ttk.Combobox(frm_top, width=9, state="readonly",
                                    values=[p[0] for p in PARITIES])
        self.cmb_par.set("None (N)")
        self.cmb_par.grid(row=0, column=6, padx=2)

        # row 1: Data + Stop + Flow + Timestamp
        ttk.Label(frm_top, text="Data (Data Bits):").grid(row=1, column=0, padx=4, pady=6, sticky="e")
        self.cmb_data = ttk.Combobox(frm_top, width=6, state="readonly",
                                     values=[d[0] for d in DATABITS])
        self.cmb_data.set("8")
        self.cmb_data.grid(row=1, column=1, padx=2, sticky="w")

        ttk.Label(frm_top, text="Stop (Stop Bits):").grid(row=1, column=3, padx=(12, 4), sticky="e")
        self.cmb_stop = ttk.Combobox(frm_top, width=6, state="readonly",
                                     values=[s[0] for s in STOPBITS])
        self.cmb_stop.set("1")
        self.cmb_stop.grid(row=1, column=4, padx=2, sticky="w")

        ttk.Label(frm_top, text="Flow (Flow Control):").grid(row=1, column=5, padx=(12, 4), sticky="e")
        self.cmb_flow = ttk.Combobox(frm_top, width=9, state="readonly",
                                     values=[f[0] for f in FLOW])
        self.cmb_flow.set("None")
        self.cmb_flow.grid(row=1, column=6, padx=2)

        self.var_ts = tk.BooleanVar(value=True)
        ttk.Checkbutton(frm_top, text="Timestamp (Timestamp)", variable=self.var_ts).grid(
            row=1, column=7, padx=10, sticky="w")

        self.btn_open = ttk.Button(frm_top, text="Open (Open Port)", width=14,
                                   command=self.toggle_port)
        self.btn_open.grid(row=0, column=7, rowspan=1, padx=10, pady=2)

        # --- RX display ---------------------------------------------------
        frm_rx = ttk.LabelFrame(self, text=" RX (Received Data) - [KEY]=Button Press  yellow / RTC Time  cyan / Error  red")
        frm_rx.pack(fill="both", expand=True, padx=8, pady=4)
        self.txt = scrolledtext.ScrolledText(frm_rx, font=("Consolas", 10), wrap="none",
                                             state="disabled", bg="#1e1e1e")
        self.txt.pack(fill="both", expand=True, padx=2, pady=2)
        for tag, color in (("key", "#e6c229"), ("time", "#4ec9b0"),
                           ("err", "#f14c4c"), ("banner", "#6a9955"),
                           ("tx", "#c586c0")):
            self.txt.tag_configure(tag, foreground=color)

        # --- TX -----------------------------------------------------------
        frm_tx = ttk.LabelFrame(self, text=" TX (Send, Enter to Send)")
        frm_tx.pack(fill="x", padx=8, pady=4)
        self.ent_tx = ttk.Entry(frm_tx)
        self.ent_tx.pack(side="left", fill="x", expand=True, padx=4, pady=4)
        self.ent_tx.bind("<Return>", lambda e: self.send_data())
        self.var_crlf = tk.BooleanVar(value=True)
        ttk.Checkbutton(frm_tx, text="Append \\r\\n", variable=self.var_crlf).pack(
            side="left", padx=6)
        ttk.Button(frm_tx, text="Send (Send)", command=self.send_data, width=10).pack(
            side="left", padx=6)

        # --- status bar ----------------------------------------------------
        self.var_status = tk.StringVar(value="Port closed (Port closed)")
        self.var_stat = tk.StringVar(value="RX 0 B   TX 0 B   0 lines")
        ttk.Label(self, textvariable=self.var_status, relief="sunken",
                  anchor="w").pack(fill="x", padx=8, pady=(0, 2))
        ttk.Label(self, textvariable=self.var_stat, relief="sunken",
                  anchor="e").pack(fill="x", padx=8, pady=(0, 8))

    # ------------------------------------------------------------------ ports
    def refresh_ports(self):
        ports = list(list_ports.comports())
        names = [f"{p.device}  -  {p.description}" for p in ports]
        self.cmb_port["values"] = names
        if not ports:
            self.var_status.set("No serial port found (No serial port found)")
            return
        # preselect: user-specified > non-STLink > first
        pick = 0
        if self.pre_port:
            for i, p in enumerate(ports):
                if p.device.lower() == self.pre_port.lower():
                    pick = i
                    break
        else:
            for i, p in enumerate(ports):
                if "ST-Link" not in p.description and "STLink" not in p.description:
                    pick = i
                    break
        self.cmb_port.current(pick)
        self.var_status.set(f"Port ready (Port ready), auto selected {ports[pick].device}")

    def _selected_device(self):
        return self.cmb_port.get().split()[0]

    # ------------------------------------------------------------------ open/close
    def toggle_port(self):
        if self.running:
            self._close_port()
        else:
            self._open_port()

    def _open_port(self):
        try:
            flow = FLOW[[f[0] for f in FLOW].index(self.cmb_flow.get())][1]
            kw = {}
            if flow == "xon":
                kw["xonxoff"] = True
            elif flow == "rtscts":
                kw["rtscts"] = True
            elif flow == "dsrdtr":
                kw["dsrdtr"] = True

            self._open_kw = dict(
                port=self._selected_device(),
                baudrate=int(self.cmb_baud.get()),
                bytesize=DATABITS[[d[0] for d in DATABITS].index(self.cmb_data.get())][1],
                parity=PARITIES[[p[0] for p in PARITIES].index(self.cmb_par.get())][1],
                stopbits=STOPBITS[[s[0] for s in STOPBITS].index(self.cmb_stop.get())][1],
                timeout=0.05, **kw)
            self.ser = serial.Serial(**self._open_kw)

            logdir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "logs")
            os.makedirs(logdir, exist_ok=True)
            self.logf = open(os.path.join(
                logdir, f"gui_{datetime.now():%Y%m%d_%H%M%S}.log"), "a", encoding="utf-8")
            self.logf.write(f"=== OPEN {self.ser.port} {self.ser.baudrate} "
                            f"{self.ser.bytesize}-{self.ser.parity}-{self.ser.stopbits} "
                            f"at {datetime.now():%Y-%m-%d %H:%M:%S} ===\n")

            self.rx_bytes = self.tx_bytes = self.lines = 0
            self.running = True
            self.rx_thread = threading.Thread(target=self._rx_loop, daemon=True)
            self.rx_thread.start()
            self.btn_open.config(text="Close (Close Port)")
            self.var_status.set(
                f"Port open (Port opened): {self.ser.port} @ {self.ser.baudrate} bps, "
                f"{self.ser.bytesize}-{self.ser.parity}-{self.ser.stopbits}, "
                f"flow control {self.cmb_flow.get()}")
            self._append(f"--- Opened {self.ser.port} @ {self.ser.baudrate} "
                         f"{self.ser.bytesize}-{self.ser.parity}-{self.ser.stopbits} ---\n",
                         "banner")
        except serial.SerialException as e:
            messagebox.showerror("Open Port Failed (Failed to open serial port)", str(e))

    def _close_port(self):
        self.running = False
        if self.rx_thread:
            self.rx_thread.join(timeout=1.0)
            self.rx_thread = None
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None
        if self.logf:
            self.logf.write(f"=== CLOSE at {datetime.now():%Y-%m-%d %H:%M:%S} ===\n")
            self.logf.close()
            self.logf = None
        self.btn_open.config(text="Open (Open Port)")
        self.var_status.set("Port closed (Port closed)")

    # ------------------------------------------------------------------ RX
    def _rx_loop(self):
        """接收线程：任何异常都不退出窗口，掉线后自动重连"""
        buf = bytearray()
        while self.running:
            if self.ser is None:
                time.sleep(0.2)
                continue
            try:
                chunk = self.ser.read(256)
            except Exception as e:
                # 掉线（USB 拔出 / 端口被抢占）：提示 + 自动重连，绝不退出程序
                self.rx_queue.put((None, f"[WARN] 串口断开: {e}"))
                self._reconnect()
                buf.clear()
                continue
            if chunk:
                self.rx_bytes += len(chunk)
                buf += chunk
                while b"\n" in buf:
                    raw, _, rest = buf.partition(b"\n")
                    buf = rest
                    text = raw.decode("ascii", "replace").rstrip("\r")
                    stamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
                    self.rx_queue.put((stamp, text + "\n"))
            else:
                time.sleep(0.01)

    def _reconnect(self):
        """掉线后每秒尝试重开一次，直到成功或用户点关闭"""
        while self.running:
            time.sleep(1.0)
            if not self.running:
                return
            params = getattr(self, "_open_kw", None)
            if not params:
                return
            try:
                self.ser = serial.Serial(**params)
                self.rx_queue.put((None, "[INFO] 串口已自动重连"))
                return
            except Exception:
                continue

    def _classify(self, text):
        if "[KEY]" in text:
            return "key"
        if any(k in text for k in ("Error", "error", "Fault", "FAIL")):
            return "err"
        if "SYSCLK" in text or "Experiment" in text or "USART2" in text or "Opened" in text:
            return "banner"
        t = text.rstrip()
        if len(t) >= 19 and t[4] == "-" and t[7] == "-" and t[10] == " " \
                and t[13] == ":" and t[16] == ":":
            return "time"
        return None

    def _poll_queue(self):
        try:
            while True:
                stamp, text = self.rx_queue.get_nowait()
                if stamp is None:                      # 程序提示（断线/重连）
                    self._append(text + "\n", "err" if "WARN" in text else "banner")
                    continue
                self.lines += 1
                shown = f"[{stamp}] {text}" if self.var_ts.get() else text
                self._append(shown, self._classify(text))
        except queue.Empty:
            pass
        except Exception:
            pass                                       # 界面异常一律吞掉，保持循环
        finally:
            self.var_stat.set(f"RX {self.rx_bytes} B   TX {self.tx_bytes} B   {self.lines} lines")
            self.after(50, self._poll_queue)           # 只在这里续期，且只续一次

    def _append(self, text, tag=None):
        self.txt.config(state="normal")
        self.txt.insert("end", text, tag or ())
        # cap buffer at ~2000 lines
        if int(self.txt.index("end-1c").split(".")[0]) > 2000:
            self.txt.delete("1.0", "200.0")
        self.txt.see("end")
        self.txt.config(state="disabled")
        if self.logf:
            try:
                self.logf.write(text if text.endswith("\n") else text + "\n")
                self.logf.flush()
            except Exception:
                pass                      # 磁盘/文件异常不影响监测

    # ------------------------------------------------------------------ TX
    def send_data(self):
        if not self.running:
            messagebox.showwarning("Notice", "Please open the port first")
            return
        msg = self.ent_tx.get()
        if not msg:
            return
        data = msg + ("\r\n" if self.var_crlf.get() else "")
        try:
            self.ser.write(data.encode("ascii"))
        except Exception as e:
            self._append(f"[WARN] 发送失败: {e}\n", "err")   # 不弹模态框，避免卡界面
            return
        self.tx_bytes += len(data)
        self._append(f"[TX] {msg}\n", "tx")
        self.ent_tx.delete(0, "end")

    # ------------------------------------------------------------------ demo
    def demo_type(self, text):
        """在发送框里逐字符“输入”，输完自动发送（用于录屏演示，效果等同手打）"""
        self.ent_tx.delete(0, "end")
        self.ent_tx.focus_set()

        def step(i=0):
            if i < len(text):
                self.ent_tx.insert("end", text[i])
                self.after(55, lambda: step(i + 1))
            else:
                self.after(250, self.send_data)

        step()

    def run_demo(self):
        """自动演示序列：合法帧 → 边界帧 → 非法秒 → 非法命令字 → 帧外字符"""
        seq = [
            (0,     "$0,1,251107,140101*"),   # ① 设置 2025-11-07 14:01:01
            (9000,  "$0,1,251231,235959*"),   # ② 边界值 2025-12-31 23:59:59
            (17000, "$0,1,251107,999999*"),   # ③ 非法秒：应被拒绝
            (24000, "$0,2,251107,120000*"),   # ④ 非法命令字：应被拒绝
            (31000, "hello"),                 # ⑤ 帧外字符：应被忽略
        ]
        self._append("[DEMO] 自动演示开始：逐条在发送框输入并回车\n", "banner")
        for delay, text in seq:
            self.after(delay, lambda t=text: self.demo_type(t))

    def _on_close(self):
        self._close_port()
        self.destroy()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", help="preselect COM port, e.g. COM11")
    ap.add_argument("-o", "--open", action="store_true",
                    help="open the port immediately at startup")
    ap.add_argument("--demo", action="store_true",
                    help="run the scripted demo sequence (types into the TX box)")
    args = ap.parse_args()
    app = SerialMonitorGUI(pre_port=args.port)
    if args.open:
        app.after(300, app._open_port)
    if args.demo:
        app.after(12000, app.run_demo)      # 等窗口稳定、开始录屏后再演示
    app.mainloop()


if __name__ == "__main__":
    main()
