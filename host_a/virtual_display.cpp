#include "virtual_display.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <iterator>
#include <string_view>
#include <thread>
#include <vector>

#include "kvm/log.h"

namespace kvm {
namespace {

constexpr std::wstring_view kAdapterName = L"Virtual Display Driver";
// Windows applies display changes asynchronously; give the desktop this long to settle.
constexpr auto kSettleTimeout = std::chrono::seconds(3);

std::string utf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

const char* disp_change_name(LONG result) {
    switch (result) {
        case DISP_CHANGE_SUCCESSFUL: return "DISP_CHANGE_SUCCESSFUL";
        case DISP_CHANGE_RESTART: return "DISP_CHANGE_RESTART";
        case DISP_CHANGE_FAILED: return "DISP_CHANGE_FAILED";
        case DISP_CHANGE_BADMODE: return "DISP_CHANGE_BADMODE";
        case DISP_CHANGE_NOTUPDATED: return "DISP_CHANGE_NOTUPDATED";
        case DISP_CHANGE_BADFLAGS: return "DISP_CHANGE_BADFLAGS";
        case DISP_CHANGE_BADPARAM: return "DISP_CHANGE_BADPARAM";
        case DISP_CHANGE_BADDUALVIEW: return "DISP_CHANGE_BADDUALVIEW";
        default: return "DISP_CHANGE_?";
    }
}

struct AttachedMonitor {
    std::wstring device;
    RECT rect{};
    HMONITOR handle = nullptr;
};

std::vector<AttachedMonitor> attached_monitors() {
    std::vector<AttachedMonitor> out;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR handle, HDC, LPRECT, LPARAM param) -> BOOL {
            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);
            if (GetMonitorInfoW(handle, &info)) {
                reinterpret_cast<std::vector<AttachedMonitor>*>(param)->push_back({info.szDevice, info.rcMonitor, handle});
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&out));
    return out;
}

// Top-left corner for a width x height monitor on `side` of `others`, aligned with the monitor that
// forms that outer edge.
POINT position_for(display::Side side, int width, int height, const std::vector<AttachedMonitor>& others) {
    if (others.empty()) return {0, 0};
    const auto by = [](auto key) { return [key](const AttachedMonitor& a, const AttachedMonitor& b) { return key(a) < key(b); }; };
    switch (side) {
        case display::Side::Left: {
            const auto& m = *std::min_element(others.begin(), others.end(), by([](const auto& m) { return m.rect.left; }));
            return {m.rect.left - width, m.rect.top};
        }
        case display::Side::Top: {
            const auto& m = *std::min_element(others.begin(), others.end(), by([](const auto& m) { return m.rect.top; }));
            return {m.rect.left, m.rect.top - height};
        }
        case display::Side::Bottom: {
            const auto& m = *std::max_element(others.begin(), others.end(), by([](const auto& m) { return m.rect.bottom; }));
            return {m.rect.left, m.rect.bottom};
        }
        case display::Side::Right:
        default: {
            const auto& m = *std::max_element(others.begin(), others.end(), by([](const auto& m) { return m.rect.right; }));
            return {m.rect.right, m.rect.top};
        }
    }
}

// Stages `dm` for `device` in the registry and applies all staged changes at once.
bool apply(const std::wstring& device, DEVMODEW& dm, const char* what) {
    const LONG staged = ChangeDisplaySettingsExW(device.c_str(), &dm, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
    if (staged != DISP_CHANGE_SUCCESSFUL) {
        log::warn("vdd: {} {}: {}", what, utf8(device), disp_change_name(staged));
        return false;
    }
    const LONG applied = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    if (applied != DISP_CHANGE_SUCCESSFUL) {
        log::warn("vdd: {} {}: apply returned {}", what, utf8(device), disp_change_name(applied));
        return false;
    }
    return true;
}

// The display scale per monitor: DisplayConfig{Get,Set}DeviceInfo with these private request types
// (what Settings > Display > Scale uses; stable since Windows 10 1607). Steps are relative to the
// monitor's recommended scale.
constexpr int kGetDpiScale = -3;
constexpr int kSetDpiScale = -4;
constexpr int kScalePercents[] = {100, 125, 150, 175, 200, 225, 250, 300, 350, 400, 450, 500};

struct DpiScaleGet {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    std::int32_t min_rel;  // 100 % relative to the recommended step (so recommended = -min_rel)
    std::int32_t cur_rel;
    std::int32_t max_rel;
};

struct DpiScaleSet {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    std::int32_t rel;
};

// The active display source that is `device` (e.g. \\.\DISPLAY5).
std::optional<DISPLAYCONFIG_PATH_SOURCE_INFO> source_of(const std::wstring& device) {
    UINT32 path_count = 0, mode_count = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) return std::nullopt;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr) !=
        ERROR_SUCCESS) {
        return std::nullopt;
    }
    for (UINT32 i = 0; i < path_count; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME name{};
        name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        name.header.size = sizeof(name);
        name.header.adapterId = paths[i].sourceInfo.adapterId;
        name.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&name.header) == ERROR_SUCCESS && device == name.viewGdiDeviceName) {
            return paths[i].sourceInfo;
        }
    }
    return std::nullopt;
}

