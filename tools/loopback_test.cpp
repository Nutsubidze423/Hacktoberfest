// SPDX-License-Identifier: Apache-2.0

// loopback_test: the two-PC paths on one PC, with the real programs. Starts kvm_host and kvm_client
// on free ports, each with its own config.json in a temp folder, without hooks, clipboard, desk mode
// or discovery (nothing on this desktop or network changes), and checks:
//   1. a peer speaking another protocol version is dropped, and the host logs why;
//   2. a peer speaking this version gets the host's hello back;
//   3. kvm_client connects (control, video and file channels) and both sides log the link (rtt);
//   4. Ctrl+Break, the way the launcher stops them, ends both normally.
// Phase 1 (pairing + TLS) extends it: an unpaired peer must be refused before anything is parsed.
//
//   loopback_test.exe            (exit code 0 = all passed; ctest label "loopback")
//   loopback_test.exe --keep     keep the temp folder with both logs

#include <winsock2.h>
#include <windows.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include "kvm/log.h"
#include "kvm/net.h"
#include "kvm/protocol.h"

namespace fs = std::filesystem;
namespace proto = kvm::proto;
using namespace std::chrono_literals;

namespace {

struct Checks {
    int failed = 0;
    void operator()(bool ok, std::string_view what) {
        kvm::log::info("{} {}", ok ? "[ok]  " : "[FAIL]", what);
        if (!ok) ++failed;
    }
};

fs::path exe_dir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return fs::path(path).parent_path();
}

// A port nothing listens on right now (the OS picks it), so an installed Sarke on 50500-50502 is
// never in the way.
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

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);  // MSVC opens it shared: the program keeps writing
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

bool wait_until(const std::function<bool()>& done, std::chrono::milliseconds timeout) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
        if (done()) return true;
        std::this_thread::sleep_for(100ms);
    }
    return done();
}

std::size_t count(const std::string& text, std::string_view what) {
    std::size_t n = 0;
    for (auto at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size())) ++n;
    return n;
}

// One of the programs under test. stdout and stderr go to its log file; it runs in its own process
// group, so Ctrl+Break reaches it alone. The job ends it if this test dies first.
class Program {
public:
    Program(HANDLE job, const fs::path& exe, const std::wstring& args, fs::path log) : log_(std::move(log)) {
        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        HANDLE out = CreateFileW(log_.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
        STARTUPINFOW si{sizeof(si)};
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = in;
        si.hStdOutput = out;
        si.hStdError = out;
        std::wstring cmd = L"\"" + exe.wstring() + L"\" " + args;
        PROCESS_INFORMATION pi{};
        if (out != INVALID_HANDLE_VALUE && in != INVALID_HANDLE_VALUE &&
            CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NEW_PROCESS_GROUP, nullptr,
                           exe.parent_path().c_str(), &si, &pi)) {
            AssignProcessToJobObject(job, pi.hProcess);
            CloseHandle(pi.hThread);
            process_ = pi.hProcess;
            pid_ = pi.dwProcessId;
        }
        if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
        if (in != INVALID_HANDLE_VALUE) CloseHandle(in);
    }
    ~Program() {
        if (!process_) return;
        if (WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) TerminateProcess(process_, 1);
        CloseHandle(process_);
    }
    Program(const Program&) = delete;
    Program& operator=(const Program&) = delete;

    bool started() const { return process_ != nullptr; }
    std::string log() const { return read_file(log_); }
    bool log_has(std::string_view what) const { return log().find(what) != std::string::npos; }

    // Ctrl+Break, like the launcher's stop: the program's console handler shuts it down normally.
    // Returns its exit code, or nullopt if it is still running after `timeout`.
    std::optional<DWORD> stop(std::chrono::milliseconds timeout) {
        if (!process_ || !GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid_)) return std::nullopt;
        if (WaitForSingleObject(process_, static_cast<DWORD>(timeout.count())) != WAIT_OBJECT_0) return std::nullopt;
        DWORD code = 0;
        GetExitCodeProcess(process_, &code);
        return code;
    }

private:
    fs::path log_;
    HANDLE process_ = nullptr;
    DWORD pid_ = 0;
};

