// SPDX-License-Identifier: Apache-2.0

// Plays "the other PC" against a real kvm_host / kvm_client for KVM debugging:
// connects (or listens), says hello, takes control of that PC (focus_enter),
// wiggles its cursor a few pixels and taps harmless keys (F13/F14) for a while,
// then hands control back (focus_leave). Prints anything the other side sends.
// The connection must survive the whole run.
//
// Like the real programs it speaks TLS and only to a paired PC, with its own identity in
// %TEMP%\sarke_fake_peer (never this user's real one). Pair the two once:
//   kvm_fake_peer.exe --print-id                     -> the fake peer's device ID
//   kvm_host.exe --pair-with <that id>               (and kvm_host.exe --print-id for the next line)
//   kvm_fake_peer.exe --pair-with <host's id> [--host 127.0.0.1] [--port 50500] [--seconds 15]
// (exit code 0 = connection survived)

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include "kvm/dpi.h"
#include "kvm/net.h"
#include "kvm/protocol.h"
#include "kvm/trust.h"

namespace proto = kvm::proto;
using namespace std::chrono_literals;

int wmain(int argc, wchar_t** argv) {
    kvm::display::enable_dpi_awareness();
    std::string host = "127.0.0.1";
    std::uint16_t port = proto::kControlPort;
    int seconds = 15;
    kvm::trust::Options trust_options;
    for (int i = 1; i < argc; ++i) {
        if (const int used = kvm::trust::parse_arg(trust_options, argc, argv, i)) {
            i += used - 1;
            continue;
        }
        const std::wstring_view arg = argv[i];
        if (arg == L"--host" && i + 1 < argc) {
            host.clear();
            for (const wchar_t c : std::wstring_view(argv[++i])) host.push_back(static_cast<char>(c));
        } else if (arg == L"--seconds" && i + 1 < argc) {
            seconds = _wtoi(argv[++i]);
        } else if (arg == L"--port" && i + 1 < argc) {
            port = static_cast<std::uint16_t>(_wtoi(argv[++i]));
        }
    }

    if (!trust_options.data_dir) trust_options.data_dir = std::filesystem::temp_directory_path() / L"sarke_fake_peer";
    auto trust = kvm::trust::setup(trust_options, "fake-peer");
    if (!trust) return 2;
    if (trust_options.print_id) return 0;

    auto pending = kvm::net::connect(host, port, trust->me);
    if (!pending) {
        std::printf("FAIL: could not connect to %s\n", host.c_str());
        return 2;
    }
    if (!kvm::trust::admit(*trust->paired, *pending->stream, pending->peer)) {
        std::printf("FAIL: the host's key isn't paired with the fake peer (--pair-with)\n");
        return 2;
    }
    auto conn = std::make_unique<kvm::net::Connection>(std::move(*pending));
    const auto screen = kvm::display::primary_monitor();
    conn->send(proto::Hello{.screen_w = screen.rect.width(),
                            .screen_h = screen.rect.height(),
                            .dpi_scale = screen.scale,
                            .hostname = "fake-peer"});
    std::printf("connected, hello sent\n");

    const auto drain = [&] {
        while (auto msg = conn->incoming().try_pop()) std::printf("  <- %s\n", proto::encode(*msg).dump().c_str());
    };
    std::this_thread::sleep_for(1s);
    drain();

    std::printf("-> focus_enter (taking control of that PC)\n");
    conn->send(proto::FocusEnter{.y_norm = 0.5});
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    int n = 0;
    while (std::chrono::steady_clock::now() < end && conn->is_open()) {
        // Wiggle in place and tap F13/F14 now and then.
        conn->send(proto::MouseMove{.dx = (n % 2) ? -4 : 4, .dy = (n % 4 < 2) ? 2 : -2});
        if (n % 50 == 0) {
            const std::uint16_t vk = (n / 50) % 2 ? VK_F14 : VK_F13;
            conn->send(proto::Key{.vk = vk, .scan = 0, .down = true});
            conn->send(proto::Key{.vk = vk, .scan = 0, .down = false});
        }
        ++n;
        drain();
        std::this_thread::sleep_for(10ms);
    }
    const bool survived = conn->is_open();
    std::printf("%d moves sent, connection %s\n", n, survived ? "still open" : "CLOSED by the other side");
    if (survived) {
        conn->send(proto::FocusLeave{.y_norm = 0.5});
        std::this_thread::sleep_for(500ms);
        drain();
        std::printf("-> focus_leave (control handed back)\n");
    }
    return survived ? 0 : 1;
}
