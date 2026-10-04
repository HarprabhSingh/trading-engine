#pragma once
#include <cstdint>
#include <cstring>
#include <string_view>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// Internal normalized message schema
//
// Design decisions (interview talking points):
//   1. Fixed-size POD structs — no heap allocation, memcpy-safe, cache-friendly
//   2. Timestamps in nanoseconds since epoch — matches exchange precision
//   3. Prices as int64 scaled integers (price * 1,000,000) — eliminates float rounding
//   4. Symbol as fixed char[8] — avoids std::string heap alloc in hot path
//   5. Separate recv_ts (when we got it) vs exchange_ts (when exchange sent it)
//      — subtraction alone does not establish network latency or clock sync
// ─────────────────────────────────────────────────────────────────────────────

namespace fh {

// Price is stored as integer: $123.45 → 123450000  (6 decimal places)
// Integer arithmetic is exact after conversion; input double rounding remains.
using Price  = int64_t;
using Qty    = int64_t;
using Nanos  = int64_t;  // nanoseconds since Unix epoch

constexpr int64_t PRICE_SCALE = 1'000'000;  // 6 decimal places

inline Price to_price(double d)  { return static_cast<Price>(d * PRICE_SCALE + 0.5); }
inline double from_price(Price p){ return static_cast<double>(p) / PRICE_SCALE; }

enum class MsgType : uint8_t {
    Quote  = 1,   // best bid/offer update
    Trade  = 2,   // execution
    Status = 3,   // trading status / halt
};

enum class Side : uint8_t { Buy = 1, Sell = 2 };

// ── Quote (BBO update) ────────────────────────────────────────────────────────
// Emitted whenever bid or ask changes.  Downstream order book only cares about
// bid_px, ask_px, bid_sz, ask_sz.
struct alignas(64) Quote {         // 64-byte cache line aligned
    MsgType msg_type   = MsgType::Quote;
    uint8_t  _pad[7]   = {};

    char     symbol[8] = {};       // null-padded, NOT null-terminated if full
    Nanos    recv_ts   = 0;        // rdtsc-calibrated ns — when we received pkt
    Nanos    exch_ts   = 0;        // exchange-provided timestamp (ms → ns)

    Price    bid_px    = 0;
    Price    ask_px    = 0;
    Qty      bid_sz    = 0;
    Qty      ask_sz    = 0;

    void set_symbol(std::string_view s) {
        size_t n = std::min(s.size(), size_t{8});
        std::memcpy(symbol, s.data(), n);
    }
    std::string_view get_symbol() const {
        size_t n = 0;
        while (n < 8 && symbol[n]) ++n;
        return {symbol, n};
    }
};
static_assert(sizeof(Quote) == 64, "Quote must be exactly one cache line");

// ── Trade ─────────────────────────────────────────────────────────────────────
struct alignas(64) Trade {
    MsgType msg_type   = MsgType::Trade;
    Side     side      = Side::Buy;
    uint8_t  _pad[6]   = {};

    char     symbol[8] = {};
    Nanos    recv_ts   = 0;
    Nanos    exch_ts   = 0;

    Price    price     = 0;
    Qty      size      = 0;
    uint64_t trade_id  = 0;

    void set_symbol(std::string_view s) {
        size_t n = std::min(s.size(), size_t{8});
        std::memcpy(symbol, s.data(), n);
    }
    std::string_view get_symbol() const {
        size_t n = 0;
        while (n < 8 && symbol[n]) ++n;
        return {symbol, n};
    }
};
static_assert(sizeof(Trade) == 64);

// ── Generic envelope (tagged union, no heap) ──────────────────────────────────
// What actually goes onto the SPSC ring buffer.
struct MarketMsg {
    MsgType type;
    union {
        Quote quote;
        Trade trade;
    };

    explicit MarketMsg(Quote q) : type(MsgType::Quote), quote(q) {}
    explicit MarketMsg(Trade t) : type(MsgType::Trade),  trade(t) {}
    MarketMsg() : type(MsgType::Status) {}
};

} // namespace fh