template <class Pred>
bool wait_until(Pred done) {
    const auto deadline = std::chrono::steady_clock::now() + kSettleTimeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return true;
}

}  // namespace

std::optional<VirtualDisplay> VirtualDisplay::find() {
    for (DWORD i = 0;; ++i) {
        DISPLAY_DEVICEW dd{};
        dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesW(nullptr, i, &dd, 0)) break;
        if (kAdapterName == dd.DeviceString) return VirtualDisplay(dd.DeviceName);
    }
    return std::nullopt;
}

bool VirtualDisplay::attached() const {
    for (DWORD i = 0;; ++i) {
        DISPLAY_DEVICEW dd{};
        dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesW(nullptr, i, &dd, 0)) return false;
        if (device_ == dd.DeviceName) return (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0;
    }
}

std::optional<VirtualDisplay::MonitorRect> VirtualDisplay::rect() const {
    for (const auto& m : attached_monitors()) {
        if (m.device == device_) return MonitorRect{m.rect, m.handle};
    }
    return std::nullopt;
}

bool VirtualDisplay::set_mode(int width, int height, int hz) {
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(device_.c_str(), ENUM_CURRENT_SETTINGS, &dm)) {
        log::warn("vdd: set_mode {}: not attached", utf8(device_));
        return false;
    }
    dm.dmPelsWidth = static_cast<DWORD>(width);
    dm.dmPelsHeight = static_cast<DWORD>(height);
    dm.dmDisplayFrequency = static_cast<DWORD>(hz);
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
    if (!apply(device_, dm, "set_mode")) {
        log::warn("vdd: is {}x{} @ {} Hz listed in C:\\VirtualDisplayDriver\\vdd_settings.xml?", width, height, hz);
        return false;
    }
    return wait_until([&] {
        const auto r = rect();
        return r && r->rect.right - r->rect.left == width && r->rect.bottom - r->rect.top == height;
    });
}

SIZE VirtualDisplay::best_mode(int width, int height) const {
    std::vector<SIZE> modes;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsW(device_.c_str(), i, &dm); ++i) {
        modes.push_back({static_cast<LONG>(dm.dmPelsWidth), static_cast<LONG>(dm.dmPelsHeight)});
    }
    if (modes.empty()) return {width, height};
    // Exact first. Otherwise the largest mode that fits inside the other screen: desk mode maps the
    // VDD 1:1 onto it, so a bigger mode would put windows beyond its edge where nobody sees them,
    // while a smaller one only leaves a strip out of reach. Nothing fits: the nearest size.
    const auto fits = [&](const SIZE& m) { return m.cx <= width && m.cy <= height; };
    const auto rank = [&](const SIZE& m) {
        if (m.cx == width && m.cy == height) return -1LL;
        if (fits(m)) return 1'000'000'000LL - static_cast<long long>(m.cx) * m.cy;
        return 2'000'000'000LL + std::llabs(static_cast<long long>(m.cx) - width) +
               std::llabs(static_cast<long long>(m.cy) - height);
    };
    return *std::min_element(modes.begin(), modes.end(), [&](const SIZE& a, const SIZE& b) { return rank(a) < rank(b); });
}

bool VirtualDisplay::match_peer(int width, int height, display::Side side) {
    if (!attached()) return false;
    const SIZE mode = best_mode(width, height);
    const auto r = rect();
    const bool same_size = r && r->rect.right - r->rect.left == mode.cx && r->rect.bottom - r->rect.top == mode.cy;
    if (!same_size && !set_mode(mode.cx, mode.cy, 60)) return false;
    const bool placed = place(side);
    log::info("vdd: {} matched to the other PC: {}x{}{} on the {}", utf8(device_), mode.cx, mode.cy,
              mode.cx == width && mode.cy == height ? "" : std::format(" (closest mode to {}x{})", width, height),
              display::to_string(side));
    return placed;
}

