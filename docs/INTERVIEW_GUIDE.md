# Interview preparation

## Your opening explanation

“I built the market-data ingestion foundation of a trading engine in C++20. It normalizes quote and trade events into fixed-size values and passes them through a bounded SPSC queue to a separate consumer. The current consumer tracks best bid and offer. My focus is ownership, memory ordering, bounded storage, and measurement. I distinguish the tested offline queue/benchmark from the experimental network adapter. It does not yet execute orders or maintain a full depth book.”

Use your own words. Be able to draw the data path and explain every arrow before discussing nanoseconds.

## Walk one event through the code

1. The intended live path receives a complete WebSocket frame through Beast. Transport, TLS, framing, and buffer growth happen before the decoder sees the frame.
2. `PolygonDecoder::decode_frame` locates an object boundary, timestamps locally, then extracts fields from a borrowed `string_view`.
3. A quote becomes an inline `Quote`: eight-byte symbol, local/exchange timestamps, scaled bid/ask prices, and quantities. It is copied into a tagged `MarketMsg`.
4. The producer checks queue capacity, copies the value into its owned slot, then publishes the next head with a release store.
5. The consumer observes head with acquire ordering, copies the message out, then releases tail to make the slot reusable.
6. The prototype consumer looks up a symbol in a fixed 64-entry array and updates its BBO. Trade events are decoded but not used by that consumer.

The decoder timestamp is not packet arrival. The current live timestamp endpoint is before the BBO assignment. Never label that interval network latency or exchange-to-book latency.

## SPSC: explain the proof, not just the vocabulary

**Why exactly one producer and one consumer?** The producer alone advances head; the consumer alone advances tail. Each thread can read its own index relaxed because no competing writer modifies it. A second producer could claim the same slot; stronger ordering alone would not fix that.

