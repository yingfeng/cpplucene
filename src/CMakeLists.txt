# lucene_core: one static library that publishes the `lucene_core` C++23 module.
#
# Layout convention (mirrors infinity):
#   src/<subsystem>/<name>.cppm        -> module interface partition  `export module lucene_core:<name>;`
#   src/<subsystem>/<name>_impl.cpp    -> partition implementation    `module lucene_core:<name>.impl;`
#
# Implementation partitions are attached to the CXX_MODULES file set: CMake needs
# every unit that belongs to the module to be part of that file set.

include(GNUInstallDirs)

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

# The module interface units are the whole public interface (macros cannot be
# exported from a module, and the one header that held them had no user).  The
# install interface still points at the include directory, because the module
# sources are installed there.
target_include_directories(lucene_core INTERFACE
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_options(lucene_core PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare)

# PUBLIC on purpose: the module interface units are recompiled in the consumer's
# build, so these features have to be used by the consumer's own translation units
# as well, otherwise clang refuses the BMIs (target-feature mismatch).
if (LUCENE_CORE_NATIVE_ARCH AND NOT CMAKE_CROSSCOMPILING)
    target_compile_options(lucene_core PUBLIC -march=native)
endif ()

# The module interface units are shipped as sources and compiled on the consumer
# side, so the C++ standard they need has to travel with the exported target:
# without this the synthesized module target of a consumer has no C++ standard at
# all and generation fails with "does not include cxx_std_20 (or newer)".
target_compile_features(lucene_core PUBLIC cxx_std_23)

# `TaskExecutor` runs leaf slices on its own threads.
find_package(Threads REQUIRED)
target_link_libraries(lucene_core PUBLIC Threads::Threads)

# `HIGH_COMPRESSION` (Deflate) is zlib.
find_package(ZLIB REQUIRED)
target_link_libraries(lucene_core PUBLIC ZLIB::ZLIB)

# A ThinLTO build stores LLVM bitcode in the archive instead of machine code, so
# the *link* step of whoever consumes the installed library has to run LTO too —
# otherwise GNU ld answers "liblucene_core.a: error adding symbols: file format
# not recognized" (it has no way to read the members).  `INTERPROCEDURAL_OPTIMIZATION`
# only adds the flag to this build's own link lines, so the requirement is
# published with the target: a consumer that links `lucene_core::lucene_core`
# (or uses the pkg-config file) gets the flag without having to know about it.
#
# The flag is emitted for the configuration that was actually built, not for the
# consumer's: a Release library is bitcode no matter what the consumer builds.
if (LUCENE_CORE_LTO AND CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND
        "${CMAKE_BUILD_TYPE}" STREQUAL "Release")
    target_link_options(lucene_core INTERFACE -flto=thin)
    set(LUCENE_CORE_PC_LTO_FLAG "-flto=thin ")
    set(LUCENE_CORE_BITCODE_ARCHIVE ON)
else ()
    set(LUCENE_CORE_PC_LTO_FLAG "")
    set(LUCENE_CORE_BITCODE_ARCHIVE OFF)
endif ()

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
        "${CMAKE_CURRENT_SOURCE_DIR}/search/span_weight_impl.cpp"
        PROPERTIES COMPILE_OPTIONS "-g"
)

# ---------------------------------------------------------------------------
# Install / packaging
#
# The library is a C++23 module, so a consumer has to build the module interface
# units itself: the `cxx_modules` file set is installed as sources under
# `<includedir>/lucene_core`, and the exported target carries it, which is what
# CMake's module support needs to compile the BMIs on the consumer side.
# ---------------------------------------------------------------------------

include(CMakePackageConfigHelpers)

install(TARGETS lucene_core
        EXPORT lucene_coreTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        FILE_SET cxx_modules DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/lucene_core"
)

install(EXPORT lucene_coreTargets
        FILE lucene_coreTargets.cmake
        NAMESPACE lucene_core::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/lucene_core")

configure_package_config_file(
        "${CMAKE_SOURCE_DIR}/cmake/lucene_coreConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/lucene_coreConfig.cmake"
        INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/lucene_core")

write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/lucene_coreConfigVersion.cmake"
        VERSION "${PROJECT_VERSION}"
        COMPATIBILITY SameMajorVersion
        ARCH_INDEPENDENT)

install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/lucene_coreConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/lucene_coreConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/lucene_core")

configure_file("${CMAKE_SOURCE_DIR}/cmake/lucene_core.pc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/lucene_core.pc" @ONLY)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/lucene_core.pc"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
