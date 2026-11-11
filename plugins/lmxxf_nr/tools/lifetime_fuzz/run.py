#!/usr/bin/env python3
"""Lifetime fuzzer runs: mgpu_harness --fuzz under the plugin's validation mode, one run per (mode, seed).

Each run stages OptiScaler + the plugin + the harness in --stage, sets [Plugin.lmxxf_nr] (Validation, TestHooks,
TriggerEvery and the mode's keys), runs the harness (under a resource lease when --reslock is given) and keeps its
output and OptiScaler.log under --out/<mode>_<seed>/. findings.md / findings.json in --out list every distinct
D3D12 debug layer message, plugin error and device loss with the switches that led to it.

    python run.py --stage DIR --out DIR --opti OptiScaler.dll --asi lmxxf_nr.asi --harness mgpu_harness.exe \
        --ini OptiScaler.ini --runtime-dir DIR [--modules-dir DIR] [--weights-dir DIR] \
        --seeds 1 2 3 --modes single mgpu [--steps 220] [--seconds 105] [--validation 2] \
        [--reslock reslock.py --owner fuzz --minutes 2] [--classes idsr] [--key K=V ...]

A device loss ends its run; the next one starts after --cooldown seconds (Windows bugchecks on 5 TDRs in a minute),
and the campaign stops after --max-losses of them. `python run.py --out DIR --report` only rebuilds the findings.
"""
import argparse
import json
import random
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

LOSS = re.compile(r'887A000[567]|887A0020|DEVICE LOST|DISPLAY CARD LOST|GAME DEVICE REMOVED|device .* removed', re.I)
STEP = re.compile(r'^fuzz (\d+) frame (\d+) t ([\d.]+) at (\d\d):(\d\d):(\d\d)\.(\d+)( \(masked\))?: (.*)$')
LOGLINE = re.compile(r'^\[(\d\d):(\d\d):(\d\d)\.(\d+)\] \[(\w)\] (.*)$')
D3D = re.compile(r'd3d12 (\w+) #(\d+) (\w+) x(\d+): (.*)$')


def set_ini(text, section, key, value):
    """Sets key in [section]; adds the section or the key when missing."""
    m = re.search(r'(?ms)^\[' + re.escape(section) + r'\]\s*$(.*?)(?=^\[|\Z)', text)
    line = f'{key}={value}'
    if not m:
        return text.rstrip('\r\n') + f'\r\n[{section}]\r\n{line}\r\n'
    body = m.group(1)
    k = re.search(r'(?m)^;?' + re.escape(key) + r'\s*=.*$', body)
    body = body[:k.start()] + line + body[k.end():] if k else '\r\n' + line + body
    return text[:m.start(1)] + body + text[m.end(1):]


def mode_keys(mode, seed):
    """[Plugin.lmxxf_nr] keys and harness options of one run; everything follows from the seed."""
    r = random.Random(seed * 7919 + (1 if mode == 'mgpu' else 0))
    keys = {'Enabled': 'true', 'Preset': str(r.choice([0, 1, 1, 2, 3])), 'Placement': str(r.choice([0, 1, 2, 2]))}
    args = ['--upscaler', '--hudless', '--render-scale', r.choice(['0.5', '0.667', '0.667', '1'])]
    if mode == 'mgpu':
        keys.update({
            'MultiGpu': 'true',
            'MgpuHudless': 'true' if r.random() < 0.75 else 'false',
            'MgpuNrEveryFrame': 'true' if r.random() < 0.7 else 'false',
            'MgpuMotionVectors': 'true' if r.random() < 0.8 else 'false',
            'MgpuEarlySend': 'true' if r.random() < 0.2 else 'false',
            'MgpuLowLatency': str(r.choice([0, 1, 2, 2])),
            'MgpuMaxFramesAhead': str(r.choice([1, 1, 1, 2])),
            'MgpuPresentIdle': 'true' if r.random() < 0.2 else 'false',
        })
        args.append('--mgpu')
    else:
        keys['MultiGpu'] = 'false'
    return keys, args


def stage(a):
    st = Path(a.stage)
    (st / 'OptiScaler' / 'plugins').mkdir(parents=True, exist_ok=True)
    shutil.copyfile(a.opti, st / 'dxgi.dll')
    shutil.copyfile(a.asi, st / 'OptiScaler' / 'plugins' / 'lmxxf_nr.asi')
    shutil.copyfile(a.harness, st / 'mgpu_harness.exe')
    return st


def kill_tree(pid):
    subprocess.run(['taskkill', '/PID', str(pid), '/T', '/F'], capture_output=True)


