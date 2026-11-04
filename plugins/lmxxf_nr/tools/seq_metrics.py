#!/usr/bin/env python3
"""Temporal scores of a plugin sequence dump (lmxxf_nr_seq.trigger): per frame the network input (fed_KK),
its motion vectors (mv_KK) and the network's answer (enh_KK), described by seq.json.

usage: seq_metrics.py <seq dir> [<seq dir> ...]

Scores follow lmxxf-nr's temporal_scene.h / nr_sequence score, adapted to game frames (no truth, no aligned
reference run): images are compared in display encoding (clamped linear ** 1/2.2), vectors point to the
previous frame (previous = current + mv, feed pixels), warps are bilinear.
  flk_static  mean|O_t - O_t-1| - mean|I_t - I_t-1| on still pixels (|mv| < 0.25) whose input is consistent
  flk_mc      mean|O_t - warp O_t-1| - mean|I_t - warp I_t-1| on moving pixels (|mv| >= 0.5) visible in both
  edit_vis    mean|O_t - I_t| on those moving pixels (the network's edit)
  edit_dis    mean|O_t - I_t| on disoccluded pixels (warp off screen, or input inconsistent): history leaking
              old content there raises it against the same run's edit_vis and against a history-off run
  psnr_mc     PSNR of O_t against warp O_t-1 on visible moving pixels (compare_dumps.py style)
  mv check    mean|I_t - warp I_t-1| on moving pixels with +mv, -mv and no motion (the sign must win)
"""
import json
import os
import sys

import numpy as np

FORMATS = {  # DXGI format -> (dtype, channels)
    2: (np.float32, 4), 10: (np.float16, 4), 16: (np.float32, 2), 34: (np.float16, 2), 33: (np.float16, 2),
    1: (np.float32, 4), 9: (np.float16, 4),
}
TAU_VIS, TAU_DIS = 0.06, 0.15


def load(path, size, fmt):
    dt, ch = FORMATS[fmt]
    w, h = size
    a = np.fromfile(path, dtype=dt).astype(np.float32)
    return a.reshape(h, w, ch)


def enc(x):
    return np.power(np.clip(x, 0.0, 1.0), 1 / 2.2)


def warp(img, mx, my):
    h, w = img.shape[:2]
    ys, xs = np.mgrid[0:h, 0:w].astype(np.float32)
    x = np.clip(xs + mx, 0, w - 1)
    y = np.clip(ys + my, 0, h - 1)
    x0 = np.floor(x).astype(np.int32); y0 = np.floor(y).astype(np.int32)
    x1 = np.minimum(x0 + 1, w - 1); y1 = np.minimum(y0 + 1, h - 1)
    fx = (x - x0)[..., None]; fy = (y - y0)[..., None]
    return ((1 - fy) * ((1 - fx) * img[y0, x0] + fx * img[y0, x1]) + fy * ((1 - fx) * img[y1, x0] + fx * img[y1, x1]))


def onscreen(mx, my):
    h, w = mx.shape
    ys, xs = np.mgrid[0:h, 0:w].astype(np.float32)
    x, y = xs + mx, ys + my
    return (x >= 0) & (y >= 0) & (x <= w - 1) & (y <= h - 1)


def vectors(seq, k, frame, W, H):
    if not frame['mv']:
        return None
    mv = load(os.path.join(seq, 'mv_%02d.raw' % k), frame['mvSize'], frame['mvFormat'])[..., :2]
    uw, uh = frame['mvUsed']
    xs = np.minimum(((np.arange(W) + 0.5) * uw / W).astype(np.int32), uw - 1)
    ys = np.minimum(((np.arange(H) + 0.5) * uh / H).astype(np.int32), uh - 1)
    m = mv[ys][:, xs]
    return m[..., 0] * frame['mvScale'][0], m[..., 1] * frame['mvScale'][1]


