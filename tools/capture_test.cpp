// capture_test: pick a window on A and dump captured frames to captures/*.jpg.
// Step 1 (current): window picking only. WGC capture + JPEG are added in later tasks.
//
//   cmake --build --preset x64-release
//   build\x64-release\tools\capture_test.exe

#include "../host_a/window_picker.h"

// TEMPORARY: tools/*.exe link only kvm_common, not host_a sources. Until there is a host-side
// library target (CMake is Luka's), compile the picker into this exe directly.
#include "../host_a/window_picker.cpp"

#include <cstdio>

int main() {
    // Physical pixels everywhere (see CLAUDE.md "DPI"). Later this comes from Demetre's dpi.h.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const auto window = kvm::pick_window_console();
    if (!window) return 0;

    std::printf("Selected HWND=0x%p pid=%lu size=%ldx%ld title=\"%s\"\n", static_cast<void*>(window->hwnd),
                window->pid, window->rect.right - window->rect.left, window->rect.bottom - window->rect.top,
                kvm::to_utf8(window->title).c_str());
    return 0;
}
