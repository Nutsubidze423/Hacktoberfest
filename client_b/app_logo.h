// App logo (docs/DESIGN.md section 9, concept C "Caret") rendered in code as an HICON at the exact
// pixel size Windows asks for, so it stays sharp at every DPI and needs no .ico/.rc file.
#pragma once

#include <windows.h>

#include <memory>
#include <type_traits>

namespace kvm::ui {

struct IconDeleter {
    void operator()(HICON icon) const { DestroyIcon(icon); }
};
using unique_icon = std::unique_ptr<std::remove_pointer_t<HICON>, IconDeleter>;

// Square logo icon of size x size pixels. Grid cell edges are rounded to whole pixels.
unique_icon create_logo_icon(int size);

// The icon sizes Windows uses at this DPI (title bar / tray, and Alt+Tab / notifications).
inline int small_icon_size(UINT dpi) {
    return GetSystemMetricsForDpi(SM_CXSMICON, dpi);
}
inline int large_icon_size(UINT dpi) {
    return GetSystemMetricsForDpi(SM_CXICON, dpi);
}

}  // namespace kvm::ui
