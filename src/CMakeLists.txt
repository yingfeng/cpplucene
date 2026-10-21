# clucene_core: one static library that publishes the `clucene` C++23 module.
#
# Layout convention (mirrors infinity):
#   src/<subsystem>/<name>.cppm        -> module interface partition  `export module clucene:<name>;`
#   src/<subsystem>/<name>_impl.cpp    -> partition implementation    `module clucene:<name>.impl;`
#
# Implementation partitions are attached to the CXX_MODULES file set: CMake needs
# every unit that belongs to the module to be part of that file set.

set(CLUCENE_SUBSYSTEMS
        util
        store
        document
        analysis
        index
        search
        queryparser
        sandbox
)

set(clucene_cpp "")
set(clucene_cppm "")
set(clucene_impl_cpp "")

foreach (subsystem ${CLUCENE_SUBSYSTEMS})
    if (NOT IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${subsystem}")
        continue ()
    endif ()
    file(GLOB_RECURSE subsystem_cpp CONFIGURE_DEPENDS "${subsystem}/*.cpp")
    list(FILTER subsystem_cpp EXCLUDE REGEX ".*_impl\\.cpp$")
    file(GLOB_RECURSE subsystem_impl_cpp CONFIGURE_DEPENDS "${subsystem}/*_impl.cpp")
    file(GLOB_RECURSE subsystem_cppm CONFIGURE_DEPENDS "${subsystem}/*.cppm")

    list(APPEND clucene_cpp ${subsystem_cpp})
    list(APPEND clucene_impl_cpp ${subsystem_impl_cpp})
    list(APPEND clucene_cppm ${subsystem_cppm})
endforeach ()

file(GLOB main_cppm CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cppm")

add_library(clucene_core ${clucene_cpp})

target_sources(clucene_core
        PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES FILES
        ${main_cppm}
        ${clucene_cppm}

        # for cmake bug: implementation partitions must also be in the module file set
        ${clucene_impl_cpp}
)

target_include_directories(clucene_core PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_compile_options(clucene_core PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare)
