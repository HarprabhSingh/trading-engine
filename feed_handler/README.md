# Feed handler module

See the [repository README](../README.md) for scope, build instructions, and implementation status.

- [Benchmark methodology](../docs/BENCHMARKS.md)
- [Implementation status](../docs/IMPLEMENTATION_STATUS.md)
- [Engineering notes](../docs/ENGINEERING_NOTES.md)

`include/` contains the schema, queue, decoder, timer, and experimental live adapter. `tests/` contains offline correctness checks and two benchmark workloads. `src/main.cpp` contains the experimental BBO consumer and live entry point.

The default CMake build intentionally covers offline components. `-DBUILD_LIVE_FEED=ON` opts into the unfinished transport, requiring Boost 1.81+ and OpenSSL. This option is not a claim that live integration has been validated. Provider subscriptions and entitlements must be checked with the provider; the old free-tier claims have been removed.
