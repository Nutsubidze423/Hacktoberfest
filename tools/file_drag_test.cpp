// file_drag_test: both halves of the cross-PC file drag with real OLE drag and drop (T10).
// FileDragSource continues a drag with two temp files ("arrived from the other PC") from the cursor,
// SendInput moves the mouse onto a FileDropCatcher in this process and lets go: the catcher must get
// both paths and the source must report a drop. Then desk mode's catch_over(): a drag that keeps
// moving still drops onto it, and a drag over a hole (this PC's own window) is left to that window.
// The mouse moves around the screen center for a few seconds and goes back afterwards. If anything
// hangs it presses Esc (which also reaches the window in front).
//
//   file_drag_test.exe

#include <ole2.h>
#include <winsock2.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

#include "kvm/dpi.h"
#include "kvm/file_drag.h"
#include "kvm/file_drop.h"
#include "kvm/file_transfer.h"
#include "kvm/log.h"
#include "kvm/net.h"
#include "kvm/win32_input.h"

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

struct Checks {
    int failed = 0;
    void operator()(bool ok, std::string_view what) {
        kvm::log::info("{} {}", ok ? "[ok]  " : "[FAIL]", what);
        if (!ok) ++failed;
    }
};

void press_escape() {
    kvm::input::send_key(VK_ESCAPE, 0, true, false);
    kvm::input::send_key(VK_ESCAPE, 0, false, false);
}

// Listens on 127.0.0.1 only (a free port), so Windows Firewall doesn't ask anything.
SOCKET listen_loopback(std::uint16_t& port) {
    kvm::net::ensure_winsock();
    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int len = sizeof(addr);
    if (s == INVALID_SOCKET || bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(s, 1) != 0 ||
        getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        if (s != INVALID_SOCKET) closesocket(s);
        return INVALID_SOCKET;
    }
    port = ntohs(addr.sin_port);
    return s;
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void move_in_steps(POINT from, POINT to) {
    for (int i = 1; i <= 10; ++i) {
        kvm::input::move_cursor_abs(from.x + (to.x - from.x) * i / 10, from.y + (to.y - from.y) * i / 10);
        std::this_thread::sleep_for(30ms);
    }
}

}  // namespace

