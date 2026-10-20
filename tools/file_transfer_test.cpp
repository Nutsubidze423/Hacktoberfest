// SPDX-License-Identifier: Apache-2.0

// file_transfer_test: the 50502 file channel on 127.0.0.1, both directions (T9). No second PC needed.
//   1. safe_destination(): what a peer may and may not write.
//   2. B -> A: a folder tree, a 3.5 MB file, an empty file and a Georgian file name, byte for byte.
//   3. A -> B over the same connection.
//   4. Sending the same things again gives "big (2).bin" and "project (2)".
//   5. A hostile peer ("../evil.txt", "C:/evil.txt", "CON"...) can't write outside the target.
// Everything happens in %TEMP%\kvm_file_test, which is deleted at the end.

#include <winsock2.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "kvm/file_transfer.h"
#include "kvm/log.h"
#include "kvm/net.h"

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

struct Checks {
    int failed = 0;
    void operator()(bool ok, std::string_view what) {
        kvm::log::info("{} {}", ok ? "[ok]  " : "[FAIL]", what);
        if (!ok) ++failed;
    }
};

// Collects on_received calls of one side.
struct Inbox {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::vector<fs::path>> transfers;

    kvm::files::Callbacks callbacks() {
        return {.on_received = [this](const std::vector<fs::path>& saved) {
            {
                std::lock_guard lock(mutex);
                transfers.push_back(saved);
            }
            cv.notify_all();
        }};
    }
    bool wait_for(std::size_t count, std::chrono::seconds timeout) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, timeout, [&] { return transfers.size() >= count; });
    }
    std::vector<fs::path> transfer(std::size_t i) {
        std::lock_guard lock(mutex);
        return i < transfers.size() ? transfers[i] : std::vector<fs::path>{};
    }
};

void write_file(const fs::path& p, std::string_view content) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary).write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

bool same_file(const fs::path& a, const fs::path& b) {
    return fs::exists(a) && fs::exists(b) && read_file(a) == read_file(b);
}

// Listens on 127.0.0.1 only (a free port), so Windows Firewall doesn't ask anything.
SOCKET listen_loopback(std::uint16_t& port) {
    kvm::net::ensure_winsock();
    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int len = sizeof(addr);
    if (s == INVALID_SOCKET || bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(s, 1) != 0 ||
        getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        if (s != INVALID_SOCKET) closesocket(s);
        return INVALID_SOCKET;
    }
    port = ntohs(addr.sin_port);
    return s;
}

SOCKET connect_loopback(std::uint16_t port) {
    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

// One framed message, written by hand like a peer that doesn't play by the rules would.
bool raw_send(SOCKET s, const nlohmann::json& header, std::string_view payload = {}) {
    const std::string h = header.dump();
    std::string buf(8, '\0');
    const u_long lens[2] = {htonl(static_cast<u_long>(h.size())), htonl(static_cast<u_long>(payload.size()))};
    std::memcpy(buf.data(), lens, 8);
    buf += h;
    buf.append(payload);
    return send(s, buf.data(), static_cast<int>(buf.size()), 0) == static_cast<int>(buf.size());
}

// Every file below `dir`.
std::vector<fs::path> files_below(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file()) out.push_back(it->path());
    }
    return out;
}

}  // namespace

