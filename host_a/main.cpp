// kvm_host.exe — runs on A (owns the physical mouse/keyboard, streams a window).
// Usage: kvm_host.exe [--config <path>]

#include <exception>
#include <filesystem>
#include <optional>
#include <string_view>

#include <windows.h>

#include "kvm/config.h"
#include "kvm/log.h"
#include "kvm/protocol.h"

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    std::optional<std::filesystem::path> config_path;
    for (int i = 1; i < argc; ++i) {
        if (std::wstring_view(argv[i]) == L"--config" && i + 1 < argc) {
            config_path = argv[++i];
        }
    }

    try {
        const kvm::Config cfg = kvm::load_config(config_path);
        kvm::log::info("kvm_host: protocol v{}, control port {}, video port {}, client on the {}",
                       kvm::proto::kProtocolVersion, cfg.control_port, cfg.video_port, cfg.client_side);
        // TODO(luka): start control/video servers.
        return 0;
    } catch (const std::exception& e) {
        kvm::log::error("{}", e.what());
        return 1;
    }
}
