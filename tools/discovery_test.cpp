// SPDX-License-Identifier: Apache-2.0

// discovery_test: kvm/discovery.h on this PC (roadmap P1.8). A Responder on a free UDP port answers
// find() with its name, control port and device ID; a malformed ID arrives as none. A PC sees its
// own answer (the launcher hides it by name), which is what this test listens for.
//
//   discovery_test.exe   (exit code 0 = all passed)

#include <winsock2.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "kvm/discovery.h"
#include "kvm/log.h"
#include "kvm/net.h"

namespace discovery = kvm::discovery;
using namespace std::chrono_literals;

namespace {

struct Checks {
    int failed = 0;
    void operator()(bool ok, std::string_view what) {
        kvm::log::info("{} {}", ok ? "[ok]  " : "[FAIL]", what);
        if (!ok) ++failed;
    }
};

std::uint16_t free_udp_port() {
    kvm::net::ensure_winsock();
    const SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
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

// The answer from the responder called `name`, as find() reports it.
std::vector<discovery::Host> answers_from(std::string_view name, std::uint16_t port) {
    auto hosts = discovery::find(1500ms, port);
    std::erase_if(hosts, [&](const discovery::Host& h) { return h.name != name; });
    return hosts;
}

}  // namespace

int main() {
    Checks check;
    const std::uint16_t port = free_udp_port();
    check(port != 0, std::format("a free UDP port ({})", port));
    const std::string name = std::format("discovery-test-{}", GetCurrentProcessId());
    const std::string id = std::string(32, 'a') + std::string(32, '0');

    {
        discovery::Responder responder(name, 50999, std::string(32, 'A') + std::string(32, '0'), port);
        const auto hosts = answers_from(name, port);
        check(!hosts.empty(), "find() gets the responder's answer");
        check(!hosts.empty() && std::all_of(hosts.begin(), hosts.end(),
                                            [&](const discovery::Host& h) {
                                                return h.control_port == 50999 && h.device_id == id &&
                                                       h.protocol_version > 0 && !h.busy;
                                            }),
              "with its control port, protocol and device ID (in lower case)");
    }
    {
        discovery::Responder responder(name, 50999, "not-a-device-id", port);
        const auto hosts = answers_from(name, port);
        check(!hosts.empty() &&
                  std::all_of(hosts.begin(), hosts.end(), [](const discovery::Host& h) { return h.device_id.empty(); }),
              "a malformed device ID arrives as none");
    }

    if (check.failed) {
        kvm::log::error("{} check(s) failed", check.failed);
        return 1;
    }
    kvm::log::info("all passed");
    return 0;
}
