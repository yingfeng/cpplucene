"""Per-band p50 report for a `run_bench.py bench --csv` dump.

Usage:
    analyze_bench.py [csv] [driver=clucene-binary] [index-dir] [queries.jsonl]

Bands are derived from the document frequency of every query, probed once with a
single `COUNT` pass through the driver.
"""
import csv
import json
import subprocess
import sys

CSV = sys.argv[1] if len(sys.argv) > 1 else "/tmp/bench-final.csv"
DRIVER = sys.argv[2] if len(sys.argv) > 2 else "/home/infominer/codebase/workspace/clucene/build-rel/benchmark/clucene_sbg"
INDEX = sys.argv[3] if len(sys.argv) > 3 else "/home/infominer/codebase/benchmark/sbg/index-clucene-wiki"
QUERIES = sys.argv[4] if len(sys.argv) > 4 else "/home/infominer/codebase/workspace/clucene/benchmark/sbg/results/queries-enwiki.jsonl"

queries = [json.loads(line)["query"] for line in open(QUERIES)]
payload = "".join(f"COUNT\t{q}\n" for q in sorted(set(queries)))
out = subprocess.run(
    [DRIVER, "serve", INDEX],
    input=payload.encode(),
    stdout=subprocess.PIPE,
    stderr=subprocess.DEVNULL,
).stdout.decode().split()
df = {q: int(c) for q, c in zip(sorted(set(queries)), out)}


def band(q):
    if q.startswith('"'):
        return "phrase"
    if q.startswith("+"):
        return "intersection"
    n = df[q]
    if n < 1000:
        return "term rare (<1k docs)"
    if n < 100000:
        return "term mid (1k-100k)"
    return "term common (>=100k)"


rows = []
with open(CSV) as f:
    for r in csv.DictReader(f):
        rows.append((r["mode"], r["query"], r["engine"], float(r["seconds"]), band(r["query"])))

ENGINES = sorted({r[2] for r in rows})
MODES = [m for m in ["COUNT", "TOP_10", "TOP_100", "TOP_1000"] if any(r[0] == m for r in rows)]
BANDS = ["term rare (<1k docs)", "term mid (1k-100k)", "term common (>=100k)", "intersection", "phrase"]


def vals_for(mode, engine, band_name):
    return [v for m, q, e, v, b in rows if m == mode and e == engine and b == band_name]


def pct(vals, p):
    if not vals:
        # A band with no query in this run (the 1M set has no >=100k-doc term).
        return 0.0
    s = sorted(vals)
    return s[min(len(s) - 1, int(len(s) * p))]


print(f"csv={CSV}  engines={ENGINES}")
for mode in MODES:
    print(f"\n== {mode}: p50 microseconds / total seconds per query band")
    header = f"{'band':<24}{'n':>5}"
    for e in ENGINES:
        header += f"{e[:8] + ' p50us':>16}"
    for e in ENGINES:
        header += f"{e[:8] + ' sec':>13}"
    if len(ENGINES) > 1:
        header += "   ratio(base/first)"
    print(header)
    base = ENGINES[0]
    sums = {e: 0.0 for e in ENGINES}
    for b in BANDS:
        n = len({q for m, q, e, v, bb in rows if m == mode and bb == b})
        if n == 0:
            continue
        line = f"{b:<24}{n:>5}"
        p50s = {}
        for e in ENGINES:
            vals = vals_for(mode, e, b)
            p50s[e] = pct(vals, 0.5) if vals else 0.0
            line += f"{p50s[e] * 1e6:16.1f}" if vals else f"{'-':>16}"
        for e in ENGINES:
            vals = vals_for(mode, e, b)
            sums[e] += sum(vals)
            line += f"{sum(vals):13.2f}" if vals else f"{'-':>13}"
        if len(ENGINES) > 1 and p50s[base] > 0:
            line += "   " + " ".join(f"{e}={p50s[e] / p50s[base]:.2f}" for e in ENGINES[1:])
        print(line)
    if len(ENGINES) > 1:
        print(f"{'p50 sum':<24}{'':>5}" + "".join(f"{sum(pct(vals_for(mode, e, b), 0.5) for b in BANDS) * 1e6:16.1f}" for e in ENGINES)
              + "   " + " ".join(f"{e}={sum(pct(vals_for(mode, e, b), 0.5) for b in BANDS) / sum(pct(vals_for(mode, base, b), 0.5) for b in BANDS):.3f}x" for e in ENGINES[1:]))
