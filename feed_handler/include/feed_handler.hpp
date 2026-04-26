#pragma once
#include "market_data.hpp"
#include "spsc_ring.hpp"
#include "polygon_decoder.hpp"
#include "timing.hpp"

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/steady_timer.hpp>

#include <string>
#include <vector>
#include <atomic>
#include <functional>
#include <memory>
#include <cstdio>
#include <chrono>

// ─────────────────────────────────────────────────────────────────────────────
// FeedHandler — Polygon.io WebSocket client
//
// Architecture:
//   Single Boost.Asio io_context thread runs all I/O (connect, read, write).
//   Decoded messages are pushed onto an SPSC ring buffer.
//   The order book runs on a separate thread, spinning on the ring.
//
// Session lifecycle:
//   connect → SSL handshake → WS handshake → authenticate → subscribe → read loop
//   On disconnect: exponential backoff reconnect (cap 30s)
//
// Interview talking points:
//   - Why not use a thread per symbol? N threads × M symbols = thundering herd
//     on reconnect.  Single I/O thread with async ops handles thousands of
//     symbols with one CPU core.
//   - Why Boost.Beast over libwebsockets? Beast is header-only, integrates
//     directly with Asio's executor model, and gives us async_read on Beast
//     flat_buffer with zero-copy reads.
// ─────────────────────────────────────────────────────────────────────────────

namespace fh {

namespace beast = boost::beast;
namespace ws    = beast::websocket;
namespace net   = boost::asio;
namespace ssl   = boost::asio::ssl;
using tcp       = net::ip::tcp;

// Ring size: 4096 messages = 256 KB (4096 × 64 bytes).  Power-of-2 required.
constexpr size_t RING_SIZE = 4096;
using MsgRing = SPSCRing<MarketMsg, RING_SIZE>;

struct FeedConfig {
    std::string api_key;
    std::string host    = "delayed.polygon.io";   // free tier (15-min delayed)
    std::string port    = "443";
    std::string target  = "/stocks";              // /stocks, /options, /crypto
    std::vector<std::string> symbols;             // e.g. {"AAPL","MSFT","TSLA"}
    int reconnect_base_ms = 1000;
    int reconnect_max_ms  = 30000;
};

class FeedHandler : public std::enable_shared_from_this<FeedHandler> {
public:
    using OnMsg  = std::function<void(const MarketMsg&)>;
    using OnStat = std::function<void(const std::string&)>;

    FeedHandler(net::io_context& ioc,
                ssl::context&    ssl_ctx,
                FeedConfig       cfg,
                MsgRing&         ring,
                TscClock&        clk)
        : ioc_(ioc)
        , resolver_(net::make_strand(ioc))
        , ws_(net::make_strand(ioc), ssl_ctx)
        , cfg_(std::move(cfg))
        , ring_(ring)
        , decoder_(clk)
        , clk_(clk)
        , reconnect_delay_ms_(cfg_.reconnect_base_ms)
    {}

    void start() { do_resolve(); }

    void stop() {
        running_ = false;
        beast::error_code ec;
        ws_.close(ws::close_code::normal, ec);
    }

    struct PubStats {
        uint64_t published  = 0;
        uint64_t ring_full  = 0;   // dropped because ring was full
    };
    const PubStats& pub_stats() const { return pub_stats_; }
    const PolygonDecoder::Stats& dec_stats() const { return decoder_.stats(); }

private:
    // ── Resolve ───────────────────────────────────────────────────────────────
    void do_resolve() {
        log("Resolving " + cfg_.host + ":" + cfg_.port);
        resolver_.async_resolve(cfg_.host, cfg_.port,
            beast::bind_front_handler(&FeedHandler::on_resolve, shared_from_this()));
    }

    void on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
        if (ec) return on_error("resolve", ec);
        log("Resolved. Connecting...");
        beast::get_lowest_layer(ws_).async_connect(results,
            beast::bind_front_handler(&FeedHandler::on_connect, shared_from_this()));
    }

