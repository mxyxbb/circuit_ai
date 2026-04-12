#pragma once
#include <array>
#include <atomic>
#include <cstddef>

// Single-Producer Single-Consumer lock-free ring buffer.
// T must be trivially copyable. Capacity must be power of 2.
template<typename T, size_t Capacity>
class SPSCRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    // Note: SimSample contains std::vector which is not trivially copyable,
    // but copy-assignment is safe for single-producer single-consumer.

public:
    bool push(const T& item) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= Capacity) return false;
        buf_[head & mask_] = item;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    bool pop(T& item) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t head = head_.load(std::memory_order_acquire);
        if (tail >= head) return false;
        item = buf_[tail & mask_];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    bool empty() const {
        return tail_.load(std::memory_order_acquire) >=
               head_.load(std::memory_order_acquire);
    }

private:
    static constexpr size_t mask_ = Capacity - 1;
    std::array<T, Capacity> buf_;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};
