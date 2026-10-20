// SPDX-License-Identifier: Apache-2.0

// identity_test: kvm/identity.h in a temp folder (roadmap P1.3, docs/SECURITY.md 6.1). Checks:
//   - the first load creates identity.bin; later loads give the same device ID;
//   - the file doesn't contain the private key in the clear (DPAPI);
//   - a damaged file, one that decrypts to something else, and an unreadable path are reported and
//     never replaced on their own;
//   - recreate() moves the broken file aside and makes a new identity.
// Not automated: another Windows user can't decrypt the file (run as two users by hand).
//
//   identity_test.exe   (exit code 0 = all passed)

#include <windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string_view>
#include <vector>

#include "kvm/identity.h"
#include "kvm/log.h"

namespace fs = std::filesystem;
namespace identity = kvm::identity;

namespace {

struct Checks {
    int failed = 0;
    void operator()(bool ok, std::string_view what) {
        kvm::log::info("{} {}", ok ? "[ok]  " : "[FAIL]", what);
        if (!ok) ++failed;
    }
};

std::vector<std::uint8_t> read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_bytes(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

bool contains(const std::vector<std::uint8_t>& haystack, const std::vector<std::uint8_t>& needle) {
    return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) != haystack.end();
}

// A DPAPI blob made the way identity.cpp makes them, around something that isn't an identity.
std::vector<std::uint8_t> protect_like_sarke(std::string_view plain) {
    static constexpr char kEntropy[] = "Sarke device identity v1";
    DATA_BLOB in{static_cast<DWORD>(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
    DATA_BLOB entropy{static_cast<DWORD>(sizeof(kEntropy) - 1), reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))};
    DATA_BLOB out{};
    std::vector<std::uint8_t> blob;
    if (CryptProtectData(&in, L"test", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        blob.assign(out.pbData, out.pbData + out.cbData);
        LocalFree(out.pbData);
    }
    return blob;
}

}  // namespace

int main() {
    Checks check;
    const fs::path dir = fs::temp_directory_path() / std::format(L"sarke_identity_test_{}", GetCurrentProcessId());
    fs::remove_all(dir);
    const fs::path file = dir / identity::kFileName;

    kvm::log::info("first start");
    auto first = identity::load_or_create(dir, "test-pc");
    check(first.credentials && first.created && first.problem == identity::Problem::None && fs::exists(file),
          "the first load creates identity.bin");
    if (!first.credentials) {
        kvm::log::error("{}", first.message);
        return 1;
    }
    const auto id = first.credentials->id();

    auto again = identity::load_or_create(dir, "test-pc");
    check(again.credentials && !again.created && again.credentials->id() == id,
          std::format("the next load gives the same device ID {}", kvm::tls::short_id(id)));
    const auto stored = read_bytes(file);
    check(!stored.empty() && !contains(stored, first.credentials->key_der()),
          "the private key isn't in the file in the clear");

    kvm::log::info("broken files are reported, never replaced on their own");
    auto damaged = stored;
    damaged[damaged.size() / 2] ^= 0x5A;
    write_bytes(file, damaged);
    auto broken = identity::load_or_create(dir, "test-pc");
    check(!broken.credentials && broken.problem == identity::Problem::CannotDecrypt && read_bytes(file) == damaged,
          std::format("a damaged file: CannotDecrypt, left as it was ({})", broken.message));

    write_bytes(file, protect_like_sarke("not an identity"));
    auto corrupt = identity::load_or_create(dir, "test-pc");
    check(!corrupt.credentials && corrupt.problem == identity::Problem::Corrupt,
          std::format("a file that decrypts to something else: Corrupt ({})", corrupt.message));

    const fs::path other = dir / L"unreadable";
    fs::create_directories(other / identity::kFileName);  // a folder where the file should be
    auto unreadable = identity::load_or_create(other, "test-pc");
    check(!unreadable.credentials && unreadable.problem == identity::Problem::Unreadable,
          std::format("an unreadable identity.bin: Unreadable ({})", unreadable.message));

    kvm::log::info("recreate, after the user agreed");
    write_bytes(file, damaged);
    auto fresh = identity::recreate(dir, "test-pc");
    bool moved_aside = false;
    for (const auto& entry : fs::directory_iterator(dir)) {
        moved_aside = moved_aside || entry.path().filename().wstring().starts_with(L"identity.bin.broken-");
    }
    check(fresh.credentials && fresh.created && fresh.credentials->id() != id, "a new identity with a new device ID");
    check(moved_aside, "the broken file is kept aside as identity.bin.broken-<time>");
    auto after = identity::load_or_create(dir, "test-pc");
    check(after.credentials && after.credentials->id() == fresh.credentials->id(), "and it loads from then on");

    std::error_code ec;
    fs::remove_all(dir, ec);
    if (check.failed) {
        kvm::log::error("{} check(s) failed", check.failed);
        return 1;
    }
    kvm::log::info("all passed");
    return 0;
}