    // ── TCP connect ───────────────────────────────────────────────────────────
    void on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type) {
        if (ec) return on_error("connect", ec);
        // Set SNI hostname for TLS
        if (!SSL_set_tlsext_host_name(
                ws_.next_layer().native_handle(), cfg_.host.c_str())) {
            return on_error("SNI", {static_cast<int>(::ERR_get_error()),
                                    net::error::get_ssl_category()});
        }
        log("TCP connected. Starting SSL handshake...");
        ws_.next_layer().async_handshake(ssl::stream_base::client,
            beast::bind_front_handler(&FeedHandler::on_ssl_handshake, shared_from_this()));
    }

    // ── SSL handshake ─────────────────────────────────────────────────────────
    void on_ssl_handshake(beast::error_code ec) {
        if (ec) return on_error("ssl_handshake", ec);
        // Configure WebSocket options before upgrade
        ws_.set_option(ws::stream_base::decorator([](ws::request_type& req) {
            req.set(boost::beast::http::field::user_agent, "fh-engine/1.0");
        }));
        log("SSL done. Upgrading to WebSocket...");
        ws_.async_handshake(cfg_.host, cfg_.target,
            beast::bind_front_handler(&FeedHandler::on_ws_handshake, shared_from_this()));
    }

    // ── WebSocket handshake ───────────────────────────────────────────────────
    void on_ws_handshake(beast::error_code ec) {
        if (ec) return on_error("ws_handshake", ec);
        log("WebSocket connected. Authenticating...");
        reconnect_delay_ms_ = cfg_.reconnect_base_ms;  // reset backoff on success
        do_read();  // start read loop — auth response comes as a message
        send_auth();
    }

    // ── Auth + subscribe ──────────────────────────────────────────────────────
    void send_auth() {
        std::string msg = R"({"action":"auth","params":")" + cfg_.api_key + R"("})";
        ws_.async_write(net::buffer(msg),
            beast::bind_front_handler(&FeedHandler::on_write, shared_from_this()));
    }

    void send_subscribe() {
        // Build subscribe message: {"action":"subscribe","params":"Q.AAPL,Q.MSFT,T.AAPL,..."}
        std::string params;
        for (const auto& sym : cfg_.symbols) {
            if (!params.empty()) params += ',';
            params += "Q." + sym;  // quotes
            params += ",T." + sym; // trades
        }
        std::string msg = R"({"action":"subscribe","params":")" + params + R"("})";
        log("Subscribing: " + msg);
        ws_.async_write(net::buffer(msg),
            beast::bind_front_handler(&FeedHandler::on_write, shared_from_this()));
    }

    void on_write(beast::error_code ec, size_t) {
        if (ec) on_error("write", ec);
        // writes are fire-and-forget for now
    }

    // ── Read loop ─────────────────────────────────────────────────────────────
    void do_read() {
        if (!running_) return;
        buf_.clear();
        ws_.async_read(buf_,
            beast::bind_front_handler(&FeedHandler::on_read, shared_from_this()));
    }

    void on_read(beast::error_code ec, size_t bytes) {
        if (ec) return on_error("read", ec);

        std::string_view frame{
            static_cast<const char*>(buf_.data().data()), bytes};

        // Check for auth/status responses before decoding as market data
        if (frame.find("\"status\":\"auth_success\"") != std::string_view::npos) {
            log("Authenticated. Subscribing to symbols...");
            send_subscribe();
        } else if (frame.find("\"status\":\"success\"") != std::string_view::npos) {
            log("Subscribed successfully.");
        } else {
            // Market data — decode and publish
            decoder_.decode_frame(frame, [this](MarketMsg&& msg) {
                publish(msg);
            });
        }

        do_read();  // immediately queue next read
    }

    // ── Publish to ring ───────────────────────────────────────────────────────
    void publish(const MarketMsg& msg) {
        if (ring_.push(msg)) {
            ++pub_stats_.published;
        } else {
            ++pub_stats_.ring_full;
            // Ring full = consumer (order book) is behind.
            // In production: record drop, consider backpressure signal.
            // For MVP: just count it.
        }
    }

    // ── Error handling + reconnect ────────────────────────────────────────────
    void on_error(const char* where, beast::error_code ec) {
        if (!running_) return;
        log(std::string("Error [") + where + "]: " + ec.message() +
            " — reconnecting in " + std::to_string(reconnect_delay_ms_) + "ms");

        // Reset WebSocket stream
        ws_ = ws::stream<ssl::stream<tcp::socket>>(
            net::make_strand(ioc_),
            *ssl_ctx_ptr_);  // reuse ssl context

        // Exponential backoff
        auto delay = std::chrono::milliseconds(reconnect_delay_ms_);
        reconnect_delay_ms_ = std::min(reconnect_delay_ms_ * 2,
                                       cfg_.reconnect_max_ms);

        auto timer = std::make_shared<net::steady_timer>(ioc_, delay);
        timer->async_wait([self = shared_from_this(), timer](beast::error_code) {
            self->do_resolve();
        });
    }

    void log(const std::string& msg) const {
        printf("[FeedHandler] %s\n", msg.c_str());
        fflush(stdout);
    }

    // ── Members ───────────────────────────────────────────────────────────────
    net::io_context&                         ioc_;
    ssl::context*                            ssl_ctx_ptr_ = nullptr;
    tcp::resolver                            resolver_;
    ws::stream<ssl::stream<tcp::socket>>     ws_;
    beast::flat_buffer                       buf_;
    FeedConfig                               cfg_;
    MsgRing&                                 ring_;
    PolygonDecoder                           decoder_;
    TscClock&                                clk_;
    std::atomic<bool>                        running_{true};
    int                                      reconnect_delay_ms_;
    PubStats                                 pub_stats_;
};

} // namespace fh
