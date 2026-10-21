// SPDX-License-Identifier: Apache-2.0

// wire_test: what someone on the network sees between two Sarke PCs (roadmap P1.14, docs/SECURITY.md
// S2). A relay on 127.0.0.1 stands where the network is: it passes the control, video and file
// channels between the real kvm_host and a paired client (this test), and keeps a copy of every byte.
// Checks, on each channel and in both directions:
//   - every byte belongs to a TLS record: the handshake, then only encrypted application data;
//   - nothing readable crossed: no JSON, no session token, no names, no video frame header, none of
//     the test video's bytes, no clipboard text.
//
//   wire_test.exe            (exit code 0 = all passed; ctest label "loopback")
//   wire_test.exe --keep     keep the temp folder (kvm_host's log)

#include <winsock2.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "kvm/log.h"
#include "kvm/net.h"
#include "kvm/protocol.h"
#include "kvm/tls.h"
#include "kvm/video.h"
#include "program_util.h"

namespace proto = kvm::proto;
using namespace kvm::test;

namespace {

// Passes TCP connections from 127.0.0.1:port() on to 127.0.0.1:target, and keeps every byte that
// crosses, per connection and direction.
class Relay {
public:
    struct Capture {
        std::string to_host;
        std::string to_client;
    };

    explicit Relay(std::uint16_t target) : target_(target) {
        kvm::net::ensure_winsock();
        listen_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int len = sizeof(addr);
        if (bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 && ::listen(listen_, 8) == 0 &&
            getsockname(listen_, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
            port_ = ntohs(addr.sin_port);
        }
        acceptor_ = std::thread([this] { accept_loop(); });
    }

    ~Relay() {
        closesocket(listen_);  // accept() returns
        acceptor_.join();
        std::lock_guard lock(mutex_);
        for (auto& link : links_) {
            for (const SOCKET s : {link.client, link.host}) {
                shutdown(s, SD_BOTH);
                CancelIoEx(reinterpret_cast<HANDLE>(s), nullptr);  // wakes a blocked recv()
            }
        }
        for (auto& link : links_) {
            for (auto& t : link.pumps) t.join();
            closesocket(link.client);
            closesocket(link.host);
        }
    }

    Relay(const Relay&) = delete;
    Relay& operator=(const Relay&) = delete;

    std::uint16_t port() const { return port_; }

    std::vector<Capture> captures() {
        std::lock_guard lock(mutex_);
        std::vector<Capture> out;
        for (const auto& link : links_) out.push_back(link.capture);
        return out;
    }

private:
    struct Link {
        SOCKET client = INVALID_SOCKET;
        SOCKET host = INVALID_SOCKET;
        Capture capture;
        std::vector<std::thread> pumps;
    };

    void accept_loop() {
        for (;;) {
            const SOCKET client = accept(listen_, nullptr, nullptr);
            if (client == INVALID_SOCKET) return;  // the destructor closed the listener
            const SOCKET host = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            sockaddr_in to{};
            to.sin_family = AF_INET;
            to.sin_port = htons(target_);
            to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (connect(host, reinterpret_cast<sockaddr*>(&to), sizeof(to)) != 0) {
                closesocket(client);
                closesocket(host);
                continue;
            }
            std::lock_guard lock(mutex_);
            Link& link = links_.emplace_back();
            link.client = client;
            link.host = host;
            link.pumps.emplace_back([this, &link] { pump(link.client, link.host, link.capture.to_host); });
            link.pumps.emplace_back([this, &link] { pump(link.host, link.client, link.capture.to_client); });
        }
    }

    // Copies from -> to and keeps a copy in `seen`, until `from` closes.
    void pump(SOCKET from, SOCKET to, std::string& seen) {
        char buf[16384];
        for (;;) {
            const int got = recv(from, buf, static_cast<int>(sizeof(buf)), 0);
            if (got <= 0) break;
            {
                std::lock_guard lock(mutex_);
                seen.append(buf, static_cast<std::size_t>(got));
            }
            if (send(to, buf, got, 0) != got) break;
        }
        shutdown(to, SD_SEND);
    }

    std::uint16_t target_;
    std::uint16_t port_ = 0;
    SOCKET listen_ = INVALID_SOCKET;
    std::mutex mutex_;
    std::list<Link> links_;  // nodes stay put: the pumps hold references
    std::thread acceptor_;
};

// Whether `bytes` is a sequence of TLS records: the handshake first, then only encrypted
// application data. Empty when it is; otherwise what is wrong.
struct RecordReport {
    std::string problem;
    std::size_t records = 0;
    std::size_t application_data = 0;
};

RecordReport tls_records(const std::string& bytes) {
    RecordReport r;
    std::size_t at = 0;
    bool encrypted = false;
    while (at + 5 <= bytes.size()) {
        const auto b = [&](std::size_t i) { return static_cast<unsigned>(static_cast<unsigned char>(bytes[at + i])); };
        const unsigned type = b(0), version = b(1) << 8 | b(2), length = b(3) << 8 | b(4);
        if (type < 0x14 || type > 0x17) {
            r.problem = std::format("byte {}: 0x{:02x} isn't a TLS record type", at, type);
            return r;
        }
        if (version != 0x0301 && version != 0x0303) {
            r.problem = std::format("byte {}: record version 0x{:04x}", at, version);
            return r;
        }
        if (length > 16384 + 256) {
            r.problem = std::format("byte {}: record of {} bytes", at, length);
            return r;
        }
        if (type == 0x17) {
            encrypted = true;
            ++r.application_data;
        } else if (type == 0x16 && encrypted) {
            r.problem = std::format("byte {}: a plaintext handshake record after encrypted data", at);
            return r;
        }
        ++r.records;
        at += 5 + length;
    }
    // A record may be cut where the connection was closed; anything else left over is wrong.
    if (at < bytes.size() && bytes.size() - at >= 5) r.problem = "bytes after the last record";
    return r;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const bool keep = argc > 1 && std::wstring_view(argv[1]) == L"--keep";
    Checks check;
    const fs::path bin = exe_dir().parent_path();
    const fs::path host_exe = bin / L"host_a" / L"kvm_host.exe";
    if (!fs::exists(host_exe)) {
        kvm::log::error("kvm_host.exe not found next to {}", bin.string());
        return 1;
    }
    const fs::path dir = fs::temp_directory_path() / std::format(L"sarke_wire_test_{}", GetCurrentProcessId());
    fs::create_directories(dir);
    const std::uint16_t control = free_port(), video = free_port(), files = free_port();
    std::ofstream(dir / "host.json") << std::format(R"({{"control_port": {}, "video_port": {}, "file_port": {}}})",
                                                    control, video, files)
                                     << "\n";

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

    const auto me = kvm::tls::Credentials::generate("wire-test-client");
    const std::wstring data = L" --data-dir \"" + (dir / L"host-data").wstring() + L"\"";
    std::optional<kvm::tls::DeviceId> host_id;
    {
        Program id(job, host_exe, data + L" --print-id", dir / "host-id.log");
        if (id.started() && id.wait_exit(30s) == 0u) host_id = device_id_in(id.log());
    }
    check(host_id.has_value(), "kvm_host has an identity");
    if (!host_id) {
        CloseHandle(job);
        return 1;
    }

    std::string token;
    std::size_t frames = 0;
    std::vector<Relay::Capture> wire;
    const std::string clipboard_text = "wire-test: this clipboard text must not be readable on the network";
    {
        // Small test frames, 20 per second: random bytes after a JSON header, like a JPEG.
        Program host(job, host_exe,
                     L"--config \"" + (dir / "host.json").wstring() +
                         L"\" --no-discovery --no-kvm --no-clipboard --no-desk --test-video --test-video-kb 8 "
                         L"--test-video-fps 20" +
                         data + L" --pair-with " + hex(me.id()),
                     dir / "host.log");
        check(host.started() && wait_until([&] { return host.log_has("listening on ports"); }, 15s), "kvm_host starts");
        Relay control_relay(control), video_relay(video), file_relay(files);
        check(control_relay.port() && video_relay.port() && file_relay.port(), "a relay in front of each port");

        auto pending = kvm::net::connect("127.0.0.1", control_relay.port(), me, 3s);
        check(pending && pending->stream->peer_id() == *host_id, "through the relay, the host's key arrives");
        if (pending) {
            kvm::net::Connection conn(std::move(*pending));
            conn.send(proto::Hello{.screen_w = 1920, .screen_h = 1080, .hostname = "wire-test-client"});
            for (auto msg = conn.incoming().pop_for(5s); msg; msg = conn.incoming().pop_for(1s)) {
                if (const auto* h = std::get_if<proto::Hello>(&*msg)) {
                    token = h->session_token;
                    break;
                }
            }
            check(token.size() == 64, "a session, with its token");
            conn.send(proto::Clipboard{.seq = 1, .kind = "text", .total = 1, .offset = 0, .data = clipboard_text});

            auto video_channel = kvm::net::connect_video("127.0.0.1", video_relay.port(), me, *host_id, token, 3s);
            auto file_channel = kvm::net::connect_side_channel("127.0.0.1", file_relay.port(), me, *host_id, token, 3s);
            check(video_channel && file_channel, "video and files through their relays");
            const auto until = std::chrono::steady_clock::now() + 2s;
            while (video_channel && std::chrono::steady_clock::now() < until) {
                if (const auto frame = video_channel->recv_frame(); frame && frame->header.codec == "test") ++frames;
            }
            check(frames >= 10, std::format("test video arrives ({} frames in 2 s)", frames));
            if (video_channel) video_channel->close();
            if (file_channel) file_channel->stream->abort();
            conn.close();
        }
        std::this_thread::sleep_for(300ms);  // the last bytes reach the relays
        for (Relay* relay : {&control_relay, &video_relay, &file_relay}) {
            for (auto& c : relay->captures()) wire.push_back(std::move(c));
        }
        host.stop(10s);
    }

    std::size_t total = 0;
    bool all_tls = true;
    for (const auto& c : wire) {
        for (const std::string* bytes : {&c.to_host, &c.to_client}) {
            total += bytes->size();
            const auto report = tls_records(*bytes);
            if (!report.problem.empty()) {
                kvm::log::error("not TLS: {}", report.problem);
                all_tls = false;
            }
        }
    }
    check(wire.size() >= 3 && total > frames * 8 * 1024,
          std::format("the relays saw {} connections, {} KB", wire.size(), total / 1024));
    check(all_tls, "every byte, both ways, on every channel, is a TLS record (handshake, then encrypted data)");
    // The check itself, on what Sarke 1.1 sent: a plaintext [u32 length]{"type":"hello",...}.
    const std::string v2_hello("\x00\x00\x00\x12{\"type\":\"hello\"}", 22);
    check(!tls_records(v2_hello).problem.empty() && v2_hello.find("\"type\"") != std::string::npos,
          "(and the same checks catch a plaintext Sarke 1.1 hello)");

    // What would be readable if anything went out in the clear.
    std::mt19937 rng(42);  // kvm_host --test-video's payload
    std::string video_bytes(32, '\0');
    for (auto& b : video_bytes) b = static_cast<char>(static_cast<std::uint8_t>(rng()));
    const std::vector<std::pair<std::string, std::string>> secrets = {
        {"\"type\"", "a JSON message"},
        {"wire-test-client", "this PC's name"},
        {"protocol_version", "the hello"},
        {"\"codec\"", "a video frame header"},
        {"channel_hello", "a side channel's first message"},
        {token.empty() ? std::string("no token") : token, "the session token"},
        {kvm::tls::to_hex(*host_id), "the host's device ID in hex"},
        {clipboard_text, "the clipboard text"},
        {video_bytes, "the test video's bytes"},
    };
    for (const auto& [secret, what] : secrets) {
        const bool seen = std::any_of(wire.begin(), wire.end(), [&](const Relay::Capture& c) {
            return c.to_host.find(secret) != std::string::npos || c.to_client.find(secret) != std::string::npos;
        });
        check(!seen, std::format("not on the wire: {}", what));
    }

    CloseHandle(job);
    if (!keep) {
        std::error_code ec;
        fs::remove_all(dir, ec);
    } else {
        kvm::log::info("kept {}", dir.string());
    }
    if (check.failed) {
        kvm::log::error("{} check(s) failed", check.failed);
        return 1;
    }
    kvm::log::info("all passed");
    return 0;
}
