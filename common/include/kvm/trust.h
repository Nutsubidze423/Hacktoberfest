// SPDX-License-Identifier: Apache-2.0

// Who kvm_host and kvm_client trust (docs/SECURITY.md 6.4): this PC's identity (kvm/identity.h),
// the PCs it is paired with (kvm/paired.h), and the check every TLS peer has to pass before a single
// application byte of it is read.
//
//   kvm::trust::Options opt;                                  // fill from the command line:
//   if (int n = kvm::trust::parse_arg(opt, argc, argv, i)) { i += n - 1; continue; }
//   auto trust = kvm::trust::setup(opt, computer_name);       // nullopt: the reason is logged
//   ... auto pending = server.accept(trust->me);
//   if (!kvm::trust::admit(*trust->paired, *pending->stream, pending->peer)) continue;  // closed
//
// Command line (both programs):
//   --data-dir <dir>      where identity.bin and paired.json live (default %LOCALAPPDATA%\Sarke)
//   --print-id            print this PC's device ID (64 hex digits) and exit
//   --pair-with <id>      trust the PC with this device ID (64 hex digits). For development and tests
//                         until pairing by code (roadmap P1.6) exists; repeatable.
//   --recreate-identity   replace an identity that can't be read (every paired PC must pair again)
#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kvm/paired.h"
#include "kvm/tls.h"

namespace kvm::trust {

struct Options {
    std::optional<std::filesystem::path> data_dir;
    std::vector<std::string> pair_with;
    bool print_id = false;
    bool recreate_identity = false;
};

// If argv[i] is one of the options above, stores it and returns how many arguments it used
// (1 or 2); otherwise 0.
int parse_arg(Options& opt, int argc, wchar_t** argv, int i);

struct Context {
    std::filesystem::path dir;
    tls::Credentials me;
    std::unique_ptr<paired::Store> paired;
};

// Loads (or creates) this PC's identity and its paired PCs, applies --pair-with and logs this PC's
// device ID. nullopt if the identity can't be used (the reason is logged). With --print-id the ID
// also goes to stdout.
std::optional<Context> setup(const Options& opt, std::string_view computer_name);

// The pinning check (SECURITY.md S1, S3): is the peer behind `stream` paired? Reloads the store
// first (the launcher may have paired or unpaired meanwhile). Logs a refusal; marks the PC as seen.
bool admit(paired::Store& paired, const tls::Stream& stream, std::string_view peer);

}  // namespace kvm::trust
