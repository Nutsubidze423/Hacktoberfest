// SPDX-License-Identifier: Apache-2.0

// paired_test: kvm/paired.h in a temp folder (roadmap P1.4, docs/SECURITY.md 6.6). Checks:
//   - add / find / all / set_auto_connect / touch / remove, and that each one survives a reload;
//   - a second Store (another process) sees changes after reload();
//   - an unparseable file means no PC is paired (fails closed), and the next save keeps it aside;
//   - entries without a valid device_id are skipped;
//   - two Stores changing one file (kvm_host pairing, the launcher setting Auto) lose nothing, also
//     when 8 threads add at once.
//
//   paired_test.exe   (exit code 0 = all passed)

#include <windows.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <string_view>
#include <thread>
#include <vector>

#include "kvm/log.h"
#include "kvm/paired.h"

namespace fs = std::filesystem;
namespace paired = kvm::paired;

namespace {

struct Checks {
    int failed = 0;
    void operator()(bool ok, std::string_view what) {
        kvm::log::info("{} {}", ok ? "[ok]  " : "[FAIL]", what);
        if (!ok) ++failed;
    }
};

kvm::tls::DeviceId make_id(std::uint8_t seed) {
    kvm::tls::DeviceId id{};
    for (std::size_t i = 0; i < id.size(); ++i) id[i] = static_cast<std::uint8_t>(seed * 31 + i);
    return id;
}

void write_text(const fs::path& path, std::string_view text) {
    std::ofstream(path, std::ios::binary) << text;
}

}  // namespace

int main() {
    Checks check;
    const fs::path dir = fs::temp_directory_path() / std::format(L"sarke_paired_test_{}", GetCurrentProcessId());
    fs::remove_all(dir);
    const fs::path file = dir / L"paired.json";

    paired::Store store(file);
    check(store.all().empty() && store.load_error().empty(), "no file: no paired PCs, no error");

    const auto alice = make_id(1), bob = make_id(2);
    check(store.add({.id = alice, .name = "alice-pc", .paired_at = paired::now_ms()}) && fs::exists(file),
          "pairing saves paired.json");
    check(store.add({.id = bob, .name = "bob-pc", .paired_at = 1}) && store.all().size() == 2, "a second PC");
    check(store.add({.id = bob, .name = "bob-renamed", .paired_at = 2}) && store.all().size() == 2 &&
              store.find(bob)->name == "bob-renamed",
          "pairing again replaces, never duplicates");
    check(store.set_auto_connect(alice, true) && store.touch(alice, 1234), "auto-connect and last seen");

    paired::Store other(file);  // another process
    const auto a = other.find(alice);
    check(a && a->name == "alice-pc" && a->auto_connect && a->last_seen == 1234, "another Store reads it back exactly");

    check(store.remove(bob) && !store.find(bob) && !store.remove(bob), "unpair removes it (once)");
    check(other.find(bob).has_value(), "the other process still has it in memory...");
    other.reload();
    check(!other.find(bob) && other.find(alice), "...until it reloads (do it before every trust decision)");

    kvm::log::info("broken files fail closed");
    write_text(file, "{ not json");
    paired::Store broken(file);
    check(broken.all().empty() && !broken.load_error().empty(), "an unparseable file: no PC is paired");
    check(!broken.find(alice), "so nobody is trusted");
    check(broken.add({.id = bob, .name = "bob-pc"}), "pairing still works");
    bool kept = false;
    for (const auto& entry : fs::directory_iterator(dir)) {
        kept = kept || entry.path().filename().wstring().starts_with(L"paired.json.broken-");
    }
    check(kept && broken.load_error().empty(), "and the unreadable file was kept aside, not overwritten");

    write_text(file, std::format(
                         R"([{{"device_id":"nothex","name":"x"}},{{"name":"no id"}},{{"device_id":"{}","name":"ok"}}])",
                         kvm::tls::to_hex(alice)));
    paired::Store partial(file);
    check(partial.all().size() == 1 && partial.find(alice), "entries without a valid device_id are skipped");

    kvm::log::info("two processes change the same file");
    {
        const fs::path shared = dir / L"shared.json";
        paired::Store host(shared), launcher(shared);  // both loaded while there is no file
        check(host.add({.id = alice, .name = "alice-pc"}), "kvm_host pairs alice-pc");
        check(launcher.add({.id = bob, .name = "bob-pc"}) && launcher.set_auto_connect(alice, true),
              "the launcher, without a reload, pairs bob-pc and switches Auto on for alice-pc");
        check(host.touch(alice, 77), "kvm_host stamps alice-pc's last seen");
        paired::Store reader(shared);
        const auto merged = reader.find(alice);
        check(merged && merged->auto_connect && merged->last_seen == 77 && reader.find(bob),
              "nothing is lost: both PCs, alice-pc's Auto and its last seen");

        const fs::path busy = dir / L"busy.json";
        std::vector<std::thread> writers;
        for (std::uint8_t i = 0; i < 8; ++i) {
            writers.emplace_back([&busy, i] {
                paired::Store own(busy);
                own.add({.id = make_id(static_cast<std::uint8_t>(100 + i)), .name = std::format("pc-{}", i)});
            });
        }
        for (auto& t : writers) t.join();
        check(paired::Store(busy).all().size() == 8, "8 Stores adding at once: all 8 PCs are saved");
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
    if (check.failed) {
        kvm::log::error("{} check(s) failed", check.failed);
        return 1;
    }
    kvm::log::info("all passed");
    return 0;
}
