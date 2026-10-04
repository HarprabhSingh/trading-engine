# Benchmark method and results

## What is measured

`bench_ring` measures a local, one-way interval from immediately before enqueue to immediately after dequeue, for the actual 128-byte `MarketMsg`. It includes timestamp overhead, message copies, cache-coherence effects, scheduling, and queue residence. It does not include network receipt or JSON decoding.

- `handoff`: one outstanding message; a separate acknowledgement gates the next send. This is a closed-loop experiment and includes no sustained queue backlog.
- `saturated`: producer retries as fast as possible, with up to 4095 queued messages. This measures latency under that offered load, not minimum queue operation cost.

Each run warms up with 10,000 messages and records the next 200,000. Sample storage is allocated before the worker starts. Samples are sorted after joining the consumer; p50/p99/p99.9 use nearest-rank selection. No positive tail samples are discarded. On a full queue the producer timestamps each new attempt; the reported latency excludes waiting in previously failed attempts. It is admitted-message latency, not end-to-end admission delay. Negative deltas are counted and make the run fail. Throughput includes warmup and final thread join, so it is a coarse workload rate, not an isolated queue throughput guarantee.

The timer calibrates against `steady_clock` and uses LFENCE/RDTSC/LFENCE on this x86 host. This is a local measurement technique, not proof of clock synchronization across all CPUs or VMs. No overhead subtraction is applied. Calibration and scheduler noise remain part of the uncertainty.

## Reproduce

```sh
cmake -S feed_handler -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/bench_ring handoff
./build/bench_ring saturated
```

Repeat each mode at least three times. Save every run, including slow ones. Record the CPU, OS, compiler/version, optimization flags, topology/affinity, payload size, queue capacity, sample count, calibration, background load, and source revision. Report per-run quantiles or ranges; averaging quantiles is not a pooled percentile.

## Interpretation limits

These runs are unpinned on a general-purpose Windows machine. They do not establish isolated-core performance, a service-level bound, an exchange latency, or a relative speedup over mutex queues. The closed-loop workload can underrepresent stalls in an independent arrival stream. A future replay benchmark should schedule arrivals independently and account for rejected events and backlog.

Histogram estimates and exact sample quantiles are different. The application histogram uses 1 ns buckets below 500 ns, 10 ns buckets below 5 us, 100 ns buckets below 50 us, and a final overflow bucket. Returned bucket values are lower bounds; 50,000 ns means at least that much. Exact benchmark quantiles are not capped at 50 us.

## Original benchmark defects

The original code streamed one million events, discarded deltas of at least 1 ms, and printed expected 100/300 ns figures. Those expectations were not run output. Its histogram could write outside its array starting at 12.4 us, and a one-sample percentile could report zero. Calibration's signed accumulator overflowed. Consequently, the original version is not suitable evidence for the resume claim.

The corrected core has regression coverage for the histogram boundary and nearest-rank cases, full/empty and wraparound behavior, and one million ordered concurrent integer transfers. This is functional testing, not a formal proof, full parser validation, or a complete allocation audit.

## Recorded environment

- Date: 2026-09-20.
- CPU: Intel Core i5-1035G1, eight logical processors reported by the environment.
- OS: Windows NT 10.0.26200.0.
- Compiler: portable LLVM-MinGW 20260616, Clang 22.1.8, target `x86_64-w64-windows-gnu`, POSIX thread model.
- Flags: `-std=c++20 -O3 -Wall -Wextra -I feed_handler/include`.
- Affinity: unpinned; background desktop load uncontrolled.
- Source: working-tree changes based on commit `321e31eb25f1c391197d26bea4d361198be4048c`; source hashes are saved alongside raw output.
- Compiler source: [LLVM-MinGW release](https://github.com/mstorsjo/llvm-mingw/releases/tag/20260616). Toolchain is stored in ignored `.tools/`; binaries in ignored `build/`.

The old MinGW GCC 9.2 installation lacks the required floating `from_chars` and native `std::thread` support. The installed MSVC build tools lack Windows SDK headers. Neither failed build is a passing validation result.

## Measured results

The [initial pilot](results/2026-09-20-pilot.txt) is included alongside [all five repeated runs per mode](results/2026-09-20-windows.txt). Nothing was selected because it matched the resume. [Source hashes](results/2026-09-20-source-hashes.txt) identify the working-tree inputs (comment cleanup after compilation did not change executable behavior).

| Workload / run | p50 (ns) | p99 (ns) | p99.9 (ns) | max (ns) |
| --- | ---: | ---: | ---: | ---: |
| Handoff pilot | 207 | 299 | 5,904 | 364,345 |
| Handoff 1 | 129 | 253 | 13,498 | 435,464 |
| Handoff 2 | 121 | 205 | 1,286 | 109,473 |
| Handoff 3 | 62 | 233 | 5,692 | 99,877 |
| Handoff 4 | 40 | 95 | 567 | 55,266 |
| Handoff 5 | 53 | 234 | 657 | 282,040 |
| Saturated 1 | 194,080 | 486,830 | 523,345 | 524,603 |
| Saturated 2 | 8,463 | 15,389 | 27,765 | 27,931 |
| Saturated 3 | 200,594 | 811,651 | 867,573 | 869,027 |
| Saturated 4 | 190,225 | 441,181 | 469,877 | 483,506 |
| Saturated 5 | 491,305 | 791,751 | 837,324 | 846,452 |

All recorded runs had zero negative samples. Across the six handoff runs, p50 ranged from **40 to 207 ns**, p99 from **95 to 299 ns**, and p99.9 from **567 to 13,498 ns**. These are ranges of run quantiles, not quantiles computed from pooled data. The wide variation makes a single “100/300 ns” headline misleading.

Saturated p50 ranged from **8.46 to 491.31 us**, with p99 **15.39 to 811.65 us**. This demonstrates the importance of workload and queue residence. Attribution to particular cores, SMT, or scheduler events would require profiling; the current data alone does not establish those causes.

For an interview: “On an i5-1035G1 Windows desktop, six unpinned closed-loop handoff runs measured p50 40–207 ns and p99 95–299 ns for 128-byte messages, with 200,000 measured samples per run. Sustained queue load produced much larger latency, and I documented both.”

## Validation completed locally

- Portable Clang release build of `test_core`, `test_decoder`, and `bench_ring` succeeded.
- Core tests passed: boundary/overflow percentiles, FIFO wraparound/full/empty, one million concurrent ordered transfers.
- Existing decoder suite: 33 checks passed, zero failed. Its coverage is intentionally limited and does not establish arbitrary JSON correctness.
- AddressSanitizer + UndefinedBehaviorSanitizer builds of both test executables also passed locally, with no reported findings on these test inputs. This does not include ThreadSanitizer or live integration.
- Live network target and hosted CI execution are not validated by these results.

## Windows portable-toolchain commands

From the repository root in PowerShell, using the locally downloaded toolchain:

```powershell
$env:PATH = (Resolve-Path .tools/llvm-mingw-20260616-ucrt-x86_64/bin).Path + ';' + $env:PATH
./feed_handler/scripts/build-core.ps1
./build/bench_ring.exe handoff
./build/bench_ring.exe saturated
```

For sanitizer builds, add `-O1 -g '-fsanitize=address,undefined' -fno-omit-frame-pointer` instead of `-O3`, and include the toolchain's `x86_64-w64-mingw32/bin` in PATH for the sanitizer runtime. Do not use instrumented builds for latency claims.