def run_one(a, st, mode, seed):
    out = Path(a.out) / f'{mode}_{seed}'
    out.mkdir(parents=True, exist_ok=True)
    keys, hargs = mode_keys(mode, seed)
    keys.update({'Validation': str(a.validation), 'TestHooks': 'true', 'TriggerEvery': '4', 'RuntimeDir': a.runtime_dir})
    if a.modules_dir:
        keys['ModulesDir'] = a.modules_dir
    if a.weights_dir:
        keys['WeightsDir'] = a.weights_dir
    for kv in a.key or []:
        k, v = kv.split('=', 1)
        keys[k] = v
    ini = Path(a.ini).read_text(encoding='utf-8', errors='replace')
    ini = set_ini(ini, 'Plugins', 'LoadAsiPlugins', 'true')
    ini = set_ini(ini, 'Log', 'LogToFile', 'true')
    ini = set_ini(ini, 'Log', 'LogLevel', '2')
    for k, v in keys.items():
        ini = set_ini(ini, 'Plugin.lmxxf_nr', k, v)
    (st / 'OptiScaler.ini').write_text(ini, encoding='utf-8', newline='')
    for f in st.glob('*.log'):
        f.unlink()
    dumps = st / 'lmxxf_nr_dumps'
    if dumps.exists():
        shutil.rmtree(dumps)  # the previous run's dumps of this stage folder
    dumps.mkdir()

    cmd = [str(st / 'mgpu_harness.exe'), str(a.seconds), '--fuzz', str(seed), '--fuzz-steps', str(a.steps)] + hargs
    if a.classes:
        cmd += ['--fuzz-classes', a.classes]
    if a.reslock:
        res = a.res or ('display,gpu3,gpu7' if mode == 'mgpu' else 'display,gpu3')
        cmd = [sys.executable, '-I', a.reslock, 'run', '--owner', a.owner, '--res', res, '--mode', 'functional',
               '--minutes', str(a.minutes), '--'] + cmd
    meta = {'mode': mode, 'seed': seed, 'keys': keys, 'cmd': cmd[-(len(hargs) + 5 + (2 if a.classes else 0)):],
            'started': time.strftime('%Y-%m-%dT%H:%M:%S')}
    with open(out / 'harness.txt', 'w', encoding='utf-8', errors='replace') as so:
        p = subprocess.Popen(cmd, cwd=st, stdout=so, stderr=subprocess.STDOUT)
        try:
            # a lease may wait in the queue: only the run itself is timed, from its first output
            t_run = None
            while p.poll() is None:
                time.sleep(1)
                if t_run is None and 'adapter0' in (out / 'harness.txt').read_text(encoding='utf-8', errors='replace'):
                    t_run = time.time()
                if t_run is not None and time.time() - t_run > a.seconds + 120:
                    meta['killed'] = True
                    break
        finally:
            if p.poll() is None:
                kill_tree(p.pid)
    meta['exit'] = p.returncode
    time.sleep(1)
    for f in st.glob('*.log'):
        shutil.copyfile(f, out / f.name)
    shutil.copyfile(st / 'OptiScaler.ini', out / 'OptiScaler.ini')
    meta['dump_files'] = sum(1 for _ in dumps.rglob('*') if _.is_file())
    (out / 'meta.json').write_text(json.dumps(meta, indent=1))
    return parse_run(out)


def norm(text):
    """A message with its pointers and counters removed: the key of a finding."""
    t = re.sub(r'0x[0-9A-Fa-f]+', '0x#', text)
    t = re.sub(r'\b[0-9A-Fa-f]{8,16}\b', '#', t)
    t = re.sub(r'\d+', 'N', t)
    return t[:400]


def tsec(h, m, s, frac):
    return int(h) * 3600 + int(m) * 60 + int(s) + float('0.' + frac)


def parse_run(out):
    out = Path(out)
    meta = json.loads((out / 'meta.json').read_text()) if (out / 'meta.json').exists() else {}
    steps, harness_events = [], []
    htxt = (out / 'harness.txt').read_text(encoding='utf-8', errors='replace') if (out / 'harness.txt').exists() else ''
    for line in htxt.splitlines():
        m = STEP.match(line)
        if m:
            steps.append({'n': int(m.group(1)), 'frame': int(m.group(2)), 't': tsec(*m.group(4, 5, 6, 7)),
                          'masked': bool(m.group(8)), 'what': m.group(9).strip()})
        elif LOSS.search(line) or 'NR FAILED' in line or line.startswith('fuzz end') or line.startswith('frames='):
            harness_events.append(line.strip())
    findings = {}

    def add(kind, key, text, t, severity, n=0):
        before = [s for s in steps if s['t'] <= t + 0.05 and not s['masked']]
        f = findings.setdefault(key, {'kind': kind, 'severity': severity, 'text': text, 'count': 0,
                                      'step': before[-1]['n'] if before else -1,
                                      'steps': [f"{s['n']}: {s['what']}" for s in before[-8:]]})
        f['count'] = max(f['count'] + 1, n)  # throttled lines carry their running count

    log = out / 'OptiScaler.log'
    lost = None
    if log.exists():
        for line in log.read_text(encoding='utf-8', errors='replace').splitlines():
            m = LOGLINE.match(line)
            if not m:
                continue
            t, level, body = tsec(*m.group(1, 2, 3, 4)), m.group(5), m.group(6)
            d = D3D.search(body)
            if d:
                add('d3d12', f'd3d12 {d.group(1)} #{d.group(2)} {norm(d.group(5))}', f'{d.group(3)} #{d.group(2)} on the '
                    f'{d.group(1)} device: {d.group(5)}', t, d.group(3), int(d.group(4)))
                continue
            if '[lmxxf_nr]' not in body:
                continue
            msg = body.split('[lmxxf_nr]', 1)[1].strip()
            if level == 'E' or LOSS.search(msg):
                if LOSS.search(msg) and lost is None:
                    lost = msg
                add('plugin', 'plugin ' + norm(msg), msg, t, 'ERROR')
    result = {'run': out.name, 'meta': meta, 'steps': len(steps), 'applied': sum(1 for s in steps if not s['masked']),
              'events': harness_events, 'lost': lost or next((e for e in harness_events if LOSS.search(e)), None),
              'findings': findings}
    (out / 'result.json').write_text(json.dumps(result, indent=1))
    return result