bool VirtualDisplay::place(display::Side side) {
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(device_.c_str(), ENUM_CURRENT_SETTINGS, &dm)) {
        log::warn("vdd: place {}: not attached", utf8(device_));
        return false;
    }
    auto others = attached_monitors();
    std::erase_if(others, [&](const AttachedMonitor& m) { return m.device == device_; });
    const POINT pos = position_for(side, static_cast<int>(dm.dmPelsWidth), static_cast<int>(dm.dmPelsHeight), others);
    dm.dmPosition = {pos.x, pos.y};
    dm.dmFields = DM_POSITION;
    if (!apply(device_, dm, "place")) return false;
    return wait_until([&] {
        const auto r = rect();
        return r && r->rect.left == pos.x && r->rect.top == pos.y;
    });
}

bool VirtualDisplay::attach(int requested_width, int requested_height, int hz, display::Side side) {
    // The other PC's screen may not be one of the driver's modes (e.g. 1920x1200): take the closest.
    const SIZE mode = best_mode(requested_width, requested_height);
    const int width = mode.cx, height = mode.cy;
    if (width != requested_width || height != requested_height) {
        log::info("vdd: {}x{} isn't a VDD mode, using {}x{}", requested_width, requested_height, width, height);
    }
    auto others = attached_monitors();
    std::erase_if(others, [&](const AttachedMonitor& m) { return m.device == device_; });
    const POINT pos = position_for(side, width, height, others);

    // Classic way: give the detached output a size and a position.
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    dm.dmPelsWidth = static_cast<DWORD>(width);
    dm.dmPelsHeight = static_cast<DWORD>(height);
    dm.dmDisplayFrequency = static_cast<DWORD>(hz);
    dm.dmPosition = {pos.x, pos.y};
    dm.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
    if (apply(device_, dm, "attach") && wait_until([&] { return rect().has_value(); })) {
        log::info("vdd: attached {} via ChangeDisplaySettingsEx", utf8(device_));
    } else {
        // Fallback: the same as Win+P -> Extend. Also re-attaches other detached displays.
        const LONG r = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_TOPOLOGY_EXTEND | SDC_APPLY);
        if (r != ERROR_SUCCESS || !wait_until([&] { return rect().has_value(); })) {
            log::error("vdd: attach {} failed (SetDisplayConfig EXTEND returned {})", utf8(device_), r);
            return false;
        }
        log::info("vdd: attached {} via SetDisplayConfig(EXTEND)", utf8(device_));
    }
    // Make sure mode and position are what was asked for, whichever way it got attached.
    return set_mode(width, height, hz) && place(side);
}

bool VirtualDisplay::detach() {
    if (!attached()) return true;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    dm.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT;  // zero size = remove from the desktop
    if (!apply(device_, dm, "detach")) return false;
    if (!wait_until([&] { return !attached(); })) {
        log::error("vdd: {} is still attached after detach", utf8(device_));
        return false;
    }
    log::info("vdd: detached {}", utf8(device_));
    return true;
}

bool VirtualDisplay::set_scale(double scale) {
    const auto source = source_of(device_);
    if (!source) {
        log::warn("vdd: set_scale {}: not attached", utf8(device_));
        return false;
    }
    DpiScaleGet get{};
    get.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(kGetDpiScale);
    get.header.size = sizeof(get);
    get.header.adapterId = source->adapterId;
    get.header.id = source->id;
    if (DisplayConfigGetDeviceInfo(&get.header) != ERROR_SUCCESS) {
        log::warn("vdd: {}: Windows doesn't report its scale", utf8(device_));
        return false;
    }
    const int recommended = -get.min_rel;
    const int last = static_cast<int>(std::size(kScalePercents)) - 1;
    const auto percent = static_cast<int>(std::lround(scale * 100));
    int want = 0;
    for (int i = 1; i <= last; ++i) {
        if (std::abs(kScalePercents[i] - percent) < std::abs(kScalePercents[want] - percent)) want = i;
    }
    want = std::clamp(want, recommended + get.min_rel, std::min(recommended + get.max_rel, last));
    if (want == recommended + get.cur_rel) return true;

    DpiScaleSet set{};
    set.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(kSetDpiScale);
    set.header.size = sizeof(set);
    set.header.adapterId = source->adapterId;
    set.header.id = source->id;
    set.rel = want - recommended;
    if (DisplayConfigSetDeviceInfo(&set.header) != ERROR_SUCCESS) {
        log::warn("vdd: {}: Windows refused scale {}%", utf8(device_), kScalePercents[want]);
        return false;
    }
    log::info("vdd: {} scale {}% (like the other PC's screen)", utf8(device_), kScalePercents[want]);
    return true;
}

}  // namespace kvm
