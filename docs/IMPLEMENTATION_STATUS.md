# Implementation status

Technical review: 2026-09-20. This document records implemented behavior, measurement evidence, and open correctness issues. The initial latency figures were unverified expectations; recorded results are in [BENCHMARKS.md](BENCHMARKS.md).

## Component status

| Component | Evidence | Current scope |
| --- | --- | --- |
| Concurrency and resource ownership | CMake selects C++20; ring atomics; Boost asynchronous adapter; `shared_from_this`; consumer thread | Prototype architecture exists. Full RAII correctness is not established: callback lifetime, buffers, and exception-safe thread teardown are distinct issues. Much of the code uses C++17-era features despite the C++20 build target. |
| SPSC queue and BBO consumer | `spsc_ring.hpp`, `bench_ring.cpp`, BBO consumer | SPSC implementation is real. Consumer is BBO tracking, not a full order book. The original latency expectations are unsupported; recorded results specify the workload and machine. |
| TSC timing and percentile reporting | `timing.hpp` | Infrastructure exists; audit found indexing and percentile bugs, now corrected and regression tested. Current benchmark reports exact sorted-sample quantiles separately from bucket estimates. |
| Storage and allocation | Fixed arrays, inline payloads, `string_view`; shared ownership of adapter | No custom allocator exists. End-to-end allocation instrumentation and a before/after comparison baseline are still missing. Queue operations on `MarketMsg` use fixed storage, without per-message queue allocation. |

## Completed corrections

- Histogram: old 1024-element array was indexed at 1024 for 12,400 ns and up to 1399 below 50,000 ns. Storage now covers all finite buckets plus overflow.
- Percentiles: floor rounding returned bucket zero for a single nonzero sample. Nearest-rank selection now uses `ceil` with a minimum rank of one; empty data returns zero.
- Overflow is explicitly reported as a lower bound. Negative histogram samples are rejected.
- Calibration: removed signed integer overflow in the busy-wait loop and used `steady_clock` for the reference interval.
- Timestamps: ordered reads on x86 and conversion relative to a startup tick reference; no universal cross-core synchronization guarantee is claimed.
- Ring: rejects capacities below two and requires always-lock-free index atomics on the build target.
- Benchmark: explicit warmup, handoff versus saturation, preallocated sample storage, exact quantiles, invalid-sample reporting, and no positive-tail filtering.
- Build: offline targets no longer depend on live networking libraries; CTest and a Linux CI configuration added.

## Open correctness and completeness issues

| Priority | Location | Finding | Required completion evidence |
| --- | --- | --- | --- |
| High | `feed_handler.hpp`: `send_auth`, `send_subscribe` | `async_write` borrows buffers from local strings destroyed on return. Handler ownership does not retain those strings. Writes can also overlap. | Owned outbound queue; one write at a time; delayed-completion lifetime test |
| High | `feed_handler.hpp`: `on_error` | `ssl_ctx_ptr_` starts null and is never assigned; reconnect dereferences it. Existing operations may still refer to the old stream. | Fresh per-session state, cancellation/generation handling, reconnect tests |
| High | `feed_handler.hpp`: `on_resolve` | Declared lowest layer is `tcp::socket`, but member `async_connect(results, ...)` uses a Beast `tcp_stream`-style interface. | Correct stream type or Asio free `async_connect`, followed by full live-target build |
| High | `main.cpp`: stats thread | Reads ordinary BBO fields and histogram storage while the consumer writes them: data races. | Single-owner reporting or synchronized snapshots; ThreadSanitizer/replay stress test |
| High | `main.cpp`, adapter shutdown | Synchronous close during async activity and manual thread joins lack a tested exception/cancellation lifecycle. | Bounded shutdown under disconnect, failed startup, and signal tests |
| High | TLS setup | Peer chain verification is enabled but explicit hostname verification is absent. SNI alone is not hostname verification. | Hostname verification and negative-certificate tests |
| Medium | `polygon_decoder.hpp` | String/key search assumes compact formatting; braces inside strings, escapes, truncation, duplicate/nested keys, and numeric suffixes are not validated fully. | Defined accepted grammar, malformed-input tests, fuzzing or validated parser |
| Medium | Normalization | Non-finite/out-of-range prices and timestamp multiplication can overflow; long symbols truncate; side defaults to Buy without evidence. | Explicit bounds, rejection counters, unknown-side representation |
| Medium | Full ring, symbol limit | Feed drops newest event when full; BBO has 64 slots, with no resync/staleness policy. | Visible loss/staleness and recovery; symbol-capacity rejection |
| Medium | Live latency | Timestamp starts after frame receipt and object boundary scan; endpoint precedes BBO update. Original filtering drops samples >=10 ms. | Correct metric name, documented boundaries, retained tails |

## Scope and measurement boundaries

The old README's universal mutex, WebSocket, parser, and DPDK latency figures have been retired because no comparative measurements support them. Replacing a WebSocket reader with a DPDK poll loop also requires protocol decoding, packet handling, sequencing, loss recovery, and operational changes.

Lock-free communication applies to the queue, and fixed allocation applies to its storage. Neither property has been established for the entire application. The BBO array is not a matching engine. The `MarketMsg` envelope has a separate tag before a 64-byte-aligned union, producing a measured size of 128 bytes. Payload size and envelope size differ.

## Implemented capabilities

- Built a C++20 market-data processing prototype with fixed-size quote/trade normalization and a bounded SPSC queue for asynchronous producer-consumer dispatch.
- Implemented acquire/release queue publication with separate cache-line-aligned indices and tested FIFO order across one million concurrent transfers.
- Developed calibrated TSC latency benchmarks with warmup, exact p50/p99/p99.9 reporting, and separate handoff and saturated workloads; documented hardware and measurement limits.
- Used preallocated ring storage, inline message payloads, and borrowed input views to avoid per-message allocation in queue transfer.

Performance results must be read with their saved workload and environment. Custom allocators and comparative efficiency measurements remain outside the implemented scope.
