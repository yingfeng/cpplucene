# clucene

A **C++23** port of [Apache Lucene](https://lucene.apache.org/): full-text indexing and search
in a single C++ module named `lucene_core`.

The C++ standard is **C++23, not C++20**: the library is built as named modules
(`export module lucene_core:<partition>;`), uses `import std;`, and relies on `std::expected`,
`std::ranges`, `std::format`, concepts and `std::span`. `import std;` requires C++23.

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

Notes:

* `-DCMAKE_BUILD_TYPE` accepts `Debug`, `RelWithDebInfo` (default) or `Release`.
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

Every `unit_test/**/*_ut.cpp` is a module interface unit and is picked up automatically by a
`GLOB_RECURSE`; adding a test file needs no CMake change.

## Using the library

```cpp
import lucene_core;   // one import gives the whole public API
```

Link against `liblucene_core.a`, compile with C++23 and matching module flags, and make sure
the compiler used by your project is the one that built the library.

## Layout

```
src/<subsystem>/<name>.cppm      module interface partition  (export module lucene_core:<name>;)
src/<subsystem>/<name>_impl.cpp  partition implementation     (module lucene_core:<name>.impl;)
src/lucene_core.cppm             primary interface: re-exports every partition
unit_test/<subsystem>/*_ut.cpp   gtest units, module lucene_core:ut.<name>
include/lucene_core/             small public headers (macros.h)
```

Subsystems mirror the reference implementation tree:

```
util  store  document  analysis  codecs  index  search  geo
```

## Status

Implemented end to end: `util`, `store`, `document`, `analysis`, `codecs` (Lucene90 postings,
norms, doc values, stored fields, term vectors, points/BKD, compression), `index` (writer,
codec/segment readers, merges), `search` (term/boolean/multi-term queries, similarity, impacts
and competitive skipping, collectors, ranking and sorting) and `geo`.

Not ported: the query parser, the sandbox package, and (by design, see [Scope](#scope)) the
vector / KNN codecs and any index format version older than the current one.

## License

Licensed under the [Apache License, Version 2.0](LICENSE).