def report(outdir):
    outdir = Path(outdir)
    runs = [parse_run(d) for d in sorted(outdir.iterdir()) if d.is_dir() and (d / 'harness.txt').exists()]
    merged = {}
    for r in runs:
        for key, f in r['findings'].items():
            m = merged.setdefault(key, dict(f, runs=[], count=0, first=r['run']))
            m['count'] += f['count']
            m['runs'].append(f"{r['run']}@{f['step']}")
    order = {'CORRUPTION': 0, 'ERROR': 1, 'WARNING': 2}
    items = sorted(merged.items(), key=lambda kv: (order.get(kv[1]['severity'], 3), kv[0]))
    lines = ['# Lifetime fuzz findings', '', '## Runs', '', '| run | steps applied | exit | device loss | dump files |',
             '|---|---|---|---|---|']
    for r in runs:
        lines.append(f"| {r['run']} | {r['applied']} | {r['meta'].get('exit')} | {r['lost'] or '-'} | "
                     f"{r['meta'].get('dump_files', '-')} |")
    lines += ['', '## Findings', '']
    for i, (key, f) in enumerate(items, 1):
        lines += [f"### {i}. {f['severity']}: {f['text'][:300]}", '', f"- seen {f['count']} times in: {', '.join(f['runs'])}",
                  f"- first run {f['first']}, the switches before it:"]
        lines += [f'  - {s}' for s in f['steps']]
        lines.append('')
    (outdir / 'findings.md').write_text('\n'.join(lines), encoding='utf-8')
    (outdir / 'findings.json').write_text(json.dumps({'runs': runs, 'findings': dict(items)}, indent=1))
    return runs, items


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', required=True)
    ap.add_argument('--report', action='store_true')
    ap.add_argument('--stage')
    ap.add_argument('--opti')
    ap.add_argument('--asi')
    ap.add_argument('--harness')
    ap.add_argument('--ini')
    ap.add_argument('--runtime-dir')
    ap.add_argument('--modules-dir')
    ap.add_argument('--weights-dir')
    ap.add_argument('--seeds', nargs='+', type=int, default=[1])
    ap.add_argument('--modes', nargs='+', default=['single', 'mgpu'], choices=['single', 'mgpu'])
    ap.add_argument('--steps', type=int, default=220)
    ap.add_argument('--seconds', type=int, default=105)
    ap.add_argument('--validation', type=int, default=2)
    ap.add_argument('--classes')
    ap.add_argument('--key', action='append')
    ap.add_argument('--reslock')
    ap.add_argument('--owner', default='fuzz')
    ap.add_argument('--res', help='default: display,gpu3 for single, display,gpu3,gpu7 for mgpu')
    ap.add_argument('--minutes', type=int, default=2)
    ap.add_argument('--cooldown', type=int, default=90)
    ap.add_argument('--max-losses', type=int, default=3)
    a = ap.parse_args()
    Path(a.out).mkdir(parents=True, exist_ok=True)
    if not a.report:
        st = stage(a)
        losses = 0
        for seed in a.seeds:
            for mode in a.modes:
                r = run_one(a, st, mode, seed)
                print(f"{r['run']}: {r['applied']} steps, exit {r['meta'].get('exit')}, {len(r['findings'])} findings"
                      f"{', DEVICE LOSS: ' + r['lost'] if r['lost'] else ''}", flush=True)
                if r['lost']:
                    losses += 1
                    if losses >= a.max_losses:
                        print(f'{losses} device losses: campaign stopped', flush=True)
                        report(a.out)
                        return 3
                    time.sleep(a.cooldown)
    runs, items = report(a.out)
    print(f'{len(runs)} runs, {len(items)} distinct findings: {Path(a.out) / "findings.md"}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
