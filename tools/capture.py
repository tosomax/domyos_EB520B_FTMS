#!/usr/bin/env python3
"""Capture serial lines with UTC host times; optionally issue one command.

Opening this port does not intentionally reset the ESP32. Use --reset for boot logs.
"""
import argparse
import datetime as dt
import pathlib
import time
import select
import sys
import serial

p = argparse.ArgumentParser()
p.add_argument('--port', default='/dev/ttyUSB0')
p.add_argument('--seconds', type=float, default=30)
p.add_argument('--command')
p.add_argument('--reset', action='store_true')
p.add_argument('--output', required=True)
p.add_argument('--interactive', action='store_true', help='Forward stdin lines to serial')
a = p.parse_args()
port = serial.Serial(port=None, baudrate=115200, timeout=0.2)
port.dtr = False
port.rts = False
port.port = a.port
port.open()
pathlib.Path(a.output).parent.mkdir(parents=True, exist_ok=True)
with port, open(a.output, 'a', buffering=1) as out:
    if a.reset:
        port.rts = True
        time.sleep(.1)
        port.rts = False
        time.sleep(.1)
    if a.command:
        port.write((a.command + '\n').encode())
    deadline = time.monotonic() + a.seconds
    pending = b''
    while time.monotonic() < deadline:
        if a.interactive and select.select([sys.stdin], [], [], 0)[0]:
            cmd = sys.stdin.readline()
            if cmd:
                port.write(cmd.encode())
                out.write(dt.datetime.now(dt.timezone.utc).isoformat() + ' HOST_COMMAND ' + cmd)
        pending += port.read(port.in_waiting or 1)
        while b'\n' in pending:
            line, pending = pending.split(b'\n', 1)
            record = dt.datetime.now(dt.timezone.utc).isoformat() + ' ' + line.decode('utf-8', 'backslashreplace').rstrip('\r')
            print(record, flush=True)
            out.write(record + '\n')
    if pending:
        out.write(dt.datetime.now(dt.timezone.utc).isoformat() + ' PARTIAL ' + repr(pending) + '\n')
