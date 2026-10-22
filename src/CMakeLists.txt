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
