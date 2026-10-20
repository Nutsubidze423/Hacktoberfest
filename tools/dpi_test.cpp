// SPDX-License-Identifier: Apache-2.0

// Checks kvm/dpi.h: outer-edge geometry on synthetic monitor layouts, then DPI
// awareness and the monitor list on this machine.
// Usage: dpi_test.exe   (exit code 0 = all passed)

#include <cmath>
#include <cstdio>
#include <limits>
#include <set>
#include <string>
#include <vector>

#include "kvm/dpi.h"

namespace display = kvm::display;
using display::Monitor;
using display::Point;
using display::Side;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what.c_str());
    if (!ok) {
        ++g_failures;
    }
}

Monitor mon(int x, int y, int w, int h, bool primary = false) {
    Monitor m;
    m.rect = {x, y, x + w, y + h};
    m.work = m.rect;
    m.primary = primary;
    return m;
}

bool near(std::optional<double> got, double want) {
    return got && std::abs(*got - want) < 1e-12;
}

bool same(Point a, Point b) {
    return a.x == b.x && a.y == b.y;
}

// Every border pixel of every monitor: the ones on the outer edge must map to a
// unique position and back to the same pixel; `want_count` of them must exist.
void round_trip(const std::vector<Monitor>& mons, Side side, long long want_count, const std::string& name) {
    std::set<Point, decltype([](Point a, Point b) { return a.x != b.x ? a.x < b.x : a.y < b.y; })> border;
    for (const Monitor& m : mons) {
        const auto& r = m.rect;
        for (int x = r.left; x < r.right; ++x) border.insert({x, r.top}), border.insert({x, r.bottom - 1});
        for (int y = r.top; y < r.bottom; ++y) border.insert({r.left, y}), border.insert({r.right - 1, y});
    }
    long long count = 0;
    bool back_ok = true;
    std::set<double> seen;
    for (Point p : border) {
        const auto t = display::outer_edge_position(mons, p, side);
        if (!t) continue;
        ++count;
        back_ok = back_ok && *t > 0.0 && *t < 1.0 && seen.insert(*t).second &&
                  same(display::point_on_outer_edge(mons, side, *t), p);
    }
    check(count == want_count, name + " " + std::string(display::to_string(side)) + ": " + std::to_string(count) +
                                   " outer edge pixels (want " + std::to_string(want_count) + ")");
    check(back_ok, name + " " + std::string(display::to_string(side)) + ": every edge pixel round-trips");
}

