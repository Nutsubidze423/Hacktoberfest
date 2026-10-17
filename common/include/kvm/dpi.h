#pragma once
// Display geometry in physical pixels: DPI awareness, the monitor list and the
// outer edges of the virtual desktop (where the cursor leaves A for B).
//
// Call enable_dpi_awareness() first thing in main(): until then Windows reports
// scaled ("virtualized") coordinates on high-DPI monitors.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kvm/win32_input.h"

namespace kvm::display {

using input::Point;

// right / bottom are exclusive, like a Win32 RECT.
struct Rect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    int width() const { return right - left; }
    int height() const { return bottom - top; }
    bool contains(Point p) const { return p.x >= left && p.x < right && p.y >= top && p.y < bottom; }
    bool operator==(const Rect&) const = default;
};

struct Monitor {
    Rect rect;            // whole monitor, physical pixels on the virtual desktop
    Rect work;            // rect minus the taskbar and docked toolbars
    unsigned dpi = 96;    // effective DPI
    double scale = 1.0;   // dpi / 96, e.g. 1.25 for 125 %
    bool primary = false;
    std::string name;     // e.g. "\\.\DISPLAY1"
};

enum class Side { Left, Right, Top, Bottom };

constexpr Side opposite(Side s) {
    switch (s) {
        case Side::Left: return Side::Right;
        case Side::Right: return Side::Left;
        case Side::Top: return Side::Bottom;
        default: return Side::Top;
    }
}

// "left" / "right" / "top" / "bottom" (config.json client_side); nullopt otherwise.
std::optional<Side> parse_side(std::string_view s);
std::string_view to_string(Side s);

// Makes the process per-monitor DPI aware (v2). Returns true if the process is
// per-monitor aware afterwards, including when a manifest already set it.
bool enable_dpi_awareness();
bool is_dpi_aware();

// Connected monitors, primary first, then left to right. Re-query after
// WM_DISPLAYCHANGE / WM_DPICHANGED.
std::vector<Monitor> monitors();

// Primary monitor; its rect always starts at (0, 0).
Monitor primary_monitor();

// Bounding box of all monitors.
Rect virtual_desktop();
Rect virtual_desktop(const std::vector<Monitor>& mons);

// The outer edge on `side` is every edge pixel of every monitor with no other
// monitor directly beyond it, ordered top to bottom (Left / Right) or left to
// right (Top / Bottom). With monitors of different sizes it can consist of
// several pieces; positions are measured along all of them joined together.

// The outer edge on one side, precomputed so lookups don't allocate (safe to
// use inside a low-level hook callback). Rebuild after the monitors change.
class OuterEdge {
public:
    OuterEdge() = default;
    OuterEdge(const std::vector<Monitor>& mons, Side side);

    Side side() const { return side_; }
    bool empty() const { return total_ == 0; }

    // If p is on this edge, its position along it in (0, 1); otherwise nullopt.
    std::optional<double> position(Point p) const;

    // The edge pixel at position t (clamped to 0..1). {0, 0} if empty().
    Point point(double t) const;

private:
    // `fixed` is the edge pixel's x (Left / Right) or y (Top / Bottom);
    // [from, to) runs along the edge.
    struct Segment {
        int fixed;
        int from;
        int to;
    };

    Side side_ = Side::Right;
    std::vector<Segment> segments_;
    long long total_ = 0;
};

// One-off versions of OuterEdge::position / point.
// A uses the position for focus_enter.y_norm, B the point to place the cursor.
std::optional<double> outer_edge_position(const std::vector<Monitor>& mons, Point p, Side side);
Point point_on_outer_edge(const std::vector<Monitor>& mons, Side side, double t);

// p moved `px` pixels away from the `side` edge, towards the desktop interior.
constexpr Point inward(Point p, Side side, int px) {
    switch (side) {
        case Side::Left: return {p.x + px, p.y};
        case Side::Right: return {p.x - px, p.y};
        case Side::Top: return {p.x, p.y + px};
        default: return {p.x, p.y - px};
    }
}

}  // namespace kvm::display
