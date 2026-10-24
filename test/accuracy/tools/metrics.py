"""Null-test metrics for the poryaaaa accuracy harness.

Compares a rendering under test against an mgba reference at the same sample
rate: time-aligns the two by cross-correlation, then reports the RMS of the
difference relative to the reference, in dB, over the whole file and over the
worst one-second window (the perceptual-null criterion from
docs/ACCURACY_PLAN.md). Metrics are computed twice: raw, and after a single
least-squares gain match (reported separately so scale errors can be told
apart from waveform errors; the gate uses the raw numbers).
"""
import math

import numpy as np
from scipy.io import wavfile
from scipy.signal import fftconvolve


def load_wav(path):
    """Return (rate, float32 array of shape (n, 2)) in -1..1. .flac needs soundfile."""
    if path.lower().endswith(".flac"):
        try:
            import soundfile
        except ImportError:
            raise SystemExit("reading .flac references needs the python 'soundfile' module")
        data, rate = soundfile.read(path, dtype="int16", always_2d=True)
    else:
        rate, data = wavfile.read(path)
    if data.dtype == np.int16:
        x = data.astype(np.float32) / 32768.0
    elif data.dtype == np.int32:
        x = data.astype(np.float32) / 2147483648.0
    elif data.dtype == np.uint8:
        x = (data.astype(np.float32) - 128.0) / 128.0
    else:
        x = data.astype(np.float32)
    if x.ndim == 1:
        x = np.stack([x, x], axis=1)
    elif x.shape[1] > 2:
        x = x[:, :2]
    return rate, x


def rms(x):
    if x.size == 0:
        return 0.0
    return float(np.sqrt(np.mean(np.square(x, dtype=np.float64))))


def db(ratio):
    if ratio <= 0.0:
        return -math.inf
    return 20.0 * math.log10(ratio)


def _mono(x):
    return x.mean(axis=1).astype(np.float64)


def _xcorr_argmax(a, b, lo_lag, hi_lag):
    """argmax over lag in [lo_lag, hi_lag] of sum_i a[i] * b[i - lag]."""
    c = fftconvolve(a, b[::-1], mode="full")
    centre = len(b) - 1
    lo = max(0, centre + lo_lag)
    hi = min(len(c), centre + hi_lag + 1)
    if hi <= lo:
        return 0
    return lo + int(np.argmax(c[lo:hi])) - centre


def find_lag(ref, test, rate, min_lag_s=-0.02, max_lag_s=0.2, fine_s=0.005, env_ms=1.0):
    """Lag (samples) by which `test` must be delayed to best match `ref`.

    Two stages, because a raw waveform cross-correlation of two renderings
    that differ in pitch, LFO phase or timbre happily locks onto a wrong
    period hundreds of ms away: first the energy envelopes (`env_ms` blocks)
    are correlated within [min_lag_s, max_lag_s], then the waveforms are
    correlated within +-fine_s of that coarse lag. The default window is
    asymmetric because of how the harness renders: poryaaaa starts the song
    at t = 0 while the ROM starts it on frame 1 and the first DMA buffer
    plays a few frames later, so the reference always lags by 50 to 120 ms.
    """
    a = _mono(ref)
    b = _mono(test)
    n = min(len(a), len(b))
    a = a[:n] - a[:n].mean()
    b = b[:n] - b[:n].mean()
    if rms(a) == 0.0 or rms(b) == 0.0:
        return 0
    blk = max(1, int(rate * env_ms / 1000.0))
    nb = n // blk
    ea = np.square(a[:nb * blk]).reshape(nb, blk).mean(axis=1)
    eb = np.square(b[:nb * blk]).reshape(nb, blk).mean(axis=1)
    ea -= ea.mean()
    eb -= eb.mean()
    coarse = _xcorr_argmax(ea, eb, int(min_lag_s * rate / blk), int(max_lag_s * rate / blk)) * blk
    fine = int(fine_s * rate)
    return _xcorr_argmax(a, b, coarse - fine, coarse + fine)


