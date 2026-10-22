find_package(GTest CONFIG QUIET)

if (NOT GTest_FOUND)
    message(WARNING "GTest not found: lucene_core unit tests are disabled. "
            "Configure with -DCMAKE_PREFIX_PATH=<prefix containing GTestConfig.cmake> to enable them.")
    return ()
endif ()

# Every test file is a module interface unit of the form
#
#   module;
#   #include <gtest/gtest.h>     // global module fragment: keeps gtest's std
#                                  // declarations out of the lucene_core module
#   module lucene_core:ut.<name>;
#   import lucene_core;
#
# This mirrors infinity's unit-test layout.
file(GLOB_RECURSE ut_modules CONFIGURE_DEPENDS "*_ut.cpp")

add_executable(unit_test)

target_sources(unit_test
        PRIVATE
        FILE_SET ut_cxx_modules TYPE CXX_MODULES FILES
        ${ut_modules}
)

target_link_libraries(unit_test PRIVATE lucene_core GTest::gtest GTest::gtest_main)
target_include_directories(unit_test PRIVATE "${CMAKE_SOURCE_DIR}/include")
target_compile_options(unit_test PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare)

include(GoogleTest)
gtest_discover_tests(unit_test DISCOVERY_TIMEOUT 60)
