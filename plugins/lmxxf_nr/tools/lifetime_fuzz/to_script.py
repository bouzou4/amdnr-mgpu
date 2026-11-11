#!/usr/bin/env python3
"""A fuzz run's steps as an mgpu_harness --script file: replays a finding from a fresh start.

    python to_script.py <run folder or harness.txt> [--from N] [--to M] [--lead FRAMES] > steps.txt

--from / --to pick the step range (default all); --lead is the gap before the first step (default 300 frames, so
the first network build is over). Multi-GPU dump counts and trigger values the step line does not print are 1.
"""
import argparse
import re
import sys
from pathlib import Path

STEP = re.compile(r'^fuzz (\d+) frame (\d+) t [\d.]+ at [\d:.]+( \(masked\))?: (.*)$')


def convert(what):
    w = what.strip()
    if w.startswith('ini '):
        return 'ini ' + ';'.join(w[4:].split())
    if w == 'frame dump':
        return 'file lmxxf_nr_dump.trigger 1'
    m = re.match(r'sequence dump of (\d+)$', w)
    if m:
        return f'file lmxxf_nr_seq.trigger {m.group(1)}'
    m = re.match(r'resize (\d+)x(\d+), buffers (\d+) frames after the window$', w)
    if m:
        return f'resize {m.group(1)} {m.group(2)} {m.group(3)}'
    m = re.match(r'minimise for (\d+) frames(, ResizeBuffers after each step)?$', w)
    if m:
        return f'minimise {m.group(1)} {1 if m.group(2) else 0}'
    m = re.match(r'render scale ([\d.]+)$', w)
    if m:
        return f'scale {m.group(1)}'
    m = re.match(r'HUD-less source (on|off)$', w)
    if m:
        return f'hud {1 if m.group(1) == "on" else 0}'
    m = re.match(r'upscale reset flag for (\d+) frames$', w)
    if m:
        return f'reset {m.group(1)}'
    m = re.match(r'no motion vectors for (\d+) frames$', w)
    if m:
        return f'nomv {m.group(1)}'
    m = re.match(r'mgpu (low latency|early send) (\d)$', w)
    if m:
        return f'file {"mgpu_ll.trigger" if m.group(1) == "low latency" else "mgpu_es.trigger"} {m.group(2)}'
    raise ValueError(f'unknown step: {w}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('run')
    ap.add_argument('--from', dest='first', type=int, default=0)
    ap.add_argument('--to', dest='last', type=int, default=1 << 30)
    ap.add_argument('--lead', type=int, default=300)
    a = ap.parse_args()
    p = Path(a.run)
    p = p / 'harness.txt' if p.is_dir() else p
    out, prev = [f'# steps {a.first}..{a.last} of {p.parent.name}'], None
    for line in p.read_text(encoding='utf-8', errors='replace').splitlines():
        m = STEP.match(line)
        if not m or m.group(3):
            continue
        n, frame, what = int(m.group(1)), int(m.group(2)), m.group(4)
        if n < a.first or n > a.last:
            continue
        gap = a.lead if prev is None else frame - prev
        prev = frame
        out.append(f'{gap} {convert(what)}')
    sys.stdout.write('\n'.join(out) + '\n')


if __name__ == '__main__':
    main()
