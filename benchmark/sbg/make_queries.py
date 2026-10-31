#!/usr/bin/env python3
"""Build a query set for the search-benchmark-game drivers.

The drivers share one wire protocol (`COMMAND<TAB>QUERY`), but only the
plain-term / quoted-phrase / `+a +b` forms are understood by every parser:
the classic `QueryParser` used by the Clucene and Java drivers as well as the
small hand-written parser of the Rust driver.  So only those forms are emitted.

Two stages:

``candidates``
    Samples the first ``--docs`` documents of a prepared corpus and writes a
    candidate pool: single terms, two-term intersections and two-word phrases,
    all of which are known to occur in the sample.

``final``
    Probes the candidate pool against a running index (through a driver) with
    ``COUNT``, then keeps a spread of rare / mid / frequent terms so that the
    benchmark exercises both the exhaustive and the competitive-skipping paths.

``subset``
    Extracts one query form from a pinned set, so a driver comparison can be
    run on phrases alone (or on intersections alone) with the same protocol.

    python3 benchmark/sbg/make_queries.py candidates --corpus corpus-1m.jsonl \
        --out /tmp/candidates.jsonl
    python3 benchmark/sbg/make_queries.py final --candidates /tmp/candidates.jsonl \
        --driver ./clucene_sbg --index /data/index --out queries.jsonl
    python3 benchmark/sbg/make_queries.py subset \
        --queries benchmark/sbg/results/queries-enwiki.jsonl --band phrase \
        --out benchmark/sbg/results/queries-phrase.jsonl
"""

from __future__ import annotations

import argparse
import collections
import json
import random
import re
import subprocess
import sys

WORD = re.compile("[a-z]{3,}")

#: The query forms every driver's parser understands: a single term, a `+a +b`
#: intersection and a quoted phrase.
BANDS = ("term", "intersection", "phrase")


def query_band(query: str) -> str:
    """Classifies a pinned query by the form it uses."""
    if query.startswith('"'):
        return "phrase"
    if query.startswith("+"):
        return "intersection"
    return "term"


def read_docs(path: str, limit: int) -> list[list[str]]:
    docs: list[list[str]] = []
    with open(path, "r", encoding="utf-8") as source:
        for line in source:
            if len(docs) >= limit:
                break
            record = json.loads(line)
            docs.append(WORD.findall(record.get("text", "")))
    return docs


