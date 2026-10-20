// SPDX-License-Identifier: Apache-2.0

#include "kvm/identity.h"

#include <windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <exception>
#include <format>
#include <fstream>
#include <iterator>
#include <span>
#include <system_error>
#include <vector>

#include "kvm/config.h"
#include "kvm/log.h"

namespace kvm::identity {

namespace fs = std::filesystem;

namespace {

// identity.bin, once decrypted: "SRKIDv1\0", then the PKCS#8 key and the certificate, each as
// [u32 big-endian size][bytes].
constexpr char kMagic[8] = {'S', 'R', 'K', 'I', 'D', 'v', '1', '\0'};
constexpr wchar_t kDescription[] = L"Sarke device identity";
// Not a secret: it ties the blob to this purpose, so a blob made for something else won't decrypt here.
constexpr char kEntropy[] = "Sarke device identity v1";

void wipe(std::vector<std::uint8_t>& bytes) {
    if (!bytes.empty()) SecureZeroMemory(bytes.data(), bytes.size());
    bytes.clear();
}

void put_u32(std::vector<std::uint8_t>& out, std::size_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
}

std::vector<std::uint8_t> pack(const std::vector<std::uint8_t>& key, const std::vector<std::uint8_t>& cert) {
    std::vector<std::uint8_t> out(std::begin(kMagic), std::end(kMagic));
    put_u32(out, key.size());
    out.insert(out.end(), key.begin(), key.end());
    put_u32(out, cert.size());
    out.insert(out.end(), cert.begin(), cert.end());
    return out;
}

bool unpack(std::span<const std::uint8_t> in, std::vector<std::uint8_t>& key, std::vector<std::uint8_t>& cert) {
    if (in.size() < sizeof(kMagic) || !std::equal(std::begin(kMagic), std::end(kMagic), in.begin())) return false;
    std::size_t at = sizeof(kMagic);
    const auto take = [&](std::vector<std::uint8_t>& out) {
        if (in.size() - at < 4) return false;
        const std::size_t n = std::size_t{in[at]} << 24 | std::size_t{in[at + 1]} << 16 | std::size_t{in[at + 2]} << 8 |
                              std::size_t{in[at + 3]};
        at += 4;
        if (in.size() - at < n) return false;
        out.assign(in.begin() + static_cast<std::ptrdiff_t>(at), in.begin() + static_cast<std::ptrdiff_t>(at + n));
        at += n;
        return true;
    };
    return take(key) && take(cert) && at == in.size();
}

DATA_BLOB entropy_blob() {
    return {static_cast<DWORD>(sizeof(kEntropy) - 1),
            reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))};  // DPAPI doesn't write to it
}

std::optional<std::vector<std::uint8_t>> protect(std::vector<std::uint8_t>& plain) {
    DATA_BLOB in{static_cast<DWORD>(plain.size()), plain.data()};
    DATA_BLOB entropy = entropy_blob();
    DATA_BLOB out{};
    if (!CryptProtectData(&in, kDescription, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> blob(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return blob;
}

std::optional<std::vector<std::uint8_t>> unprotect(std::vector<std::uint8_t>& blob) {
    DATA_BLOB in{static_cast<DWORD>(blob.size()), blob.data()};
    DATA_BLOB entropy = entropy_blob();
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> plain(out.pbData, out.pbData + out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return plain;
}

std::optional<std::vector<std::uint8_t>> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) return std::nullopt;
    return bytes;
}

// Writes next to the target, then replaces it in one step: a crash never leaves half a file.
bool write_file(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    const fs::path temp = path.wstring() + L".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out.flush()) return false;
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ec;
        fs::remove(temp, ec);
        return false;
    }
    return true;
}

std::string utf8(const fs::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

Result create(const fs::path& dir, std::string_view name) {
    Result result;
    try {
        std::error_code ec;
        fs::create_directories(dir, ec);
        auto credentials = tls::Credentials::generate(name);
        auto key = credentials.key_der();
        auto plain = pack(key, credentials.cert_der());
        wipe(key);
        const auto blob = protect(plain);
        wipe(plain);
        if (!blob || !write_file(dir / kFileName, *blob)) {
            result.problem = Problem::CannotWrite;
            result.message = std::format("identity: can't save a new identity to {}", utf8(dir / kFileName));
            return result;
        }
        log::info("identity: created this PC's device identity {} ({})", tls::short_id(credentials.id()),
                  utf8(dir / kFileName));
        result.credentials = std::move(credentials);
        result.created = true;
    } catch (const std::exception& e) {
        result.problem = Problem::CannotWrite;
        result.message = std::format("identity: can't create a new identity: {}", e.what());
    }
    return result;
}

}  // namespace

fs::path default_dir() {
    return user_config_file().parent_path();
}

Result load_or_create(const fs::path& dir, std::string_view name) {
    const fs::path path = dir / kFileName;
    std::error_code ec;
    if (!fs::exists(path, ec) && !ec) return create(dir, name);

    Result result;
    auto blob = read_file(path);
    if (!blob) {
        result.problem = Problem::Unreadable;
        result.message = std::format("identity: can't read {}", utf8(path));
        return result;
    }
    auto plain = unprotect(*blob);
    if (!plain) {
        result.problem = Problem::CannotDecrypt;
        result.message = std::format(
            "identity: can't decrypt {} (error {}): it belongs to another Windows user or another PC, or it is "
            "damaged. Making a new identity means pairing every PC again.",
            utf8(path), GetLastError());
        return result;
    }
    std::vector<std::uint8_t> key, cert;
    const bool unpacked = unpack(*plain, key, cert);
    wipe(*plain);
    auto credentials = unpacked ? tls::Credentials::load(key, cert) : std::nullopt;
    wipe(key);
    if (!credentials) {
        result.problem = Problem::Corrupt;
        result.message = std::format("identity: {} doesn't hold a valid key and certificate", utf8(path));
        return result;
    }
    result.credentials = std::move(credentials);
    return result;
}

Result recreate(const fs::path& dir, std::string_view name) {
    const fs::path path = dir / kFileName;
    std::error_code ec;
    if (fs::exists(path, ec)) {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_s(&local, &now);
        const fs::path aside =
            dir / std::format(L"{}.broken-{:04}{:02}{:02}-{:02}{:02}{:02}", kFileName, local.tm_year + 1900,
                              local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
        fs::rename(path, aside, ec);
        if (ec) {
            Result result;
            result.problem = Problem::CannotWrite;
            result.message = std::format("identity: can't move {} aside: {}", utf8(path), ec.message());
            return result;
        }
        log::warn("identity: the old identity was moved to {}; every paired PC has to pair again", utf8(aside));
    }
    return create(dir, name);
}

}  // namespace kvm::identity
