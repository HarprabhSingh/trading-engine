#pragma once
#include <cstdint>
#include <chrono>
#include <array>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <cstdio>
#include <cinttypes>
#if defined(_MSC_VER) || defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

// Local calibrated counter timing. x86 reads use LFENCE/RDTSC/LFENCE.
// Clock ordering and cross-core synchronization are host assumptions; no fixed
// instruction-cost claim is made. Benchmark quantiles include timer overhead.

namespace fh {

inline uint64_t rdtsc() noexcept {
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
    // Ordered local timestamp. Assumes a CPU with the required LFENCE/TSC
    // semantics; cross-core synchronization must be checked on the test host.
    _mm_lfence();
    const uint64_t ticks = __rdtsc();
    _mm_lfence();
    return ticks;
#else
    // Fallback for non-x86 (ARM, etc.)
    return static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

inline void spin_pause() noexcept {
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#endif
}

// Calibrates TSC frequency at startup. Returns ns per tick.
inline double calibrate_tsc(int samples = 10) {
    double total = 0;
    for (int i = 0; i < samples; ++i) {
        auto t1 = std::chrono::steady_clock::now();
        uint64_t r1 = rdtsc();
        // Wait against a monotonic reference without arithmetic overflow.
        while (std::chrono::steady_clock::now() - t1 < std::chrono::milliseconds(2))
            spin_pause();
        uint64_t r2 = rdtsc();
        auto t2 = std::chrono::steady_clock::now();

        double ns = std::chrono::duration<double, std::nano>(t2 - t1).count();
        double ticks = static_cast<double>(r2 - r1);
        total += ns / ticks;
    }
    return total / samples;
}

// Global calibration result (set once at startup)
struct TscClock {
    double ns_per_tick = 0.0;
    int64_t epoch_offset_ns = 0;  // wall clock ns at startup
    uint64_t base_ticks = 0;

    void calibrate() {
        ns_per_tick = calibrate_tsc();
        auto now = std::chrono::system_clock::now().time_since_epoch();
        epoch_offset_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
        base_ticks = rdtsc();
    }

    int64_t tsc_to_ns(uint64_t tsc) const noexcept {
        const double delta = tsc >= base_ticks
            ? static_cast<double>(tsc - base_ticks)
            : -static_cast<double>(base_ticks - tsc);
        return static_cast<int64_t>(delta * ns_per_tick) + epoch_offset_ns;
    }

    int64_t now_ns() const noexcept {
        return tsc_to_ns(rdtsc());
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Latency histogram — tracks p50/p99/p999 without heap allocation
// ─────────────────────────────────────────────────────────────────────────────
template<size_t BUCKETS = 1401>
class LatencyHistogram {
    static_assert(BUCKETS >= 1401, "Need all finite buckets plus overflow");
    std::array<uint64_t, BUCKETS> counts_{};
    uint64_t total_  = 0;
    uint64_t sum_ns_ = 0;
    int64_t  min_ns_ = INT64_MAX;
    int64_t  max_ns_ = 0;

    // Finite-resolution buckets, then overflow at >= 50,000 ns.
    static int bucket(int64_t ns) noexcept {
        if (ns < 0)   return 0;
        if (ns < 500) return static_cast<int>(ns);          // 1 ns resolution < 500ns
        if (ns < 5000)return 500 + static_cast<int>((ns - 500) / 10);   // 10ns res
        if (ns < 50000)return 950 + static_cast<int>((ns - 5000) / 100); // 100ns res
        return 1400; // overflow: >= 50 us
    }

public:
    void record(int64_t ns) noexcept {
        if (ns < 0) return;
        ++counts_[bucket(ns)];
        ++total_;
        sum_ns_ += ns;
        min_ns_ = std::min(min_ns_, ns);
        max_ns_ = std::max(max_ns_, ns);
    }

    int64_t percentile(double pct) const noexcept {
        if (!total_ || !std::isfinite(pct)) return 0;
        pct = std::clamp(pct, 0.0, 100.0);
        uint64_t target = std::max(uint64_t{1},
            static_cast<uint64_t>(std::ceil(total_ * pct / 100.0)));
        uint64_t running = 0;
        for (size_t i = 0; i < BUCKETS; ++i) {
            running += counts_[i];
            if (running >= target) {
                // reverse map bucket → ns (approximate)
                if (i < 500)  return static_cast<int64_t>(i);
                if (i < 950)  return 500 + static_cast<int64_t>(i - 500) * 10;
                if (i < 1400) return 5000 + static_cast<int64_t>(i - 950) * 100;
                return 50000; // overflow lower bound, not an exact quantile
            }
        }
        return max_ns_;
    }

    int64_t mean_ns()  const noexcept { return total_ ? sum_ns_ / total_ : 0; }
    int64_t min_ns()   const noexcept { return min_ns_ == INT64_MAX ? 0 : min_ns_; }
    int64_t max_ns()   const noexcept { return max_ns_; }
    uint64_t count()   const noexcept { return total_; }

    void print(const char* label) const {
        printf("[%s] n=%" PRIu64 " mean=%" PRId64 "ns p50=%" PRId64 "ns p99=%" PRId64 "ns p999=%" PRId64 "ns max=%" PRId64 "ns (bucket lower bounds; >=50000ns is overflow)\n",
            label, total_, mean_ns(),
            percentile(50), percentile(99), percentile(99.9), max_ns_);
    }

    void reset() noexcept {
        counts_.fill(0);
        total_ = sum_ns_ = 0;
        min_ns_ = INT64_MAX;
        max_ns_ = 0;
    }
};

} // namespace fh
