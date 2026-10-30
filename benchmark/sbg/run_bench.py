#!/usr/bin/env python3
"""Run the wikipedia search benchmark across several Lucene implementations.

Every driver speaks the same subprocess protocol:

    <driver> <index|serve|verify> <index-dir>

with one `COMMAND<TAB>QUERY` line per request and one line per response.  The
requests are the pinned 962-query set of the search-benchmark-game, in five
modes (COUNT, TOP_10, TOP_100, TOP_1000, TOP_100_COUNT).

`verify` sends every request to every (driver, index) pair and compares the full
JSON results: ordered stored ids, internal doc ids, raw f32 score bits, the hit
count and its relation.  Running the matrix over both drivers *and* both
indexes proves two things at once:

  * the engines agree on the same physical index, and
  * each engine produces the same results from either engine's index,
    i.e. the indexes are interchangeable.

`bench` then times one index per driver with the same request stream.

Examples
--------
    python3 benchmark/sbg/run_bench.py verify \
        --driver clucene=build-rel/benchmark/clucene_sbg \
        --driver rlucene=/path/to/the-cost-diagnostic \
        --index clucene=/data/index-clucene --index rlucene=/data/index-rlucene \
        --queries /path/to/queries.jsonl

    python3 benchmark/sbg/run_bench.py bench \
        --driver clucene=... --driver rlucene=... \
        --index clucene=/data/index-clucene \
        --queries /path/to/queries.jsonl --rounds 12
"""

from __future__ import annotations

import argparse
import json
import os
import random
import statistics
import subprocess
import sys
import time

MODES = ["COUNT", "TOP_10", "TOP_100", "TOP_1000", "TOP_100_COUNT"]


def parse_pairs(values: list[str], what: str) -> list[tuple[str, str]]:
    pairs = []
    for value in values:
        name, sep, path = value.partition("=")
        if not sep or not name or not path:
            raise SystemExit(f"--{what} expects name=path, got {value!r}")
        if not os.path.exists(path):
            raise SystemExit(f"--{what} {name}: {path} does not exist")
        pairs.append((name, os.path.abspath(path)))
    return pairs


def load_queries(path: str) -> list[str]:
    queries = []
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            queries.append(json.loads(line)["query"])
    return queries


def requests_for(queries: list[str], modes: list[str]) -> list[tuple[str, str]]:
    return [(mode, query) for mode in modes for query in queries]


