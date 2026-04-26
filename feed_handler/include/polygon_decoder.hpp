#pragma once
#include "market_data.hpp"
#include "timing.hpp"
#include <string_view>
#include <optional>
#include <charconv>
#include <cstring>
#include <cstdio>

// ─────────────────────────────────────────────────────────────────────────────
// Polygon.io WebSocket feed decoder
//
// Polygon sends JSON arrays of events over WebSocket.
// We subscribe to:
//   Q.*  — quotes (BBO updates)
//   T.*  — trades
//
// Example quote message:
//   [{"ev":"Q","sym":"AAPL","bp":182.50,"bs":3,"ap":182.51,"as":5,"t":1700000000000}]
//
// Design: hand-rolled JSON field extraction (no full parse tree).
// Interview talking point: simdjson or RapidJSON add ~50-200ns per message.
// For MVP a hand-rolled scanner is fine and demonstrates you understand the cost.
//
// In production you would:
//   - Use simdjson's On-Demand API (parses only fields you touch, ~100ns/msg)
//   - Or better: switch to Polygon's binary WebSocket format (flatbuffers)
// ─────────────────────────────────────────────────────────────────────────────

namespace fh {

class PolygonDecoder {
public:
    struct Stats {
        uint64_t quotes_decoded  = 0;
        uint64_t trades_decoded  = 0;
        uint64_t parse_errors    = 0;
        uint64_t unknown_type    = 0;
    };

    explicit PolygonDecoder(const TscClock& clk) : clk_(clk) {}

    // Parse a raw WebSocket text frame (JSON array) and call cb for each message.
    // cb signature: void(MarketMsg&&)
    template<typename Callback>
    void decode_frame(std::string_view frame, Callback&& cb) {
        // Polygon sends arrays: [{"ev":"Q",...},{"ev":"T",...}]
        // Iterate over each object in the array
        const char* p   = frame.data();
        const char* end = p + frame.size();

        while (p < end) {
            // Find next '{'
            while (p < end && *p != '{') ++p;
            if (p >= end) break;
            const char* obj_start = p;

            // Find matching '}' — naive but fine for flat objects
            int depth = 0;
            const char* obj_end = p;
            while (obj_end < end) {
                if (*obj_end == '{') ++depth;
                else if (*obj_end == '}') { --depth; if (depth == 0) { ++obj_end; break; } }
                ++obj_end;
            }
            std::string_view obj{obj_start, static_cast<size_t>(obj_end - obj_start)};

            int64_t recv_ts = clk_.now_ns();  // timestamp immediately on decode

            auto ev = extract_string(obj, "ev");
            if (!ev) { ++stats_.parse_errors; p = obj_end; continue; }

            if (*ev == "Q") {
                if (auto q = decode_quote(obj, recv_ts)) {
                    ++stats_.quotes_decoded;
                    cb(MarketMsg{*q});
                } else {
                    ++stats_.parse_errors;
                }
            } else if (*ev == "T") {
                if (auto t = decode_trade(obj, recv_ts)) {
                    ++stats_.trades_decoded;
                    cb(MarketMsg{*t});
                } else {
                    ++stats_.parse_errors;
                }
            } else if (*ev == "status") {
                // Polygon sends {"ev":"status","status":"connected"} on connect
                // Not an error, just informational
            } else {
                ++stats_.unknown_type;
            }

            p = obj_end;
        }
    }

    const Stats& stats() const { return stats_; }
    void reset_stats() { stats_ = {}; }

private:
    const TscClock& clk_;
    Stats stats_;

    std::optional<Quote> decode_quote(std::string_view obj, int64_t recv_ts) {
        auto sym = extract_string(obj, "sym");
        auto bp  = extract_double(obj, "bp");   // bid price
        auto ap  = extract_double(obj, "ap");   // ask price
        auto bs  = extract_int64(obj, "bs");    // bid size
        auto as_ = extract_int64(obj, "as");    // ask size
        auto t   = extract_int64(obj, "t");     // exchange timestamp ms

        if (!sym || !bp || !ap || !bs || !as_ || !t) return std::nullopt;

        Quote q;
        q.recv_ts = recv_ts;
        q.exch_ts = *t * 1'000'000LL;  // ms → ns
        q.bid_px  = to_price(*bp);
        q.ask_px  = to_price(*ap);
        q.bid_sz  = *bs;
        q.ask_sz  = *as_;
        q.set_symbol(*sym);
        return q;
    }

    std::optional<Trade> decode_trade(std::string_view obj, int64_t recv_ts) {
        auto sym = extract_string(obj, "sym");
        auto p   = extract_double(obj, "p");    // price
        auto s   = extract_int64(obj, "s");     // size
        auto t   = extract_int64(obj, "t");     // timestamp ms
        auto i   = extract_int64(obj, "i");     // trade id (optional)

        if (!sym || !p || !s || !t) return std::nullopt;

        Trade tr;
        tr.recv_ts  = recv_ts;
        tr.exch_ts  = *t * 1'000'000LL;
        tr.price    = to_price(*p);
        tr.size     = *s;
        tr.trade_id = i.value_or(0);
        tr.set_symbol(*sym);
        return tr;
    }

    // ── Minimal JSON field extractors ─────────────────────────────────────────
    // These avoid full JSON parsing — they scan for the key and read the value.
    // Fast enough for MVP; replace with simdjson On-Demand for production.

    static std::optional<std::string_view> extract_string(
            std::string_view obj, std::string_view key) {
        // Search for "key":"value"
        char needle[64];
        int n = snprintf(needle, sizeof(needle), "\"%.*s\":\"",
                         static_cast<int>(key.size()), key.data());
        auto pos = obj.find(std::string_view{needle, static_cast<size_t>(n)});
        if (pos == std::string_view::npos) return std::nullopt;
        pos += n;
        auto end = obj.find('"', pos);
        if (end == std::string_view::npos) return std::nullopt;
        return obj.substr(pos, end - pos);
    }

    static std::optional<double> extract_double(
            std::string_view obj, std::string_view key) {
        char needle[64];
        int n = snprintf(needle, sizeof(needle), "\"%.*s\":",
                         static_cast<int>(key.size()), key.data());
        auto pos = obj.find(std::string_view{needle, static_cast<size_t>(n)});
        if (pos == std::string_view::npos) return std::nullopt;
        pos += n;
        while (pos < obj.size() && obj[pos] == ' ') ++pos;
        double val = 0;
        auto [ptr, ec] = std::from_chars(
            obj.data() + pos, obj.data() + obj.size(), val);
        if (ec != std::errc{}) return std::nullopt;
        return val;
    }

    static std::optional<int64_t> extract_int64(
            std::string_view obj, std::string_view key) {
        char needle[64];
        int n = snprintf(needle, sizeof(needle), "\"%.*s\":",
                         static_cast<int>(key.size()), key.data());
        auto pos = obj.find(std::string_view{needle, static_cast<size_t>(n)});
        if (pos == std::string_view::npos) return std::nullopt;
        pos += n;
        while (pos < obj.size() && obj[pos] == ' ') ++pos;
        int64_t val = 0;
        auto [ptr, ec] = std::from_chars(
            obj.data() + pos, obj.data() + obj.size(), val);
        if (ec != std::errc{}) return std::nullopt;
        return val;
    }
};

} // namespace fh
