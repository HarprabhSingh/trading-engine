# Feed Handler MVP

WebSocket market data feed handler in C++20.  
Connects to **Polygon.io** (free, 15-min delayed), decodes quotes and trades,  
publishes onto a **lock-free SPSC ring buffer**, and prints live BBO + latency stats.

---

## Architecture

```
Polygon.io WSS
      │
      ▼  (Boost.Beast async_read, SSL)
 ┌────────────┐
 │ FeedHandler│  Thread 1 (I/O)
 │  decode    │  PolygonDecoder: JSON → MarketMsg (no heap alloc)
 │  publish   │  rdtsc timestamp on every message
 └─────┬──────┘
       │ SPSC ring (4096 × 64 bytes, lock-free)
 ┌─────▼──────┐
 │ BookConsumer│ Thread 2 (spin-poll)
 │  BBO update │ Measures decode→book latency per message
 └─────┬──────┘
       │ atomic reads (no lock)
 ┌─────▼──────┐
 │StatsPrinter│ Thread 3 (every 5s)
 │ p50/p99/999│ BBO table for all symbols
 └────────────┘
```

## Design decisions (interview talking points)

| Decision | What | Why |
|---|---|---|
| **Scaled integer prices** | `int64_t price * 1_000_000` | Eliminates float rounding; spread `ask-bid` is exact |
| **SPSC over mutex queue** | Lock-free ring, acquire/release ordering | No kernel involvement in message pass; ~100ns vs ~1µs |
| **Cache-line aligned structs** | `alignas(64)` on `Quote`, `Trade` | One struct = one cache line; no false sharing with adjacent data |
| **rdtsc over clock_gettime** | ~3ns vs ~20ns | clock_gettime is a vDSO call; rdtsc is a single instruction |
| **Hand-rolled JSON scanner** | Field scan, not full parse | Full parse tree = heap alloc; scanner = zero alloc, ~200ns/msg |
| **Single I/O thread** | One `io_context`, async ops | N symbols handled by one core; no per-symbol thread thundering herd on reconnect |
| **Separate head_/tail_ cache lines** | `alignas(64)` on each atomic | Prevents producer/consumer false-sharing the same cache line |

## What this project does NOT do (and why that's honest)

- No kernel bypass (DPDK): WebSocket over TCP adds ~2–10 µs vs ~200 ns for raw UDP multicast. For a resume project, the architecture is identical — the DPDK version swaps `Boost.Beast` for a PMD poll loop.
- No FPGA decode: same reasoning. The decoder logic is the same; the transport differs.
- No real exchange connection: Polygon free tier is delayed. The OMS in Phase 2 uses paper fills.

The latency numbers you measure here are **honest** — they reflect the WebSocket transport cost, not a fabricated claim. That honesty + the ability to explain what DPDK would change is the interview answer.

## Latency numbers (typical, without core pinning)

| Stage | p50 | p99 | p999 |
|---|---|---|---|
| SPSC ring push→pop | ~80 ns | ~250 ns | ~800 ns |
| WebSocket decode→BBO | ~3–8 µs | ~15–40 µs | ~100 µs |

The decode→BBO number is dominated by WebSocket/TCP stack latency, not your code.  
With DPDK on raw UDP multicast, the transport cost drops to ~300–500 ns.

## Build

```bash
# Prerequisites (Ubuntu/Debian)
sudo apt install libboost-dev libssl-dev cmake build-essential

# Build
./scripts/build.sh

# Unit tests (no API key needed)
./scripts/build.sh test

# Ring latency benchmark
./scripts/build.sh bench

# Connect to live delayed feed
./scripts/build.sh run <YOUR_POLYGON_API_KEY> AAPL MSFT TSLA
```

## Get a free API key

1. Sign up at https://polygon.io (free tier, no credit card)
2. Free tier gives: 15-min delayed WebSocket, unlimited symbols, 5 REST calls/min
3. Real-time requires paid tier (~$29/month) — not needed for this project

## Phase 2 (next)

- Replace `std::map`-based BBO with flat array price-level book
- Add FIX gateway (QuickFIX/N) for order submission
- Wire strategy layer: naive market-making algorithm
- CPU affinity + `isolcpus` for stable latency numbers
- Prometheus metrics + Grafana dashboard
