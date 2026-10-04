#include "market_data.hpp"
#include "spsc_ring.hpp"
#include "timing.hpp"
#include <thread>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
int main() {
    fh::LatencyHistogram<> h;
    CHECK(h.percentile(50) == 0);
    h.record(200);
    CHECK(h.percentile(50) == 200);
    h.reset();
    for (auto n : {0, 499, 500, 4999, 5000, 12399, 12400, 49999, 50000, 1000000}) h.record(n);
    CHECK(h.count() == 10);
    CHECK(h.percentile(100) == 50000);
    CHECK(h.max_ns() == 1000000);
    h.record(-1);
    CHECK(h.count() == 10);
    h.reset();
    for (int i=0; i<99; ++i) h.record(100);
    h.record(1000);
    CHECK(h.percentile(99) == 100);
    CHECK(h.percentile(99.9) == 1000);
    fh::SPSCRing<int, 8> ring;
    for (int round=0; round<100; ++round) {
        CHECK(ring.empty());
        for (int i=0; i<7; ++i) CHECK(ring.push(i));
        CHECK(!ring.push(99));
        for (int i=0; i<7; ++i) { auto n=ring.pop(); CHECK(n && *n==i); }
        CHECK(!ring.pop());
    }
    constexpr int count=1000000;
    std::thread producer([&] { for (int i=0; i<count; ++i) while (!ring.push(i)) fh::spin_pause(); });
    for (int i=0; i<count;) { auto n=ring.pop(); if (n) { CHECK(*n==i); ++i; } else fh::spin_pause(); }
    producer.join();
    CHECK(ring.empty());
    std::printf("PASS: histogram boundaries, nearest rank, overflow, wraparound, full/empty, %d ordered concurrent transfers\n", count);
    std::printf("Quote=%zu Trade=%zu MarketMsg=%zu Ring=%zu\n", sizeof(fh::Quote), sizeof(fh::Trade), sizeof(fh::MarketMsg), sizeof(fh::SPSCRing<fh::MarketMsg,4096>));
}
