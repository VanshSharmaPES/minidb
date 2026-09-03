cmake_minimum_required(VERSION 3.14)
project(minidb CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ZLIB REQUIRED)

add_library(minidb src/db.cpp)
target_include_directories(minidb PUBLIC include)
target_link_libraries(minidb PUBLIC ZLIB::ZLIB)

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.16.0.zip
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()
add_executable(minidb_tests tests/test_smoke.cpp)
target_link_libraries(minidb_tests PRIVATE minidb GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(minidb_tests)
