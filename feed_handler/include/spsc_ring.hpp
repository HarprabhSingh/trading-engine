#pragma once
#include <atomic>
#include <array>
#include <optional>
#include <cstddef>

// ─────────────────────────────────────────────────────────────────────────────
// Single-Producer Single-Consumer lock-free ring buffer
//
// Interview talking points:
//   1. SPSC constraint is intentional — MPMC rings need CAS loops which add
//      latency.  Feed handler = 1 writer, order book = 1 reader, always.
//   2. head_/tail_ on separate cache lines (alignas(64)) — prevents false
//      sharing where writer and reader invalidate each other's cache lines.
//   3. Acquire/release memory ordering — no seq_cst needed for SPSC, which
//      expresses the publication/reuse edges; instruction selection is compiler-specific.
//   4. Size must be power-of-2 so (idx & mask) replaces modulo (no division).
//   5. No dynamic allocation — the ring is embedded by value.
// ─────────────────────────────────────────────────────────────────────────────

namespace fh {

template<typename T, size_t N>
class SPSCRing {
    static_assert(N >= 2 && (N & (N-1)) == 0, "N must be a power of 2 >= 2");
    static_assert(std::atomic<size_t>::is_always_lock_free,
                  "This ring requires lock-free index atomics");
    static constexpr size_t MASK = N - 1;

    // Producer writes head_, reads tail_ (stale read is fine — worst case we
    // think ring is slightly fuller than it is, never corrupt).
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};

    std::array<T, N> buf_{};

public:
    // Called by producer (feed handler thread).
    // Returns false if ring is full — caller should handle backpressure.
    bool push(const T& item) noexcept {
        size_t h = head_.load(std::memory_order_relaxed);
        size_t next = (h + 1) & MASK;
        if (next == tail_.load(std::memory_order_acquire))
            return false;  // full
        buf_[h] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Called by consumer (order book thread).
    std::optional<T> pop() noexcept {
        size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire))
            return std::nullopt;  // empty
        T item = buf_[t];
        tail_.store((t + 1) & MASK, std::memory_order_release);
        return item;
    }

    size_t size() const noexcept {
        size_t h = head_.load(std::memory_order_acquire);
        size_t t = tail_.load(std::memory_order_acquire);
        return (h - t) & MASK;
    }

    bool empty() const noexcept {
        return head_.load(std::memory_order_acquire)
            == tail_.load(std::memory_order_acquire);
    }
};

} // namespace fh
