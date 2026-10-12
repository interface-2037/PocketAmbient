#!/usr/bin/env python3
"""Automated audio analysis for Ambient Pocket renders (spec section 8.3).

    python tools/analyze_audio.py renders/drift.wav [more.wav ...] [--report DIR] [--plots]

Each WAV may have a sibling .csv composer trace (render --events), which enables
the event-based checks (stuck notes, repetition, keys, pitch classes).
Streams the audio in chunks, so 8-hour RF64 files are fine.
Exit status is non-zero if any threshold is breached.
"""
import argparse
import datetime as dt
import hashlib
import json
import math
import os
import sys

import numpy as np
import soundfile as sf
from scipy import signal

FS = 48000
CHUNK = FS * 60

# --- thresholds (section 8.3) ---------------------------------------------------
TH = dict(
    sample_peak_dbfs=-1.0,
    true_peak_dbtp=-0.5,
    lufs_target=-20.0, lufs_tol=3.0, mode_spread_lu=2.0,
    dead_air_lufs=-50.0, dead_air_max_s=20.0,
    jump_lu=8.0,
    jump_lu_mother=8.0,    # D35 interim 10 LU no longer needed after the D37 rework
    dc_dbfs=-60.0,
    hf_rel_db=-50.0, lf_rel_db=-40.0,
    mono_lu=3.0,
    voice_budget_max_s=60.0,
    min_keys=3,
)
# Derivative-of-HF vs local RMS. Tuned on clean renders (bell attacks, aligned saw edges and rain
# bursts reach ~18x); K=25 flags steps above ~-30 dBFS. Quieter discontinuities are covered
# white-box by the steal-level check (voice steals are the only mechanism that can cause one).
CLICK_K = 25.0
STEAL_MAX_DB = -60.0
CLICK_FLOOR = 10 ** (-66 / 20)
MAX_NOTE_MS = {0: 600_000, 1: 12_000, 2: 2_400_000}   # per mode, ticks (ms)

# K-weighting, BS.1770 at 48 kHz
K1_B = [1.53512485958697, -2.69169618940638, 1.19839281085285]
K1_A = [1.0, -1.69065929318241, 0.73248077421585]
K2_B = [1.0, -2.0, 1.0]
K2_A = [1.0, -1.99004745483398, 0.99007225036621]
K_SOS = np.vstack([np.r_[K1_B, K1_A], np.r_[K2_B, K2_A]])
HP8K_SOS = signal.butter(4, 8000, "highpass", fs=FS, output="sos")


def db(x):
    return 10 * math.log10(x) if x > 0 else -math.inf


def gated_loudness(block_ms):
    """BS.1770 integrated loudness from 400 ms block mean-square sums."""
    b = np.asarray(block_ms)
    b = b[b > 0]
    if not len(b):
        return -math.inf
    lk = -0.691 + 10 * np.log10(b)
    b = b[lk > -70]
    if not len(b):
        return -math.inf
    rel = -0.691 + 10 * np.log10(b.mean()) - 10
    b2 = b[-0.691 + 10 * np.log10(b) > rel]
    return -0.691 + 10 * np.log10(b2.mean()) if len(b2) else -math.inf


