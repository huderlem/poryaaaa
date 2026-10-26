#!/usr/bin/env python3
"""Diff two accuracy reports (JSON from accuracy.py): per-test change in null depth.

  compare_reports.py <before.json> <after.json> [--threshold 1.0]

Prints every test with before/after raw null, worst-window and drift values
and flags regressions (after worse than before by more than --threshold dB).
Exit status 1 if any regression.
"""
import argparse
import json
import math


def fmt(v):
    if v is None or (isinstance(v, float) and math.isnan(v)):
        return "    -  "
    if isinstance(v, float) and math.isinf(v):
        return "  -inf " if v < 0 else "   inf "
    return "%7.1f" % v


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("before")
    ap.add_argument("after")
    ap.add_argument("--threshold", type=float, default=1.0, help="dB of worsening that counts as a regression")
    a = ap.parse_args()
    rb, ra = json.load(open(a.before)), json.load(open(a.after))
    for key in ("input", "mixer"):       # the baseline families: song / midi input, vanilla / hq test ROM
        vb, va = rb.get(key, "song" if key == "input" else "vanilla"), ra.get(key, "song" if key == "input" else "vanilla")
        if vb != va:
            raise SystemExit(f"refusing to compare reports of different {key} ({vb} vs {va}): separate baseline families")
    before = {r["name"]: r for r in rb["results"]}
    after = {r["name"]: r for r in ra["results"]}
    regressions = 0
    print(f"{'test':18s} {'null before':>11s} {'after':>7s} {'delta':>7s} | {'worst before':>12s} {'after':>7s} | {'drift b/a':>13s} | note")
    for name in list(before) + [n for n in after if n not in before]:
        b, c = before.get(name), after.get(name)
        if not b or not c:
            print(f"{name:18s} {'(only in before)' if b else '(only in after)'}")
            continue
        d = c["null_db"] - b["null_db"] if not (math.isnan(c["null_db"]) or math.isnan(b["null_db"])) else float("nan")
        note = ""
        if not math.isnan(d) and d > a.threshold:
            note = "REGRESSION"
            regressions += 1
        elif not math.isnan(d) and d < -a.threshold:
            note = "improved"
        print(f"{name:18s} {fmt(b['null_db'])}     {fmt(c['null_db'])} {fmt(d)} | {fmt(b['worst_db'])}      {fmt(c['worst_db'])} | "
              f"{fmt(b.get('drift_ms_per_s'))}{fmt(c.get('drift_ms_per_s'))} | {note}")
    print(f"\n{regressions} regression(s) over {a.threshold} dB")
    return 1 if regressions else 0


if __name__ == "__main__":
    raise SystemExit(main())
