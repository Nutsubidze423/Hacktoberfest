// SPDX-License-Identifier: Apache-2.0

// net_listen_test: what Sarke says when it can't listen on a port (roadmap P0.10). A port another
// program holds must name that program ("in use by <name> (pid N)"); a port Windows reserved
// (Hyper-V, WSL and Docker reserve ranges) must say so. Both point at the ports in config.json.
//
//   net_listen_test.exe                    (exit code 0 = all passed)
//   net_listen_test.exe --reserved <port>  also check a port in one of this PC's reserved ranges
//                                          (`netsh interface ipv4 show excludedportrange protocol=tcp`)

#include <winsock2.h>
#include <windows.h>

#include <cstdint>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "kvm/log.h"
#include "kvm/net.h"

namespace {

struct Checks {
    int failed = 0;
    void operator()(bool ok, std::string_view what) {
        kvm::log::info("{} {}", ok ? "[ok]  " : "[FAIL]", what);
        if (!ok) ++failed;
    }
};

std::uint16_t free_port() {
    kvm::net::ensure_winsock();
    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    int len = sizeof(addr);
    std::uint16_t port = 0;
    if (s != INVALID_SOCKET && bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
        getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
        port = ntohs(addr.sin_port);
    }
    if (s != INVALID_SOCKET) closesocket(s);
    return port;
}

// The error a Server on `port` throws, or nullopt if it could listen.
std::optional<std::string> listen_error(std::uint16_t port) {
    try {
        kvm::net::Server server(port);
        return std::nullopt;
    } catch (const std::exception& e) {
        return e.what();
    }
}

bool contains(const std::optional<std::string>& text, std::string_view what) {
    return text && text->find(what) != std::string::npos;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    Checks check;

    kvm::log::info("1) a port this program already listens on");
    {
        const std::uint16_t port = free_port();
        kvm::net::Server holder(port);
        const auto error = listen_error(port);
        if (error) kvm::log::info("   {}", *error);
        check(error.has_value(), std::format("a second listen on {} fails", port));
        check(contains(error, std::format("in use by net_listen_test.exe (pid {})", GetCurrentProcessId())),
              "the error names the program that holds the port");
        check(contains(error, "config.json"), "and says where to pick other ports");
    }

    kvm::log::info("2) a free port");
    check(!listen_error(free_port()).has_value(), "listening works");

    if (argc > 2 && std::wstring_view(argv[1]) == L"--reserved") {
        const auto port = static_cast<std::uint16_t>(_wtoi(argv[2]));
        kvm::log::info("3) port {}, reserved by Windows on this PC", port);
        const auto error = listen_error(port);
        if (error) kvm::log::info("   {}", *error);
        check(contains(error, "Windows reserved it"), "the error says Windows reserved it");
    }

    if (check.failed) {
        kvm::log::error("{} check(s) failed", check.failed);
        return 1;
    }
    kvm::log::info("all passed");
    return 0;
}
