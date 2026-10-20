// SPDX-License-Identifier: Apache-2.0

// layout_sync_test: where the other PC stands stays the same on both PCs (kvm/layout_sync.h), per
// peer (config.json "peers"), and the layout / identify messages survive the wire.
//
//   layout_sync_test.exe   (exit code 0 = all passed)

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "kvm/config.h"
#include "kvm/layout_sync.h"
#include "kvm/protocol.h"

namespace {

int g_failed = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what.c_str());
    if (!ok) ++g_failed;
}

using kvm::display::Side;
using kvm::layout_sync::View;

}  // namespace

int main() {
    namespace ls = kvm::layout_sync;
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // every line out even if a check crashes

    std::printf("-- the view for a peer\n");
    kvm::Config cfg;
    cfg.client_side = "left";  // the host's default: the client stands on its left
    cfg.peers["tabuladze"] = {.side = "right", .set_at = 1000};
    check(ls::for_peer(cfg, "tabuladze", true).side == Side::Right && ls::for_peer(cfg, "tabuladze", true).set_at == 1000,
          "a saved peer keeps its own place");
    check(ls::for_peer(cfg, "tabuladze", false).side == Side::Right, "...whichever role this PC has now");
    check(ls::for_peer(cfg, "new-pc", true).side == Side::Left && ls::for_peer(cfg, "new-pc", true).set_at == 0,
          "an unknown peer: host default = client_side");
    check(ls::for_peer(cfg, "new-pc", false).side == Side::Right, "an unknown peer: client default = the opposite");

    std::printf("\n-- both PCs agree\n");
    // Luka (host) set "tabuladze is on my right" at 2000; tabuladze (client) still has its default.
    const View luka{Side::Right, 2000};
    const View temo{Side::Right, 0};  // wrong: it thinks Luka is on its right too
    const auto to_temo = ls::to_message(luka);
    check(to_temo.you_are == "right" && to_temo.set_at == 2000, "the message says where the receiver stands");
    const auto at_temo = ls::resolve(temo, to_temo, /*this_is_host=*/false);
    check(at_temo.adopted && at_temo.view.side == Side::Left && at_temo.view.set_at == 2000,
          "the newer view wins: Luka is on tabuladze's left");
    const auto at_luka = ls::resolve(luka, ls::to_message(temo), /*this_is_host=*/true);
    check(!at_luka.adopted && at_luka.view.side == Side::Right, "the older view doesn't change the newer one");
    check(ls::resolve(at_temo.view, ls::to_message(luka), false).adopted == false, "agreeing views change nothing");

    // Later tabuladze's user moves Luka to the top: that is newer, Luka follows (tabuladze is below).
    const View temo_moved{Side::Top, 3000};
    const auto moved = ls::resolve(luka, ls::to_message(temo_moved), true);
    check(moved.adopted && moved.view.side == Side::Bottom && moved.view.set_at == 3000, "a later change on the other PC wins");

    // Two defaults: the host's wins, the client takes the opposite.
    const View host_default{Side::Left, 0}, client_default{Side::Left, 0};
    check(!ls::resolve(host_default, ls::to_message(client_default), true).adopted, "tie: the host keeps its view");
    const auto tie = ls::resolve(client_default, ls::to_message(host_default), false);
    check(tie.adopted && tie.view.side == Side::Right, "tie: the client takes the host's, mirrored");

    check(!ls::resolve(luka, {.you_are = "sideways", .set_at = 9999}, false).adopted, "a malformed side is ignored");
    check(!ls::resolve(luka, {.you_are = "left", .set_at = -5}, false).adopted, "a negative time is ignored");

    std::printf("\n-- the messages\n");
    const kvm::proto::Message layout = kvm::proto::Layout{.you_are = "bottom", .set_at = 123456789012};
    const auto back = kvm::proto::decode(kvm::proto::encode(layout));
    const auto* l = back ? std::get_if<kvm::proto::Layout>(&*back) : nullptr;
    check(l && l->you_are == "bottom" && l->set_at == 123456789012, "layout round-trips");
    const kvm::proto::Message identify = kvm::proto::Identify{};
    const auto json = kvm::proto::encode(identify);
    const auto id_back = kvm::proto::decode(json);
    check(json.value("type", "") == "identify" && id_back && std::holds_alternative<kvm::proto::Identify>(*id_back),
          "identify round-trips: " + json.dump());

    std::printf("\n-- saved per peer in config.json\n");
    const auto dir = std::filesystem::temp_directory_path() / L"kvm_layout_sync_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto file = dir / L"config.json";
    std::ofstream(file) << R"({"host_ip": "auto", "client_side": "left", "jpeg_quality": 80, "custom": 7})";
    check(kvm::save_peer_layout(file, "tabuladze", {.side = "right", .set_at = 2000}), "saved");
    check(kvm::save_peer_layout(file, "DESKTOP-7Q0UR7J", {.side = "left", .set_at = 2500}), "a second PC saved");
    check(kvm::save_peer_layout(file, "tabuladze", {.side = "top", .set_at = 3000}), "the first one changed");
    const auto loaded = kvm::load_config(file);
    const auto t = loaded.peers.find("tabuladze");
    const auto d = loaded.peers.find("DESKTOP-7Q0UR7J");
    check(loaded.peers.size() == 2 && t != loaded.peers.end() && t->second.side == "top" && t->second.set_at == 3000 &&
              d != loaded.peers.end() && d->second.side == "left",
          "each PC keeps its own place");
    nlohmann::json raw;
    {
        std::ifstream in(file);
        raw = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
    }
    check(raw.is_object() && raw.value("client_side", "") == "left" && raw.value("jpeg_quality", 0) == 80 &&
              raw.value("custom", 0) == 7,
          "every other setting is kept");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    std::printf("\n%s\n", g_failed == 0 ? "all passed" : "FAILED");
    return g_failed == 0 ? 0 : 1;
}
