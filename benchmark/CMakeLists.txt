# Micro-benchmarks.  Off by default: they are not tests, they just print numbers.
add_executable(bench_posting_decode posting_decode_bench.cpp)
target_link_libraries(bench_posting_decode PRIVATE lucene_core)
target_compile_options(bench_posting_decode PRIVATE -Wall -Wextra -Wno-unused-parameter)
