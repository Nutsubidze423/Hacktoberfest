// SPDX-License-Identifier: Apache-2.0

// The PCs this one is paired with (docs/SECURITY.md 6.6): paired.json next to identity.bin,
//   [{"device_id": "<64 hex>", "name": "...", "paired_at": ms, "last_seen": ms,
//     "auto_connect": false, "permissions": {}}]
// It holds public data only, so it isn't encrypted. Anyone who can write it already runs as this
// user.
//
// Fails closed: a file that doesn't parse counts as "no paired PCs" (nobody is trusted). It is kept
// as paired.json.broken-<time> by the next save, never silently overwritten.
//
// Several processes use the file (kvm_host and kvm_client pin against it, the launcher pairs and
// unpairs): reload() before a decision that must see the others' changes. Saves are atomic (a temp
// file, then a replace). The methods are thread-safe.
#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "kvm/tls.h"

namespace kvm::paired {

struct Device {
    tls::DeviceId id{};
    std::string name;                                     // shown only; the ID is what counts
    std::int64_t paired_at = 0;                           // ms since 1970 (UTC)
    std::int64_t last_seen = 0;                           // ms since 1970 (UTC); 0 = never since pairing
    bool auto_connect = false;                            // connect by itself when discovery sees it
    nlohmann::json permissions = nlohmann::json::object();  // filled in roadmap Phase 4
};

std::int64_t now_ms();  // wall clock, ms since 1970 (UTC)

class Store {
public:
    // Loads `file` (a missing file is an empty store).
    explicit Store(std::filesystem::path file);
    // identity::default_dir()\paired.json
    static std::filesystem::path default_file();

    // Reads the file again (another process may have paired or unpaired).
    void reload();
    // Empty when the last load was fine; otherwise why the store is empty.
    std::string load_error() const;

    std::optional<Device> find(const tls::DeviceId& id) const;
    std::vector<Device> all() const;

    // Adds the device, or replaces the one with the same ID. Saves. False if saving failed.
    bool add(Device device);
    // Unpairs. Saves. False if it wasn't paired or saving failed.
    bool remove(const tls::DeviceId& id);
    bool set_auto_connect(const tls::DeviceId& id, bool on);
    bool touch(const tls::DeviceId& id, std::int64_t when = now_ms());

private:
    void load_locked();
    bool save_locked();

    const std::filesystem::path file_;
    mutable std::mutex mutex_;
    std::vector<Device> devices_;
    std::string load_error_;
    bool keep_broken_ = false;  // the file on disk didn't parse: move it aside before writing
};

}  // namespace kvm::paired