// The first hello in `conn`'s messages within `timeout` (other messages are skipped).
std::optional<proto::Hello> wait_for_hello(kvm::net::Connection& conn, std::chrono::milliseconds timeout) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now());
        auto msg = conn.incoming().pop_for(left);
        if (!msg) return std::nullopt;
        if (const auto* hello = std::get_if<proto::Hello>(&*msg)) return *hello;
    }
    return std::nullopt;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const bool keep = argc > 1 && std::wstring_view(argv[1]) == L"--keep";
    Checks check;

    // Ctrl+Break needs a console shared with the programs: get one (hidden) if ctest gave none.
    if (GetConsoleCP() == 0 && AllocConsole()) {
        if (const HWND console = GetConsoleWindow()) ShowWindow(console, SW_HIDE);
    }

    const fs::path bin = exe_dir().parent_path();  // build\<preset>
    const fs::path host_exe = bin / L"host_a" / L"kvm_host.exe";
    const fs::path client_exe = bin / L"client_b" / L"kvm_client.exe";
    if (!fs::exists(host_exe) || !fs::exists(client_exe)) {
        kvm::log::error("kvm_host.exe / kvm_client.exe not found next to {}", bin.string());
        return 1;
    }

    const fs::path dir = fs::temp_directory_path() / std::format(L"sarke_loopback_test_{}", GetCurrentProcessId());
    fs::create_directories(dir);
    const std::uint16_t control = free_port(), video = free_port(), files = free_port();
    check(control && video && files && control != video && video != files && control != files,
          std::format("free ports {} / {} / {}", control, video, files));
    const std::string ports =
        std::format(R"("control_port": {}, "video_port": {}, "file_port": {})", control, video, files);
    std::ofstream(dir / "host.json") << "{" << ports << "}\n";
    std::ofstream(dir / "client.json") << "{" << ports << R"(, "host_ip": "127.0.0.1"})" << "\n";

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

    {
        const std::wstring quiet = L" --no-kvm --no-clipboard --no-desk";
        Program host(job, host_exe, L"--config \"" + (dir / "host.json").wstring() + L"\" --no-discovery" + quiet,
                     dir / "host.log");
        check(host.started() && wait_until([&] { return host.log_has("listening on ports"); }, 15s),
              "kvm_host starts and listens");

        kvm::log::info("1) a peer speaking protocol v{}", proto::kProtocolVersion + 1);
        {
            auto peer = kvm::net::connect("127.0.0.1", control, 3s);
            check(peer != nullptr, "it connects");
            if (peer) {
                peer->send(proto::Hello{.protocol_version = proto::kProtocolVersion + 1, .hostname = "loopback-old"});
                check(wait_until([&] { return !peer->is_open(); }, 6s), "the host drops it");
                check(wait_until([&] { return host.log_has("but this host speaks v"); }, 3s),
                      "the host logs the version mismatch");
            }
        }

        kvm::log::info("2) a peer speaking protocol v{}", proto::kProtocolVersion);
        {
            auto peer = kvm::net::connect("127.0.0.1", control, 3s);
            check(peer != nullptr, "it connects");
            if (peer) {
                peer->send(proto::Hello{.screen_w = 1920, .screen_h = 1080, .hostname = "loopback-peer"});
                const auto hello = wait_for_hello(*peer, 5s);
                check(hello && hello->protocol_version == proto::kProtocolVersion,
                      std::format("the host answers hello v{}", hello ? hello->protocol_version : 0));
                peer->close();
                check(wait_until([&] { return host.log_has("'loopback-peer' session ended"); }, 5s),
                      "the host ends that session when the peer leaves");
            }
        }

        kvm::log::info("3) kvm_client");
        {
            Program client(job, client_exe,
                           L"--config \"" + (dir / "client.json").wstring() + L"\" --host 127.0.0.1" + quiet,
                           dir / "client.log");
            check(client.started() && wait_until([&] { return client.log_has("connected to host"); }, 15s),
                  "the client connects");
            check(wait_until([&] { return client.log_has("video channel connected"); }, 5s), "video channel");
            check(wait_until([&] { return client.log_has("file channel connected"); }, 5s), "file channel");
            check(wait_until([&] { return count(host.log(), " connected from ") >= 2; }, 5s),
                  "the host takes the client");
            // Both sides log the link every 5 s once the first ping/pong is through.
            check(wait_until([&] { return client.log_has("link: rtt") && host.log_has("link: rtt"); }, 15s),
                  "both sides log the link (rtt)");

            kvm::log::info("4) Ctrl+Break, the way the launcher stops them");
            const auto client_code = client.stop(10s);
            check(client_code == 0u, std::format("the client stops with exit code 0 (got {})",
                                                 client_code ? static_cast<long>(*client_code) : -1L));
        }
        const auto host_code = host.stop(10s);
        check(host_code == 0u,
              std::format("the host stops with exit code 0 (got {})", host_code ? static_cast<long>(*host_code) : -1L));
    }
    CloseHandle(job);

    if (check.failed || keep) {
        kvm::log::info("logs: {}", dir.string());
    } else {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    if (check.failed) {
        kvm::log::error("{} check(s) failed", check.failed);
        return 1;
    }
    kvm::log::info("all passed");
    return 0;
}
