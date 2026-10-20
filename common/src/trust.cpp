// SPDX-License-Identifier: Apache-2.0

#include "kvm/trust.h"

#include <windows.h>

#include <cstdio>
#include <string_view>

#include "kvm/identity.h"
#include "kvm/log.h"

namespace kvm::trust {

namespace {

std::string narrow(std::wstring_view w) {
    std::string out;
    out.reserve(w.size());
    for (const wchar_t c : w) out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    return out;
}

}  // namespace

int parse_arg(Options& opt, int argc, wchar_t** argv, int i) {
    const std::wstring_view arg = argv[i];
    const bool has_value = i + 1 < argc;
    if (arg == L"--data-dir" && has_value) {
        opt.data_dir = std::filesystem::path(argv[i + 1]);
        return 2;
    }
    if (arg == L"--pair-with" && has_value) {
        opt.pair_with.push_back(narrow(argv[i + 1]));
        return 2;
    }
    if (arg == L"--print-id") {
        opt.print_id = true;
        return 1;
    }
    if (arg == L"--recreate-identity") {
        opt.recreate_identity = true;
        return 1;
    }
    return 0;
}

std::optional<Context> setup(const Options& opt, std::string_view computer_name) {
    const auto dir = opt.data_dir.value_or(identity::default_dir());
    auto loaded =
        opt.recreate_identity ? identity::recreate(dir, computer_name) : identity::load_or_create(dir, computer_name);
    if (!loaded.credentials) {
        log::error("{}", loaded.message);
        if (loaded.problem != identity::Problem::CannotWrite) {
            log::error("identity: start once with --recreate-identity to make a new one (pair every PC again)");
        }
        return std::nullopt;
    }
    Context context{dir, std::move(*loaded.credentials), std::make_unique<paired::Store>(dir / L"paired.json")};
    log::info("identity: this PC is {} ({})", tls::short_id(context.me.id()), tls::to_hex(context.me.id()));
    if (opt.print_id) {
        std::printf("%s\n", tls::to_hex(context.me.id()).c_str());
        std::fflush(stdout);
    }
    for (const auto& text : opt.pair_with) {
        const auto id = tls::from_hex(text);
        if (!id) {
            log::error("trust: --pair-with needs a device ID of 64 hex digits (kvm_host --print-id), got '{}'", text);
            return std::nullopt;
        }
        if (!context.paired->find(*id)) {
            context.paired->add({.id = *id, .name = "(added with --pair-with)", .paired_at = paired::now_ms()});
            log::info("trust: paired with {} (--pair-with)", tls::short_id(*id));
        }
    }
    const auto count = context.paired->all().size();
    log::info("trust: {} paired PC(s)", count);
    return context;
}

bool admit(paired::Store& paired, const tls::Stream& stream, std::string_view peer) {
    paired.reload();
    if (paired.find(stream.peer_id())) {
        paired.touch(stream.peer_id());
        return true;
    }
    log::warn("trust: {} has the key {} ('{}'), which isn't paired with this PC - connection closed", peer,
              tls::short_id(stream.peer_id()), stream.peer_name());
    return false;
}

}  // namespace kvm::trust