class Analyzer:
    def __init__(self):
        self.zi_k = [signal.sosfilt_zi(K_SOS) * 0 for _ in range(3)]   # L, R, mono
        self.zi_hp = [signal.sosfilt_zi(HP8K_SOS) * 0 for _ in range(2)]
        self.sub = {"st": [], "mono": []}     # 100 ms K-weighted mean squares (sum over channels)
        self.peak = 0
        self.fullscale = 0
        self.tp = 0.0
        self.tp_tail = None
        self.dc = []
        self.clicks = []
        self.psd_acc = None
        self.psd_n = 0
        self.n = 0
        self.prev_hp_last = np.zeros(2)
        self.chroma = []
        self.carry = np.zeros((0, 2))

    def feed(self, x16):
        x = x16.astype(np.float64) / 32768.0
        self.peak = max(self.peak, int(np.abs(x16.astype(np.int32)).max(initial=0)))
        self.fullscale += int(np.count_nonzero((x16 == 32767) | (x16 == -32768)))

        # true peak (4x), with a short overlap so chunk edges don't hide overs
        seg = x if self.tp_tail is None else np.vstack([self.tp_tail, x])
        up = signal.resample_poly(seg, 4, 1, axis=0)
        self.tp = max(self.tp, float(np.abs(up).max(initial=0)))
        self.tp_tail = x[-64:]

        # K-weighted 100 ms mean squares
        kl, self.zi_k[0] = signal.sosfilt(K_SOS, x[:, 0], zi=self.zi_k[0])
        kr, self.zi_k[1] = signal.sosfilt(K_SOS, x[:, 1], zi=self.zi_k[1])
        m = 0.5 * (x[:, 0] + x[:, 1])
        km, self.zi_k[2] = signal.sosfilt(K_SOS, m, zi=self.zi_k[2])
        nsub = len(x) // 4800
        if nsub:
            e = (kl[: nsub * 4800] ** 2 + kr[: nsub * 4800] ** 2).reshape(nsub, 4800).mean(1)
            em = (2 * km[: nsub * 4800] ** 2).reshape(nsub, 4800).mean(1)   # dual-mono
            self.sub["st"].extend(e.tolist())
            self.sub["mono"].extend(em.tolist())

        # DC per 10 s
        for i in range(0, len(x) - FS * 10 + 1, FS * 10):
            self.dc.append(float(np.abs(x[i:i + FS * 10].mean(0)).max()))

        # clicks: derivative of the 8 kHz high-passed signal vs its local RMS
        for ch in range(2):
            hp, self.zi_hp[ch] = signal.sosfilt(HP8K_SOS, x[:, ch], zi=self.zi_hp[ch])
            d = np.diff(np.r_[self.prev_hp_last[ch], hp])
            self.prev_hp_last[ch] = hp[-1]
            blk = 480
            nb = len(d) // blk
            if not nb:
                continue
            dd = d[: nb * blk].reshape(nb, blk)
            rms = np.sqrt((dd ** 2).mean(1))
            # local level = max of this block's and neighbours' RMS, excluding the peak itself
            loc = np.maximum.reduce([np.r_[rms[1:], rms[-1]], np.r_[rms[0], rms[:-1]],
                                     np.sqrt(np.maximum((dd ** 2).sum(1) - (dd ** 2).max(1), 0) / (blk - 1))])
            thr = CLICK_K * loc + CLICK_FLOOR
            hit = np.abs(dd).max(1) > thr
            for bi in np.nonzero(hit)[0][:50]:
                self.clicks.append((self.n + bi * blk) / FS)

        # spectrum
        f, p = signal.welch(x.mean(1), fs=FS, nperseg=65536)   # 0.73 Hz bins: no leakage into <20 Hz
        self.psd_f = f
        self.psd_acc = p if self.psd_acc is None else self.psd_acc + p
        self.psd_n += 1

        # chroma, 10 s frames
        for i in range(0, len(x) - FS * 10 + 1, FS * 10):
            seg = x[i:i + FS * 10].mean(1)
            spec = np.abs(np.fft.rfft(seg * np.hanning(len(seg))))
            freqs = np.fft.rfftfreq(len(seg), 1 / FS)
            sel = (freqs > 60) & (freqs < 4000)
            pcs = np.round(12 * np.log2(freqs[sel] / 440.0)).astype(int) % 12
            ch = np.bincount(pcs, weights=spec[sel] ** 2, minlength=12)
            self.chroma.append(ch / (np.linalg.norm(ch) + 1e-12))

        self.n += len(x)

    def results(self, fade_regions, marks=(), jump_lu=None):
        jump_lu = TH["jump_lu"] if jump_lu is None else jump_lu
        r = {}
        st = np.asarray(self.sub["st"])
        mono = np.asarray(self.sub["mono"])
        blocks = np.convolve(st, np.ones(4) / 4, "valid")             # 400 ms, 75% overlap
        mblocks = np.convolve(mono, np.ones(4) / 4, "valid")
        r["duration_s"] = self.n / FS
        r["sample_peak_dbfs"] = 20 * math.log10(self.peak / 32768) if self.peak else -math.inf
        r["fullscale_samples"] = self.fullscale
        r["true_peak_dbtp"] = 20 * math.log10(self.tp) if self.tp else -math.inf
        playing = np.ones(len(blocks), bool)
        for a, b in fade_regions:
            playing[int(a * 10):int(b * 10) + 1] = False
        r["integrated_lufs"] = gated_loudness(blocks[playing] if playing.any() else blocks)
        r["mono_lufs"] = gated_loudness(mblocks[playing[: len(mblocks)]] if playing.any() else mblocks)
        r["mono_delta_lu"] = r["mono_lufs"] - r["integrated_lufs"]

        # short-term (3 s) loudness at 1 s hops
        stl = np.convolve(st, np.ones(30) / 30, "valid")[::10]
        stl_db = -0.691 + 10 * np.log10(np.maximum(stl, 1e-20))
        excl = np.zeros(len(stl_db), bool)
        excl[:6] = True                                               # fade-in
        for a, b in fade_regions:
            excl[max(0, int(a) - 3):int(b) + 1] = True
        quiet = (stl_db < TH["dead_air_lufs"]) & ~excl
        run = longest = 0
        for q in quiet:
            run = run + 1 if q else 0
            longest = max(longest, run)
        r["dead_air_longest_s"] = float(longest)
        intentional = np.zeros(len(stl_db), bool)
        for a, b in marks:                                            # composer-marked stingers / swells
            intentional[max(0, int(a)):int(b) + 4] = True
        jumps = []
        for i in range(30, len(stl)):
            if excl[i] or intentional[i]:
                continue
            prev = stl[i - 30:i].mean()
            if prev > 0 and db(stl[i]) - db(prev) > jump_lu:
                jumps.append(i)
        r["loudness_jumps"] = len(jumps)
        r["loudness_jump_times_s"] = jumps[:20]
        r["dc_worst_dbfs"] = 20 * math.log10(max(self.dc)) if self.dc and max(self.dc) > 0 else -math.inf
        r["clicks"] = len(self.clicks)
        r["click_times_s"] = [round(t, 3) for t in self.clicks[:20]]
        p = self.psd_acc / max(self.psd_n, 1)
        tot = p.sum()
        r["hf_rel_db"] = db(p[self.psd_f > 16000].sum() / tot) if tot else -math.inf
        r["lf_rel_db"] = db(p[self.psd_f < 20].sum() / tot) if tot else -math.inf
        r["_stl_db"] = stl_db
        r["_chroma"] = np.asarray(self.chroma)
        return r


