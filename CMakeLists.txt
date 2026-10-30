cmake_minimum_required(VERSION 4.0.3...4.3.4)

cmake_policy(SET CMP0167 OLD)
set(CMAKE_SUPPRESS_DEVELOPER_WARNINGS TRUE)

# `import std;` support (same UUIDs as infinity).
if (CMAKE_VERSION GREATER_EQUAL 4.0.3 AND CMAKE_VERSION LESS_EQUAL 4.2.5)
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")
else (CMAKE_VERSION GREATER 4.2.5 AND CMAKE_VERSION LESS_EQUAL 4.3.4)
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-reserved-module-identifier")
endif ()

project(lucene_core VERSION 0.1.0 LANGUAGES CXX
        DESCRIPTION "A C++23 port of Lucene: index-format compatible codecs, index and search")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_CXX_MODULE_STD ON)

# `CXX_MODULE_STD` needs the standard library to ship a module manifest: CMake
# generates the `__CMAKE::CXX23` imported target from `libstdc++.modules.json`
# / `libc++.modules.json`.  Without it the configure step fails deep inside
# `src/CMakeLists.txt` with a message about the imported target, which says
# nothing about the actual fix, so fail here instead.
if (CMAKE_CXX_MODULE_STD AND NOT TARGET __CMAKE::CXX23)
    message(FATAL_ERROR
            "`import std;` is not available: the C++ standard library in use does not ship a module manifest.\n"
            "CMake looked for `libstdc++.modules.json` (or `libc++.modules.json`) and found none.\n"
            "Fix it in one of these ways:\n"
            "  * libstdc++ (default): install GCC 15 headers, e.g. `apt-get install libstdc++-15-dev`;\n"
            "  * libc++: build with `-stdlib=libc++` and install libc++ 20 (`libc++-20-dev`);\n"
            "  * point CMake at the manifest explicitly:\n"
            "    `-DCMAKE_CXX_STDLIB_MODULES_JSON=/path/to/libstdc++.modules.json`.")
endif ()

if (NOT CMAKE_GENERATOR STREQUAL "Ninja")
    message(FATAL_ERROR "This project requires the Ninja generator. "
            "Refers to https://cmake.org/cmake/help/latest/manual/cmake-cxxmodules.7.html#generator-support")
endif ()

message(STATUS "CXX: ${CMAKE_CXX_COMPILER}")
execute_process(COMMAND ${CMAKE_CXX_COMPILER} --version OUTPUT_VARIABLE CLANG_FULL_VERSION_STRING)
string(REGEX REPLACE ".*clang version ([0-9]+\\.[0-9]+).*" "\\1" CLANG_VERSION_STRING ${CLANG_FULL_VERSION_STRING})
if (CLANG_VERSION_STRING VERSION_LESS 20)
    message(FATAL_ERROR "Please use clang version 20.0 and above, current version: ${CLANG_VERSION_STRING} ${CMAKE_CXX_COMPILER}")
endif ()
message(STATUS "Building ${PROJECT_NAME} with CMake ${CMAKE_VERSION} on CLANG-${CLANG_VERSION_STRING}")

if (NOT CMAKE_CROSSCOMPILING)
    add_link_options(-fuse-ld=mold)
endif ()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wno-unused-parameter -Wno-unused-private-field -pthread -fcolor-diagnostics")

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif ()

if ("${CMAKE_BUILD_TYPE}" STREQUAL "Release")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -DNDEBUG")
elseif ("${CMAKE_BUILD_TYPE}" STREQUAL "RelWithDebInfo")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -g -DNDEBUG")
elseif ("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O0 -g")
else ()
    message(FATAL_ERROR "Only support CMake build type: Debug, RelWithDebInfo, and Release")
endif ()
message(STATUS "Build type = ${CMAKE_BUILD_TYPE}")

# ThinLTO for Release builds.
#
# The hot paths of this library are specialisations that live in a module
# interface unit (`TermScorer::fast_score`, `BlockPostingsEnum::freq`, ...) and
# are consumed from another subsystem's translation unit.  A module BMI makes the
# body available for inlining, but clang only acts on it when the units are linked
# together, so without LTO the scoring and decoding loops keep a call per
# document that the reference implementation inlines away.  Measured on the 1M
# document benchmark: ~20% fewer instructions for the bulk scoring loop, ~13% for
# phrase queries.
#
# This is link-time inlining only: no profile data is involved, so the flag is
# safe to enable for any build and is on by default.
option(LUCENE_CORE_LTO "Link with ThinLTO in Release builds" ON)
if (LUCENE_CORE_LTO AND "${CMAKE_BUILD_TYPE}" STREQUAL "Release" AND
        CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -flto=thin")
    message(STATUS "ThinLTO = ON")
endif ()

option(LUCENE_CORE_BUILD_TESTS "Build the lucene_core unit tests" ON)
option(LUCENE_CORE_BUILD_BENCHMARKS "Build the micro-benchmarks" OFF)
option(ENABLE_SANITIZER_ADDRESS "Enable address sanitizer" OFF)
option(ENABLE_SANITIZER_THREAD "Enable thread sanitizer" OFF)
option(ENABLE_SANITIZER_UNDEFINED "Enable undefined-behaviour sanitizer" OFF)

if (ENABLE_SANITIZER_ADDRESS)
    add_compile_options(-fsanitize=address -fsanitize=leak -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address -fsanitize=leak -fno-omit-frame-pointer)
endif ()
if (ENABLE_SANITIZER_THREAD)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread -fno-omit-frame-pointer)
endif ()
if (ENABLE_SANITIZER_UNDEFINED)
    # `-fsanitize-recover=all` keeps the run going after the first report so one
    # CI run lists every finding instead of only the first one.
    add_compile_options(-fsanitize=undefined -fsanitize-recover=all -fno-sanitize-recover=null
            -fno-omit-frame-pointer)
    add_link_options(-fsanitize=undefined)
endif ()

# `-march=native` must be attached to the library target as a *PUBLIC* option, not
# as a directory-wide `add_compile_options()`.  Reasons:
#   * CMake records a target's own compile options in `IMPORTED_CXX_MODULES_COMPILE_OPTIONS`
#     of the exported package, because the consumer has to rebuild the module
#     interface units.  Directory-wide options leak there too, but the consumer's
#     own translation units do not see them -- and clang then rejects the BMI with
#     "AST file ... was compiled with the target feature '+avx2' but the current
#     translation unit is not".
#   * The static library objects are already built with these features, so a
#     consumer must use the same ones anyway.
# Being PUBLIC it also reaches the in-tree unit tests through the link dependency,
# which is equally required (they import the same BMIs).
option(LUCENE_CORE_NATIVE_ARCH "Compile with -march=native" ON)

add_subdirectory(src)

if (LUCENE_CORE_BUILD_TESTS)
    add_subdirectory(unit_test)
endif ()

if (LUCENE_CORE_BUILD_BENCHMARKS)
    add_subdirectory(benchmark)
endif ()
