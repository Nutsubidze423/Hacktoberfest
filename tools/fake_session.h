// SPDX-License-Identifier: Apache-2.0

// fake_streamer and viewer_test --connect on the same PC. The video channel is TLS and belongs to a
// control session (docs/SECURITY.md 6.4), which these two don't have, so they share a stand-in:
// one identity in %TEMP%\sarke_fake_streamer (never this user's real one) and the session token,
// which fake_streamer writes there when it starts and viewer_test reads before each connect.
#pragma once

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

#include "kvm/identity.h"
#include "kvm/log.h"
#include "kvm/tls.h"

namespace kvm::fake {

inline std::filesystem::path session_dir() {
    return std::filesystem::temp_directory_path() / L"sarke_fake_streamer";
}

inline std::optional<tls::Credentials> session_credentials() {
    auto result = identity::load_or_create(session_dir(), "fake-streamer");
    if (!result.credentials) log::error("fake session identity: {}", result.message);
    return std::move(result.credentials);
}

inline bool write_session_token(std::string_view token) {
    std::ofstream out(session_dir() / L"session_token.txt", std::ios::trunc);
    out << token;
    return static_cast<bool>(out);
}

inline std::optional<std::string> read_session_token() {
    std::ifstream in(session_dir() / L"session_token.txt");
    std::string token;
    if (!(in >> token)) return std::nullopt;
    return token;
}

}  // namespace kvm::fake
