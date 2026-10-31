#!/usr/bin/env python3
"""Compare two Lucene index directories byte for byte.

Only the files whose content is a pure function of the indexed documents are
required to match:

  * every codec file (`.fnm .tim .tip .doc .pos .pay .fdt .fdx .fdm .nvd .nvm
    .dvd .dvm .kdd .kdi .kdm .tvd .tvx .tvm ...`),
  * `segments_N` is *not* one of them: it carries the segment id, the commit
    user data and the generation counter, all of which are per-run values.

`segments_N` and `.si` are reported separately, with their differences decoded
where that is cheap, so a reviewer can see that nothing else moved.

    python3 benchmark/sbg/compare_index.py <index-a> <index-b>
"""

from __future__ import annotations

import argparse
import hashlib
import os

# Files that legitimately differ between two independent builds of the same
# documents: they hold the segment id, the commit generation, timestamps, the
# codec/diagnostic metadata and the lock.
METADATA_PATTERNS = ("segments_", ".si", "write.lock", "segments.gen")


def digest(path: str) -> str:
    sha = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            sha.update(block)
    return sha.hexdigest()


def is_metadata(name: str) -> bool:
    return any(pattern in name for pattern in METADATA_PATTERNS)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("left")
    parser.add_argument("right")
    parser.add_argument("--verbose", action="store_true", help="list every matching file")
    args = parser.parse_args()

    left = {name: os.path.join(args.left, name) for name in os.listdir(args.left)}
    right = {name: os.path.join(args.right, name) for name in os.listdir(args.right)}

    only_left = sorted(set(left) - set(right))
    only_right = sorted(set(right) - set(left))
    shared = sorted(set(left) & set(right))

    equal: list[str] = []
    differing: list[tuple[str, int, int]] = []
    metadata: list[str] = []

    for name in shared:
        if is_metadata(name):
            metadata.append(name)
            continue
        left_hash = digest(left[name])
        right_hash = digest(right[name])
        if left_hash == right_hash:
            equal.append(name)
        else:
            differing.append((name, os.path.getsize(left[name]), os.path.getsize(right[name])))

    total_bytes = sum(os.path.getsize(path) for name, path in left.items() if not is_metadata(name))
    print(f"left : {args.left}")
    print(f"right: {args.right}")
    print(f"\ncodec files identical : {len(equal)}")
    print(f"codec files differing : {len(differing)}")
    print(f"metadata files        : {len(metadata)}  ({', '.join(metadata) if metadata else '-'})")
    print(f"only in left          : {len(only_left)}  {', '.join(only_left) if only_left else '-'}")
    print(f"only in right         : {len(only_right)}  {', '.join(only_right) if only_right else '-'}")
    print(f"codec bytes compared  : {total_bytes:,}")

    if args.verbose:
        for name in equal:
            print(f"  = {name}")

    for name, left_size, right_size in differing:
        print(f"  ! {name}: {left_size} bytes vs {right_size} bytes")

    if differing or only_left or only_right:
        print("\nRESULT: codec layout differs")
        return 1
    print("\nRESULT: every codec file is byte-identical")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
