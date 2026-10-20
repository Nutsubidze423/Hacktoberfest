// SPDX-License-Identifier: Apache-2.0

// Checks kvm/queue.h: ordering, bounded drop-oldest, LatestSlot overwrite, timeouts, close(),
// and a multi-threaded producer/consumer run.
// Usage: queue_test.exe   (exit code 0 = all passed)

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "kvm/queue.h"

using namespace std::chrono_literals;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what);
    if (!ok) {
        ++g_failures;
    }
}

void test_queue_basics() {
    kvm::BlockingQueue<int> q;
    q.push(1);
    q.push(2);
    q.push(3);
    const auto a = q.pop();
    const auto b = q.try_pop();
    const auto c = q.pop_for(10ms);
    check(a == 1 && b == 2 && c == 3, "queue: FIFO order");
    check(!q.try_pop(), "queue: try_pop on empty -> nullopt");

    const auto start = std::chrono::steady_clock::now();
    const auto none = q.pop_for(50ms);
    const auto waited = std::chrono::steady_clock::now() - start;
    check(!none && waited >= 45ms, "queue: pop_for times out");
}

void test_queue_bounded() {
    kvm::BlockingQueue<int> q(2);
    const bool r1 = q.push(1);
    const bool r2 = q.push(2);
    const bool r3 = q.push(3);  // drops 1
    check(r1 && r2 && !r3, "bounded: push reports drop when full");
    const auto first = q.pop();
    const auto second = q.pop();
    check(first == 2 && second == 3 && q.dropped() == 1, "bounded: oldest item dropped");
}

void test_queue_close() {
    kvm::BlockingQueue<int> q;
    q.push(7);
    q.close();
    check(!q.push(8), "close: push rejected after close");
    const auto drained = q.pop();
    const auto after = q.pop();
    check(drained == 7 && !after, "close: remaining items drained, then nullopt");

    kvm::BlockingQueue<int> waiting;
    std::atomic<bool> woke{false};
    std::thread consumer([&] {
        const auto v = waiting.pop();
        woke = !v.has_value();
    });
    std::this_thread::sleep_for(20ms);
    waiting.close();
    consumer.join();
    check(woke, "close: wakes a blocked pop()");
}

void test_queue_move_only() {
    kvm::BlockingQueue<std::unique_ptr<int>> q;
    q.push(std::make_unique<int>(42));
    const auto v = q.pop();
    check(v && *v && **v == 42, "queue: move-only type");
}

void test_queue_threads() {
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 20000;
    kvm::BlockingQueue<int> q;
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&q, p] {
            for (int i = 0; i < kPerProducer; ++i) {
                q.push(p * kPerProducer + i);
            }
        });
    }

    std::vector<int> last_seen(kProducers, -1);
    bool ordered = true;
    long long count = 0;
    std::thread consumer([&] {
        while (const auto v = q.pop()) {
            const int producer = *v / kPerProducer;
            ordered = ordered && *v > last_seen[producer];
            last_seen[producer] = *v;
            ++count;
        }
    });
    for (auto& t : producers) {
        t.join();
    }
    q.close();
    consumer.join();
    check(count == kProducers * kPerProducer && ordered, "queue: 4 producers x 20000, none lost, per-producer order kept");
}

void test_slot() {
    kvm::LatestSlot<int> slot;
    check(!slot.try_take(), "slot: empty -> nullopt");
    slot.put(1);
    slot.put(2);
    slot.put(3);
    const auto latest = slot.take();
    check(latest == 3 && slot.overwritten() == 2, "slot: keeps only the newest value");
    check(!slot.try_take(), "slot: take() empties it");
    check(!slot.take_for(20ms), "slot: take_for times out");

    std::atomic<bool> woke{false};
    std::thread consumer([&] { woke = !slot.take().has_value(); });
    std::this_thread::sleep_for(20ms);
    slot.close();
    consumer.join();
    check(woke, "slot: close wakes a blocked take()");
    slot.put(9);
    check(!slot.try_take(), "slot: put ignored after close");
}

void test_slot_slow_consumer() {
    // Producer at ~1000/s, consumer at ~50/s: consumer must always see increasing, fresh values.
    kvm::LatestSlot<int> slot;
    std::atomic<bool> stop{false};
    std::thread producer([&] {
        for (int i = 0; !stop; ++i) {
            slot.put(i);
            std::this_thread::sleep_for(1ms);
        }
    });
    int previous = -1;
    bool increasing = true;
    int taken = 0;
    for (int n = 0; n < 10; ++n) {
        std::this_thread::sleep_for(20ms);
        if (const auto v = slot.take_for(100ms)) {
            increasing = increasing && *v > previous;
            previous = *v;
            ++taken;
        }
    }
    stop = true;
    producer.join();
    check(taken == 10 && increasing && slot.overwritten() > 0, "slot: slow consumer skips stale frames");
}

}  // namespace

int main() {
    test_queue_basics();
    test_queue_bounded();
    test_queue_close();
    test_queue_move_only();
    test_queue_threads();
    test_slot();
    test_slot_slow_consumer();

    std::printf("\n%s\n", g_failures == 0 ? "all passed" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
