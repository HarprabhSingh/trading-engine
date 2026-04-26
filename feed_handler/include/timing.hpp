#pragma once
#include <cstdint>
#include <chrono>
#include <array>
#include <algorithm>
#include <numeric>

// ─────────────────────────────────────────────────────────────────────────────
// rdtsc-based timer
//
// Interview talking points:
//   clock_gettime() costs ~20-30 ns (a vDSO call).  rdtsc is ~7 cycles (~3 ns).
//   For timestamping inside the hot decode loop, this matters.
//
//   Caveat: TSC frequency varies at boot and across cores (though modern Intel
//   CPUs with invariant TSC are stable).  We calibrate once at startup against
//   CLOCK_REALTIME to convert ticks → nanoseconds.
// ─────────────────────────────────────────────────────────────────────────────

namespace fh {

inline uint64_t rdtsc() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
#else
    // Fallback for non-x86 (ARM, etc.)
    return static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

// Calibrates TSC frequency at startup. Returns ns per tick.
inline double calibrate_tsc(int samples = 10) {
    double total = 0;
    for (int i = 0; i < samples; ++i) {
        auto t1 = std::chrono::high_resolution_clock::now();
        uint64_t r1 = rdtsc();
        // busy wait ~1ms
        volatile int x = 0;
        for (int j = 0; j < 1'000'000; ++j) x += j;
        uint64_t r2 = rdtsc();
        auto t2 = std::chrono::high_resolution_clock::now();

        double ns = std::chrono::duration<double, std::nano>(t2 - t1).count();
        double ticks = static_cast<double>(r2 - r1);
        total += ns / ticks;
    }
    return total / samples;
}

// Global calibration result (set once at startup)
struct TscClock {
    double ns_per_tick = 0.0;
    uint64_t epoch_offset_ns = 0;  // wall clock ns at startup

    void calibrate() {
        ns_per_tick = calibrate_tsc();
        auto now = std::chrono::system_clock::now().time_since_epoch();
        epoch_offset_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
        uint64_t tsc_now = rdtsc();
        // adjust epoch so tsc_to_ns(tsc_now) == epoch_offset_ns
        epoch_offset_ns -= static_cast<uint64_t>(tsc_now * ns_per_tick);
    }

    int64_t tsc_to_ns(uint64_t tsc) const noexcept {
        return static_cast<int64_t>(tsc * ns_per_tick) + epoch_offset_ns;
    }

    int64_t now_ns() const noexcept {
        return tsc_to_ns(rdtsc());
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Latency histogram — tracks p50/p99/p999 without heap allocation
// ─────────────────────────────────────────────────────────────────────────────
template<size_t BUCKETS = 1024>
class LatencyHistogram {
    std::array<uint64_t, BUCKETS> counts_{};
    uint64_t total_  = 0;
    uint64_t sum_ns_ = 0;
    int64_t  min_ns_ = INT64_MAX;
    int64_t  max_ns_ = 0;

    // Bucket edges: 0–100ns in 1ns steps, then log-ish beyond
    static int bucket(int64_t ns) noexcept {
        if (ns < 0)   return 0;
        if (ns < 500) return static_cast<int>(ns);          // 1 ns resolution < 500ns
        if (ns < 5000)return 500 + static_cast<int>((ns - 500) / 10);   // 10ns res
        if (ns < 50000)return 950 + static_cast<int>((ns - 5000) / 100); // 100ns res
        return static_cast<int>(BUCKETS) - 1;
    }

public:
    void record(int64_t ns) noexcept {
        ++counts_[bucket(ns)];
        ++total_;
        sum_ns_ += ns;
        min_ns_ = std::min(min_ns_, ns);
        max_ns_ = std::max(max_ns_, ns);
    }

    int64_t percentile(double pct) const noexcept {
        uint64_t target = static_cast<uint64_t>(total_ * pct / 100.0);
        uint64_t running = 0;
        for (size_t i = 0; i < BUCKETS; ++i) {
            running += counts_[i];
            if (running >= target) {
                // reverse map bucket → ns (approximate)
                if (i < 500)  return static_cast<int64_t>(i);
                if (i < 950)  return 500 + static_cast<int64_t>(i - 500) * 10;
                return 5000 + static_cast<int64_t>(i - 950) * 100;
            }
        }
        return max_ns_;
    }

    int64_t mean_ns()  const noexcept { return total_ ? sum_ns_ / total_ : 0; }
    int64_t min_ns()   const noexcept { return min_ns_ == INT64_MAX ? 0 : min_ns_; }
    int64_t max_ns()   const noexcept { return max_ns_; }
    uint64_t count()   const noexcept { return total_; }

    void print(const char* label) const {
        printf("[%s] n=%lu mean=%ldns p50=%ldns p99=%ldns p999=%ldns max=%ldns\n",
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
