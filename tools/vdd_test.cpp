// vdd_test: checks host_a/virtual_display on this machine, step by step:
//   list displays -> attach the VDD (1920x1080 @ 60 Hz, right) -> place on the opposite side and back
//   -> detach -> attach again -> detach.
// The VDD is always detached at the end, also when a step fails.
//
//   vdd_test.exe [--side left|right|top|bottom] [--size 1920x1080] [--hz 60]

#include <cstdio>
#include <format>
#include <string>
#include <string_view>

#include "kvm/dpi.h"
#include "kvm/log.h"
#include "virtual_display.h"

namespace {

using kvm::display::Side;

std::string utf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

void print_displays(std::string_view when) {
    kvm::log::info("displays {}:", when);
    for (DWORD i = 0;; ++i) {
        DISPLAY_DEVICEW dd{};
        dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesW(nullptr, i, &dd, 0)) break;
        DISPLAY_DEVICEW mon{};
        mon.cb = sizeof(mon);
        EnumDisplayDevicesW(dd.DeviceName, 0, &mon, 0);
        const bool attached = (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0;
        if (!attached && mon.DeviceString[0] == L'\0') continue;  // unused output
        std::string mode = "detached";
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (attached && EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) {
            mode = std::format("{}x{} @ {} Hz at ({}, {})", dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency,
                               dm.dmPosition.x, dm.dmPosition.y);
        }
        kvm::log::info("  {:<14} {:<34} {}", utf8(dd.DeviceName), utf8(dd.DeviceString), mode);
    }
}

struct Checks {
    int failed = 0;
    void operator()(bool ok, std::string_view what) {
        kvm::log::info("{} {}", ok ? "[ok]  " : "[FAIL]", what);
        if (!ok) ++failed;
    }
};

// True if the VDD sits right next to the other monitors on `side`.
bool is_on_side(const kvm::VirtualDisplay& vdd, Side side) {
    const auto r = vdd.rect();
    if (!r) return false;
    const auto me = utf8(vdd.device());
    bool any = false;
    int edge = 0;
    for (const auto& m : kvm::display::monitors()) {
        if (m.name == me) continue;
        const int e = side == Side::Right ? m.rect.right : side == Side::Left ? m.rect.left
                    : side == Side::Bottom ? m.rect.bottom : m.rect.top;
        const bool further = side == Side::Right || side == Side::Bottom ? e > edge : e < edge;
        if (!any || further) edge = e;
        any = true;
    }
    if (!any) return true;
    switch (side) {
        case Side::Right: return r->rect.left == edge;
        case Side::Left: return r->rect.right == edge;
        case Side::Bottom: return r->rect.top == edge;
        default: return r->rect.bottom == edge;
    }
}

bool has_size(const kvm::VirtualDisplay& vdd, int width, int height) {
    const auto r = vdd.rect();
    return r && r->rect.right - r->rect.left == width && r->rect.bottom - r->rect.top == height;
}

}  // namespace

int main(int argc, char** argv) {
    kvm::display::enable_dpi_awareness();

    Side side = Side::Right;
    int width = 1920, height = 1080, hz = 60;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string_view arg = argv[i];
        if (arg == "--side") {
            if (const auto s = kvm::display::parse_side(argv[i + 1])) side = *s;
        } else if (arg == "--size") {
            sscanf_s(argv[i + 1], "%dx%d", &width, &height);
        } else if (arg == "--hz") {
            hz = std::atoi(argv[i + 1]);
        }
    }

    print_displays("before");
    auto vdd = kvm::VirtualDisplay::find();
    if (!vdd) {
        kvm::log::error("Virtual Display Driver not found. Install it first, see ARTEFACT/B როგორც მესამე მონიტორი.md");
        return 1;
    }
    kvm::log::info("VDD output: {} (attached: {})", utf8(vdd->device()), vdd->attached());

    // Never leave the VDD attached: a forgotten invisible monitor swallows the cursor and windows.
    struct DetachOnExit {
        kvm::VirtualDisplay& vdd;
        ~DetachOnExit() { vdd.detach(); }
    } guard{*vdd};

    Checks check;
    const auto mode = std::format("{}x{} @ {} Hz", width, height, hz);
    const auto side_name = std::string(kvm::display::to_string(side));
    const auto other_name = std::string(kvm::display::to_string(kvm::display::opposite(side)));

    check(vdd->attach(width, height, hz, side), std::format("attach {} on the {}", mode, side_name));
    check(has_size(*vdd, width, height), std::format("size is {}x{}", width, height));
    check(is_on_side(*vdd, side), std::format("next to the other monitors on the {}", side_name));
    print_displays("after attach");

    check(vdd->place(kvm::display::opposite(side)), std::format("place on the {}", other_name));
    check(is_on_side(*vdd, kvm::display::opposite(side)), std::format("now on the {}", other_name));
    check(vdd->place(side), std::format("place back on the {}", side_name));
    check(is_on_side(*vdd, side), std::format("back on the {}", side_name));

    check(vdd->detach(), "detach");
    check(!vdd->attached() && !vdd->rect(), "not part of the desktop");
    print_displays("after detach");

    check(vdd->attach(width, height, hz, side), "attach again");
    check(has_size(*vdd, width, height) && is_on_side(*vdd, side), std::format("{} on the {} again", mode, side_name));

    check(vdd->detach(), "final detach");
    print_displays("after");

    if (check.failed) {
        kvm::log::error("{} check(s) failed", check.failed);
        return 1;
    }
    kvm::log::info("all passed");
    return 0;
}
