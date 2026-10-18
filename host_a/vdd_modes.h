// The Virtual Display Driver's list of resolutions (C:\VirtualDisplayDriver\vdd_settings.xml).
//
// Desk mode maps the VDD 1:1 onto the other PC's screen, so the VDD must have exactly that screen's
// size: a smaller mode leaves part of the other screen out of reach (e.g. its taskbar below 1080 on a
// 1920x1200 laptop), a bigger one hides windows beyond its edge. The driver only offers the modes in
// its settings file and reads that file when its device starts.
//
//   if (kvm::vdd_modes::ensure(1920, 1200)) vdd = kvm::VirtualDisplay::find();  // driver restarted
//
// The settings file is writable without admin rights; restarting the device asks Windows for
// permission once (UAC). tools/vdd_modes.ps1 adds the common laptop resolutions up front.
#pragma once

#include <filesystem>

namespace kvm::vdd_modes {

// C:\VirtualDisplayDriver\vdd_settings.xml
std::filesystem::path settings_path();

// width x height is listed in the settings file.
bool listed(int width, int height);

// Adds width x height to the settings file (no-op if listed). False if the file can't be written.
bool add(int width, int height);

// Restarts the driver's device so it reads the settings again. Asks for admin rights (UAC) and
// waits for the restart; false if refused or it failed. The VDD comes back detached.
bool restart_driver();

// Makes width x height one of the VDD's modes: adds it and restarts the driver when it was missing.
// True only if the driver was restarted (call VirtualDisplay::find() again: its name may change).
bool ensure(int width, int height);

}  // namespace kvm::vdd_modes
