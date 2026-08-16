# --------------------------------------------------------------------------
# object-pool - a fixed-capacity, header-only object pool for C++17
# --------------------------------------------------------------------------
# 3.14 is the floor because FetchContent_MakeAvailable() (used for Google
# Benchmark below) was introduced there. Asking for less would let the
# configure step get further before failing with a confusing error.
cmake_minimum_required(VERSION 3.14)

project(object_pool
        VERSION      1.0.0
        DESCRIPTION  "Fixed-capacity, header-only object pool with an RAII handle"
        LANGUAGES    CXX)

# --------------------------------------------------------------------------
# Language level
# --------------------------------------------------------------------------
# EXTENSIONS OFF matters: without it CMake passes -std=gnu++17 instead of
# -std=c++17, and code that accidentally relies on a GNU extension compiles
# here but fails on MSVC. Turning it off makes portability failures local.
set(CMAKE_CXX_STANDARD          17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)   # hard error instead of silently downgrading
set(CMAKE_CXX_EXTENSIONS        OFF)

# Single-config generators (Makefiles, plain Ninja) default CMAKE_BUILD_TYPE to
# the empty string, which means NO optimisation flags at all - not "Debug".
# Benchmarking such a build would be meaningless, so default it explicitly.
# (Multi-config generators - Visual Studio, Ninja Multi-Config - ignore this
# variable entirely; there you must pass --config Release at build time.)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY
                 STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# --------------------------------------------------------------------------
# The library itself
# --------------------------------------------------------------------------
# INTERFACE, not STATIC or SHARED: there are no .cpp files to compile. An
# INTERFACE library produces no artifact; it is a named bundle of usage
# requirements (include paths, compile features, defines) that propagates to
# anything that links it.
add_library(objectpool INTERFACE)

# The namespaced alias is not cosmetic. `target_link_libraries(x PRIVATE
# objectpool::objectpool)` fails at *configure* time if the name is unknown,
# whereas a bare `objectpool` typo is treated as a system library name and
# fails much later, at link time, with a worse message.
add_library(objectpool::objectpool ALIAS objectpool)

# Generator expressions keep the path correct in both situations: while
# building from this tree, and after an install() to a different prefix.
target_include_directories(objectpool INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

# Propagates "consumers need at least C++17" through the link graph, so a
# downstream target set to C++14 gets raised rather than failing obscurely.
target_compile_features(objectpool INTERFACE cxx_std_17)

# --------------------------------------------------------------------------
# Warnings
# --------------------------------------------------------------------------
# Deliberately NOT attached to the INTERFACE library: warning flags are a
# property of *how we build this project*, not a requirement we impose on
# anyone who includes the header. Forcing /W4 onto consumers is a common and
# widely disliked packaging mistake.
if(MSVC)
    set(OBJECTPOOL_WARNINGS /W4 /permissive-)
else()
    set(OBJECTPOOL_WARNINGS -Wall -Wextra -Wpedantic)
endif()

option(OBJECTPOOL_BUILD_TESTS      "Build the object-pool test suite" ON)
option(OBJECTPOOL_BUILD_BENCHMARKS "Build the object-pool benchmarks" OFF)

# --------------------------------------------------------------------------
# Tests
# --------------------------------------------------------------------------
if(OBJECTPOOL_BUILD_TESTS)
    enable_testing()

    add_executable(smoke tests/smoke.cpp)
    target_link_libraries(smoke PRIVATE objectpool::objectpool)
    target_compile_options(smoke PRIVATE ${OBJECTPOOL_WARNINGS})

    # Registers with CTest so `ctest --test-dir build` runs it. The test
    # passes if the exit code is 0.
    add_test(NAME smoke COMMAND smoke)
endif()

# --------------------------------------------------------------------------
# Benchmarks  (off by default: they pull ~10 MB of sources over the network)
# --------------------------------------------------------------------------
if(OBJECTPOOL_BUILD_BENCHMARKS)
    include(FetchContent)

    # These must be set BEFORE MakeAvailable, because that is when Benchmark's
    # own CMakeLists reads them. CACHE ... FORCE is required: they are declared
    # as cache options over there, and a plain set() would be shadowed.
    set(BENCHMARK_ENABLE_TESTING     OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL     OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_WERROR      OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(benchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG        v1.9.1          # pinned: a moving tag makes results unreproducible
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(benchmark)

    add_executable(pool_benchmark benchmarks/pool_benchmark.cpp)
    target_link_libraries(pool_benchmark PRIVATE
        objectpool::objectpool
        benchmark::benchmark)
    target_compile_options(pool_benchmark PRIVATE ${OBJECTPOOL_WARNINGS})
endif()
