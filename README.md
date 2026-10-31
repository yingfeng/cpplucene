# clucene

A **C++23** port of [Apache Lucene](https://lucene.apache.org/): full-text indexing and search
in a single C++ module named `lucene_core`.

The C++ standard is **C++23, not C++20**: the library is built as named modules
(`export module lucene_core:<partition>;`), uses `import std;`, and relies on `std::expected`,
`std::ranges`, `std::format`, concepts and `std::span`. `import std;` requires C++23.

Index files are byte-compatible with **Apache Lucene 10.1** and are cross-verified against it
(see [Correctness](#correctness)), so the same index can be opened by either side.

**Compatibility target: Apache Lucene 10.1.** Every format version, codec name (`Lucene101`
postings, `Lucene90` norms/doc values/stored fields/term vectors/points, `Lucene90`
compound files), analysis chain and query semantic in this port is the one of
`lucene-core 10.1.0`, and cross-verification is run against that exact release. Nothing is
ported from 9.x and nothing is forward-ported from an unreleased 10.x: the port is a 10.1
implementation, not a "10.x compatible" one.

## Scope

This port covers Lucene's **full-text indexing and search** — and deliberately nothing else:

* **No KNN / vector indexing.** The vector field types and every HNSW-based format
  (dense and scalar-quantized HNSW, flat vectors, `Knn*Query` / `Knn*Field`) are out of scope
  and not ported. Documents are indexed and retrieved lexically; there is no approximate
  nearest neighbour search path, so vector similarity search must be provided elsewhere.
* **Latest index format only.** Each file format is implemented for exactly one version — the
  current one. There is no backwards compatibility with older on-disk formats: an index
  written by an older Lucene version cannot be opened (it is rejected as too old instead of
  taking a legacy read path), and writing an older format is not supported either. Freshly
  built indexes are the only supported case; index files are not portable across Lucene
  versions.

## Requirements

| Tool | Version |
| --- | --- |
| clang++ | 20 or newer (C++23 modules, `import std`) |
| CMake | 4.0.3 – 4.3.x |
| Ninja | required generator (enforced at configure time) |
| mold | used as the linker |
| GTest | optional, only for the unit tests |

## Build

```sh
cmake -S . -B build -G Ninja \
      -DCMAKE_CXX_COMPILER=clang++-20 \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 6
```

Artifacts:

* `build/src/liblucene_core.a` — the static library publishing module `lucene_core`
* `build/unit_test/unit_test` — the test binary (built only when `LUCENE_CORE_BUILD_TESTS=ON`)
* `build/benchmark/clucene_sbg` — the search-benchmark-game driver (see [Benchmark](#benchmark))

Notes:

* `-DCMAKE_BUILD_TYPE` accepts `Debug`, `RelWithDebInfo` (default) or `Release`. `Release` turns
  on ThinLTO (`-DLUCENE_CORE_LTO=OFF` to disable); the archive then holds LLVM bitcode, so only
  clang can link it.
* Keep `-j` at 6 or below; the module build is memory hungry.
* If GTest is not installed system-wide, point CMake at it with
  `-DCMAKE_PREFIX_PATH=<prefix containing GTestConfig.cmake>`, otherwise the tests are
  disabled with a warning.
* `-DENABLE_SANITIZER_ADDRESS=ON` / `-DENABLE_SANITIZER_THREAD=ON` enable ASan / TSan.

## Test

```sh
./build/unit_test/unit_test              # all tests
./build/unit_test/unit_test --gtest_brief=1
./build/unit_test/unit_test --gtest_filter='*IndexWriter*'
```

`unit_test/**/*_ut.cpp` is picked up automatically by a `GLOB_RECURSE`, so adding a test file
needs no CMake change. The suite is currently **2364 tests in 514 suites** (one opt-in
100k-document end-to-end test is skipped by default).

`ci/consumer` is a standalone CMake project used as an install smoke test: it does
`find_package(lucene_core)` against an install prefix, then indexes three documents, commits,
reopens the index and runs a `TermQuery`.

```sh
cmake --install build --prefix /tmp/lc-prefix
cmake -S ci/consumer -B /tmp/consumer-build -G Ninja \
      -DCMAKE_PREFIX_PATH=/tmp/lc-prefix -DCMAKE_CXX_COMPILER=clang++-20
cmake --build /tmp/consumer-build
```

## Using the library

```cpp
import lucene_core;   // one import gives the whole public API
```

Compile with C++23, link against `liblucene_core.a`, and use the same compiler that built the
library. The module interface units *are* the public interface; there is no header directory.

### Installing

```sh
cmake --install build --prefix /opt/lucene_core
```

The install tree holds the static library, the module units under
`include/lucene_core/**` (interface *and* implementation partitions, because a consumer has to
build the BMIs itself), a CMake package in `lib/cmake/lucene_core` and
`lib/pkgconfig/lucene_core.pc` (relocatable via `${pcfiledir}`).

### Consuming it from CMake

```cmake
cmake_minimum_required(VERSION 4.0.3...4.3.4)

# Both switches have to be set *before* project() enables the CXX language.
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")  # CMake 4.0.3–4.2.x
set(CMAKE_CXX_MODULE_STD ON)

project(app LANGUAGES CXX)      # Ninja generator only
find_package(lucene_core 0.1 REQUIRED)

add_executable(app main.cpp)
target_link_libraries(app PRIVATE lucene_core::lucene_core)
```

`lucene_core::lucene_core` carries `cxx_std_23`, the module file set, `Threads::Threads` and
`ZLIB::ZLIB`, so nothing else has to be spelled out. Two things are worth knowing:

* The module units are compiled again inside the consumer build, so the first build of a
  consuming project costs about as much as building the library.
* The library is built with `-march=native` (`-DLUCENE_CORE_NATIVE_ARCH=OFF` to disable). That
  option is `PUBLIC` on purpose: the static objects and the module BMIs are both compiled with
  it, and clang rejects a BMI imported by a translation unit that does not use the same target
  features.

## Layout

```
src/<subsystem>/<name>.cppm      module interface partition  (export module lucene_core:<name>;)
src/<subsystem>/<name>_impl.cpp  partition implementation     (module lucene_core:<name>.impl;)
src/lucene_core.cppm             primary interface: re-exports every partition
unit_test/<subsystem>/*_ut.cpp   gtest units, module lucene_core:ut.<name>
benchmark/sbg/                   search-benchmark-game drivers and runner
ci/consumer/                     install smoke test (find_package + import)
```

Subsystems mirror the reference implementation tree:

```
util  store  document  analysis  codecs  index  search  geo  queryparser  sandbox
```

## Status

Every subsystem is implemented end to end: `util` (the automaton/Levenshtein/FST stack, packed
ints, direct monotonic writers, radix sorters), `store` (byte-buffer, buffered and
memory-mapped directories, compound files, `FSLockFactory`, NRTCaching, `MMapDirectory`),
`document`, `analysis` (standard, English and CJK chains, Porter stemming, n-grams, shingles,
ASCII folding, synonyms and synonym *graphs*, word delimiter graphs, char filters including
HTML strip, URL/email tokenizer, the usual length/count/elision/keyword filters),
`codecs` (Lucene90/101 postings, norms, doc values, stored fields, term vectors, points/BKD,
compression, per-field formats, `CheckIndex`), `index` (writer with concurrent merges, readers,
index sorting, doc values updates, soft deletes, snapshots, `SearcherManager`), `search`
(term/boolean/phrase/multi-term/spans/disjunction-max/join queries, BM25 plus the
TF-IDF/classic similarities, impacts and competitive skipping, collectors, ranking and sorting),
`queryparser` (the classic syntax, hand written), `geo`, and `sandbox`.

Known gaps, all outside the supported scope or performance-only:

* **By design** (see [Scope](#scope)): vector / KNN codecs and every index format version
  older than the current one. There is also no compound-file *writer* for merge output (the
  reference implementation has no such writer either).
* **Debug/test codecs**: `DirectPostingsFormat` and the `SimpleText` codec are not ported.
* **Performance-only**: most of the hand-written SIMD kernels (`internal/vectorization`) are
  absent — the port uses the generic direct-writer/direct-reader paths instead. The two that
  mattered for the benchmark corpus are implemented with compiler vector extensions
  (`splitInts` for 1- and 2-bit blocks, `findNextGEQ` over a decoded block). `-march=native`
  code generation is used, but there is no runtime feature dispatch and no PGO.
* **Smaller deviations**: index sorting is applied by reordering documents before the segment
  is written rather than through sorting stored-fields/term-vectors consumers, which means a
  `StringIndexSorter` cannot take part in a merge (numeric sorters can); merge temporary files
  are written under their final names instead of through a tracking wrapper.

## Benchmark

The harness is [search-benchmark-game](benchmark/sbg): every driver speaks the same line
protocol (`<driver> <index|serve|verify> <index-dir>`, one `COMMAND<TAB>QUERY` per request), so
the port, Apache Lucene and any other implementation can be compared on the *same* corpus,
index and query stream.

**Machine** AMD Ryzen 7 5800H (8 cores / 16 threads), Linux, single process per driver.
**Build** clang 20.1.8, `-O3 -march=native`, release with ThinLTO.
**Compared with** Apache Lucene 10.1.0 on OpenJDK 21 (`benchmark/sbg/java_sbg.java`). Same schema, analyzer (standard, empty stop set) and
similarity (BM25) on all three sides.
**Queries** the pinned 223-query set of the game — 123 single terms, 60 `+a +b`
intersections and 40 quoted phrases — in five modes: `COUNT`, `TOP_10`, `TOP_100`,
`TOP_1000`, `TOP_100_COUNT`. `run_bench.py bench` reports every mode twice: once over the
whole set, once split by query form, because a phrase costs an order of magnitude more than
a term and averaging the two hides which engine decodes positions faster.

### 33.3M documents (8.0 GB index, single segment)

Lucene and the port read the **same physical index** here, so the numbers compare engines, not
index layouts. Three rounds, 669 samples per engine per mode after a 30 s warm-up. Times in
milliseconds; `mean` is dominated by the expensive queries, `p50` by the cheap ones.

| mode | Java p50 | Java p90 | Java mean | clucene p50 | clucene p90 | clucene mean | speed-up (mean) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `COUNT` | 0.092 | 5.208 | 8.874 | 0.061 | 3.986 | 7.346 | **1.21x** |
| `TOP_10` | 0.160 | 4.833 | 2.325 | 0.120 | 3.979 | 1.859 | **1.25x** |
| `TOP_100` | 0.160 | 5.487 | 3.091 | 0.121 | 4.195 | 2.413 | **1.28x** |
| `TOP_1000` | 0.304 | 6.199 | 4.363 | 0.237 | 4.494 | 3.353 | **1.30x** |
| `TOP_100_COUNT` | 0.154 | 7.696 | 10.747 | 0.110 | 6.146 | 9.141 | **1.18x** |

The port is 18–30% faster than Java on the mean and 20–25% on `p90`. Small-query `p50` is not a
useful signal at this scale: the driver's IPC round trip alone costs tens of microseconds.

### 1M documents (287 MB index, single segment)

Three engines, each on its own index (the indexes are interchangeable — see
[Correctness](#correctness)). Five rounds × two batches, 2230 samples per engine per mode;
`mean`, milliseconds.

| mode | clucene | Apache Lucene 10.1 | vs Java |
| --- | ---: | ---: | ---: |
| `COUNT` | 0.395 | 0.469 | **1.19x** |
| `TOP_10` | 0.179 | 0.213 | **1.19x** |
| `TOP_100` | 0.223 | 0.270 | **1.21x** |
| `TOP_1000` | 0.335 | 0.414 | **1.24x** |
| `TOP_100_COUNT` | 0.490 | 0.558 | **1.14x** |



### Phrase queries

Phrases are the one band where the port does not lead, so the harness now measures them on
their own: `benchmark/sbg/results/queries-phrase.jsonl` is exactly the 40 quoted queries of the
pinned set (`make_queries.py subset --band phrase`), and the same protocol applies. 1M
documents, eight rounds × two batches, 640 samples per engine per mode, `mean` in milliseconds:

| mode | clucene | Apache Lucene 10.1 | vs Java |
| --- | ---: | ---: | ---: |
| `COUNT` | 1.978 | 2.371 | **1.20x** |
| `TOP_10` | 0.727 | 0.871 | **1.20x** |
| `TOP_100` | 0.941 | 1.153 | **1.23x** |
| `TOP_1000` | 1.422 | 1.778 | **1.25x** |
| `TOP_100_COUNT` | 2.418 | 2.834 | **1.17x** |


### Correctness

`verify` sends every request to every `(driver, index)` pair and compares the full response:
ordered stored ids, internal doc ids, **raw f32 score bits**, hit count and its relation.

* 1M corpus, 223 queries × 3 modes: the 4 `(driver, index)` pairs of clucene and Lucene agree on
  **all 669 responses, byte for byte** — each engine produces identical results from either
  engine's index, i.e. the index format is interchangeable in both directions.
* 33.3M corpus: the port and Lucene agree on all responses apart from one hit whose BM25 score
  differs in the last float bit (`'begun'`, `TOP_10`).
* Phrase subset, 1M corpus, 40 queries × 3 modes: all 9 `(driver, index)` pairs of the three
  engines agree on **all 120 responses, byte for byte** — phrase matching is where the
  implementations could diverge most easily (positions, offsets, phrase frequency), so it is
  verified separately.

`benchmark/sbg/compare_index.py <index-a> <index-b>` checks the other direction: it hashes every
codec file of two index directories and reports whether the two builds are byte-identical,
skipping only `segments_N` and `.si`, which carry per-run ids and the commit generation.

### Reproducing

```sh
# Drivers: build the port, compile the Java driver.
cmake --build build-rel -j 6 --target clucene_sbg
javac -cp "$LUCENE_JARS" -d benchmark/sbg/classes benchmark/sbg/java_sbg.java

# Time clucene against Lucene on one index (each driver is `name=command`).
python3 benchmark/sbg/run_bench.py bench \
    --driver clucene=build-rel/benchmark/clucene_sbg \
    --driver "java=java -cp $LUCENE_JARS:benchmark/sbg/classes java_sbg" \
    --index clucene=/data/index-clucene-wiki \
    --queries benchmark/sbg/results/queries-enwiki.jsonl \
    --modes COUNT,TOP_10,TOP_100,TOP_1000,TOP_100_COUNT \
    --rounds 3 --warmup 30

# Prove both engines agree on both indexes.
python3 benchmark/sbg/run_bench.py verify \
    --driver clucene=build-rel/benchmark/clucene_sbg \
    --driver "java=java -cp $LUCENE_JARS:benchmark/sbg/classes java_sbg" \
    --index clucene=/data/index-clucene --index java=/data/index-java \
    --queries benchmark/sbg/results/queries-enwiki.jsonl --modes COUNT,TOP_10,TOP_100

```

`benchmark/sbg/prepare_corpus.py` turns the enwiki extract into the corpus format and
`make_queries.py` pins the query set (`final`) and extracts one query form of it
(`subset --band term|intersection|phrase`); `benchmark/posting_decode_bench.cpp` is a
micro-benchmark for the postings decoder.

## License

Licensed under the [Apache License, Version 2.0](LICENSE).