int main() {
    kvm::display::enable_dpi_awareness();
    Checks check;

    const fs::path dir = fs::temp_directory_path() / L"kvm_drag_test";
    fs::create_directories(dir);
    const std::vector<fs::path> files{dir / L"a.txt", dir / L"b.txt"};
    for (const auto& f : files) std::ofstream(f) << "from the other PC";

    std::mutex mutex;
    std::vector<fs::path> dropped;
    kvm::files::FileDropCatcher catcher(
        [&](std::vector<fs::path> paths) {
            std::lock_guard lock(mutex);
            dropped = std::move(paths);
        },
        L"Drop here (test)");
    kvm::files::FileDragSource drag;

    const auto primary = kvm::display::primary_monitor().rect;
    const POINT center{(primary.left + primary.right) / 2, (primary.top + primary.bottom) / 2};
    const POINT target{center.x + 300, center.y};
    catcher.show_at(target, 10s);

    kvm::input::Point cursor_before;
    kvm::input::get_cursor_pos(cursor_before);
    kvm::input::move_cursor_abs(center.x, center.y);
    std::this_thread::sleep_for(200ms);

    std::promise<DWORD> done;
    auto result = done.get_future();
    check(drag.start(files, [&](DWORD effect) { done.set_value(effect); }), "drag started");
    std::this_thread::sleep_for(300ms);
    check(drag.dragging(), "the drag is running from the cursor");

    for (int i = 1; i <= 10; ++i) {
        kvm::input::move_cursor_abs(center.x + 30 * i, center.y);
        std::this_thread::sleep_for(30ms);
    }
    std::this_thread::sleep_for(300ms);
    kvm::input::send_mouse_button(kvm::input::MouseButton::Left, false);  // the user lets go

    if (result.wait_for(3s) != std::future_status::ready) {
        press_escape();  // never leave a drag (and a pressed button) behind
        kvm::input::send_mouse_button(kvm::input::MouseButton::Left, false);
    }
    const DWORD effect = result.wait_for(2s) == std::future_status::ready ? result.get() : DROPEFFECT_NONE;
    kvm::input::move_cursor_abs(cursor_before.x, cursor_before.y);

    check(effect == DROPEFFECT_COPY || effect == DROPEFFECT_MOVE, std::format("dropped (effect {})", effect));
    std::this_thread::sleep_for(200ms);
    {
        // Same files, though Windows may spell the temp folder long (localadmin) or short (LOCALA~1).
        std::lock_guard lock(mutex);
        std::error_code ec;
        const bool same = dropped.size() == files.size() && fs::equivalent(dropped[0], files[0], ec) &&
                          fs::equivalent(dropped[1], files[1], ec);
        check(same, std::format("the catcher got both paths ({} item(s))", dropped.size()));
    }
    check(!drag.dragging(), "the drag is over");

    // 1b) The user keeps moving the mouse while the files arrive (as in the two-PC test): the press
    // that starts the drag must still land on the drag window, not on whatever the cursor passes.
    {
        {
            std::lock_guard lock(mutex);
            dropped.clear();
        }
        catcher.show_at(target, 10s);
        kvm::input::move_cursor_abs(center.x, center.y);
        std::this_thread::sleep_for(200ms);
        std::atomic<bool> wiggle{true};
        std::thread mover([&] {
            for (int i = 0; wiggle; ++i) {
                kvm::input::move_cursor_abs(center.x + (i % 2 ? 25 : -25), center.y + (i % 3 - 1) * 25);
                std::this_thread::sleep_for(3ms);
            }
        });
        std::promise<DWORD> moving_done;
        auto moving_result = moving_done.get_future();
        check(drag.start(files, [&](DWORD effect) { moving_done.set_value(effect); }), "drag started while moving");
        std::this_thread::sleep_for(400ms);
        wiggle = false;
        mover.join();
        check(drag.dragging(), "the drag runs although the cursor moved during the start");
        for (int i = 1; i <= 10; ++i) {
            kvm::input::move_cursor_abs(center.x + 30 * i, center.y);
            std::this_thread::sleep_for(30ms);
        }
        std::this_thread::sleep_for(300ms);
        kvm::input::send_mouse_button(kvm::input::MouseButton::Left, false);
        if (moving_result.wait_for(3s) != std::future_status::ready) {
            press_escape();
            kvm::input::send_mouse_button(kvm::input::MouseButton::Left, false);
        }
        const DWORD moving_effect =
            moving_result.wait_for(2s) == std::future_status::ready ? moving_result.get() : DROPEFFECT_NONE;
        check(moving_effect != DROPEFFECT_NONE, std::format("dropped after a moving start (effect {})", moving_effect));
        kvm::input::move_cursor_abs(cursor_before.x, cursor_before.y);
        std::this_thread::sleep_for(200ms);
    }

    kvm::log::info("2) the whole cross-PC drag on one PC (A and B over 127.0.0.1, no KVM)");
    {
        std::uint16_t port = 0;
        const SOCKET listener = listen_loopback(port);
        std::unique_ptr<kvm::files::FileTransfer> a;
        std::thread acceptor([&] {
            const SOCKET s = accept(listener, nullptr, nullptr);
            if (s != INVALID_SOCKET) a = std::make_unique<kvm::files::FileTransfer>(s, "A", kvm::files::Callbacks{}, dir / L"a_in");
        });
        kvm::files::FileDragSource drag_on_b;
        std::promise<DWORD> b_done;
        auto b_result = b_done.get_future();
        kvm::files::Callbacks b_callbacks;
        b_callbacks.on_drag_received = [&](const std::vector<fs::path>& saved) {
            drag_on_b.start(saved, [&](DWORD effect) { b_done.set_value(effect); });  // B: the drag goes on here
        };
        auto b = kvm::files::connect_files("127.0.0.1", port, b_callbacks, dir / L"b_in");
        acceptor.join();
        closesocket(listener);

        // A: an invisible catcher hands the dragged files to the drag transfer.
        kvm::files::FileDropCatcher catcher_a([&](std::vector<fs::path> paths) { a->send(std::move(paths), true); });
        // B: stands for any folder on B where the user lets go.
        std::vector<fs::path> landed;
        std::mutex landed_mutex;
        kvm::files::FileDropCatcher folder_on_b(
            [&](std::vector<fs::path> paths) {
                std::lock_guard lock(landed_mutex);
                landed = std::move(paths);
            },
            L"a folder on B (test)");
        kvm::files::FileDragSource drag_on_a;  // stands for Explorer's drag on A

        if (!a || !b) {
            check(false, "connected A and B");
        } else {
            const POINT start{center.x - 300, center.y};
            const POINT edge{center.x, center.y};
            const POINT folder{center.x + 300, center.y};
            kvm::input::move_cursor_abs(start.x, start.y);
            std::this_thread::sleep_for(150ms);
            std::promise<DWORD> a_done;
            auto a_result = a_done.get_future();
            drag_on_a.start(files, [&](DWORD effect) { a_done.set_value(effect); });
            std::this_thread::sleep_for(300ms);
            move_in_steps(start, edge);  // the user drags to the edge towards B...
            catcher_a.catch_now(edge);   // ...and the drag drops onto the invisible catcher on A
            check(a_result.wait_for(3s) == std::future_status::ready && a_result.get() != DROPEFFECT_NONE,
                  "A: the running drag dropped onto catch_now()");

            for (int i = 0; i < 30 && !drag_on_b.dragging(); ++i) std::this_thread::sleep_for(100ms);
            check(drag_on_b.dragging(), "B: the files arrived and the drag goes on at the cursor");
            folder_on_b.show_at(folder, 10s);
            std::this_thread::sleep_for(200ms);
            move_in_steps(edge, folder);  // the user keeps moving on B...
            std::this_thread::sleep_for(300ms);
            kvm::input::send_mouse_button(kvm::input::MouseButton::Left, false);  // ...and lets go over a folder
            if (b_result.wait_for(3s) != std::future_status::ready) {
                press_escape();
                kvm::input::send_mouse_button(kvm::input::MouseButton::Left, false);
            }
            check(b_result.wait_for(1s) == std::future_status::ready && b_result.get() != DROPEFFECT_NONE,
                  "B: dropped onto the folder");
            std::this_thread::sleep_for(200ms);
            std::lock_guard lock(landed_mutex);
            const bool same = landed.size() == 2 && read_file(landed[0]) == read_file(files[0]) &&
                              read_file(landed[1]) == read_file(files[1]);
            check(same, std::format("the folder on B got both files with the same content ({} item(s))", landed.size()));
            if (!landed.empty()) kvm::log::info("   B's temp copy: {}", landed[0].parent_path().string());
        }
        kvm::input::move_cursor_abs(cursor_before.x, cursor_before.y);
    }

    kvm::log::info("3) desk mode: catch_over() the other PC's screen (the area around the center)");
    {
        const RECT area{center.x - 400, center.y - 200, center.x + 400, center.y + 200};
        const RECT hole{center.x + 150, center.y - 100, center.x + 390, center.y + 100};  // this PC's own window
        const POINT in_hole{(hole.left + hole.right) / 2, (hole.top + hole.bottom) / 2};
        std::mutex catch_mutex;
        std::vector<fs::path> caught, own;
        kvm::files::FileDropCatcher catcher_over(
            [&](std::vector<fs::path> paths) {
                std::lock_guard lock(catch_mutex);
                caught = std::move(paths);
            },
            L"catch over (test)");
        kvm::files::FileDropCatcher own_window(
            [&](std::vector<fs::path> paths) {
                std::lock_guard lock(catch_mutex);
                own = std::move(paths);
            },
            L"own window (test)");

        // 3a) The user keeps moving fast after the catcher came up: the files still drop onto it.
        {
            const POINT from{center.x - 600, center.y};
            const POINT spot{center.x - 300, center.y + 50};
            kvm::input::move_cursor_abs(from.x, from.y);
            std::this_thread::sleep_for(150ms);
            std::promise<DWORD> a_done;
            auto a_result = a_done.get_future();
            kvm::files::FileDragSource source;  // stands for Explorer's drag on this PC
            source.start(files, [&](DWORD effect) { a_done.set_value(effect); });
            std::this_thread::sleep_for(300ms);
            move_in_steps(from, spot);
            catcher_over.catch_over(area, {hole});
            std::atomic<bool> moving{true};
            std::thread mover([&] {  // keeps moving around inside the area, away from the first point
                for (int i = 0; moving; ++i) {
                    kvm::input::move_cursor_abs(spot.x + (i % 20) * 12, spot.y - (i % 7) * 15);
                    std::this_thread::sleep_for(4ms);
                }
            });
            const bool a_finished = a_result.wait_for(3s) == std::future_status::ready;
            moving = false;
            mover.join();
            if (!a_finished) {
                press_escape();
                kvm::input::send_mouse_button(kvm::input::MouseButton::Left, false);
            }
            const DWORD a_effect = a_result.wait_for(1s) == std::future_status::ready ? a_result.get() : DROPEFFECT_NONE;
            std::this_thread::sleep_for(300ms);
            std::lock_guard lock(catch_mutex);
            check(a_effect != DROPEFFECT_NONE, std::format("3a: the moving drag dropped onto catch_over() (effect {})", a_effect));
            check(caught.size() == files.size(), std::format("3a: the catcher got the files ({})", caught.size()));
        }
        check(!catcher_over.visible(), "3a: the catcher is gone after the drop");

        // 3b) Over this PC's own window on the other screen: that window takes the drop, not the catcher.
        {
            {
                std::lock_guard lock(catch_mutex);
                caught.clear();
            }
            own_window.show_at(in_hole, 10s);
            std::this_thread::sleep_for(200ms);
            const POINT from{center.x - 600, center.y};
            kvm::input::move_cursor_abs(from.x, from.y);
            std::this_thread::sleep_for(150ms);
            std::promise<DWORD> b_done;
            auto b_result = b_done.get_future();
            kvm::files::FileDragSource source;
            source.start(files, [&](DWORD effect) { b_done.set_value(effect); });
            std::this_thread::sleep_for(300ms);
            move_in_steps(from, in_hole);
            catcher_over.catch_over(area, {hole});
            std::this_thread::sleep_for(400ms);  // no file drag enters the catcher: it steps aside
            check(!catcher_over.visible(), "3b: the catcher left the drag alone over the hole");
            kvm::input::send_mouse_button(kvm::input::MouseButton::Left, false);  // the user lets go there
            if (b_result.wait_for(3s) != std::future_status::ready) {
                press_escape();
                kvm::input::send_mouse_button(kvm::input::MouseButton::Left, false);
            }
            std::this_thread::sleep_for(300ms);
            std::lock_guard lock(catch_mutex);
            check(own.size() == files.size() && caught.empty(),
                  std::format("3b: the own window got the drop, not the catcher ({} vs {})", own.size(), caught.size()));
        }
        own_window.hide();

        // 3c) After catch_over() a plain catch_now() box is a small rectangle again.
        {
            std::lock_guard lock(catch_mutex);
            caught.clear();
        }
        const POINT edge{center.x - 300, center.y};
        kvm::input::move_cursor_abs(center.x - 600, center.y);
        std::this_thread::sleep_for(150ms);
        std::promise<DWORD> c_done;
        auto c_result = c_done.get_future();
        kvm::files::FileDragSource source_c;
        source_c.start(files, [&](DWORD effect) { c_done.set_value(effect); });
        std::this_thread::sleep_for(300ms);
        move_in_steps({center.x - 600, center.y}, edge);
        catcher_over.catch_now(edge);
        const bool c_ok = c_result.wait_for(3s) == std::future_status::ready && c_result.get() != DROPEFFECT_NONE;
        if (!c_ok) {
            press_escape();
            kvm::input::send_mouse_button(kvm::input::MouseButton::Left, false);
        }
        std::this_thread::sleep_for(300ms);
        {
            std::lock_guard lock(catch_mutex);
            check(c_ok && caught.size() == files.size(), "3c: catch_now() after catch_over() still catches");
        }
        kvm::input::move_cursor_abs(cursor_before.x, cursor_before.y);
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
    if (check.failed) {
        kvm::log::error("{} check(s) failed", check.failed);
        return 1;
    }
    kvm::log::info("all passed");
    return 0;
}
