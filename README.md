# Trading Engine

I'm building a trading engine in C++, starting with the market-data pipeline: decode quotes and trades, move them between threads, and measure the cost of that handoff.

The current version is a **C++20 market-data processing prototype**. It has a fixed-size message format, an SPSC ring buffer, a decoder, and offline latency benchmarks. The live WebSocket adapter and BBO consumer are experimental. Order matching, execution, and risk management are still ahead.

## How it fits together

```mermaid
flowchart LR
    A[WebSocket feed] --> B[Decode and normalize]
    B --> C[SPSC ring buffer]
    C --> D[BBO consumer]
```

One I/O thread owns ingestion and queue publication. One consumer thread reads messages and tracks the best bid and offer for each symbol. This ownership model is the reason for using a single-producer/single-consumer queue.

The offline tests and benchmarks exercise the core without a market-data subscription. The live adapter is excluded from the default build while its connection lifecycle and shared statistics are being hardened.

## Design decisions

| Decision | Reason | Tradeoff |
| --- | --- | --- |
| Bounded SPSC ring | Each index has one writer; publication uses acquire/release ordering | Exactly one producer and one consumer; overload needs an explicit policy |
| Preallocated storage and inline messages | No per-message allocation in queue transfer | Fixed capacity and larger copies than passing a pointer |
| Integer prices at a scale of 1,000,000 | Exact price arithmetic after conversion | Decoder currently goes through `double`, so conversion still needs rounding and range checks |
| Eight-byte symbol field | Avoids owning strings inside messages | Long symbols currently truncate |
| Separate cache-line-aligned indices | Reduces false sharing between index writes | Does not remove coherence traffic |
| Busy-polling consumer | Avoids a sleep/wakeup for every message | Consumes CPU while idle |
| Calibrated TSC timestamps | Measures short local intervals | Depends on clock ordering and host assumptions; timing overhead remains in the result |

The ring has 4096 slots and **4095 usable entries**: one slot distinguishes full from empty. Quote and trade payloads are each 64 bytes, but the tagged message envelope is **128 bytes** on the tested ABI. That makes the backing array 512 KiB, before queue metadata.

## What I measured

Recorded on **September 20, 2026**, using an **Intel Core i5-1035G1**, Windows build 26200, and **Clang 22.1.8 with `-O3`**. Threads were unpinned on a normal desktop session. Each run used 10,000 warmup messages followed by 200,000 measured samples.

I used two workloads because an almost-empty queue and a queue under sustained load answer different questions:

- **Handoff:** one outstanding message, with an acknowledgement before sending the next.
- **Saturated:** the producer sends continuously and retries when the ring is full.

| Workload | Runs | p50 range | p99 range | p99.9 range |
| --- | ---: | ---: | ---: | ---: |
| Handoff | 6 | 40–207 ns | 95–299 ns | 567–13,498 ns |
| Saturated | 5 | 8.46–491.31 µs | 15.39–811.65 µs | 27.77–867.57 µs |

These are **ranges of per-run percentiles**, not pooled percentiles. The interval starts just before enqueue and ends just after dequeue. It includes copying, queue residence, scheduling, and timestamp overhead. Failed enqueue attempts are outside the admitted message's measured interval. It does not measure JSON decoding or network latency.

The biggest observation was how much the workload changed the result. Low-backlog handoff was fast, but sustained load produced much larger queueing delays. The unpinned runs also varied enough that quoting one number would hide useful information. All positive tail samples were retained.

[Full methodology and results](docs/BENCHMARKS.md) · [Raw runs](docs/results/2026-09-20-windows.txt) · [Initial pilot](docs/results/2026-09-20-pilot.txt)

## Build and run

The core requires a C++20 compiler with floating-point `std::from_chars`, native thread support, and CMake 3.20+. It does not require Boost, OpenSSL, or an API key.

```sh
cmake -S feed_handler -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
./build/bench_ring handoff
./build/bench_ring saturated
```

With a multi-configuration generator, the executables are under `build/Release/`. On Windows, with LLVM-MinGW's `bin` directory on PATH, the standalone PowerShell script also builds and tests the core:

```powershell
./feed_handler/scripts/build-core.ps1
./build/bench_ring.exe handoff
./build/bench_ring.exe saturated
```

The experimental live target can be enabled with `-DBUILD_LIVE_FEED=ON` and requires Boost 1.81+ and OpenSSL. It still has open correctness issues; use the offline targets for a reproducible demonstration.

## Tests and lessons from debugging

Local validation passed the existing 33 decoder/core checks, the additional ring and histogram regression tests, and one million concurrent FIFO transfers. AddressSanitizer and UndefinedBehaviorSanitizer runs also passed on these test inputs. A GitHub Actions workflow is included for Linux Release and sanitizer-enabled Debug tests.

Two bugs changed how I approached the measurements:

- The histogram originally had fewer buckets than its index calculation could address. A 12.4 µs sample could write past the array. It now has space for every finite bucket and an explicit overflow bucket.
- Percentile rank rounding could return zero for a single nonzero sample. It now uses nearest-rank selection, with regression coverage for that case.

The benchmark reports exact quantiles from a preallocated sample array; the reusable histogram reports bucket lower bounds. Those are different levels of precision.

## Reading the code

| File | Responsibility |
| --- | --- |
| [market_data.hpp](feed_handler/include/market_data.hpp) | Quote/trade layout, scaled prices, and tagged message envelope |
| [spsc_ring.hpp](feed_handler/include/spsc_ring.hpp) | Fixed-capacity queue and publication/reuse ordering |
| [polygon_decoder.hpp](feed_handler/include/polygon_decoder.hpp) | Borrowed-frame field scanning and normalization |
| [timing.hpp](feed_handler/include/timing.hpp) | Counter calibration and fixed-storage histogram |
| [bench_ring.cpp](feed_handler/tests/bench_ring.cpp) | Handoff and saturated experiments |
| [test_core.cpp](feed_handler/tests/test_core.cpp) | Ring boundaries, wraparound, concurrent ordering, and histogram regression tests |
| [feed_handler.hpp](feed_handler/include/feed_handler.hpp) | Experimental asynchronous network adapter |
| [main.cpp](feed_handler/src/main.cpp) | Experimental BBO consumer and application setup |

## Current limits and next steps

This is a BBO tracker, not a full depth book or matching engine. The decoder accepts a limited compact JSON shape rather than implementing complete JSON validation. The live adapter needs owned asynchronous write buffers, a safe reconnect/shutdown lifecycle, hostname verification, and synchronized telemetry.

Queue transfer uses fixed storage, but I have not established zero allocations across the entire network-to-book path. There is no custom allocator in this revision and no measured speedup against a mutex-queue baseline.

Next steps are to harden the existing pipeline, add deterministic feed replay and allocation instrumentation, and define loss/recovery behavior before extending the book and adding paper execution.

For a deeper walkthrough of ownership, memory ordering, parsing, measurement, and alternatives, see the [project and interview guide](docs/INTERVIEW_GUIDE.md). The [implementation audit](docs/CLAIM_AUDIT.md) tracks the remaining issues in detail.
