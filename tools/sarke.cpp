// sarke.exe — start Sarke on this PC: runs kvm_host.exe or kvm_client.exe in the background with a
// tray icon, notifications, Display arrangement and a settings menu (client_b/launcher.h).
//
// Usage: sarke.exe [--host | --client] [--background] [--config <path>] [-- <args for the app>]
//        sarke.exe --quit | --restart      (tell the running Sarke)
//   --host / --client   which app this PC runs (remembered; otherwise found on the network once)
//   --background        don't open the list of PCs on the network (Windows autostart): tray icon only
//   --config <path>     config.json to use (default: the one the apps would find)
//   -- ...              everything after is passed to the app, e.g. -- --no-clipboard

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <string_view>

#include "kvm/dpi.h"
#include "launcher.h"

// A window app: no console window (tools/CMakeLists builds console exes).
#pragma comment(linker, "/SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup")

int wmain(int argc, wchar_t** argv) {
    kvm::display::enable_dpi_awareness();
    kvm::launcher::Options options;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg = argv[i];
        if (arg == L"--quit") {
            options.command = kvm::launcher::Command::Quit;
        } else if (arg == L"--restart") {
            options.command = kvm::launcher::Command::Restart;
        } else if (arg == L"--host") {
            options.role = kvm::layout::Role::Host;
        } else if (arg == L"--client") {
            options.role = kvm::layout::Role::Client;
        } else if (arg == L"--background") {
            options.show_devices = false;
        } else if (arg == L"--config" && i + 1 < argc) {
            options.config_path = std::filesystem::path(argv[++i]);
        } else if (arg == L"--") {
            for (++i; i < argc; ++i) options.app_args.emplace_back(argv[i]);
        } else {
            std::fputs("usage: sarke.exe [--host | --client] [--background] [--config <path>] [-- <args for the app>]\n", stderr);
            return 2;
        }
    }
    return kvm::launcher::run(options);
}
