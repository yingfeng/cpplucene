#!/usr/bin/env python3
"""Turn the raw `enwiki.txt` dump into the corpus format the
search-benchmark-game drivers index.

Upstream reads a Wikipedia JSONL (`url`, `body`) and applies
`corpus_transform.py`:

    id         = url
    text       = re.sub("[^a-zA-Z]+", " ", body.lower())
    sort_field = random.randint(0, 2**32 - 1)   with random.seed(42)

The local dump is a TSV with one article per line:

    title <TAB> date <TAB> text

so the same transform is applied to the third column, and the first column
becomes the stored id.  Documents are emitted in file order, which is the order
every engine must index them in for internal doc ids (and therefore scores and
byte-level layouts) to line up.

    python3 benchmark/sbg/prepare_corpus.py --docs 1000000 \
        --input ~/codebase/benchmark/enwiki.txt --output /data/corpus-1m.jsonl
"""

from __future__ import annotations

import argparse
import json
import random
import re
import sys
import time

PTN = re.compile("[^a-zA-Z]+")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, help="enwiki.txt (title, date, text TSV)")
    parser.add_argument("--output", required=True, help="JSONL to write")
    parser.add_argument("--docs", type=int, default=1_000_000, help="documents to emit (0 = all)")
    parser.add_argument("--skip", type=int, default=0, help="skip this many input lines first")
    args = parser.parse_args()

    random.seed(42)
    started = time.monotonic()
    emitted = 0
    read = 0
    with open(args.input, "r", encoding="utf-8", errors="replace") as source, open(
        args.output, "w", encoding="ascii", newline="\n"
    ) as sink:
        for line in source:
            read += 1
            if read <= args.skip:
                continue
            title, sep, rest = line.partition("\t")
            if not sep:
                continue
            _date, sep, body = rest.partition("\t")
            if not sep:
                continue
            title = title.strip()
            if not title:
                continue
            record = {
                "id": title,
                "text": PTN.sub(" ", body.lower()),
                "sort_field": random.randint(0, 2**32 - 1),
            }
            sink.write(json.dumps(record))
            sink.write("\n")
            emitted += 1
            if emitted % 100_000 == 0:
                elapsed = time.monotonic() - started
                print(
                    f"{emitted} docs, {elapsed:.0f}s, {emitted / elapsed:,.0f} docs/s",
                    file=sys.stderr,
                    flush=True,
                )
            if args.docs and emitted >= args.docs:
                break

    elapsed = time.monotonic() - started
    print(f"wrote {emitted} documents to {args.output} in {elapsed:.0f}s", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
