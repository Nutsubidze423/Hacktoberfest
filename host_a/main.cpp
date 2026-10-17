// kvm_host.exe — runs on A (owns the physical mouse/keyboard, streams a window).
// Usage: kvm_host.exe [--config <path>]

#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <optional>
#include <string_view>

#include <windows.h>

#include "kvm/config.h"
#include "kvm/log.h"
#include "kvm/net.h"
#include "kvm/protocol.h"

namespace {

using namespace std::chrono_literals;
namespace proto = kvm::proto;

constexpr auto kHelloTimeout = 3s;
constexpr auto kStatsInterval = 5s;

std::atomic<bool> g_running{true};
std::atomic<kvm::net::Server*> g_server{nullptr};

BOOL WINAPI on_console_ctrl(DWORD) {
    g_running = false;
    if (auto* server = g_server.load()) {
        server->stop();
    }
    return TRUE;
}

// Waits for the client's hello and checks the protocol version. nullopt = reject the client.
std::optional<proto::Hello> handshake(kvm::net::Connection& conn) {
    const auto first = conn.incoming().pop_for(kHelloTimeout);
    if (!first) {
        kvm::log::warn("{}: no hello within {} s, dropping", conn.peer(), kHelloTimeout.count());
        return std::nullopt;
    }
    const auto* hello = std::get_if<proto::Hello>(&*first);
    if (!hello) {
        kvm::log::warn("{}: expected hello, got {}, dropping", conn.peer(), proto::type_of(*first));
        return std::nullopt;
    }
    if (hello->protocol_version != proto::kProtocolVersion) {
        kvm::log::error("{}: protocol v{} but this host speaks v{} — rebuild both sides from the same commit",
                        conn.peer(), hello->protocol_version, proto::kProtocolVersion);
        return std::nullopt;
    }
    return *hello;
}

void run_session(kvm::net::Connection& conn) {
    const auto hello = handshake(conn);
    if (!hello) {
        return;
    }
    kvm::log::info("client '{}' connected from {}: screen {}x{}, dpi scale {:.2f}", hello->hostname, conn.peer(),
                   hello->screen_w, hello->screen_h, hello->dpi_scale);

    auto next_stats = std::chrono::steady_clock::now() + kStatsInterval;
    while (g_running && conn.is_open()) {
        if (const auto msg = conn.incoming().pop_for(500ms)) {
            // TODO(luka): route to remote_input (sandro) once it exists.
            kvm::log::info("from client: {}", proto::encode(*msg).dump());
        }
        if (std::chrono::steady_clock::now() >= next_stats) {
            const auto stats = conn.stats();
            kvm::log::info("link: rtt {:.2f} ms, clock offset {:.1f} ms", stats.rtt_us / 1000.0,
                           stats.clock_offset_us / 1000.0);
            next_stats += kStatsInterval;
        }
    }
    kvm::log::info("client '{}' session ended", hello->hostname);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetConsoleCtrlHandler(on_console_ctrl, TRUE);

    std::optional<std::filesystem::path> config_path;
    for (int i = 1; i < argc; ++i) {
        if (std::wstring_view(argv[i]) == L"--config" && i + 1 < argc) {
            config_path = argv[++i];
        }
    }

    try {
        const kvm::Config cfg = kvm::load_config(config_path);
        kvm::net::Server server(cfg.control_port);
        g_server = &server;
        kvm::log::info("kvm_host: protocol v{}, listening on port {} (client on the {}). Ctrl+C to quit.",
                       proto::kProtocolVersion, cfg.control_port, cfg.client_side);

        while (g_running) {
            auto conn = server.accept();
            if (!conn) {
                continue;  // stop() or a transient accept error
            }
            run_session(*conn);
        }
        g_server = nullptr;
        kvm::log::info("kvm_host: bye");
        return 0;
    } catch (const std::exception& e) {
        kvm::log::error("{}", e.what());
        return 1;
    }
}