void synthetic_layouts() {
    std::printf("-- synthetic layouts\n");

    const std::vector<Monitor> single{mon(0, 0, 1920, 1080, true)};
    check(near(display::outer_edge_position(single, {1919, 0}, Side::Right), 0.5 / 1080), "single: top of right edge");
    check(near(display::outer_edge_position(single, {1919, 1079}, Side::Right), 1079.5 / 1080),
          "single: bottom of right edge");
    check(!display::outer_edge_position(single, {1918, 500}, Side::Right),
          "single: one pixel in from the edge -> nullopt");
    check(!display::outer_edge_position(single, {1920, 500}, Side::Right), "single: off screen -> nullopt");
    check(!display::outer_edge_position(single, {0, 500}, Side::Right), "single: left edge is not the right edge");
    check(same(display::point_on_outer_edge(single, Side::Right, 0.5), {1919, 540}),
          "single: right edge at 0.5 -> (1919, 540)");
    check(same(display::point_on_outer_edge(single, Side::Left, 0.0), {0, 0}), "single: left edge at 0 -> (0, 0)");
    check(same(display::point_on_outer_edge(single, Side::Right, 1.0), {1919, 1079}), "single: t = 1 -> last pixel");
    check(same(display::point_on_outer_edge(single, Side::Right, -5.0), {1919, 0}), "single: t < 0 clamps");
    check(same(display::point_on_outer_edge(single, Side::Right, 7.0), {1919, 1079}), "single: t > 1 clamps");
    check(same(display::point_on_outer_edge(single, Side::Right, std::numeric_limits<double>::quiet_NaN()), {1919, 0}),
          "single: NaN -> start of edge");
    for (Side s : {Side::Left, Side::Right}) round_trip(single, s, 1080, "single");
    for (Side s : {Side::Top, Side::Bottom}) round_trip(single, s, 1920, "single");

    // 1080p + taller 1440p to its right, tops aligned.
    const std::vector<Monitor> side_by_side{mon(0, 0, 1920, 1080, true), mon(1920, 0, 2560, 1440)};
    check(!display::outer_edge_position(side_by_side, {1919, 500}, Side::Right), "side-by-side: inner edge -> nullopt");
    check(near(display::outer_edge_position(side_by_side, {4479, 0}, Side::Right), 0.5 / 1440),
          "side-by-side: right edge is the 1440p's");
    check(!display::outer_edge_position(side_by_side, {1920, 1079}, Side::Left),
          "side-by-side: covered part of 1440p left edge");
    check(near(display::outer_edge_position(side_by_side, {1920, 1080}, Side::Left), 1080.5 / 1440),
          "side-by-side: 1440p left edge below the 1080p is outer");
    check(same(display::point_on_outer_edge(side_by_side, Side::Left, 0.99), {1920, 1425}),
          "side-by-side: left edge at 0.99 -> exposed piece of 1440p");
    round_trip(side_by_side, Side::Right, 1440, "side-by-side");
    round_trip(side_by_side, Side::Left, 1080 + 360, "side-by-side");
    round_trip(side_by_side, Side::Top, 4480, "side-by-side");
    round_trip(side_by_side, Side::Bottom, 4480, "side-by-side");

    // Secondary to the left of the primary: negative coordinates, offset up by 200.
    const std::vector<Monitor> negative{mon(0, 0, 1920, 1080, true), mon(-1280, -200, 1280, 1024)};
    check(near(display::outer_edge_position(negative, {-1, -200}, Side::Right), 0.5 / 1280),
          "negative: secondary right edge above the primary is outer");
    check(!display::outer_edge_position(negative, {-1, 0}, Side::Right),
          "negative: secondary right edge beside primary -> nullopt");
    check(near(display::outer_edge_position(negative, {1919, 0}, Side::Right), 200.5 / 1280),
          "negative: primary right edge follows");
    check(near(display::outer_edge_position(negative, {-1280, -200}, Side::Left), 0.5 / 1280),
          "negative: left edge starts at secondary");
    round_trip(negative, Side::Right, 200 + 1080, "negative");
    round_trip(negative, Side::Left, 1024 + 256, "negative");

    // Two monitors stacked vertically.
    const std::vector<Monitor> stacked{mon(0, -1080, 1920, 1080), mon(0, 0, 1920, 1080, true)};
    check(near(display::outer_edge_position(stacked, {0, 0}, Side::Left), 1080.5 / 2160),
          "stacked: left edge runs over both");
    check(!display::outer_edge_position(stacked, {5, 0}, Side::Top), "stacked: inner top edge -> nullopt");
    round_trip(stacked, Side::Left, 2160, "stacked");
    round_trip(stacked, Side::Right, 2160, "stacked");
    round_trip(stacked, Side::Top, 1920, "stacked");
    round_trip(stacked, Side::Bottom, 1920, "stacked");

    // A's right edge -> B's left edge, different resolutions.
    const auto t = display::outer_edge_position(side_by_side, {4479, 720}, Side::Right);
    check(t && same(display::point_on_outer_edge(single, display::opposite(Side::Right), *t), {0, 540}),
          "A 1440p right edge y=720 -> B 1080p left edge y=540");

    const std::vector<Monitor> none;
    check(!display::outer_edge_position(none, {0, 0}, Side::Right), "no monitors -> nullopt");
    check(same(display::point_on_outer_edge(none, Side::Right, 0.5), {0, 0}), "no monitors -> (0, 0)");
    check(display::virtual_desktop(side_by_side) == display::Rect{0, 0, 4480, 1440},
          "virtual_desktop(mons) bounding box");
    check(display::virtual_desktop(negative) == display::Rect{-1280, -200, 1920, 1080},
          "virtual_desktop(mons) negative");

    check(display::parse_side("right") == Side::Right && display::parse_side("Left") == Side::Left &&
              display::parse_side("TOP") == Side::Top && display::parse_side("bottom") == Side::Bottom,
          "parse_side");
    check(!display::parse_side("") && !display::parse_side("middle"), "parse_side rejects junk");
    check(display::opposite(Side::Right) == Side::Left && display::opposite(Side::Top) == Side::Bottom, "opposite");
}

void this_machine() {
    std::printf("\n-- this machine\n");
    std::printf("dpi aware before enable: %s\n", display::is_dpi_aware() ? "yes" : "no");
    check(display::enable_dpi_awareness(), "enable_dpi_awareness");
    check(display::enable_dpi_awareness(), "enable_dpi_awareness again (already set)");
    check(display::is_dpi_aware(), "is_dpi_aware");

    const auto mons = display::monitors();
    for (const Monitor& m : mons) {
        std::printf("  %s %s %d,%d %dx%d  dpi %u (%.0f%%)  work %d,%d %dx%d\n", m.name.c_str(),
                    m.primary ? "primary" : "       ", m.rect.left, m.rect.top, m.rect.width(), m.rect.height(), m.dpi,
                    m.scale * 100, m.work.left, m.work.top, m.work.width(), m.work.height());
    }
    check(!mons.empty(), "monitors() not empty");
    if (mons.empty()) return;

    int primaries = 0;
    bool sane = true;
    for (const Monitor& m : mons) {
        primaries += m.primary;
        sane = sane && m.rect.width() > 0 && m.rect.height() > 0 && m.dpi >= 96 && m.scale == m.dpi / 96.0 &&
               m.work.left >= m.rect.left && m.work.top >= m.rect.top && m.work.right <= m.rect.right &&
               m.work.bottom <= m.rect.bottom && !m.name.empty();
    }
    check(primaries == 1 && mons.front().primary, "exactly one primary, listed first");
    check(mons.front().rect.left == 0 && mons.front().rect.top == 0, "primary starts at (0, 0)");
    check(sane, "sizes, dpi, scale, work area and names are sane");
    check(display::primary_monitor().rect == mons.front().rect, "primary_monitor()");
    check(display::virtual_desktop() == display::virtual_desktop(mons), "virtual_desktop() matches the monitor list");
    for (Side s : {Side::Left, Side::Right}) {
        const auto pt = display::point_on_outer_edge(mons, s, 0.5);
        const auto back = display::outer_edge_position(mons, pt, s);
        check(back && std::abs(*back - 0.5) < 0.01,
              std::string("live ") + std::string(display::to_string(s)) + " edge midpoint round-trips");
    }
}

}  // namespace

int main() {
    synthetic_layouts();
    this_machine();
    std::printf("\n%s\n", g_failures == 0 ? "all passed" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