def chroma_stripes(ch, min_gap_frames=30, run=6, thr=0.98):
    if len(ch) < run + min_gap_frames:
        return 0
    s = ch @ ch.T
    n = len(ch)
    stripes = 0
    for off in range(min_gap_frames, n - run):
        diag = np.diagonal(s, off) > thr
        cnt = 0
        for v in diag:
            cnt = cnt + 1 if v else 0
            if cnt == run:
                stripes += 1
    return stripes


def event_checks(csv_path, mode, dur_s):
    r = {}
    ev = np.loadtxt(csv_path, delimiter=",", skiprows=1, dtype=np.int64, ndmin=2)
    if not len(ev):
        return r, [], []
    tick0 = ev[0, 0]
    on = {}
    stuck = 0
    too_long = 0
    notes = []
    gated = 0
    gated_full_since = None
    full_longest = 0
    fades = []
    stop_at = None
    pcs = np.zeros(12)
    keys = set()
    max_len = {}
    steal_max = 0
    marks = []
    ticks = []
    for t, k, a, b, c in ev:
        t -= tick0
        if k == 1:
            if a in on:
                stuck += 1          # re-triggered while still gated
            else:
                gated += 1
            on[a] = t
            if mode == 3 and (int(c) & 0xFF) == 2:   # MOTHER readout ticks: designed clicks
                ticks.append(t / 1000)
            max_len[a] = (int(c) >> 16) * 1000 or MAX_NOTE_MS.get(mode, 600_000)
            if int(b) > 0:          # noise beds carry pitch 0
                notes.append((t, int(b) // 100))
                pcs[(int(b) // 100) % 12] += 1
        elif k in (2,):
            if a in on:
                if t - on[a] > max_len[a] + 1000:
                    too_long += 1
                del on[a]
                gated -= 1
        elif k == 10:
            too_long += 1
        elif k == 9:
            steal_max = max(steal_max, int(b))
        elif k == 11:
            marks.append((t / 1000, (t + max(int(b), 1000)) / 1000))
        elif k == 3:
            keys.add((int(a), int(b)))
        elif k == 6:
            if a == 2:
                stop_at = t
            elif stop_at is not None:
                fades.append((stop_at / 1000, t / 1000))
                stop_at = None
        if gated >= 32:
            gated_full_since = t if gated_full_since is None else gated_full_since
        elif gated_full_since is not None:
            full_longest = max(full_longest, t - gated_full_since)
            gated_full_since = None
    if stop_at is not None:
        fades.append((stop_at / 1000, dur_s))
    end_open = sum(1 for v, t in on.items() if dur_s * 1000 - t > max_len[v] + 1000)
    r["steal_max_db"] = 20 * math.log10(steal_max / 65536) if steal_max else -math.inf
    r["notes"] = len(notes)
    r["retriggered_while_gated"] = stuck
    r["notes_over_max_len"] = too_long + end_open
    r["all_voices_gated_longest_s"] = full_longest / 1000
    r["keys_visited"] = len(keys)
    tot = pcs.sum()
    if tot:
        p = pcs / tot
        r["pitch_class_entropy_bits"] = float(-(p[p > 0] * np.log2(p[p > 0])).sum())
    # repetition: 30 s windows of (pitch, quantised delta) with >= 4 notes
    seen = {}
    dup = 0
    notes_arr = notes
    j = 0
    for i, (t, _) in enumerate(notes_arr):
        while j < len(notes_arr) and notes_arr[j][0] < t + 30000:
            j += 1
        win = notes_arr[i:j]
        if len(win) < 4:
            continue
        sig = []
        prev = t
        for (tt, p_) in win:
            sig.append((p_, (tt - prev) // 100))
            prev = tt
        h = hashlib.sha1(repr(sig).encode()).hexdigest()
        if h in seen and t - seen[h] >= 30000:
            dup += 1
        seen.setdefault(h, t)
    r["repeated_30s_windows"] = dup
    r["intentional_marks"] = len(marks)
    r["_ticks"] = ticks
    return r, fades, marks


def check(r, stress=False):
    fails = []
    def f(cond, msg):
        if not cond:
            fails.append(msg)
    f(r["sample_peak_dbfs"] <= TH["sample_peak_dbfs"], "sample peak")
    f(r["fullscale_samples"] == 0, "full-scale samples")
    f(r["true_peak_dbtp"] <= TH["true_peak_dbtp"], "true peak")
    if not stress:
        f(abs(r["integrated_lufs"] - TH["lufs_target"]) <= TH["lufs_tol"], "integrated loudness")
    f(r["dead_air_longest_s"] <= TH["dead_air_max_s"], "dead air")
    if not stress:
        f(r["loudness_jumps"] == 0, "loudness jumps")
    f(r["dc_worst_dbfs"] < TH["dc_dbfs"], "DC offset")
    f(r["clicks"] == 0, "clicks")
    f(r["hf_rel_db"] < TH["hf_rel_db"], "HF energy (aliasing)")
    f(r["lf_rel_db"] < TH["lf_rel_db"], "LF energy (<20 Hz)")
    f(abs(r["mono_delta_lu"]) <= TH["mono_lu"], "mono compatibility")
    if "notes" in r:
        f(r["retriggered_while_gated"] == 0 and r["notes_over_max_len"] == 0, "stuck notes")
        f(r["steal_max_db"] <= STEAL_MAX_DB, "audible voice steal")
        f(r["all_voices_gated_longest_s"] <= TH["voice_budget_max_s"], "voices at budget")
        f(r["repeated_30s_windows"] == 0, "event repetition")
        if r["duration_s"] >= 6 * 3600:
            f(r["keys_visited"] >= TH["min_keys"], "keys visited")
    return fails


def plot(name, r, outdir):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return
    fig, ax = plt.subplots(2, 1, figsize=(10, 6))
    t = np.arange(len(r["_stl_db"])) / 60
    ax[0].plot(t, r["_stl_db"], lw=0.6)
    ax[0].set_ylabel("short-term LUFS")
    ax[0].set_xlabel("minutes")
    ax[0].set_ylim(-70, 0)
    ax[0].axhline(TH["lufs_target"], color="gray", ls="--", lw=0.5)
    ch = r["_chroma"]
    if len(ch) > 1:
        ax[1].imshow(ch @ ch.T, origin="lower", aspect="auto", vmin=0.5, vmax=1)
        ax[1].set_title("chroma self-similarity (10 s frames)")
    fig.tight_layout()
    fig.savefig(os.path.join(outdir, f"{name}.png"), dpi=90)
    plt.close(fig)


def mode_of(path):
    b = os.path.basename(path).lower()
    return 0 if "drift" in b else 1 if "tides" in b else 2 if "hush" in b else 3 if "mother" in b else -1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wavs", nargs="+")
    ap.add_argument("--report", default=os.path.join("reports", dt.date.today().isoformat()))
    ap.add_argument("--plots", action="store_true")
    ap.add_argument("--no-chroma", action="store_true")
    ap.add_argument("--stress", action="store_true", help="switching stress run: skip loudness-target and loudness-jump checks")
    args = ap.parse_args()
    os.makedirs(args.report, exist_ok=True)

    summary = {}
    any_fail = False
    for path in args.wavs:
        a = Analyzer()
        info = sf.info(path)
        for blk in sf.blocks(path, blocksize=CHUNK, dtype="int16", always_2d=True):
            a.feed(blk)
        csv = os.path.splitext(path)[0] + ".csv"
        ev, fades, marks = ({}, [], [])
        if os.path.exists(csv):
            ev, fades, marks = event_checks(csv, mode_of(path), info.duration)
        r = a.results(fades, marks, TH["jump_lu_mother"] if mode_of(path) == 3 else None)
        ticks = np.asarray(ev.pop("_ticks", []))
        if len(ticks) and a.clicks:
            # clicks that coincide (+/-15 ms) with a designed MOTHER readout tick are not faults
            real = [t for t in a.clicks if not np.any(np.abs(ticks - t) < 0.015)]
            r["clicks_designed_ticks"] = len(a.clicks) - len(real)
            r["clicks"] = len(real)
            r["click_times_s"] = [round(t, 3) for t in real[:20]]
        r.update(ev)
        if not args.no_chroma:
            r["chroma_stripes"] = chroma_stripes(r["_chroma"])
        name = os.path.splitext(os.path.basename(path))[0]
        if args.plots:
            plot(name, r, args.report)
        r = {k: v for k, v in r.items() if not k.startswith("_")}
        fails = check(r, args.stress)
        r["fails"] = fails
        any_fail |= bool(fails)
        summary[name] = r
        print(f"== {name}  ({r['duration_s'] / 60:.1f} min)")
        for k, v in r.items():
            if k in ("fails", "click_times_s", "loudness_jump_times_s"):
                continue
            print(f"   {k:28s} {v:.2f}" if isinstance(v, float) else f"   {k:28s} {v}")
        if r.get("click_times_s"):
            print("   click times (s):", r["click_times_s"])
        if r.get("loudness_jump_times_s"):
            print("   jump times (s):", r["loudness_jump_times_s"])
        print("   RESULT:", "PASS" if not fails else "FAIL: " + ", ".join(fails))

    louds = [v["integrated_lufs"] for v in summary.values() if math.isfinite(v["integrated_lufs"])]
    if len(louds) > 1:
        spread = max(louds) - min(louds)
        print(f"== mode loudness spread {spread:.2f} LU (limit {2 * TH['mode_spread_lu']:.1f} LU total, +/-{TH['mode_spread_lu']})")
        summary["_mode_spread_lu"] = spread
        if spread > 2 * TH["mode_spread_lu"]:
            any_fail = True
    with open(os.path.join(args.report, "summary.json"), "w") as f:
        json.dump(summary, f, indent=2, default=float)
    sys.exit(1 if any_fail else 0)


if __name__ == "__main__":
    main()