def score(seq):
    meta = json.load(open(os.path.join(seq, 'seq.json')))
    W, H = meta['feed']
    frames = meta['frames']
    I, O, M = [], [], []
    for k, f in enumerate(frames):
        I.append(enc(load(os.path.join(seq, 'fed_%02d.raw' % k), f['fedSize'], f['fedFormat'])[:H, :W, :3]) if f['fed'] else None)
        O.append(enc(load(os.path.join(seq, 'enh_%02d.raw' % k), f['enhSize'], f['enhFormat'])[:H, :W, :3]) if f['enh'] else None)
        M.append(vectors(seq, k, f, W, H))
    acc = {k: [] for k in ('flk_static', 'flk_mc', 'edit_vis', 'edit_dis', 'psnr_mc', 'mv_pos', 'mv_neg', 'mv_zero',
                           'moving', 'disoccl', 'mv_px')}
    rows = []
    for t in range(1, len(frames)):
        if any(x is None for x in (I[t], I[t - 1], O[t], O[t - 1], M[t])) or frames[t]['reset']:
            continue
        mx, my = M[t]
        mag = np.hypot(mx, my)
        on = onscreen(mx, my)
        wI, wO = warp(I[t - 1], mx, my), warp(O[t - 1], mx, my)
        din = np.abs(I[t] - wI).max(axis=2)
        still = (mag < 0.25) & (np.abs(I[t] - I[t - 1]).max(axis=2) < TAU_VIS)
        moving = mag >= 0.5
        vis = moving & on & (din < TAU_VIS)
        dis = (~on) | (din >= TAU_DIS)
        e = np.abs(O[t] - I[t]).mean(axis=2)
        r = {}
        r['flk_static'] = float(np.abs(O[t] - O[t - 1])[still].mean() - np.abs(I[t] - I[t - 1])[still].mean()) if still.any() else np.nan
        r['flk_mc'] = float(np.abs(O[t] - wO)[vis].mean() - np.abs(I[t] - wI)[vis].mean()) if vis.any() else np.nan
        r['edit_vis'] = float(e[vis].mean()) if vis.any() else np.nan
        r['edit_dis'] = float(e[dis].mean()) if dis.any() else np.nan
        mse = float(((O[t] - wO) ** 2)[vis].mean()) if vis.any() else np.nan
        r['psnr_mc'] = 10 * np.log10(1 / mse) if mse and mse > 0 else np.nan
        if moving.any():
            r['mv_pos'] = float(np.abs(I[t] - wI)[moving & on].mean())
            r['mv_neg'] = float(np.abs(I[t] - warp(I[t - 1], -mx, -my))[moving].mean())
            r['mv_zero'] = float(np.abs(I[t] - I[t - 1])[moving].mean())
        else:
            r['mv_pos'] = r['mv_neg'] = r['mv_zero'] = np.nan
        r['moving'] = float(moving.mean())
        r['disoccl'] = float(dis.mean())
        r['mv_px'] = float(mag.mean())
        for k2, v in r.items():
            acc[k2].append(v)
        rows.append((t, r))
    temporal = sum(f['temporal'] for f in frames)
    print('%s: %d frames, %dx%d, temporal frames %d, resets %d, scored pairs %d' % (
        os.path.basename(os.path.normpath(seq)), len(frames), W, H, temporal, sum(f['reset'] for f in frames), len(rows)))
    for t, r in rows:
        print('  %2d flk_static %+.5f flk_mc %+.5f edit_vis %.5f edit_dis %.5f psnr_mc %5.2f  moving %.2f dis %.3f |mv| %.2f px  mv +%.4f -%.4f 0:%.4f' % (
            t, r['flk_static'], r['flk_mc'], r['edit_vis'], r['edit_dis'], r['psnr_mc'], r['moving'], r['disoccl'], r['mv_px'],
            r['mv_pos'], r['mv_neg'], r['mv_zero']))
    mean = {k: float(np.nanmean(v)) if v else np.nan for k, v in acc.items()}
    print('  mean flk_static %+.5f flk_mc %+.5f edit_vis %.5f edit_dis %.5f psnr_mc %.2f | moving %.2f dis %.3f |mv| %.2f px | mv check +%.4f -%.4f 0:%.4f' % (
        mean['flk_static'], mean['flk_mc'], mean['edit_vis'], mean['edit_dis'], mean['psnr_mc'], mean['moving'], mean['disoccl'],
        mean['mv_px'], mean['mv_pos'], mean['mv_neg'], mean['mv_zero']))
    return mean


if __name__ == '__main__':
    for d in sys.argv[1:]:
        score(d)
