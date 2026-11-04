"""Checks on multi-GPU frame dumps (mgpu_dumps: fNNNNNN_<stage>.raw, tight rows).

  hudless  per frame: UI pixels (|bb - hudless| > t1) must equal the back buffer, pixels with bb == hudless must equal NR
  temporal over a sequence: flicker on static pixels = mean|O_t - O_t-1| - mean|I_t - I_t-1| (input unchanged there);
           ghost = share of changed pixels whose output is closer to the previous input than to the current one

usage: dump_metrics.py <dumps dir> [--w 3840 --h 2160] [--in hudless|ingest] [--out nr|display]
"""
import argparse
import json
import pathlib
import re

import numpy as np

T0, T1 = 2 / 1023, 16 / 1023


def load(p, w, h):
    v = np.fromfile(p, dtype=np.uint32)
    if v.size != w * h:
        raise ValueError(f"{p}: {v.size} texels, expected {w * h}")
    v = v.reshape(h, w)
    return np.stack([(v & 1023), (v >> 10) & 1023, (v >> 20) & 1023], -1).astype(np.float32) / 1023.0


def frames(d):
    out = {}
    for p in d.glob("f*_*.raw"):
        m = re.match(r"f(\d+)_(\w+)\.raw", p.name)
        if m:
            out.setdefault(int(m.group(1)), {})[m.group(2)] = p
    return dict(sorted(out.items()))


def hudless_check(fs, w, h):
    rows = []
    for n, st in fs.items():
        if not {"ingest", "hudless", "display"} <= st.keys():
            continue
        bb, hud, disp = load(st["ingest"], w, h), load(st["hudless"], w, h), load(st["display"], w, h)
        e = np.abs(bb - hud).max(-1)
        ui = e > T1
        same = e == 0
        r = {
            "frame": n,
            "ui_share": float((e > T0).mean()),
            "ui_px": int(ui.sum()),
            "ui_px_not_bb": int((np.abs(disp - bb).max(-1)[ui] > 0).sum()),
        }
        if "nr" in st:
            nr = load(st["nr"], w, h)
            r["scene_px"] = int(same.sum())
            r["scene_px_not_nr"] = int((np.abs(disp - nr).max(-1)[same] > 0).sum())
            r["nr_edit_mean"] = float(np.abs(nr - hud).mean())
        rows.append(r)
    return rows


def temporal(fs, w, h, src, dst):
    seq = [(n, st) for n, st in fs.items() if src in st and dst in st]
    res = []
    for (n0, a), (n1, b) in zip(seq, seq[1:]):
        if n1 != n0 + 1:
            continue
        i0, i1 = load(a[src], w, h), load(b[src], w, h)
        o0, o1 = load(a[dst], w, h), load(b[dst], w, h)
        di = np.abs(i1 - i0).max(-1)
        static = di == 0
        changed = di > 0.05
        r = {"frame": n1, "static_share": float(static.mean()), "changed_share": float(changed.mean())}
        if static.any():
            fo = np.abs(o1 - o0).max(-1)[static].mean()
            r["flicker"] = float(fo)  # input is identical there, so the input term is 0
            r["flicker_minus_input"] = float(fo - di[static].mean())
        if changed.any():
            prev = np.abs(o1 - i0).max(-1)[changed]
            cur = np.abs(o1 - i1).max(-1)[changed]
            r["ghost_share"] = float((prev < cur).mean())
        res.append(r)
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--w", type=int, default=3840)
    ap.add_argument("--h", type=int, default=2160)
    ap.add_argument("--in", dest="src", default="hudless")
    ap.add_argument("--out", dest="dst", default="nr")
    a = ap.parse_args()
    fs = frames(pathlib.Path(a.dir))
    src = a.src if any(a.src in st for st in fs.values()) else "ingest"
    dst = a.dst if any(a.dst in st for st in fs.values()) else "display"
    t = temporal(fs, a.w, a.h, src, dst)
    o = {"frames": len(fs), "hudless": hudless_check(fs, a.w, a.h), "temporal_in": src, "temporal_out": dst,
         "temporal": t}
    if t:
        for k in ("flicker", "flicker_minus_input", "ghost_share"):
            v = [r[k] for r in t if k in r]
            if v:
                o[k + "_mean"] = float(np.mean(v))
    print(json.dumps(o, indent=1))


if __name__ == "__main__":
    main()