def candidates(args: argparse.Namespace) -> int:
    docs = read_docs(args.corpus, args.docs)
    if not docs:
        print("no documents read", file=sys.stderr)
        return 1

    doc_freq: collections.Counter[str] = collections.Counter()
    for tokens in docs:
        doc_freq.update(set(tokens))

    # A spread across the frequency range: the head exercises competitive
    # skipping and exhaustive counting, the tail is dominated by per-query
    # overhead.
    ranked = [term for term, _ in doc_freq.most_common()]
    third = max(1, len(ranked) // 3)
    head = ranked[:third]
    middle_rank = ranked[third : 2 * third]
    tail = [term for term, count in doc_freq.items() if 2 <= count <= 8]

    rng = random.Random(42)
    share = max(1, args.singles // 3)
    single = (
        rng.sample(head, min(share, len(head)))
        + rng.sample(middle_rank, min(share, len(middle_rank)))
        + rng.sample(tail, min(share, len(tail)))
    )
    pool = head + middle_rank

    # Two-term intersections: only pairs that really co-occur, otherwise every
    # engine trivially agrees on zero hits.
    by_term: dict[str, set[int]] = collections.defaultdict(set)
    for index, tokens in enumerate(docs):
        for token in set(tokens):
            by_term[token].add(index)

    pairs: list[tuple[str, str]] = []
    attempts = 0
    while len(pairs) < args.pairs and attempts < args.pairs * 200:
        attempts += 1
        left, right = rng.sample(pool, 2)
        if left == right:
            continue
        shared = by_term[left] & by_term[right]
        if not shared:
            continue
        pairs.append((left, right))

    phrases: list[tuple[str, str]] = []
    attempts = 0
    while len(phrases) < args.phrases and attempts < args.phrases * 200:
        attempts += 1
        tokens = docs[rng.randrange(len(docs))]
        if len(tokens) < 4:
            continue
        start = rng.randrange(len(tokens) - 1)
        phrases.append((tokens[start], tokens[start + 1]))

    with open(args.out, "w", encoding="utf-8") as sink:
        for term in single:
            sink.write(json.dumps({"query": term, "kind": "term"}) + "\n")
        for left, right in pairs:
            sink.write(json.dumps({"query": f"+{left} +{right}", "kind": "intersection"}) + "\n")
        for left, right in phrases:
            sink.write(json.dumps({"query": f'"{left} {right}"', "kind": "phrase"}) + "\n")
    print(
        f"{len(single)} terms, {len(pairs)} intersections, {len(phrases)} phrases -> {args.out}",
        file=sys.stderr,
    )
    return 0


def probe(driver: str, index: str, queries: list[str]) -> list[int] | None:
    payload = "".join(f"COUNT\t{query}\n" for query in queries)
    result = subprocess.run(
        [driver, "serve", index],
        input=payload.encode(),
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    counts = []
    for line in result.stdout.decode(errors="replace").splitlines():
        line = line.strip()
        if not line:
            continue
        counts.append(int(line))
    if len(counts) != len(queries):
        return None
    return counts


def final(args: argparse.Namespace) -> int:
    entries = [json.loads(line) for line in open(args.candidates, "r", encoding="utf-8")]
    counts = probe(args.driver, args.index, [entry["query"] for entry in entries])
    if counts is None:
        print("probing failed", file=sys.stderr)
        return 1

    terms = [(entry, count) for entry, count in zip(entries, counts) if entry["kind"] == "term"]
    terms.sort(key=lambda pair: pair[1])

    def take(low: int, high: int, wanted: int) -> list[dict]:
        picked = [entry for entry, count in terms if low <= count < high]
        return picked[:wanted]

    rare = take(1, args.rare_max, args.n_rare)
    middle = take(args.rare_max, args.mid_max, args.n_mid)
    common = take(args.mid_max, 1 << 62, args.n_common)

    chosen: list[dict] = []
    for entry in rare + middle + common:
        chosen.append({"query": entry["query"], "kind": "term"})

    rest = [
        (entry, count)
        for entry, count in zip(entries, counts)
        if entry["kind"] != "term" and count > 0
    ]
    intersections = [entry for entry, _ in rest if entry["kind"] == "intersection"][: args.n_pairs]
    phrases = [entry for entry, _ in rest if entry["kind"] == "phrase"][: args.n_phrases]
    chosen.extend(intersections)
    chosen.extend(phrases)

    with open(args.out, "w", encoding="utf-8") as sink:
        for entry in chosen:
            sink.write(json.dumps({"query": entry["query"]}) + "\n")
    print(
        f"rare={len(rare)} mid={len(middle)} common={len(common)} "
        f"intersections={len(intersections)} phrases={len(phrases)} -> {args.out}",
        file=sys.stderr,
    )
    return 0


def subset(args: argparse.Namespace) -> int:
    """Keeps one query form so that form can be timed on its own."""
    kept: list[str] = []
    with open(args.queries, "r", encoding="utf-8") as source:
        for line in source:
            line = line.strip()
            if not line:
                continue
            query = json.loads(line)["query"]
            if query_band(query) == args.band:
                kept.append(query)
    if not kept:
        print(f"no {args.band} query in {args.queries}", file=sys.stderr)
        return 1
    with open(args.out, "w", encoding="utf-8") as sink:
        for query in kept:
            sink.write(json.dumps({"query": query}) + "\n")
    print(f"{len(kept)} {args.band} queries -> {args.out}", file=sys.stderr)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    cand = subparsers.add_parser("candidates")
    cand.add_argument("--corpus", required=True)
    cand.add_argument("--docs", type=int, default=200_000)
    cand.add_argument("--terms", type=int, default=3000)
    cand.add_argument("--singles", type=int, default=1200)
    cand.add_argument("--pairs", type=int, default=120)
    cand.add_argument("--phrases", type=int, default=80)
    cand.add_argument("--out", required=True)
    cand.set_defaults(func=candidates)

    fin = subparsers.add_parser("final")
    fin.add_argument("--candidates", required=True)
    fin.add_argument("--driver", required=True)
    fin.add_argument("--index", required=True)
    fin.add_argument("--rare-max", type=int, default=1000)
    fin.add_argument("--mid-max", type=int, default=200_000)
    fin.add_argument("--n-rare", type=int, default=40)
    fin.add_argument("--n-mid", type=int, default=60)
    fin.add_argument("--n-common", type=int, default=40)
    fin.add_argument("--n-pairs", type=int, default=60)
    fin.add_argument("--n-phrases", type=int, default=40)
    fin.add_argument("--out", required=True)
    fin.set_defaults(func=final)

    sub = subparsers.add_parser("subset", help="keep one query form of a pinned set")
    sub.add_argument("--queries", required=True)
    sub.add_argument("--band", choices=BANDS, default="phrase")
    sub.add_argument("--out", required=True)
    sub.set_defaults(func=subset)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
