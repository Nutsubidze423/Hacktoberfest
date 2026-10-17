// Thread-safe hand-off between threads (hook thread -> net thread, capture -> encoder -> sender...).
//
//   BlockingQueue<T> — FIFO, nothing is dropped. Use for input/control messages.
//   LatestSlot<T>    — holds only the newest value; put() overwrites an unread one. Use for video
//                      frames so a slow consumer always gets the latest frame instead of a backlog.
//
// Both support close(): wakes every waiting consumer, after which pop()/take() return nullopt once
// the remaining items are drained. Use it for clean shutdown of worker threads.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace kvm {

template <class T>
class BlockingQueue {
public:
    // capacity 0 = unbounded. When bounded and full, push() drops the OLDEST item and returns false,
    // so a stalled consumer can never block the producer (important for hook callbacks).
    explicit BlockingQueue(std::size_t capacity = 0) : capacity_(capacity) {}

    BlockingQueue(const BlockingQueue&) = delete;
    BlockingQueue& operator=(const BlockingQueue&) = delete;

    // Never blocks. Returns false if an old item had to be dropped or the queue is closed.
    bool push(T value) {
        bool dropped = false;
        {
            std::lock_guard lock(mutex_);
            if (closed_) {
                return false;
            }
            if (capacity_ != 0 && items_.size() >= capacity_) {
                items_.pop_front();
                ++dropped_;
                dropped = true;
            }
            items_.push_back(std::move(value));
        }
        cv_.notify_one();
        return !dropped;
    }

    // Blocks until an item is available. nullopt = closed and empty.
    std::optional<T> pop() {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return !items_.empty() || closed_; });
        return take_front_locked();
    }

    // Blocks up to `timeout`. nullopt = timed out, or closed and empty.
    template <class Rep, class Period>
    std::optional<T> pop_for(std::chrono::duration<Rep, Period> timeout) {
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, timeout, [this] { return !items_.empty() || closed_; });
        return take_front_locked();
    }

    // Never blocks. nullopt = empty.
    std::optional<T> try_pop() {
        std::lock_guard lock(mutex_);
        return take_front_locked();
    }

    void close() {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
        }
        cv_.notify_all();
    }

    bool closed() const {
        std::lock_guard lock(mutex_);
        return closed_;
    }

    std::size_t size() const {
        std::lock_guard lock(mutex_);
        return items_.size();
    }

    // Items dropped because the queue was full (diagnostics).
    std::uint64_t dropped() const {
        std::lock_guard lock(mutex_);
        return dropped_;
    }

private:
    std::optional<T> take_front_locked() {
        if (items_.empty()) {
            return std::nullopt;
        }
        std::optional<T> value(std::move(items_.front()));
        items_.pop_front();
        return value;
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<T> items_;
    std::size_t capacity_;
    std::uint64_t dropped_ = 0;
    bool closed_ = false;
};

template <class T>
class LatestSlot {
public:
    LatestSlot() = default;
    LatestSlot(const LatestSlot&) = delete;
    LatestSlot& operator=(const LatestSlot&) = delete;

    // Never blocks. Replaces an unread value (counted in overwritten()). Ignored after close().
    void put(T value) {
        {
            std::lock_guard lock(mutex_);
            if (closed_) {
                return;
            }
            if (value_) {
                ++overwritten_;
            }
            value_.emplace(std::move(value));
        }
        cv_.notify_one();
    }

    // Blocks until a value is available. nullopt = closed and empty.
    std::optional<T> take() {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return value_.has_value() || closed_; });
        return std::exchange(value_, std::nullopt);
    }

    // Blocks up to `timeout`. nullopt = timed out, or closed and empty.
    template <class Rep, class Period>
    std::optional<T> take_for(std::chrono::duration<Rep, Period> timeout) {
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, timeout, [this] { return value_.has_value() || closed_; });
        return std::exchange(value_, std::nullopt);
    }

    // Never blocks. nullopt = empty.
    std::optional<T> try_take() {
        std::lock_guard lock(mutex_);
        return std::exchange(value_, std::nullopt);
    }

    void close() {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
        }
        cv_.notify_all();
    }

    bool closed() const {
        std::lock_guard lock(mutex_);
        return closed_;
    }

    // Values replaced before anyone read them, i.e. dropped frames (diagnostics).
    std::uint64_t overwritten() const {
        std::lock_guard lock(mutex_);
        return overwritten_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<T> value_;
    std::uint64_t overwritten_ = 0;
    bool closed_ = false;
};

}  // namespace kvm
