cmake_minimum_required(VERSION 3.20)
project(LiteOLAP
    VERSION 0.1.0
    DESCRIPTION "A vectorized column-store analytical engine in C++17"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    add_compile_options(-Wall -Wextra -Werror -Wpedantic)
endif()

include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.3
)
FetchContent_MakeAvailable(Catch2)

# --- Core engine library ------------------------------------------------------
add_library(liteolap_core
    src/storage/disk_manager.cpp
    src/storage/buffer_pool.cpp
    src/storage/paged_stream.cpp
    src/common/value.cpp
    src/compression/encoding.cpp
    src/column/column_chunk.cpp
    src/column/column_writer.cpp
    src/column/column_reader.cpp
    src/vector/vector.cpp
    src/vector/data_chunk.cpp
    src/catalog/schema.cpp
    src/catalog/catalog.cpp
    src/execution/expression.cpp
    src/execution/column_scan.cpp
    src/execution/filter.cpp
    src/execution/projection.cpp
    src/execution/hash_aggregate.cpp
    src/execution/hash_join.cpp
    src/execution/order_by.cpp
    src/execution/limit.cpp
    src/sql/lexer.cpp
    src/sql/parser.cpp
    src/planner/planner.cpp
    src/db.cpp
)
target_include_directories(liteolap_core PUBLIC src)
target_compile_features(liteolap_core PUBLIC cxx_std_17)

# --- Benchmark (DuckDB comparison opt-in) ------------------------------------
option(LITEOLAP_WITH_DUCKDB "Link DuckDB into the benchmark for comparison" OFF)
add_executable(liteolap_bench bench/bench_main.cpp)
target_link_libraries(liteolap_bench PRIVATE liteolap_core)
if(LITEOLAP_WITH_DUCKDB)
    find_package(duckdb REQUIRED)
    target_link_libraries(liteolap_bench PRIVATE duckdb)
    target_compile_definitions(liteolap_bench PRIVATE LITEOLAP_WITH_DUCKDB=1)
endif()

# --- Tests --------------------------------------------------------------------
enable_testing()
include(CTest)
include(Catch)

add_executable(liteolap_tests
    tests/test_disk_manager.cpp
    tests/test_buffer_pool.cpp
    tests/test_paged_stream.cpp
    tests/test_vector.cpp
    tests/test_compression.cpp
    tests/test_column_storage.cpp
    tests/test_operators.cpp
    tests/test_aggregate.cpp
    tests/test_join.cpp
    tests/test_parser.cpp
    tests/test_db.cpp
)
target_include_directories(liteolap_tests PRIVATE tests)
target_link_libraries(liteolap_tests PRIVATE liteolap_core Catch2::Catch2WithMain)
catch_discover_tests(liteolap_tests)
