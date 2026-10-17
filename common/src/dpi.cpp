#include "kvm/dpi.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <utility>

namespace kvm::display {
namespace {

Rect to_rect(const RECT& r) {
    return {r.left, r.top, r.right, r.bottom};
}

std::string narrow(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

BOOL CALLBACK add_monitor(HMONITOR h, HDC, LPRECT, LPARAM param) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(h, &info)) return TRUE;

    Monitor m;
    m.rect = to_rect(info.rcMonitor);
    m.work = to_rect(info.rcWork);
    m.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    m.name = narrow(info.szDevice);
    UINT dpi_x = 0, dpi_y = 0;
    if (SUCCEEDED(GetDpiForMonitor(h, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y)) && dpi_x != 0) m.dpi = dpi_x;
    m.scale = m.dpi / 96.0;
    reinterpret_cast<std::vector<Monitor>*>(param)->push_back(std::move(m));
    return TRUE;
}

bool along_x(Side s) {
    return s == Side::Top || s == Side::Bottom;
}

// One piece of an outer edge: `fixed` is the edge pixel's x (Left / Right) or
// y (Top / Bottom), and [from, to) runs along the edge.
struct Segment {
    int fixed;
    int from;
    int to;
};

std::vector<Segment> outer_segments(const std::vector<Monitor>& mons, Side side) {
    std::vector<Segment> out;
    for (const Monitor& m : mons) {
        const Rect& r = m.rect;
        if (r.width() <= 0 || r.height() <= 0) continue;

        Segment edge{};
        switch (side) {
            case Side::Left:   edge = {r.left, r.top, r.bottom}; break;
            case Side::Right:  edge = {r.right - 1, r.top, r.bottom}; break;
            case Side::Top:    edge = {r.top, r.left, r.right}; break;
            case Side::Bottom: edge = {r.bottom - 1, r.left, r.right}; break;
        }

        // Spans of this edge that a neighbouring monitor continues past.
        std::vector<std::pair<int, int>> covered;
        for (const Monitor& n : mons) {
            if (&n == &m) continue;
            const Rect& o = n.rect;
            bool touches = false;
            switch (side) {
                case Side::Left:   touches = o.right == r.left; break;
                case Side::Right:  touches = o.left == r.right; break;
                case Side::Top:    touches = o.bottom == r.top; break;
                case Side::Bottom: touches = o.top == r.bottom; break;
            }
            if (!touches) continue;
            const int lo = std::max(along_x(side) ? o.left : o.top, edge.from);
            const int hi = std::min(along_x(side) ? o.right : o.bottom, edge.to);
            if (lo < hi) covered.emplace_back(lo, hi);
        }

        std::sort(covered.begin(), covered.end());
        int pos = edge.from;
        for (const auto& [lo, hi] : covered) {
            if (lo > pos) out.push_back({edge.fixed, pos, lo});
            pos = std::max(pos, hi);
        }
        if (pos < edge.to) out.push_back({edge.fixed, pos, edge.to});
    }
    std::sort(out.begin(), out.end(), [](const Segment& a, const Segment& b) {
        return a.from != b.from ? a.from < b.from : a.fixed < b.fixed;
    });
    return out;
}

long long total_length(const std::vector<Segment>& segs) {
    long long total = 0;
    for (const Segment& s : segs) total += s.to - s.from;
    return total;
}

char lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
                                              [](char x, char y) { return lower(x) == lower(y); });
}

}  // namespace

std::optional<Side> parse_side(std::string_view s) {
    for (Side side : {Side::Left, Side::Right, Side::Top, Side::Bottom}) {
        if (iequals(s, to_string(side))) return side;
    }
    return std::nullopt;
}

std::string_view to_string(Side s) {
    switch (s) {
        case Side::Left: return "left";
        case Side::Right: return "right";
        case Side::Top: return "top";
        default: return "bottom";
    }
}

bool is_dpi_aware() {
    return GetAwarenessFromDpiAwarenessContext(GetDpiAwarenessContextForProcess(GetCurrentProcess())) ==
           DPI_AWARENESS_PER_MONITOR_AWARE;
}

bool enable_dpi_awareness() {
    if (SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return true;
    // Fails if awareness was already set (manifest or an earlier call); v1 is
    // the fallback for Windows builds without v2.
    if (!is_dpi_aware()) SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
    return is_dpi_aware();
}

std::vector<Monitor> monitors() {
    std::vector<Monitor> mons;
    EnumDisplayMonitors(nullptr, nullptr, add_monitor, reinterpret_cast<LPARAM>(&mons));
    std::sort(mons.begin(), mons.end(), [](const Monitor& a, const Monitor& b) {
        if (a.primary != b.primary) return a.primary;
        return a.rect.left != b.rect.left ? a.rect.left < b.rect.left : a.rect.top < b.rect.top;
    });
    return mons;
}

Monitor primary_monitor() {
    std::vector<Monitor> mons = monitors();
    if (!mons.empty() && mons.front().primary) return std::move(mons.front());

    Monitor m;
    m.rect = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    m.work = m.rect;
    m.dpi = GetDpiForSystem();
    m.scale = m.dpi / 96.0;
    m.primary = true;
    return m;
}

Rect virtual_desktop() {
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    return {x, y, x + GetSystemMetrics(SM_CXVIRTUALSCREEN), y + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
}

Rect virtual_desktop(const std::vector<Monitor>& mons) {
    if (mons.empty()) return {};
    Rect box = mons.front().rect;
    for (const Monitor& m : mons) {
        box.left = std::min(box.left, m.rect.left);
        box.top = std::min(box.top, m.rect.top);
        box.right = std::max(box.right, m.rect.right);
        box.bottom = std::max(box.bottom, m.rect.bottom);
    }
    return box;
}

std::optional<double> outer_edge_position(const std::vector<Monitor>& mons, Point p, Side side) {
    const std::vector<Segment> segs = outer_segments(mons, side);
    const long long total = total_length(segs);
    if (total == 0) return std::nullopt;

    const int fixed = along_x(side) ? p.y : p.x;
    const int along = along_x(side) ? p.x : p.y;
    long long offset = 0;
    for (const Segment& s : segs) {
        if (s.fixed == fixed && along >= s.from && along < s.to) {
            // Pixel centre, so point_on_outer_edge maps it back to the same pixel.
            return (static_cast<double>(offset + (along - s.from)) + 0.5) / static_cast<double>(total);
        }
        offset += s.to - s.from;
    }
    return std::nullopt;
}

Point point_on_outer_edge(const std::vector<Monitor>& mons, Side side, double t) {
    const std::vector<Segment> segs = outer_segments(mons, side);
    const long long total = total_length(segs);
    if (total == 0) return {};

    if (!(t >= 0.0)) t = 0.0;  // also catches NaN
    long long k = std::min(static_cast<long long>(std::min(t, 1.0) * static_cast<double>(total)), total - 1);
    for (const Segment& s : segs) {
        const long long len = s.to - s.from;
        if (k < len) {
            const int along = s.from + static_cast<int>(k);
            return along_x(side) ? Point{along, s.fixed} : Point{s.fixed, along};
        }
        k -= len;
    }
    const Segment& last = segs.back();
    return along_x(side) ? Point{last.to - 1, last.fixed} : Point{last.fixed, last.to - 1};
}

}  // namespace kvm::display
