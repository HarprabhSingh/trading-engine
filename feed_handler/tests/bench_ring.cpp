#include "spsc_ring.hpp"
#include "market_data.hpp"
#include "timing.hpp"
#include <thread>
#include <vector>
#include <string_view>
#include <algorithm>
#include <cstdio>
#include <cinttypes>

// One-way timestamp-before-enqueue to timestamp-after-pop, including copies,
// cache coherence, queue residence, scheduling, and timestamp overhead.
// handoff: one outstanding message; saturated: producer runs freely.
int main(int argc, char** argv) {
    const std::string_view mode = argc > 1 ? argv[1] : "handoff";
    if (mode != "handoff" && mode != "saturated") {
        std::fprintf(stderr, "Usage: bench_ring [handoff|saturated]\n");
        return 1;
    }
    constexpr size_t warmup = 10000, samples = 200000, total = warmup + samples;
    fh::TscClock clk;
    clk.calibrate();
    fh::SPSCRing<fh::MarketMsg, 4096> ring;
    std::vector<int64_t> latencies(samples); // allocated before either worker runs
    alignas(64) std::atomic<size_t> acknowledged{0};
    std::atomic<bool> ready{false};
    size_t retries = 0;
    std::thread consumer([&] {
        ready.store(true, std::memory_order_release);
        for (size_t i = 0; i < total;) {
            auto msg = ring.pop();
            if (!msg) { fh::spin_pause(); continue; }
            const int64_t latency = clk.now_ns() - msg->quote.recv_ts;
            if (i >= warmup) latencies[i - warmup] = latency;
            ++i;
            if (mode == "handoff") acknowledged.store(i, std::memory_order_release);
        }
    });
    while (!ready.load(std::memory_order_acquire)) fh::spin_pause();
    const auto begin = std::chrono::steady_clock::now();
    fh::Quote q;
    q.set_symbol("BENCH");
    q.bid_px = 100000000;
    q.ask_px = 100010000;
    for (size_t i = 0; i < total;) {
        if (mode == "handoff")
            while (acknowledged.load(std::memory_order_acquire) != i) fh::spin_pause();
        q.recv_ts = clk.now_ns();
        if (ring.push(fh::MarketMsg{q})) ++i;
        else { ++retries; fh::spin_pause(); }
    }
    consumer.join();
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    const auto invalid = std::count_if(latencies.begin(), latencies.end(), [](auto n) { return n < 0; });
    std::sort(latencies.begin(), latencies.end());
    auto percentile = [&](double p) { return latencies[static_cast<size_t>(std::ceil(p * samples)) - 1]; };
    std::printf("mode=%.*s samples=%zu warmup=%zu affinity=unpinned message_bytes=%zu ring_slots=4096 usable=4095\n",
                static_cast<int>(mode.size()), mode.data(), samples, warmup, sizeof(fh::MarketMsg));
    std::printf("ns_per_tick=%.9f negative_samples=%zu retries=%zu elapsed_s=%.6f messages_per_s=%.0f\n",
                clk.ns_per_tick, static_cast<size_t>(invalid), retries, elapsed, total / elapsed);
    std::printf("exact_p50_ns=%" PRId64 " exact_p99_ns=%" PRId64 " exact_p999_ns=%" PRId64 " max_ns=%" PRId64 "\n",
                percentile(.5), percentile(.99), percentile(.999), latencies.back());
    std::puts("No tail samples discarded. Quantiles include timestamp overhead. Throughput includes warmup and join.\nHandoff is a closed-loop experiment; saturated includes queueing. Neither measures network latency.");
    return invalid ? 2 : 0;
}
