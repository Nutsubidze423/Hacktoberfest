// SPDX-License-Identifier: Apache-2.0

#include "kvm/paired.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <cwctype>
#include <format>
#include <fstream>
#include <system_error>
#include <utility>

#include "kvm/identity.h"
#include "kvm/log.h"

namespace kvm::paired {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::string utf8(const fs::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

json to_json(const Device& d) {
    return {{"device_id", tls::to_hex(d.id)}, {"name", d.name},
            {"paired_at", d.paired_at},       {"last_seen", d.last_seen},
            {"auto_connect", d.auto_connect}, {"permissions", d.permissions}};
}

std::optional<Device> from_json(const json& j) {
    if (!j.is_object() || !j.contains("device_id") || !j["device_id"].is_string()) return std::nullopt;
    const auto id = tls::from_hex(j["device_id"].get<std::string>());
    if (!id) return std::nullopt;
    Device d;
    d.id = *id;
    d.name = j.value("name", std::string());
    d.paired_at = j.value("paired_at", std::int64_t{0});
    d.last_seen = j.value("last_seen", std::int64_t{0});
    d.auto_connect = j.value("auto_connect", false);
    if (j.contains("permissions") && j["permissions"].is_object()) d.permissions = j["permissions"];
    return d;
}

// The named mutex every process takes around read-modify-save of one paired.json. kvm_host,
// kvm_client and the launcher run in the same session, so a Local\ name is enough.
class FileLock {
public:
    explicit FileLock(const fs::path& file) {
        // FNV-1a of the lower-case path: a mutex name can't hold the path itself (no backslashes).
        std::wstring key = file.lexically_normal().wstring();
        std::transform(key.begin(), key.end(), key.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        std::uint64_t hash = 14695981039346656037ull;
        for (const wchar_t c : key) {
            hash = (hash ^ static_cast<std::uint64_t>(c)) * 1099511628211ull;
        }
        const std::wstring name = std::format(L"Local\\Sarke-paired-{:016x}", hash);
        mutex_ = CreateMutexW(nullptr, FALSE, name.c_str());
        if (!mutex_) {
            log::warn("paired: no lock for {} ({}); saving without it", utf8(file), GetLastError());
            return;
        }
        // WAIT_ABANDONED: a process died holding it; the file is still whole (atomic saves).
        const DWORD got = WaitForSingleObject(mutex_, 5000);
        owned_ = got == WAIT_OBJECT_0 || got == WAIT_ABANDONED;
        if (!owned_) log::warn("paired: {} stayed locked for 5 s; saving without the lock", utf8(file));
    }
    ~FileLock() {
        if (owned_) ReleaseMutex(mutex_);
        if (mutex_) CloseHandle(mutex_);
    }
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;

private:
    HANDLE mutex_ = nullptr;
    bool owned_ = false;
};

std::string timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    return std::format("{:04}{:02}{:02}-{:02}{:02}{:02}", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                       local.tm_hour, local.tm_min, local.tm_sec);
}

}  // namespace

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

fs::path Store::default_file() {
    return identity::default_dir() / L"paired.json";
}

Store::Store(fs::path file) : file_(std::move(file)) {
    std::lock_guard lock(mutex_);
    const FileLock file_lock(file_);  // a reader holding the file open makes another process's save fail
    load_locked();
}

void Store::reload() {
    std::lock_guard lock(mutex_);
    const FileLock file_lock(file_);
    load_locked();
}

std::string Store::load_error() const {
    std::lock_guard lock(mutex_);
    return load_error_;
}

void Store::load_locked() {
    devices_.clear();
    const std::string before = std::exchange(load_error_, std::string());  // log a broken file once, not every reload
    keep_broken_ = false;
    std::error_code ec;
    if (!fs::exists(file_, ec)) return;
    std::ifstream in(file_, std::ios::binary);
    const json j = json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_array()) {
        load_error_ =
            std::format("paired: {} doesn't parse; no PC is paired until it is fixed or replaced", utf8(file_));
        keep_broken_ = true;
        if (load_error_ != before) log::error("{}", load_error_);
        return;
    }
    for (const auto& entry : j) {
        if (auto d = from_json(entry)) {
            std::erase_if(devices_, [&](const Device& o) { return o.id == d->id; });
            devices_.push_back(std::move(*d));
        } else {
            log::warn("paired: skipping an entry without a valid device_id in {}", utf8(file_));
        }
    }
}

bool Store::save_locked() {
    std::error_code ec;
    fs::create_directories(file_.parent_path(), ec);
    if (keep_broken_ && fs::exists(file_, ec)) {
        const std::string stamp = timestamp();
        const fs::path aside = file_.wstring() + L".broken-" + std::wstring(stamp.begin(), stamp.end());
        fs::rename(file_, aside, ec);
        if (ec) {
            log::error("paired: can't move the unreadable {} aside: {}", utf8(file_), ec.message());
            return false;
        }
        log::warn("paired: the unreadable file was kept as {}", utf8(aside));
        keep_broken_ = false;
    }
    json j = json::array();
    for (const auto& d : devices_) j.push_back(to_json(d));
    const fs::path temp = file_.wstring() + L".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << j.dump(2) << '\n';
        if (!out.flush()) return false;
    }
    // Another program (an antivirus, the search indexer, an editor) may have the file open for a
    // moment: the replace fails with access denied or a sharing violation. Try again for about 1 s.
    DWORD error = ERROR_SUCCESS;
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (MoveFileExW(temp.c_str(), file_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            error = ERROR_SUCCESS;
            break;
        }
        error = GetLastError();
        if (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION) break;
        Sleep(50);
    }
    if (error != ERROR_SUCCESS) {
        fs::remove(temp, ec);
        log::error("paired: can't save {} (error {})", utf8(file_), error);
        return false;
    }
    load_error_.clear();
    return true;
}

std::optional<Device> Store::find(const tls::DeviceId& id) const {
    std::lock_guard lock(mutex_);
    const auto it = std::find_if(devices_.begin(), devices_.end(), [&](const Device& d) { return d.id == id; });
    if (it == devices_.end()) return std::nullopt;
    return *it;
}

std::vector<Device> Store::all() const {
    std::lock_guard lock(mutex_);
    return devices_;
}

template <class Change>
bool Store::modify(Change&& change) {
    std::lock_guard lock(mutex_);
    const FileLock file_lock(file_);
    load_locked();  // another process may have changed it since
    if (!change(devices_)) return false;
    return save_locked();
}

bool Store::add(Device device) {
    return modify([&](std::vector<Device>& devices) {
        std::erase_if(devices, [&](const Device& d) { return d.id == device.id; });
        devices.push_back(std::move(device));
        return true;
    });
}

bool Store::remove(const tls::DeviceId& id) {
    return modify([&](std::vector<Device>& devices) {
        return std::erase_if(devices, [&](const Device& d) { return d.id == id; }) > 0;
    });
}

bool Store::set_auto_connect(const tls::DeviceId& id, bool on) {
    return modify([&](std::vector<Device>& devices) {
        const auto it = std::find_if(devices.begin(), devices.end(), [&](const Device& d) { return d.id == id; });
        if (it == devices.end()) return false;
        it->auto_connect = on;
        return true;
    });
}

bool Store::touch(const tls::DeviceId& id, std::int64_t when) {
    return modify([&](std::vector<Device>& devices) {
        const auto it = std::find_if(devices.begin(), devices.end(), [&](const Device& d) { return d.id == id; });
        if (it == devices.end()) return false;
        it->last_seen = when;
        return true;
    });
}

}  // namespace kvm::paired