**Why release/acquire?** Slot writes precede publication of head. A consumer acquire load that observes that publication establishes visibility of those writes. In the other direction, copying the payload precedes release of tail; the producer's acquire observation prevents reuse before the consumer finishes. Both directions matter. See the [C++ draft's ordering rules](https://eel.is/c++draft/atomics.order).

**Why can the payload be non-atomic?** Ownership transfers through the indices. No thread accesses a slot concurrently with another thread modifying it when the protocol is obeyed. Atomic indices do not automatically protect unrelated shared objects such as the live statistics table.

**Full and empty?** Empty means head equals tail. Full means advancing head would reach tail. One unused slot distinguishes these states, so N=4096 stores 4095 events. A mask wraps indices because N is a power of two. Compilers can also optimize constant modulo; avoid claiming the mask alone explains speed.

**Lock-free versus wait-free?** The target must implement the index atomics without locks; a compile-time check now enforces this. Each try-push/try-pop has bounded algorithmic work for this fixed-size payload, with no CAS retry loop. A caller spinning until push succeeds can wait indefinitely if the consumer stops. Neither term promises bounded OS scheduling latency or makes the full application lock-free.

**False sharing?** Separating head and tail reduces invalidations caused by unrelated writes landing on one cache line. Each side still reads the other's index, so true sharing remains. Alignment does not guarantee a cache hit. In a generic ring whose payload has small alignment, storage can also lie near tail's cache line; analyze actual layout.

**What if the ring fills?** The live producer drops the incoming message and counts the failure. The benchmark retries; these are different workloads. Dropping book deltas can corrupt state and should trigger resynchronization. Even snapshot-style BBO updates can leave stale state until another update arrives. Alternatives: bounded blocking, shedding, coalescing quotes, or pausing reads; each has implications for loss and latency.

**How would you scale?** Start with one queue per feed or shard. Route each symbol to one book owner to retain ordering. A merger must define cross-feed ordering and failure handling. Use MPMC only where the topology requires it; compare complexity, contention, fairness, and throughput empirically.

## Storage, RAII, and allocation

**Why inline messages?** Fixed-size fields allow value transfer, predictable storage, and no per-event ownership allocation in the ring. `std::optional<T>` holds the value inline. The fixed ring reserves memory at construction.

**Why are messages 128 bytes if quotes are 64?** `MarketMsg` places a separate tag before an aligned union. Padding moves the union to a 64-byte boundary; total size is 128 on the measured target. Reducing redundant tags could reduce footprint, but changing active union/member rules carelessly can introduce undefined behavior. Validate layout with `sizeof`, not intuition.

**Do smart pointers remove allocations?** No. `make_shared` allocates ownership/object storage; shared pointer copies update ownership counts. Here `shared_from_this` keeps the asynchronous handler alive. Payloads are passed by value instead of smart pointer. There is no custom allocator in the project.

**What would a custom allocator add?** A pool could bound allocation for variable-size events, sessions, or callbacks. It needs alignment, exhaustion behavior, thread ownership, and lifetime rules. It is unnecessary for an already inline fixed-capacity ring. Adding one just to match a resume sentence would not establish a benefit.

**Is RAII sufficient for async correctness?** Resource destruction is automatic, but destruction must occur at the right time. A local string is destroyed too early when an asynchronous write still borrows its bytes. Keeping the handler alive does not keep every external buffer alive. Beast requires the underlying buffers to survive completion and serializes writes through a one-write-at-a-time contract: [official documentation](https://www.boost.org/doc/libs/latest/libs/beast/doc/html/beast/ref/boost__beast__websocket__stream/async_write.html).

**How would you prove zero allocation?** Define the boundary first, e.g. decode a preloaded frame, enqueue, dequeue, update a preinitialized BBO. Warm up, then instrument all relevant allocation entry points, including aligned allocation and library allocators. Preallocate the instrumentation itself. Count over many messages and malformed/full-queue paths. Separate application `new` counts from library `malloc`/OS allocations. This repository has not yet run that full experiment.

## Normalization and parsing choices

**Why integer prices?** Arithmetic on representable scaled integers is exact after conversion. At scale 1,000,000, a one-cent spread is 10,000 units. Current ingestion goes through `double`, so conversion is still a rounding boundary. Direct decimal parsing would provide clearer rounding/overflow rules. Signed prices and instrument-specific tick sizes require an explicit policy.

**Why a scanner instead of a JSON library?** Avoiding an owned parse tree can reduce allocations, but it is not proof of better speed. The current scanner handles a narrow compact format and misses valid JSON forms and malformed-input cases. A validated on-demand parser with reused storage is a reasonable alternative. Benchmark only after correctness requirements are equal.

**What are the main edge cases?** Empty/long symbols, embedded braces and escaped quotes, whitespace around keys, incomplete arrays, duplicate fields, numeric prefixes with junk suffixes, missing fields, non-finite or out-of-range values, multiplication overflow in timestamps, unknown events, and reordered updates.

**BBO versus an order book?** BBO stores the best visible bid and ask. A depth book stores multiple levels or individual orders and processes adds, modifications, cancellations, and executions. A matching engine also enforces matching priority and produces fills. This project currently provides none of those latter guarantees.

## Benchmark defense

Start by stating the workload, hardware, compiler flags, payload size, sample count, and percentile definition. Then give the measured numbers from [BENCHMARKS.md](BENCHMARKS.md).

**What does push-to-pop mean here?** Timestamp before constructing/pushing the envelope to timestamp after popping it. It includes copying, publication, coherence, scheduling, and queue residence, plus measurement overhead. It is not the cost of just one atomic or one function call.

**Why two workloads?** Handoff permits one outstanding message using a separate acknowledgement, limiting queue buildup. Saturated sends as quickly as possible into a 4095-entry queue. A slow consumer may give good throughput and poor latency due to backlog. The acknowledgement affects handoff behavior even though its full roundtrip is not inside each latency sample.

**Why p50/p99/p99.9?** They describe the middle and tails of a distribution. For 200,000 samples, nearest-rank p99.9 selects the 199,800th sorted value, leaving about 200 observations above it. They are not confidence intervals and one run is not a stable bound. Report repeatability and maximum as well.

**Histogram versus exact quantiles?** The histogram stores finite-resolution bucket counts and returns bucket lower bounds. Its overflow bucket means at least 50 microseconds. The benchmark preallocates a sample vector, then sorts after the timed phase to calculate exact nearest-rank quantiles. Recording samples affects workload even when sorting does not.

**Why TSC?** It provides a fine-grained counter on the supported x86 host. Calibration estimates nanoseconds per counter tick against `steady_clock`; these ticks are not necessarily current core-clock cycles. The timer uses ordering around reads. Intel documents that plain RDTSC is not serializing: [instruction reference](https://cdrdv2-public.intel.com/782156/325383-sdm-vol-2abcd.pdf). CPU/VM behavior and synchronized counters across cores still need validation. Do not quote a universal instruction cost.

**What can bias results?** Thread migration, SMT siblings, NUMA, cache warmth, page faults, background tasks, power state, turbo behavior, instrumentation, compiler optimization, and virtualization. Pin each thread separately for controlled topology comparisons; pinning the whole process to one core can make a two-thread spin benchmark worse.

**What about coordinated omission?** Closed-loop handoff stops issuing while waiting for acknowledgement. It does not represent an independent arrival stream during stalls. Saturation is also not a market workload. A future open-loop replay should preserve scheduled arrival times and include overload, loss, and end-to-end completion.

**Did you achieve 100/300 ns?** “Those initial numbers were unverified. I audited the benchmark, corrected histogram/timing issues, and replaced them with recorded results for defined workloads.” State the new results; do not retroactively describe them as the original measurement.

**Why no speedup percentage?** There is no controlled baseline. Compare against a bounded mutex queue using the same payload, arrival process, core placement, and measurement boundaries before claiming improvement. Include CPU utilization and loss, not just latency.

## Difficult questions to rehearse

| Question | What a strong answer must contain |
| --- | --- |
| Why not use a mutex? | Simpler synchronization is valid; topology motivated SPSC, measured comparison still pending |
| Why C++20? | Build standard versus features actually used; value layout, atomics, lifetime control; no invented C++20 feature usage |
| What happens on disconnect? | Current reconnect defects; desired session ownership, backoff, cancellation and resubscription |
| Why not atomics on every BBO field? | Per-field race freedom does not provide a coherent multi-field snapshot |
| Could a seqlock fix telemetry? | Ordinary concurrent C++ reads/writes remain data races without a valid implementation; prefer an owned snapshot queue |
| What did testing prove? | Ring FIFO and boundary behavior plus histogram cases; not a mathematical proof or full network validation |
| What is your worst bug? | Histogram out-of-bounds and discarded tails undermined measurement; regression tests and explicit boundaries address it |
| Is this production ready? | No; describe specific missing acceptance criteria and implementation priorities |

## Two-day preparation plan

**Day one, first half:** Read the schema and ring line by line. Draw ownership for empty, partially full, full, and wraparound states. Reproduce tests. Explain both acquire/release edges without notes.

**Day one, second half:** Run both benchmark modes repeatedly. Explain why they differ. Read the audit and rehearse the opening explanation plus the allocator and BBO corrections. Know where every claimed number comes from.

**Day two, first half:** Practice resource lifetime and failure scenarios: callbacks outliving locals, reconnect during outstanding operations, consumer lag, dropped quotes, malformed messages, and shutdown. Outline fixes before coding them.

**Day two, second half:** Give a five-minute source walkthrough and a ten-minute adversarial mock interview. Reproduce one benchmark run, interpret it, then discuss the next engineering step. Avoid adding new features immediately before the interview at the expense of understanding the existing code.

## Five-minute demonstration

1. Open the README and explain current scope.
2. Run the offline test executables.
3. Show `push` and `pop`; explain slot ownership and memory ordering.
4. Run handoff and saturated modes; explain measurement boundaries and result variation.
5. Open the audit and identify the highest-priority live-adapter fix.

A successful defense shows precise reasoning and ownership of limitations. Memorized speed claims are much easier to challenge than reproducible experiments.

## Source walkthrough: how the pieces work together

Read the files in this order: `market_data.hpp`, `spsc_ring.hpp`, `timing.hpp`, `polygon_decoder.hpp`, `test_core.cpp`, `bench_ring.cpp`, then the experimental `feed_handler.hpp` and `main.cpp`. The README links directly to each file.

### C++ types and lifetime

`Price`, `Qty`, and `Nanos` are aliases of integer types, not distinct strong types. The compiler therefore cannot prevent accidentally mixing a price and a timestamp. Strong wrapper types are a possible future improvement.

`alignas(64)` raises an object's alignment requirement. `static_assert` verifies the expected payload size at compile time. The tagged union holds either a quote or a trade; the tag tells the reader which member is active. Reading an inactive member is not a general-purpose conversion. The default message has a Status tag but no initialized quote/trade payload, so code must check the tag before inspecting one.

The ring's `std::array<T, N>` owns its storage. `push(const T&)` copies into a slot; `pop()` copies out into an optional value before releasing the slot. These are value copies, not a zero-copy pipeline. For generic types, copying could allocate or throw; the current methods are `noexcept`, so a throwing payload operation would terminate. The fixed-size message used here avoids that kind of ownership work.

`string_view` borrows bytes rather than owning them. The decoder's views remain valid only while the frame buffer remains valid. Symbols are copied into the message before it crosses the queue boundary, so queued messages do not retain a view into the network buffer.

### A small ring example

For N=8, start with head=tail=0. Seven successful pushes fill slots 0 through 6 and leave head=7, tail=0. The next candidate head is `(7+1)&7`, or zero, which equals tail: the queue is full. Pop slot zero and release tail=1. The producer can now write slot seven and wrap head to zero. Slot zero becomes available for a later push only after the consumer's release has been observed.

`size()` loads two indices independently. During concurrent activity this is a diagnostic estimate rather than a consistent snapshot of both indices at one instant. Never use a prior `size()` or `empty()` result as a replacement for checking the return value of `push` or `pop`.

### Asynchronous I/O is a state machine

The intended sequence is DNS resolution, TCP connection, TLS handshake, WebSocket upgrade, authentication, subscription, and repeated reads. Each completion callback initiates the next operation. `io_context::run()` executes ready callbacks on the calling I/O thread; asynchronous does not mean one new thread per operation.

TLS encrypts/authenticates the transport, while WebSocket frames application messages over TCP. SNI tells the server which hostname is requested; it does not verify the certificate hostname. Those are different responsibilities.

`enable_shared_from_this` lets callbacks retain the session object through `shared_from_this`. The reference must originate from a valid shared owner, as in the application's `make_shared`. It extends the session lifetime but not the lifetime of a separate local write buffer. A strand serializes handlers submitted through that strand when multiple threads run the executor; it does not make unrelated consumer/statistics accesses safe.

The current reconnect design intends exponential backoff capped at 30 seconds. Its implementation is unfinished: the SSL-context pointer is unset, the stream reset is unsafe with outstanding operations, and connection setup needs a corrected API call. Explain the intended state machine separately from these actual defects.

### Consumer, reporting, and shutdown

The BBO consumer scans a fixed array of at most 64 symbols, then updates bid, ask, sizes, and local update time. Lookup is linear in the symbol limit, not a hash-table lookup. For this small bounded prototype it is easy to inspect; larger symbol universes need an explicit indexing strategy and capacity policy.

The stats thread reads ordinary BBO and histogram fields concurrently, so the current live path has data races. A stop flag being atomic does not protect those fields. A practical redesign is to keep book/histogram ownership on the consumer and send immutable snapshots to the reporting thread.

Normal shutdown signals the worker and joins threads. An exception before manual joins can still leave a joinable `std::thread`, whose destructor terminates the process. A scope guard or carefully designed `std::jthread` ownership can help, but workers also need a working cancellation condition; automatic joining alone does not stop a spinning loop. Decide explicitly whether shutdown drains or discards queued messages.

### Build and test boundaries

CMake selects C++20 and registers the decoder and core executables with CTest. The benchmark is a separate executable, not a pass/fail latency test: scheduling makes fixed latency thresholds unreliable on shared machines. The default build omits the unfinished network adapter and its Boost/OpenSSL dependencies.

The concurrent test checks one million increasing integers for FIFO order without loss or duplication. It does not test multiple producers, and it should not: those are outside the queue contract. Sanitizers test executed paths; passing them does not establish that every input, interleaving, or live-network failure is safe.

To check your understanding, explain why both index directions need synchronization, why a retained session can still have a dangling buffer, why a queue can have high throughput and high latency, and why allocation-free message storage does not imply an allocation-free application. Those four explanations connect most of the project's design choices.