class Driver:
    """One driver process in `mode` over `index`."""

    def __init__(self, name: str, binary: str, mode: str, index: str):
        self.name = name
        self.mode = mode
        self.process = subprocess.Popen(
            [binary, mode, index],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        # The drivers print index geometry to stderr; keep it for the report.
        self.banner: list[str] = []

    def ask(self, command: str, query: str) -> tuple[str, float]:
        started = time.perf_counter()
        self.process.stdin.write(f"{command}\t{query}\n")
        self.process.stdin.flush()
        line = self.process.stdout.readline()
        elapsed = time.perf_counter() - started
        if not line:
            stderr = self.process.stderr.read()
            raise SystemExit(f"{self.name} died: {stderr}")
        return line.rstrip("\n"), elapsed

    def close(self) -> None:
        if self.process.poll() is None:
            self.process.stdin.close()
            try:
                self.process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                self.process.kill()
        self.banner = self.process.stderr.read().splitlines() if self.process.stderr else []


def canonical(response: str, ignore_score_bits: bool = False) -> str:
    """Normalise a verify response so drivers may order JSON keys freely."""
    if not response.startswith("{"):
        return response
    try:
        parsed = json.loads(response)
    except json.JSONDecodeError:
        return response
    if ignore_score_bits:
        for hit in parsed.get("docs", []):
            hit.pop("score_bits", None)
    return json.dumps(parsed, sort_keys=True, separators=(",", ":"))


def run_verify(args) -> int:
    drivers = parse_pairs(args.driver, "driver")
    indexes = parse_pairs(args.index, "index")
    queries = load_queries(args.queries)
    modes = args.modes.split(",")
    requests = requests_for(queries, modes)
    print(f"{len(queries)} queries x {len(modes)} modes = {len(requests)} requests per pair", flush=True)

    results: dict[tuple[str, str], list[str]] = {}
    for driver_name, binary in drivers:
        for index_name, index_path in indexes:
            key = (driver_name, index_name)
            driver = Driver(f"{driver_name}/{index_name}", binary, "verify", index_path)
            responses = []
            try:
                for position, (command, query) in enumerate(requests):
                    response, _ = driver.ask(command, query)
                    responses.append(canonical(response, args.ignore_score_bits))
                    if (position + 1) % 500 == 0:
                        print(f"  {key[0]} on {key[1]}: {position + 1}/{len(requests)}", flush=True)
            finally:
                driver.close()
            for line in driver.banner:
                if line.startswith("INDEX"):
                    print(f"  {key[0]} on {key[1]}: {line}", flush=True)
            results[key] = responses

    baseline_key = next(iter(results))
    baseline = results[baseline_key]
    print(f"\nbaseline: {baseline_key[0]} on {baseline_key[1]}")
    failed = False
    for key, responses in results.items():
        if key == baseline_key:
            continue
        mismatches = []
        for position, (got, want) in enumerate(zip(responses, baseline)):
            if got == want:
                continue
            mismatches.append(position)
            if len(mismatches) <= 3:
                command, query = requests[position]
                print(f"  MISMATCH {key[0]} on {key[1]} [{command}] {query!r}")
                print(f"    baseline: {want[:200]}")
                print(f"    actual  : {got[:200]}")
        if mismatches:
            failed = True
            print(f"  {key[0]} on {key[1]}: {len(mismatches)}/{len(requests)} responses differ")
        else:
            print(f"  {key[0]} on {key[1]}: all {len(responses)} responses identical")

    if failed:
        print("\nRESULT: results differ")
        return 1
    print(f"\nRESULT: {len(results)} (driver, index) pairs agree on all {len(requests)} requests")
    return 0


def run_bench(args) -> int:
    drivers = parse_pairs(args.driver, "driver")
    indexes = parse_pairs(args.index, "index")
    index_by_name = dict(indexes)
    queries = load_queries(args.queries)
    modes = args.modes.split(",")
    rounds = args.rounds
    warmup_seconds = args.warmup
    seed = args.seed

    timings: dict[str, dict[str, list[float]]] = {
        name: {mode: [] for mode in modes} for name, _ in drivers
    }
    samples: list[tuple[int, str, str, str, float]] = []

    for batch in range(args.batches):
        order = list(range(len(queries)))
        random.Random(seed + batch).shuffle(order)
        sessions = []
        for name, binary in drivers:
            index_path = index_by_name[args.index_for.get(name, indexes[0][0])]
            sessions.append((name, Driver(name, binary, "serve", index_path)))

        try:
            # Warmup: drive every engine through a mixed request stream.
            deadline = time.perf_counter() + warmup_seconds
            position = 0
            while time.perf_counter() < deadline:
                query = queries[order[position % len(order)]]
                mode = modes[position % len(modes)]
                for _, session in sessions:
                    session.ask(mode, query)
                position += 1
            print(f"batch {batch + 1}: warmup done ({position} mixed requests)", flush=True)

            for round_index in range(rounds):
                for query_index in order:
                    query = queries[query_index]
                    for mode_index, mode in enumerate(modes):
                        measurements = []
                        # Rotate the engine order so no engine always runs first
                        # (the first request of a group sets the CPU frequency
                        # and warms the page cache for the others).
                        shift = (query_index + mode_index + round_index) % len(sessions)
                        rotated = sessions[shift:] + sessions[:shift]
                        for name, session in rotated:
                            _, elapsed = session.ask(mode, query)
                            measurements.append((name, elapsed))
                        for name, elapsed in measurements:
                            timings[name][mode].append(elapsed)
                            samples.append((batch, mode, query, name, elapsed))
                print(f"batch {batch + 1}: round {round_index + 1}/{rounds}", flush=True)
        finally:
            for _, session in sessions:
                session.close()
                for line in session.banner:
                    if line.startswith("INDEX"):
                        print(f"  {session.name}: {line}", flush=True)

    if args.csv:
        with open(args.csv, "w", encoding="utf-8") as sink:
            sink.write("batch,mode,query,engine,seconds\n")
            for batch, mode, query, name, elapsed in samples:
                sink.write(f'{batch},{mode},"{query.replace(chr(34), chr(34) * 2)}",{name},{elapsed:.9f}\n')
        print(f"\nsamples written to {args.csv}")

    print("\nmode            engine        requests    p50 ms    p90 ms    mean ms")
    summary: dict[tuple[str, str], float] = {}
    for mode in modes:
        for name, _ in drivers:
            samples = sorted(timings[name][mode])
            if not samples:
                continue
            p50 = statistics.median(samples)
            p90 = samples[min(len(samples) - 1, int(len(samples) * 0.9))]
            mean = statistics.fmean(samples)
            summary[(name, mode)] = p50
            print(f"{mode:<15} {name:<12} {len(samples):>9} {p50 * 1e3:9.3f} {p90 * 1e3:9.3f} {mean * 1e3:9.3f}")

    names = [name for name, _ in drivers]
    if len(names) == 2:
        first, second = names
        print(f"\nrelative p50 ({second} / {first}); > 1 means {first} is faster")
        for mode in modes:
            if (first, mode) in summary and (second, mode) in summary:
                ratio = summary[(second, mode)] / summary[(first, mode)]
                print(f"  {mode:<15} {ratio:6.3f}x")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    subparsers = parser.add_subparsers(dest="command", required=True)

    def add_common(sub):
        sub.add_argument("--driver", action="append", required=True, help="name=/path/to/driver")
        sub.add_argument("--index", action="append", required=True, help="name=/path/to/index")
        sub.add_argument("--queries", required=True, help="queries.jsonl")
        sub.add_argument("--modes", default=",".join(MODES))

    verify = subparsers.add_parser("verify", help="compare full results across drivers and indexes")
    add_common(verify)
    verify.add_argument(
        "--ignore-score-bits",
        action="store_true",
        help="compare hit sets and counts only, not the last bit of each score",
    )
    verify.set_defaults(func=run_verify)

    bench = subparsers.add_parser("bench", help="time the drivers")
    add_common(bench)
    bench.add_argument("--csv", help="write every individual measurement to this CSV")
    bench.add_argument("--rounds", type=int, default=12)
    bench.add_argument("--batches", type=int, default=2)
    bench.add_argument("--warmup", type=float, default=60.0, help="warmup seconds per batch")
    bench.add_argument("--seed", type=int, default=2026101900)
    bench.add_argument(
        "--index-for",
        action="append",
        default=[],
        metavar="DRIVER=INDEX",
        help="which index a driver should search (default: the first --index)",
    )
    bench.set_defaults(func=run_bench)

    args = parser.parse_args()
    if getattr(args, "index_for", None):
        args.index_for = dict(pair.split("=", 1) for pair in args.index_for)
    else:
        args.index_for = {}
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
