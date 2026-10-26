#!/usr/bin/env python3
"""Check transmitted CPS 0x0030 frames independently from the live UI.

Print interval cadence/speed from revolution and event-time differences.
Samples must be less than 32 seconds apart for the wheel timestamp rollover.
"""
import argparse
import json
import re
import struct


def decode(raw):
    if len(raw) != 14:
        raise ValueError('CPS full frame must contain 14 bytes')
    flags, power, wheel, wheel_time, crank, crank_time = struct.unpack('<HhIHHH', raw)
    if flags != 0x30:
        raise ValueError(f'Unexpected flags: {flags:#x}')
    return dict(power_w=power, wheel=wheel, wheel_ticks=wheel_time,
                crank=crank, crank_ticks=crank_time)


def interval(previous, current):
    wheel_count = (current['wheel'] - previous['wheel']) & 0xffffffff
    crank_count = (current['crank'] - previous['crank']) & 0xffff
    wheel_ticks = (current['wheel_ticks'] - previous['wheel_ticks']) & 0xffff
    crank_ticks = (current['crank_ticks'] - previous['crank_ticks']) & 0xffff
    return {
        'cadence_rpm_interval': round(crank_count * 60 * 1024 / crank_ticks, 2) if crank_ticks else 0,
        'speed_kmh_interval': round(wheel_count * 2.1 * 3.6 * 2048 / wheel_ticks, 2) if wheel_ticks else 0,
    }


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('log')
    a = p.parse_args()
    previous = None
    previous_ms = None
    pattern = re.compile(r'^(\S+) (\d+) CPS_WIRE hex=([0-9a-f]+)')
    with open(a.log) as source:
        for line in source:
            m = pattern.search(line)
            if not m:
                continue
            timestamp, millis, raw = m.groups()
            current = decode(bytes.fromhex(raw))
            row = dict(timestamp_utc=timestamp, **current)
            if previous is not None and 0 < int(millis) - previous_ms < 32000:
                row.update(interval(previous, current))
            print(json.dumps(row))
            previous, previous_ms = current, int(millis)