int main() {
    Checks check;
    const fs::path root = fs::temp_directory_path() / L"kvm_file_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path src = root / L"src", dst_a = root / L"dst_a", dst_b = root / L"dst_b";

    // Source files: a folder tree with an empty file, a multi-chunk binary and a Georgian name.
    write_file(src / L"hello.txt", "hello from A");
    write_file(src / L"project" / L"readme.md", "# project\n");
    write_file(src / L"project" / L"src" / L"main.cpp", "int main() { return 0; }\n");
    write_file(src / L"project" / L"empty.txt", "");
    write_file(src / L"ფაილი.txt", "ქართული ტექსტი");
    std::string big(3'500'000, '\0');
    std::mt19937 rng(42);
    for (auto& c : big) c = static_cast<char>(rng());
    write_file(src / L"big.bin", big);

    kvm::log::info("1) safe_destination");
    {
        const fs::path base = L"C:\\target";
        for (const std::string_view good : {"a.txt", "dir/sub/b.bin", "ფაილი.txt", ".gitignore"}) {
            const auto d = kvm::files::safe_destination(base, good);
            check(d.has_value(), std::format("allowed: \"{}\"", good));
        }
        for (const std::string_view bad : {"", "../x", "dir/../../x", "/abs", "C:/x", "c:x", "\\\\?\\C:\\x", "a\\b",
                                           "CON", "con.txt", "dir/NUL", "COM1.log", "a:stream", "dir//x", "x.", "x ",
                                           "dir/", "./x", "a|b"}) {
            check(!kvm::files::safe_destination(base, bad).has_value(), std::format("refused: \"{}\"", bad));
        }
    }

    kvm::log::info("2-4) two FileTransfers over 127.0.0.1");
    {
        std::uint16_t port = 0;
        const SOCKET listener = listen_loopback(port);
        Inbox inbox_a, inbox_b;
        std::unique_ptr<kvm::files::FileTransfer> a;
        std::thread acceptor([&] {
            const SOCKET s = accept(listener, nullptr, nullptr);
            if (s != INVALID_SOCKET) a = std::make_unique<kvm::files::FileTransfer>(s, "A", inbox_a.callbacks(), dst_a);
        });
        std::atomic<std::uint64_t> progress_sent = 0, progress_total = 0;
        auto b_callbacks = inbox_b.callbacks();
        b_callbacks.on_send_progress = [&](std::uint64_t sent, std::uint64_t total) {
            progress_sent = sent;
            progress_total = total;
        };
        auto b = kvm::files::connect_files("127.0.0.1", port, b_callbacks, dst_b);
        acceptor.join();
        closesocket(listener);
        if (!a || !b) {
            kvm::log::error("could not connect the two sides");
            return 1;
        }

        b->send({src / L"project", src / L"big.bin", src / L"ფაილი.txt"});
        check(inbox_a.wait_for(1, 10s), "B -> A: the transfer arrived");
        const auto saved = inbox_a.transfer(0);
        check(saved == std::vector<fs::path>{dst_a / L"project", dst_a / L"big.bin", dst_a / L"ფაილი.txt"},
              "on_received lists the 3 top-level entries");
        check(same_file(src / L"project" / L"readme.md", dst_a / L"project" / L"readme.md") &&
                  same_file(src / L"project" / L"src" / L"main.cpp", dst_a / L"project" / L"src" / L"main.cpp"),
              "folder tree arrived byte for byte");
        check(fs::exists(dst_a / L"project" / L"empty.txt") && fs::file_size(dst_a / L"project" / L"empty.txt") == 0,
              "empty file arrived");
        check(same_file(src / L"big.bin", dst_a / L"big.bin"), "3.5 MB file arrived byte for byte (4 chunks)");
        check(same_file(src / L"ფაილი.txt", dst_a / L"ფაილი.txt"), "Georgian file name and content arrived");
        const std::uint64_t expected_total = 3'500'000 + 10 + 25 + 0 + std::string("ქართული ტექსტი").size();
        check(progress_total == expected_total && progress_sent == expected_total,
              std::format("send progress ended at {} of {} bytes", progress_sent.load(), progress_total.load()));

        a->send({src / L"hello.txt"});
        check(inbox_b.wait_for(1, 10s) && same_file(src / L"hello.txt", dst_b / L"hello.txt"),
              "A -> B over the same connection");

        b->send({src / L"big.bin", src / L"project"});
        check(inbox_a.wait_for(2, 10s), "second transfer arrived");
        check(same_file(src / L"big.bin", dst_a / L"big (2).bin") &&
                  same_file(src / L"project" / L"readme.md", dst_a / L"project (2)" / L"readme.md") &&
                  same_file(src / L"big.bin", dst_a / L"big.bin"),
              "existing names get \" (2)\" and nothing is overwritten");
    }

    kvm::log::info("5) a hostile peer");
    {
        std::uint16_t port = 0;
        const SOCKET listener = listen_loopback(port);
        const fs::path dst_v = root / L"dst_victim";
        Inbox inbox_v;
        std::unique_ptr<kvm::files::FileTransfer> victim;
        std::thread acceptor([&] {
            const SOCKET s = accept(listener, nullptr, nullptr);
            if (s != INVALID_SOCKET) victim = std::make_unique<kvm::files::FileTransfer>(s, "victim", inbox_v.callbacks(), dst_v);
        });
        const SOCKET raw = connect_loopback(port);
        acceptor.join();
        closesocket(listener);

        int id = 0;
        for (const std::string_view path : {"../evil1.txt", "C:/evil2.txt", "CON", "sub/../../evil4.txt",
                                            "\\\\?\\C:\\evil5.txt", "ok.txt"}) {
            ++id;
            raw_send(raw, {{"type", "file_begin"}, {"id", id}, {"path", path}, {"size", 4}});
            raw_send(raw, {{"type", "file_chunk"}, {"id", id}, {"offset", 0}}, "evil");
            raw_send(raw, {{"type", "file_end"}, {"id", id}});
        }
        raw_send(raw, {{"type", "transfer_end"}, {"count", id}});
        check(inbox_v.wait_for(1, 5s), "the hostile transfer ended");
        const auto written = files_below(root);
        const auto outside = std::count_if(written.begin(), written.end(), [&](const fs::path& p) {
            const auto s = p.native();
            return !s.starts_with((src / L"").native()) && !s.starts_with((dst_a / L"").native()) &&
                   !s.starts_with((dst_b / L"").native()) && !s.starts_with((dst_v / L"").native());
        });
        check(outside == 0 && !fs::exists(L"C:\\evil2.txt"), "nothing was written outside the target folder");
        check(files_below(dst_v) == std::vector<fs::path>{dst_v / L"ok.txt"}, "only ok.txt was saved");
        closesocket(raw);
    }

    fs::remove_all(root, ec);
    if (check.failed) {
        kvm::log::error("{} check(s) failed", check.failed);
        return 1;
    }
    kvm::log::info("all passed");
    return 0;
}
