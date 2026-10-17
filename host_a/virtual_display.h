// The Virtual Display Driver (VDD) monitor on A: the "parking spot" for the window sent to B.
// It is attached to the desktop only while a window is on B, next to A's monitors on the side where
// B stands. Coordinates are physical pixels: call kvm::display::enable_dpi_awareness() first.
//
//   auto vdd = kvm::VirtualDisplay::find();              // nullopt: driver not installed
//   vdd->attach(1920, 1080, 60, kvm::display::Side::Right);
//   auto r = vdd->rect();                                 // r->rect, r->monitor (for capture)
//   ...
//   vdd->detach();
//
// Display changes are saved to the registry (CDS_UPDATEREGISTRY), so always detach() when done.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <optional>
#include <string>

#include "kvm/dpi.h"

namespace kvm {

class VirtualDisplay {
public:
    struct MonitorRect {
        RECT rect{};                 // physical pixels on the virtual desktop
        HMONITOR monitor = nullptr;  // for ScreenCapture::for_monitor
    };

    // The first output of the "Virtual Display Driver" adapter. nullopt if the driver isn't installed.
    static std::optional<VirtualDisplay> find();

    // e.g. L"\\.\DISPLAY4". Differs between machines: never hard-code it.
    const std::wstring& device() const { return device_; }

    // True while the monitor is part of the desktop.
    bool attached() const;

    // Where the monitor is now. nullopt while detached.
    std::optional<MonitorRect> rect() const;

    // Changes the resolution of the attached monitor. The mode must be listed in
    // C:\VirtualDisplayDriver\vdd_settings.xml (1920x1080 and other common modes are).
    bool set_mode(int width, int height, int hz);

    // The driver's mode closest to width x height: exactly that if it's listed, else the same width
    // with the nearest height, else the nearest size. {width, height} if the driver lists nothing.
    SIZE best_mode(int width, int height) const;

    // On connection: if the VDD is attached (e.g. Windows put it back at the driver's 800x600), give
    // it the other PC's real resolution (or the closest mode) and put it on `side`, so the cursor
    // crosses a same-sized screen instead of catching on a tiny one. False if not attached.
    bool match_peer(int width, int height, display::Side side);

    // Moves the attached monitor next to the other attached monitors. The primary stays at (0, 0).
    bool place(display::Side side);

    // Adds the monitor to the desktop with this mode, next to the others on `side`.
    bool attach(int width, int height, int hz, display::Side side);

    // Removes the monitor from the desktop. Windows moves its windows to the remaining monitors.
    bool detach();

private:
    explicit VirtualDisplay(std::wstring device) : device_(std::move(device)) {}
    std::wstring device_;
};

}  // namespace kvm
