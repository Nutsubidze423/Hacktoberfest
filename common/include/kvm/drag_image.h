// SPDX-License-Identifier: Apache-2.0

// The picture under the cursor during a file drag, exactly as the shell draws it (Explorer's icon or
// thumbnail with the name, as when files go from one monitor to another). The drag that goes on on
// the other PC shows this same picture.
//
//   FileDropCatcher::take_drag_image()  ->  FileTransfer::send(..., image)  ->  FileDragSource::start(..., image)
#pragma once

#include <cstdint>
#include <vector>

namespace kvm::files {

struct DragImage {
    int width = 0;
    int height = 0;
    int cursor_x = 0;  // where the cursor sits on the picture
    int cursor_y = 0;
    std::vector<std::uint8_t> bgra;  // width * height * 4, top-down rows, premultiplied alpha
};

}  // namespace kvm::files
