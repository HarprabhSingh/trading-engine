cmake_minimum_required(VERSION 3.20)
project(feed_handler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Compiler flags ────────────────────────────────────────────────────────────
# -O3           : full optimisation (critical for hot path)
# -march=native : use CPU-specific instructions (AVX2 etc.) — don't cross-compile
# -funroll-loops: auto unroll tight decode loops
# -fno-rtti     : saves binary size, no dynamic_cast in hot path
# -Wall -Wextra : catch issues early
add_compile_options(
    -O3 -march=native -funroll-loops
    -Wall -Wextra -Wno-unused-parameter
    $<$<CONFIG:Debug>:-g -O0 -fsanitize=address,undefined>
)
add_link_options($<$<CONFIG:Debug>:-fsanitize=address,undefined>)

# ── Dependencies ──────────────────────────────────────────────────────────────
find_package(Boost 1.81 REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# ── Main executable ───────────────────────────────────────────────────────────
add_executable(feed_handler src/main.cpp)

target_include_directories(feed_handler PRIVATE include)
target_link_libraries(feed_handler
    Boost::system
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
)

# ── Decoder unit tests ────────────────────────────────────────────────────────
# Run with: cmake --build build && ./build/test_decoder
add_executable(test_decoder tests/test_decoder.cpp)
target_include_directories(test_decoder PRIVATE include)
target_link_libraries(test_decoder Threads::Threads)

# ── Latency benchmark ─────────────────────────────────────────────────────────
add_executable(bench_ring tests/bench_ring.cpp)
target_include_directories(bench_ring PRIVATE include)
target_link_libraries(bench_ring Threads::Threads)

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Boost version: ${Boost_VERSION}")
