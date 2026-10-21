// SPDX-License-Identifier: Apache-2.0

// Checks kvm/bitrate_control.h, the H.264 sender's bitrate rule (roadmap P2.6): it stays at the
// configured bitrate on a link that keeps up, goes down while pictures wait to be sent (from the
// second tick in a row: one is a keyframe's burst) or the viewer's latency rises, never below the
// minimum, and comes back up only after a calm spell. A link that is slow but steady (high latency
// that doesn't rise) is not counted as full.
// Usage: bitrate_test.exe   (exit code 0 = all passed)

#include <cstdint>
#include <cstdio>
#include <optional>

#include "kvm/bitrate_control.h"

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what);
    if (!ok) {
        ++g_failures;
    }
}

constexpr std::int64_t kTick = 500;

}  // namespace

int main() {
    {
        kvm::BitrateControl c({.max = 8'000'000});
        std::int64_t t = 0;
        bool steady = true;
        for (int i = 0; i < 20; ++i, t += kTick) steady = steady && c.tick(t, 10, 40) == 8'000'000;
        check(steady, "a link that keeps up: stays at the configured 8 Mbit/s");

        // One tick of backlog alone (a keyframe's burst) doesn't count.
        const int burst = c.tick(t, 250, std::nullopt);
        const int after = c.tick(t + kTick, 0, std::nullopt);
        check(burst == 8'000'000 && after == 8'000'000, "one tick of backlog (a burst): no change");
        t += 2 * kTick;
        // Pictures keep waiting to be sent: down x0.7 per tick from the second tick.
        const int first = c.tick(t, 250, std::nullopt);
        const int second = c.tick(t + kTick, 250, std::nullopt);
        const int third = c.tick(t + 2 * kTick, 250, std::nullopt);
        check(first == 8'000'000 && second == 5'600'000 && third == 3'920'000,
              "a backlog tick after tick: 8 -> 5.6 -> 3.92 Mbit/s, one step per tick");
        t += 3 * kTick;
        int floor = 0;
        for (int i = 0; i < 20; ++i, t += kTick) floor = c.tick(t, 500, std::nullopt);
        check(floor == 1'000'000, "never below the 1 Mbit/s minimum");

        // Calm again: nothing for calm_ms (1.5 s), then x1.15 per tick.
        const int held = c.tick(t, 0, std::nullopt);
        check(held == 1'000'000, "right after a full link: no step up yet");
        t += 3 * kTick;
        const int up = c.tick(t, 0, std::nullopt);
        check(up == 1'150'000, "after 1.5 s calm: up x1.15");
        int top = 0;
        for (int i = 0; i < 60; ++i) top = c.tick(t += kTick, 0, std::nullopt);
        check(top == 8'000'000, "and back to the configured bitrate, never above it");
    }
    {
        // A full link whose speed is known: straight to 80% of it, not x0.7 at a time.
        kvm::BitrateControl c({.max = 8'000'000});
        c.tick(0, 300, std::nullopt, 1'500'000);
        const int jumped = c.tick(500, 300, std::nullopt, 1'500'000);
        check(jumped == 1'200'000, "a full link carrying 1.5 Mbit/s: 8 -> 1.2 Mbit/s in one step");
        const int floor = c.tick(1000, 300, std::nullopt, 500'000);
        check(floor == 1'000'000, "a link slower than the minimum: the minimum");
        kvm::BitrateControl d({.max = 8'000'000});
        d.tick(0, 300, std::nullopt, 20'000'000);
        check(d.tick(500, 300, std::nullopt, 20'000'000) == 5'600'000,
              "a measured speed above x0.7 changes nothing: still x0.7");
    }
    {
        // The viewer's latency: a rise over its recent lowest counts, a steady high latency doesn't.
        kvm::BitrateControl c({.max = 8'000'000});
        std::int64_t t = 0;
        for (int i = 0; i < 10; ++i, t += kTick) c.tick(t, 0, 200);
        check(c.bitrate() == 8'000'000, "a slow but steady link (200 ms, not rising): stays at 8 Mbit/s");
        const int rose = c.tick(t, 0, 320);
        check(rose == 5'600'000, "the viewer's latency rises 120 ms over its lowest: down");
        const int small = c.tick(t + kTick, 0, 250);
        check(small == 5'600'000, "a rise of 50 ms (under 80): no step down, and no step up while not calm");
    }

    std::printf("\n%s\n", g_failures == 0 ? "all passed" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
