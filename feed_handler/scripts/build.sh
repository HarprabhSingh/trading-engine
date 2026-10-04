#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode="${1:-build}"
config=Release
[[ "$mode" == debug ]] && config=Debug
live=OFF
[[ "$mode" == run ]] && live=ON
cmake -S . -B build -DCMAKE_BUILD_TYPE="$config" -DBUILD_LIVE_FEED="$live"
cmake --build build --parallel
case "$mode" in
    test|debug) ctest --test-dir build --output-on-failure ;;
    bench) ./build/bench_ring "${2:-handoff}" ;;
    run) shift; ./build/feed_handler "$@" ;;
esac
