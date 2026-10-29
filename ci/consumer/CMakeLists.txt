# Standalone consumer project used by CI to prove that an *installed* lucene_core
# can be consumed with `find_package` + `import lucene_core;`.
#
# It deliberately knows nothing about the source tree: the only input is
# `CMAKE_PREFIX_PATH` pointing at an install prefix.
cmake_minimum_required(VERSION 4.0.3...4.3.4)

# Both switches must be set before project() enables the CXX language.
if (CMAKE_VERSION GREATER_EQUAL 4.0.3 AND CMAKE_VERSION LESS_EQUAL 4.2.5)
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")
else ()
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-reserved-module-identifier")
endif ()
set(CMAKE_CXX_MODULE_STD ON)

project(lc_consumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT CMAKE_GENERATOR STREQUAL "Ninja")
    message(FATAL_ERROR "lucene_core requires the Ninja generator")
endif ()

find_package(lucene_core 0.1 REQUIRED)
if (NOT TARGET lucene_core::lucene_core)
    message(FATAL_ERROR "the exported target lucene_core::lucene_core is missing")
endif ()

get_target_property(mods lucene_core::lucene_core CXX_MODULE_SET_cxx_modules)
if (NOT mods)
    message(FATAL_ERROR "the exported target carries no CXX_MODULES file set")
endif ()
list(LENGTH mods mod_count)
message(STATUS "lucene_core ${lucene_core_VERSION} found; ${mod_count} module units to build")

add_executable(lc_smoke main.cpp)
target_link_libraries(lc_smoke PRIVATE lucene_core::lucene_core)
