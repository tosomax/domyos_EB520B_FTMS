#!/usr/bin/env python3
"""Decode captured EB520 Indoor Bike Data into JSONL or CSV.

Observed EB520 layout uses TWO bytes for resistance, retained as a raw integer
until the console scale has been checked. No offsets inferred from names.
"""
import argparse
import csv
import json
import re
import sys


def decode(data):
    offset = 0

    def take(n, signed=False):
        nonlocal offset
        if offset + n > len(data):
            raise ValueError(f'truncated at offset {offset}, need {n} bytes')
        value = int.from_bytes(data[offset:offset+n], 'little', signed=signed)
        offset += n
        return value

    flags = take(2)
    if flags & 0xe000:
        raise ValueError(f'unsupported reserved flags: {flags:04x}')
    out = {'flags': f'0x{flags:04x}', 'more_data': bool(flags & 1)}
    if not flags & 1:
        out['speed_kmh'] = take(2) / 100
    if flags & 2:
        out['average_speed_kmh'] = take(2) / 100
    if flags & 4:
        out['cadence_rpm'] = take(2) / 2
    if flags & 8:
        out['average_cadence_rpm'] = take(2) / 2
    if flags & 16:
        out['distance_m'] = take(3)
    if flags & 32:
        out['resistance_raw'] = take(2, True)
    if flags & 64:
        out['power_w'] = take(2, True)
    if flags & 128:
        out['average_power_w'] = take(2, True)
    if flags & 256:
        for key, size, sentinel in [('energy_kcal', 2, 65535), ('energy_kcal_h', 2, 65535), ('energy_kcal_min', 1, 255)]:
            value = take(size)
            out[key] = None if value == sentinel else value
    if flags & 512:
        out['heart_rate_bpm'] = take(1)
    if flags & 1024:
        out['metabolic_equivalent'] = take(1) / 10
    if flags & 2048:
        out['elapsed_s'] = take(2)
    if flags & 4096:
        out['remaining_s'] = take(2)
    if offset != len(data):
        raise ValueError(f'{len(data)-offset} trailing bytes')
    return out


def records(path):
    pattern = re.compile(r'^(\S+) (\d+) NOTIFY uuid=00002ad2-0000-1000-8000-00805f9b34fb handle=\d+ len=(\d+) hex=([0-9a-f]+)')
    with open(path) as source:
        for line in source:
            m = pattern.search(line)
            if not m:
                continue
            timestamp, uptime, length, raw = m.groups()
            try:
                data = bytes.fromhex(raw)
                if len(data) != int(length):
                    raise ValueError('logged length mismatch')
                yield {'timestamp_utc': timestamp, 'uptime_ms': int(uptime), **decode(data), 'hex': raw}
            except ValueError as e:
                raise ValueError(f'{timestamp}: {e}') from e


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('log')
    p.add_argument('--csv', action='store_true')
    a = p.parse_args()
    rows = list(records(a.log))
    if a.csv and rows:
        keys = list(dict.fromkeys(k for row in rows for k in row))
        writer = csv.DictWriter(sys.stdout, fieldnames=keys)
        writer.writeheader()
        writer.writerows(rows)
    elif not a.csv:
        for row in rows:
            print(json.dumps(row))
