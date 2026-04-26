#include "feed_handler.hpp"
#include "market_data.hpp"
#include "spsc_ring.hpp"
#include "timing.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/ssl/context.hpp>

#include <thread>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// MVP main
//
// Thread layout:
//   Thread 1 (main)   — Boost.Asio io_context (I/O, WS, decode, ring push)
//   Thread 2          — Order book consumer (ring pop, BBO update, latency log)
//   Thread 3          — Stats printer (every 5s, reads atomics — no locks)
//
// This is the simplest setup that demonstrates the SPSC pattern.
// Phase 2 will pin these threads to isolated cores.
// ─────────────────────────────────────────────────────────────────────────────

namespace fh { namespace net = boost::asio; }

// ── Simple order book (MVP) ───────────────────────────────────────────────────
// Just tracks BBO per symbol. Phase 2 will replace this with a proper
// price-level book. Kept intentionally simple so the feed handler is the focus.
struct BBO {
    fh::Price bid = 0, ask = 0;
    fh::Qty   bid_sz = 0, ask_sz = 0;
    int64_t   last_update_ns = 0;
};

// Track up to 64 symbols for MVP (fixed-size, no heap in hot path)
constexpr int MAX_SYMBOLS = 64;
struct SymbolBBO {
    char      sym[8] = {};
    BBO       bbo    = {};
    bool      active = false;
};
static SymbolBBO book[MAX_SYMBOLS];

static int find_or_alloc_slot(const char* sym) {
    for (int i = 0; i < MAX_SYMBOLS; ++i) {
        if (book[i].active && memcmp(book[i].sym, sym, 8) == 0) return i;
    }
    for (int i = 0; i < MAX_SYMBOLS; ++i) {
        if (!book[i].active) {
            book[i].active = true;
            memcpy(book[i].sym, sym, 8);
            return i;
        }
    }
    return -1;
}

// ── Consumer thread ───────────────────────────────────────────────────────────
void run_book_consumer(fh::MsgRing& ring,
                       fh::TscClock& clk,
                       fh::LatencyHistogram<>& hist,
                       std::atomic<bool>& stop) {
    // Pin to a dedicated core in production:
    // cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(2, &cs);
    // pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);

    while (!stop.load(std::memory_order_relaxed)) {
        auto msg = ring.pop();
        if (!msg) {
            // Ring empty — spin (in production: pause instruction to reduce power)
            __builtin_ia32_pause();
            continue;
        }

        int64_t now = clk.now_ns();

        if (msg->type == fh::MsgType::Quote) {
            const fh::Quote& q = msg->quote;

            // Latency: recv_ts is when decode started; now is when book updated
            int64_t latency_ns = now - q.recv_ts;
            if (latency_ns > 0 && latency_ns < 10'000'000)  // sanity: < 10ms
                hist.record(latency_ns);

            int slot = find_or_alloc_slot(q.symbol);
            if (slot >= 0) {
                book[slot].bbo = {q.bid_px, q.ask_px,
                                  q.bid_sz, q.ask_sz, now};
            }
        }
        // Trade handling: Phase 2 (strategy layer needs it, book doesn't)
    }
}

// ── Stats printer thread ──────────────────────────────────────────────────────
void run_stats_printer(fh::MsgRing& ring,
                       fh::LatencyHistogram<>& hist,
                       std::atomic<bool>& stop) {
    int iter = 0;
    while (!stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (stop) break;

        printf("\n═══ Stats [t=%ds] ═══\n", ++iter * 5);
        hist.print("decode→book");

        printf("Ring occupancy: %zu / %zu\n", ring.size(), fh::RING_SIZE);

        // Print BBO for active symbols
        printf("%-8s  %10s  %6s  %10s  %6s  %s\n",
               "Symbol", "Bid", "BidSz", "Ask", "AskSz", "Spread");
        for (int i = 0; i < MAX_SYMBOLS; ++i) {
            if (!book[i].active) continue;
            char sym[9] = {};
            memcpy(sym, book[i].sym, 8);
            const BBO& b = book[i].bbo;
            double spread = fh::from_price(b.ask - b.bid);
            printf("%-8s  %10.4f  %6ld  %10.4f  %6ld  %.4f\n",
                   sym,
                   fh::from_price(b.bid), b.bid_sz,
                   fh::from_price(b.ask), b.ask_sz,
                   spread);
        }
        printf("═══════════════════\n\n");
        fflush(stdout);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    // Usage: ./feed_handler <API_KEY> [SYM1 SYM2 ...]
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <POLYGON_API_KEY> [AAPL MSFT TSLA ...]\n", argv[0]);
        fprintf(stderr, "  Get a free API key at https://polygon.io (free tier = 15min delayed)\n");
        return 1;
    }

    std::string api_key = argv[1];
    std::vector<std::string> symbols;
    for (int i = 2; i < argc; ++i) symbols.push_back(argv[i]);
    if (symbols.empty()) symbols = {"AAPL", "MSFT", "TSLA", "NVDA"};

    printf("Feed Handler MVP\n");
    printf("API key: %s...  Symbols: ", api_key.substr(0, 8).c_str());
    for (const auto& s : symbols) printf("%s ", s.c_str());
    printf("\n\n");

    // ── Calibrate TSC clock ───────────────────────────────────────────────────
    printf("Calibrating TSC clock...\n");
    fh::TscClock clk;
    clk.calibrate();
    printf("TSC calibrated: %.3f ns/tick\n\n", clk.ns_per_tick);

    // ── Shared ring buffer (lives on stack — feed handler + book share ptr) ───
    fh::MsgRing ring;
    fh::LatencyHistogram<> hist;

    // ── Asio + SSL setup ──────────────────────────────────────────────────────
    fh::net::io_context ioc{1};
    fh::ssl::context ssl_ctx{fh::ssl::context::tlsv12_client};
    ssl_ctx.set_default_verify_paths();
    ssl_ctx.set_verify_mode(fh::ssl::verify_peer);

    fh::FeedConfig cfg;
    cfg.api_key = api_key;
    cfg.symbols = symbols;

    auto handler = std::make_shared<fh::FeedHandler>(ioc, ssl_ctx, cfg, ring, clk);

    // ── Signal handling (Ctrl-C) ──────────────────────────────────────────────
    fh::net::signal_set signals(ioc, SIGINT, SIGTERM);
    std::atomic<bool> stop{false};
    signals.async_wait([&](auto, auto) {
        printf("\nShutting down...\n");
        stop = true;
        handler->stop();
        ioc.stop();
    });

    // ── Launch consumer + stats threads ──────────────────────────────────────
    std::thread book_thread([&] {
        run_book_consumer(ring, clk, hist, stop);
    });
    std::thread stats_thread([&] {
        run_stats_printer(ring, hist, stop);
    });

    // ── Start feed handler and run I/O loop (blocks until stop) ──────────────
    handler->start();
    ioc.run();

    stop = true;
    book_thread.join();
    stats_thread.join();

    // Final stats
    printf("\n═══ Final stats ═══\n");
    hist.print("decode→book");
    const auto& ds = handler->dec_stats();
    printf("Quotes decoded: %lu  Trades decoded: %lu  Errors: %lu\n",
           ds.quotes_decoded, ds.trades_decoded, ds.parse_errors);
    const auto& ps = handler->pub_stats();
    printf("Ring published: %lu  Dropped (ring full): %lu\n",
           ps.published, ps.ring_full);

    return 0;
}