def shift_and_trim(ref, test, lag):
    """Apply `lag` (see find_lag) and cut both to the common overlap."""
    if lag > 0:
        test = np.concatenate([np.zeros((lag, test.shape[1]), test.dtype), test])
    elif lag < 0:
        test = test[-lag:]
    n = min(len(ref), len(test))
    return ref[:n], test[:n]


def worst_window(ref, test, rate, window_s=1.0, hop_s=0.1, floor_dbfs=-60.0):
    """Worst (highest) null depth over sliding windows where the reference is audible."""
    w = int(window_s * rate)
    h = max(1, int(hop_s * rate))
    n = len(ref)
    if n < w:
        w = n
    worst = -math.inf
    worst_t = 0.0
    floor = 10 ** (floor_dbfs / 20.0)
    for start in range(0, n - w + 1, h):
        r = ref[start:start + w]
        rr = rms(r)
        if rr < floor:
            continue
        d = db(rms(r - test[start:start + w]) / rr)
        if d > worst:
            worst = d
            worst_t = start / rate
    return worst, worst_t


def lag_drift(ref, test, rate, segment_s=4.0, **lag_kw):
    """Slope of the best lag over time, in ms per second (tempo mismatch).

    Each `segment_s` chunk is aligned on its own with a wide window; the
    slope of a least-squares line through those lags is returned, with the
    per-segment lags in ms. None when the file is shorter than two segments.
    """
    seg = int(segment_s * rate)
    n = min(len(ref), len(test))
    if n < 2 * seg:
        return None, []
    lags, times = [], []
    for start in range(0, n - seg + 1, seg):
        lag = find_lag(ref[start:start + seg], test[start:start + seg], rate,
                       min_lag_s=-0.5, max_lag_s=0.5, **lag_kw)
        lags.append(1000.0 * lag / rate)
        times.append((start + seg / 2) / rate)
    slope = float(np.polyfit(times, lags, 1)[0])
    return slope, lags


def compare(ref, test, rate, min_lag_s=-0.02, max_lag_s=0.2, window_s=1.0, drift=True):
    """Full comparison. Returns a dict of metrics (dB values, lag, gain, drift)."""
    lag = find_lag(ref, test, rate, min_lag_s, max_lag_s)
    r, t = shift_and_trim(ref, test, lag)
    ref_rms = rms(r)
    out = {
        "lag_samples": int(lag),
        "lag_ms": 1000.0 * lag / rate,
        "seconds": len(r) / rate,
        "ref_rms_dbfs": db(ref_rms),
        "test_rms_dbfs": db(rms(t)),
        "ref_peak_dbfs": db(float(np.max(np.abs(r))) if r.size else 0.0),
        "test_peak_dbfs": db(float(np.max(np.abs(t))) if t.size else 0.0),
    }
    if drift:
        slope, seg_lags = lag_drift(ref, test, rate)
        out["drift_ms_per_s"] = slope
        out["segment_lags_ms"] = seg_lags
    if ref_rms == 0.0:
        out.update(null_db=math.nan, worst_db=math.nan, worst_t=0.0,
                   gain=1.0, gain_db=0.0, gm_null_db=math.nan, gm_worst_db=math.nan, gm_worst_t=0.0)
        return out
    out["null_db"] = db(rms(r - t) / ref_rms)
    out["worst_db"], out["worst_t"] = worst_window(r, t, rate, window_s)
    tt = float(np.sum(np.square(t, dtype=np.float64)))
    gain = float(np.sum(r.astype(np.float64) * t) / tt) if tt > 0 else 0.0
    out["gain"] = gain
    out["gain_db"] = db(gain) if gain > 0 else -math.inf
    tg = t * gain
    out["gm_null_db"] = db(rms(r - tg) / ref_rms)
    out["gm_worst_db"], out["gm_worst_t"] = worst_window(r, tg, rate, window_s)
    return out


def compare_files(ref_path, test_path, **kw):
    rate, ref = load_wav(ref_path)
    rate2, test = load_wav(test_path)
    if rate != rate2:
        raise ValueError(f"sample rate mismatch: {ref_path} {rate} vs {test_path} {rate2}")
    return compare(ref, test, rate, **kw)
