// kvm_client.exe — runs on B (receives input and the window stream from A).
// Usage: kvm_client.exe [--host <A's IP>] [--config <path>]

#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <windows.h>

#include "kvm/config.h"
#include "kvm/log.h"
#include "kvm/protocol.h"

namespace {

std::string narrow(std::wstring_view s) {
    const int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len, nullptr, nullptr);
    return out;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    std::optional<std::filesystem::path> config_path;
    std::optional<std::string> host_override;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg = argv[i];
        if (arg == L"--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == L"--host" && i + 1 < argc) {
            host_override = narrow(argv[++i]);
        }
    }

    try {
        kvm::Config cfg = kvm::load_config(config_path);
        if (host_override) {
            cfg.host_ip = *host_override;
        }
        kvm::log::info("kvm_client: protocol v{}, host {}:{}", kvm::proto::kProtocolVersion, cfg.host_ip,
                       cfg.control_port);
        // TODO(luka): connect to host, send hello, reconnect loop.
        return 0;
    } catch (const std::exception& e) {
        kvm::log::error("{}", e.what());
        return 1;
    }
}
