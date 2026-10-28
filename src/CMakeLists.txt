# lucene_core: one static library that publishes the `lucene_core` C++23 module.
#
# Layout convention (mirrors infinity):
#   src/<subsystem>/<name>.cppm        -> module interface partition  `export module lucene_core:<name>;`
#   src/<subsystem>/<name>_impl.cpp    -> partition implementation    `module lucene_core:<name>.impl;`
#
# Implementation partitions are attached to the CXX_MODULES file set: CMake needs
# every unit that belongs to the module to be part of that file set.

set(LUCENE_CORE_SUBSYSTEMS
        util
        store
        document
        analysis
        codecs
        index
        search
        geo
        queryparser
        sandbox
)

set(lucene_core_cpp "")
set(lucene_core_cppm "")
set(lucene_core_impl_cpp "")

foreach (subsystem ${LUCENE_CORE_SUBSYSTEMS})
    if (NOT IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${subsystem}")
        continue ()
    endif ()
    file(GLOB_RECURSE subsystem_cpp CONFIGURE_DEPENDS "${subsystem}/*.cpp")
    list(FILTER subsystem_cpp EXCLUDE REGEX ".*_impl\\.cpp$")
    file(GLOB_RECURSE subsystem_impl_cpp CONFIGURE_DEPENDS "${subsystem}/*_impl.cpp")
    file(GLOB_RECURSE subsystem_cppm CONFIGURE_DEPENDS "${subsystem}/*.cppm")

    list(APPEND lucene_core_cpp ${subsystem_cpp})
    list(APPEND lucene_core_impl_cpp ${subsystem_impl_cpp})
    list(APPEND lucene_core_cppm ${subsystem_cppm})
endforeach ()

file(GLOB main_cppm CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cppm")

add_library(lucene_core ${lucene_core_cpp})

target_sources(lucene_core
        PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES FILES
        ${main_cppm}
        ${lucene_core_cppm}

        # for cmake bug: implementation partitions must also be in the module file set
        ${lucene_core_impl_cpp}
)

target_include_directories(lucene_core PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_compile_options(lucene_core PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare)

# `TaskExecutor` runs leaf slices on its own threads.
find_package(Threads REQUIRED)
target_link_libraries(lucene_core PUBLIC Threads::Threads)

# `HIGH_COMPRESSION` (Deflate) is zlib.
find_package(ZLIB REQUIRED)
target_link_libraries(lucene_core PUBLIC ZLIB::ZLIB)

# clang 20 (20.1.8) crashes in LLVM IR generation on these two implementation
# units of a Release build: "Generating code for declaration
# 'std::__uniq_ptr_data<lucene::NumericDocValues, ...>::__uniq_ptr_data'"
# segfaults.  The crash depends on debug info, not on the optimization level:
# -O0/-O1/-O2/-O3 all crash without `-g`, and all of them compile with `-g`
# (`-gline-tables-only` is not enough).  The same units compile fine in a Debug
# build, which is why this only shows up in Release.  Forcing full debug info
# for just these two units keeps the rest of the library optimized and their own
# code at -O3; drop the workaround once the toolchain stops reproducing.
set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/search/phrase_query_impl.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/search/multi_phrase_query_impl.cpp"
        PROPERTIES COMPILE_OPTIONS "-g"
)
