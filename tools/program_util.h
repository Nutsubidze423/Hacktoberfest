// SPDX-License-Identifier: Apache-2.0

// Tests that run the real programs (loopback label): start kvm_host / kvm_client with a log file and
// a stdin pipe, wait for log lines, free ports, read the device IDs they print.
//
//   using namespace kvm::test;
//   Program host(job, exe_dir().parent_path() / L"host_a" / L"kvm_host.exe", args, dir / "host.log");
//   wait_until([&] { return host.log_has("listening on ports"); }, 15s);
#pragma once

#include <winsock2.h>
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

#include "kvm/log.h"
#include "kvm/net.h"
#include "kvm/tls.h"

namespace kvm::test {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

struct Checks {
    int failed = 0;
    void operator()(bool ok, std::string_view what) {
        kvm::log::info("{} {}", ok ? "[ok]  " : "[FAIL]", what);
        if (!ok) ++failed;
    }
};

inline fs::path exe_dir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return fs::path(path).parent_path();
}

// A port nothing listens on right now (the OS picks it), so an installed Sarke on 50500-50502 is
// never in the way.
inline std::uint16_t free_port() {
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

inline std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);  // MSVC opens it shared: the program keeps writing
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

inline bool wait_until(const std::function<bool()>& done, std::chrono::milliseconds timeout) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
        if (done()) return true;
        std::this_thread::sleep_for(100ms);
    }
    return done();
}

inline std::size_t count(const std::string& text, std::string_view what) {
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
        // stdin is a pipe, as when the launcher starts the app: send_line() writes launcher commands.
        HANDLE in = INVALID_HANDLE_VALUE;
        if (!CreatePipe(&in, &stdin_write_, &inherit, 0)) in = INVALID_HANDLE_VALUE;
        if (stdin_write_) SetHandleInformation(stdin_write_, HANDLE_FLAG_INHERIT, 0);
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
        if (stdin_write_) CloseHandle(stdin_write_);
        if (!process_) return;
        if (WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) TerminateProcess(process_, 1);
        CloseHandle(process_);
    }

    // One launcher command (kvm/launcher_commands.h), e.g. "pair yes".
    bool send_line(std::string_view line) {
        const std::string text = std::string(line) + '\n';
        DWORD written = 0;
        return stdin_write_ && WriteFile(stdin_write_, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    }
    Program(const Program&) = delete;
    Program& operator=(const Program&) = delete;

    bool started() const { return process_ != nullptr; }
    std::string log() const { return read_file(log_); }
    bool log_has(std::string_view what) const { return log().find(what) != std::string::npos; }

    // For a program that ends by itself (--print-id): its exit code, or nullopt after `timeout`.
    std::optional<DWORD> wait_exit(std::chrono::milliseconds timeout) {
        if (!process_ || WaitForSingleObject(process_, static_cast<DWORD>(timeout.count())) != WAIT_OBJECT_0) {
            return std::nullopt;
        }
        DWORD code = 0;
        GetExitCodeProcess(process_, &code);
        return code;
    }

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
    HANDLE stdin_write_ = nullptr;
    HANDLE process_ = nullptr;
    DWORD pid_ = 0;
};

// The device ID a program logged as "identity: this PC is XXXX-XXXX-XXXX-XXXX (<64 hex>)".
inline std::optional<kvm::tls::DeviceId> device_id_in(const std::string& log) {
    const auto at = log.find("identity: this PC is ");
    const auto open = at == std::string::npos ? std::string::npos : log.find('(', at);
    if (open == std::string::npos || open + 65 > log.size()) return std::nullopt;
    return kvm::tls::from_hex(std::string_view(log).substr(open + 1, 64));
}

inline std::wstring hex(const kvm::tls::DeviceId& id) {
    const std::string text = kvm::tls::to_hex(id);
    return std::wstring(text.begin(), text.end());
}

}  // namespace kvm::test
