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

#include "kvm/config.h"
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

// TODO(demetre): switch to the dpi module once it exists (multi-monitor geometry).
proto::Hello make_hello() {
    wchar_t name[256];
    DWORD name_len = static_cast<DWORD>(std::size(name));
    if (!GetComputerNameExW(ComputerNameDnsHostname, name, &name_len)) {
        name_len = 0;
    }
    return proto::Hello{
        .screen_w = GetSystemMetrics(SM_CXSCREEN),
        .screen_h = GetSystemMetrics(SM_CYSCREEN),
        .dpi_scale = GetDpiForSystem() / 96.0,
        .hostname = narrow(std::wstring_view(name, name_len)),
    };
}

void run_session(kvm::net::Connection& conn) {
    if (!conn.send(make_hello())) {
        return;
    }
    kvm::log::info("connected to host {}", conn.peer());

    auto next_stats = std::chrono::steady_clock::now() + kStatsInterval;
    while (g_running && conn.is_open()) {
        if (const auto msg = conn.incoming().pop_for(500ms)) {
            // TODO(luka): route to input_inject (demetre) / viewer (sandro) once they exist.
            kvm::log::info("from host: {}", proto::encode(*msg).dump());
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

        while (g_running) {
            if (auto conn = kvm::net::connect(cfg.host_ip, cfg.control_port)) {
                run_session(*conn);
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
