#include "polygon_decoder.hpp"
#include "spsc_ring.hpp"
#include "timing.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// Unit tests for PolygonDecoder
// Run these without needing any API key or network connection.
// These are the tests you demo in an interview to show production discipline.
// ─────────────────────────────────────────────────────────────────────────────

static int passed = 0, failed = 0;
#define ASSERT(cond, msg) \
    do { if (cond) { ++passed; printf("  PASS: %s\n", msg); } \
         else { ++failed; printf("  FAIL: %s  [line %d]\n", msg, __LINE__); } } while(0)

fh::TscClock g_clk;

void test_quote_decode() {
    printf("test_quote_decode\n");
    fh::PolygonDecoder dec(g_clk);
    int count = 0;

    std::string frame =
        R"([{"ev":"Q","sym":"AAPL","bp":182.50,"bs":300,"ap":182.51,"as":500,"t":1700000000000}])";

    dec.decode_frame(frame, [&](fh::MarketMsg msg) {
        ++count;
        ASSERT(msg.type == fh::MsgType::Quote, "msg type is Quote");
        ASSERT(msg.quote.get_symbol() == "AAPL", "symbol is AAPL");
        ASSERT(msg.quote.bid_px == fh::to_price(182.50), "bid price correct");
        ASSERT(msg.quote.ask_px == fh::to_price(182.51), "ask price correct");
        ASSERT(msg.quote.bid_sz == 300, "bid size correct");
        ASSERT(msg.quote.ask_sz == 500, "ask size correct");
        ASSERT(msg.quote.exch_ts == 1700000000000LL * 1'000'000LL, "exchange timestamp ns");
        ASSERT(msg.quote.recv_ts > 0, "recv timestamp set");
    });
    ASSERT(count == 1, "exactly one message decoded");
    ASSERT(dec.stats().quotes_decoded == 1, "stats: quotes_decoded == 1");
    ASSERT(dec.stats().parse_errors == 0, "stats: no parse errors");
    printf("\n");
}

void test_trade_decode() {
    printf("test_trade_decode\n");
    fh::PolygonDecoder dec(g_clk);
    int count = 0;

    std::string frame =
        R"([{"ev":"T","sym":"MSFT","p":415.30,"s":100,"t":1700000001000,"i":9876543}])";

    dec.decode_frame(frame, [&](fh::MarketMsg msg) {
        ++count;
        ASSERT(msg.type == fh::MsgType::Trade, "msg type is Trade");
        ASSERT(msg.trade.get_symbol() == "MSFT", "symbol is MSFT");
        ASSERT(msg.trade.price == fh::to_price(415.30), "price correct");
        ASSERT(msg.trade.size  == 100, "size correct");
        ASSERT(msg.trade.trade_id == 9876543, "trade_id correct");
    });
    ASSERT(count == 1, "exactly one trade decoded");
    printf("\n");
}

void test_multi_message_frame() {
    printf("test_multi_message_frame\n");
    fh::PolygonDecoder dec(g_clk);
    int count = 0;

    // Polygon batches multiple events per frame
    std::string frame =
        R"([)"
        R"({"ev":"Q","sym":"AAPL","bp":182.50,"bs":100,"ap":182.51,"as":200,"t":1700000000001},)"
        R"({"ev":"Q","sym":"TSLA","bp":240.10,"bs":50,"ap":240.15,"as":75,"t":1700000000002},)"
        R"({"ev":"T","sym":"AAPL","p":182.505,"s":50,"t":1700000000003,"i":111})"
        R"(])";

    dec.decode_frame(frame, [&](fh::MarketMsg) { ++count; });
    ASSERT(count == 3, "three messages decoded from one frame");
    ASSERT(dec.stats().quotes_decoded == 2, "two quotes");
    ASSERT(dec.stats().trades_decoded == 1, "one trade");
    printf("\n");
}

void test_malformed_frame() {
    printf("test_malformed_frame\n");
    fh::PolygonDecoder dec(g_clk);
    int count = 0;

    // Missing required fields
    std::string frame = R"([{"ev":"Q","sym":"AAPL","bp":182.50}])";
    dec.decode_frame(frame, [&](fh::MarketMsg) { ++count; });
    ASSERT(count == 0, "malformed message not decoded");
    ASSERT(dec.stats().parse_errors == 1, "parse error counted");
    printf("\n");
}

void test_auth_status_frame() {
    printf("test_auth_status_frame\n");
    fh::PolygonDecoder dec(g_clk);
    int count = 0;

    // Status messages should not decode to market messages
    std::string frame = R"([{"ev":"status","status":"auth_success","message":"authenticated"}])";
    dec.decode_frame(frame, [&](fh::MarketMsg) { ++count; });
    ASSERT(count == 0, "status message not decoded as market msg");
    printf("\n");
}

void test_price_precision() {
    printf("test_price_precision\n");
    // Verify scaled integer price math
    double raw = 182.509999;  // float imprecision
    fh::Price p = fh::to_price(raw);
    // with 6 decimal places, 182.509999 → 182509999 (rounds to 182510000)
    ASSERT(p > 0, "price is positive");

    // Spread calculation must be exact in integer domain
    fh::Price bid = fh::to_price(182.50);
    fh::Price ask = fh::to_price(182.51);
    fh::Price spread = ask - bid;
    // $0.01 = 10000 price units
    ASSERT(spread == 10000, "spread is exactly 0.01 in integer domain");
    printf("\n");
}

void test_spsc_ring() {
    printf("test_spsc_ring (single-threaded correctness)\n");
    fh::SPSCRing<int, 8> ring;
    ASSERT(ring.empty(), "ring starts empty");

    for (int i = 0; i < 7; ++i) ring.push(i);  // fill 7 of 8 slots
    ASSERT(ring.size() == 7, "ring has 7 items");
    ASSERT(!ring.push(99), "push fails when full");

    auto v = ring.pop();
    ASSERT(v.has_value() && *v == 0, "pop returns first item");

    ASSERT(ring.push(99), "push succeeds after pop");
    printf("\n");
}

void test_latency_histogram() {
    printf("test_latency_histogram\n");
    fh::LatencyHistogram<> hist;
    // Record 100 samples at known values
    for (int i = 0; i < 100; ++i) hist.record(200);   // 200 ns
    for (int i = 0; i < 100; ++i) hist.record(5000);  // 5 µs

    ASSERT(hist.count() == 200, "count is 200");
    ASSERT(hist.percentile(50) <= 500, "p50 is ~200ns");
    ASSERT(hist.percentile(99) >= 4000, "p99 is ~5µs");
    printf("\n");
}

int main() {
    g_clk.calibrate();

    printf("═══ Feed Handler Unit Tests ═══\n\n");
    test_quote_decode();
    test_trade_decode();
    test_multi_message_frame();
    test_malformed_frame();
    test_auth_status_frame();
    test_price_precision();
    test_spsc_ring();
    test_latency_histogram();

    printf("═══ Results: %d passed, %d failed ═══\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
