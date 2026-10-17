// kvm_client.exe — runs on B (receives input and the window stream from A).
// Usage: kvm_client.exe [--host <A's IP>] [--config <path>] [--log <file>]

#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include <windows.h>

#include "input_inject.h"
#include "kvm/config.h"
#include "kvm/dpi.h"
#include "kvm/log.h"
#include "kvm/net.h"
#include "kvm/protocol.h"

namespace {

using namespace std::chrono_literals;
namespace proto = kvm::proto;

constexpr auto kReconnectDelay = 1s;
constexpr auto kStatsInterval = 5s;

std::atomic<bool> g_running{true};

BOOL WINAPI on_console_ctrl(DWORD) {
    g_running = false;
    return TRUE;
}

std::string narrow(std::wstring_view s) {
    const int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len, nullptr, nullptr);
    return out;
}

// Screen = B's primary monitor, in physical pixels.
proto::Hello make_hello() {
    wchar_t name[256];
    DWORD name_len = static_cast<DWORD>(std::size(name));
    if (!GetComputerNameExW(ComputerNameDnsHostname, name, &name_len)) {
        name_len = 0;
    }
    const auto primary = kvm::display::primary_monitor();
    return proto::Hello{
        .screen_w = primary.rect.width(),
        .screen_h = primary.rect.height(),
        .dpi_scale = primary.scale,
        .hostname = narrow(std::wstring_view(name, name_len)),
    };
}

void run_session(kvm::net::Connection& conn, kvm::display::Side host_side) {
    if (!conn.send(make_hello())) {
        return;
    }
    kvm::log::info("connected to host {}", conn.peer());

    // KVM: replays A's input while B has control; releases everything held when the session ends.
    kvm::client::InputInject inject(host_side, [&conn](const proto::Message& m) { return conn.send_json(proto::encode(m)); });

    auto next_stats = std::chrono::steady_clock::now() + kStatsInterval;
    while (g_running && conn.is_open()) {
        if (const auto msg = conn.incoming().pop_for(500ms)) {
            if (!inject.handle(*msg)) {
                // TODO(luka): route to viewer (sandro) once it exists.
                kvm::log::info("from host: {}", proto::encode(*msg).dump());
            }
        }
        if (std::chrono::steady_clock::now() >= next_stats) {
            const auto stats = conn.stats();
            kvm::log::info("link: rtt {:.2f} ms, clock offset {:.1f} ms", stats.rtt_us / 1000.0,
                           stats.clock_offset_us / 1000.0);
            next_stats += kStatsInterval;
        }
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetConsoleCtrlHandler(on_console_ctrl, TRUE);

    std::optional<std::filesystem::path> config_path;
    std::optional<std::string> host_override;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg = argv[i];
        if (arg == L"--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == L"--host" && i + 1 < argc) {
            host_override = narrow(argv[++i]);
        } else if (arg == L"--log" && i + 1 < argc) {
            kvm::log::open_file(argv[++i]);
        }
    }

    try {
        kvm::Config cfg = kvm::load_config(config_path);
        if (host_override) {
            cfg.host_ip = *host_override;
        }
        kvm::log::info("kvm_client: protocol v{}, host {}:{}. Ctrl+C to quit.", proto::kProtocolVersion, cfg.host_ip,
                       cfg.control_port);

        // client_side says where B stands next to A, so A is on the opposite side of B.
        const auto side = kvm::display::parse_side(cfg.client_side);
        if (!side || (*side != kvm::display::Side::Left && *side != kvm::display::Side::Right)) {
            kvm::log::error("config: client_side must be \"left\" or \"right\", got \"{}\"", cfg.client_side);
            return 1;
        }
        const auto host_side = kvm::display::opposite(*side);

        while (g_running) {
            if (auto conn = kvm::net::connect(cfg.host_ip, cfg.control_port)) {
                run_session(*conn, host_side);
            }
            if (g_running) {
                kvm::log::info("reconnecting in {} s...", kReconnectDelay.count());
                std::this_thread::sleep_for(kReconnectDelay);
            }
        }
        kvm::log::info("kvm_client: bye");
        return 0;
    } catch (const std::exception& e) {
        kvm::log::error("{}", e.what());
        return 1;
    }
}
