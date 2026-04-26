#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# build.sh — build, test, and optionally run the feed handler
# Usage:
#   ./scripts/build.sh             # build release
#   ./scripts/build.sh test        # build + run unit tests
#   ./scripts/build.sh bench       # build + run ring latency benchmark
#   ./scripts/build.sh run <KEY>   # build + connect to Polygon
# ─────────────────────────────────────────────────────────────────────────────
set -e
cd "$(dirname "$0")/.."

BUILD_DIR="build"
MODE="${1:-build}"

echo "═══ Feed Handler Build ═══"
echo "Mode: $MODE"
echo ""

# Configure (Release by default, Debug if MODE=debug)
CMAKE_BUILD_TYPE="Release"
[[ "$MODE" == "debug" ]] && CMAKE_BUILD_TYPE="Debug"

cmake -B "$BUILD_DIR" \
      -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
      -S . \
      2>&1 | tail -5

cmake --build "$BUILD_DIR" --parallel "$(nproc)"
echo ""
echo "Build successful."
echo ""

case "$MODE" in
  test)
    echo "═══ Unit tests ═══"
    ./"$BUILD_DIR"/test_decoder
    ;;
  bench)
    echo "═══ Ring latency benchmark ═══"
    echo "Tip: for stable numbers, run as:"
    echo "  taskset -c 0 ./$BUILD_DIR/bench_ring  (producer)"
    echo "  (consumer auto-pins in bench code)"
    echo ""
    ./"$BUILD_DIR"/bench_ring
    ;;
  run)
    API_KEY="${2:-}"
    if [[ -z "$API_KEY" ]]; then
      echo "Usage: ./scripts/build.sh run <POLYGON_API_KEY> [SYM1 SYM2 ...]"
      echo ""
      echo "Get a free API key at https://polygon.io"
      echo "Free tier: 15-minute delayed data, 5 API calls/min, unlimited WebSocket"
      exit 1
    fi
    shift 2
    SYMBOLS="${*:-AAPL MSFT TSLA NVDA}"
    echo "Connecting to Polygon.io delayed feed..."
    echo "Symbols: $SYMBOLS"
    echo "Press Ctrl-C to stop and print final stats."
    echo ""
    ./"$BUILD_DIR"/feed_handler "$API_KEY" $SYMBOLS
    ;;
  *)
    echo "Binaries built in ./$BUILD_DIR/"
    echo "  feed_handler  — main executable"
    echo "  test_decoder  — unit tests (no API key needed)"
    echo "  bench_ring    — SPSC ring latency benchmark"
    echo ""
    echo "Next steps:"
    echo "  ./scripts/build.sh test              # verify decoder correctness"
    echo "  ./scripts/build.sh bench             # measure ring latency"
    echo "  ./scripts/build.sh run <API_KEY>     # connect to live delayed feed"
    ;;
esac
