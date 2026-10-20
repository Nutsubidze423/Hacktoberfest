// SPDX-License-Identifier: Apache-2.0

// This PC's device identity for this Windows user (docs/SECURITY.md 6.1): an ECDSA P-256 key and
// its self-signed certificate (kvm/tls.h), kept in identity.bin encrypted with DPAPI for the
// current user. Other users of the PC, and copies of the file on other PCs, can't decrypt it.
//
//   auto id = kvm::identity::load_or_create(kvm::identity::default_dir(), "DESKTOP-1");
//   if (!id.credentials) { tell the user id.message; only with their agreement: recreate(...) }
//
// A file that exists but can't be read, decrypted or parsed is never replaced silently: a new key
// means every paired PC has to pair again, so the user decides (recreate()).
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "kvm/tls.h"

namespace kvm::identity {

inline constexpr std::wstring_view kFileName = L"identity.bin";

// %LOCALAPPDATA%\Sarke: per Windows user, for installed and source builds alike.
std::filesystem::path default_dir();

enum class Problem {
    None,
    Unreadable,     // the file exists but can't be opened or read
    CannotDecrypt,  // DPAPI refuses it: another user's or another PC's file, or damaged
    Corrupt,        // decrypted, but not a key and certificate that belong together
    CannotWrite,    // a new identity couldn't be saved
};

struct Result {
    std::optional<tls::Credentials> credentials;
    Problem problem = Problem::None;
    bool created = false;  // a new identity was made (no file before)
    std::string message;   // for the log / the user when credentials is empty
};

// Loads `dir`\identity.bin, or creates and saves a new identity when there is none.
// `name` goes into a new certificate's CN (shown to people only).
Result load_or_create(const std::filesystem::path& dir, std::string_view name);

// After the user agreed: moves a broken identity.bin aside (identity.bin.broken-<time>) and creates
// a new identity. Every paired PC has to pair again.
Result recreate(const std::filesystem::path& dir, std::string_view name);

}  // namespace kvm::identity
