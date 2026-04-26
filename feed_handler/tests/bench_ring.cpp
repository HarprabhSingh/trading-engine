#include "spsc_ring.hpp"
#include "market_data.hpp"
#include "timing.hpp"
#include <thread>
#include <cstdio>
#include <atomic>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: SPSC ring push/pop roundtrip latency
//
// Producer thread pushes N messages.
// Consumer thread pops them and records recv_ts → pop_ts delta.
//
// Expected result on a modern Linux box:
//   p50 ~80–150 ns, p99 ~200–400 ns (higher with kernel interference)
//   → use these numbers in your README latency table
// ─────────────────────────────────────────────────────────────────────────────

static constexpr size_t N = 1'000'000;
static fh::TscClock clk;

int main() {
    clk.calibrate();
    printf("TSC: %.3f ns/tick\n", clk.ns_per_tick);

    fh::SPSCRing<fh::MarketMsg, 4096> ring;
    fh::LatencyHistogram<> hist;

    std::atomic<bool> consumer_done{false};
    size_t consumed = 0;

    // Consumer thread
    std::thread consumer([&] {
        while (consumed < N) {
            auto msg = ring.pop();
            if (!msg) { __builtin_ia32_pause(); continue; }
            int64_t now = clk.now_ns();
            int64_t lat = now - msg->quote.recv_ts;
            if (lat > 0 && lat < 1'000'000) hist.record(lat);
            ++consumed;
        }
        consumer_done = true;
    });

    // Producer (main thread)
    size_t produced = 0;
    while (produced < N) {
        fh::Quote q;
        q.bid_px  = fh::to_price(100.0);
        q.ask_px  = fh::to_price(100.01);
        q.bid_sz  = 100;
        q.ask_sz  = 100;
        q.recv_ts = clk.now_ns();  // timestamp just before push
        q.set_symbol("BENCH");

        if (ring.push(fh::MarketMsg{q})) ++produced;
        else __builtin_ia32_pause();
    }

    while (!consumer_done) std::this_thread::yield();
    consumer.join();

    printf("\n═══ SPSC Ring Latency Benchmark (N=%zu) ═══\n", N);
    hist.print("push→pop");
    printf("\nNote: pin producer to core 0 and consumer to core 1\n");
    printf("      (taskset or pthread_setaffinity) for stable numbers.\n");
    printf("Expected: p50~100ns p99~300ns without core pinning\n");

    return 0;
}
