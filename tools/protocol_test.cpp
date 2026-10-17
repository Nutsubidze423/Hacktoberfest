// Round-trip check for kvm/protocol.h: every control message survives encode -> dump -> parse -> decode.
// Usage: protocol_test.exe   (exit code 0 = all passed)

#include <cstdio>
#include <string>
#include <variant>

#include "kvm/protocol.h"

namespace proto = kvm::proto;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what);
    if (!ok) {
        ++g_failures;
    }
}

template <class M>
void round_trip(const M& msg) {
    const std::string wire = proto::encode(msg).dump();
    const auto decoded = proto::decode(nlohmann::json::parse(wire));
    const bool ok = decoded && std::holds_alternative<M>(*decoded) &&
                    proto::encode(std::get<M>(*decoded)) == proto::encode(msg);
    check(ok, (std::string(M::kType) + "  " + wire).c_str());
}

}  // namespace

int main() {
    round_trip(proto::Hello{.screen_w = 1920, .screen_h = 1080, .dpi_scale = 1.25, .hostname = "laptop-b", .desk = true});
    round_trip(proto::FocusEnter{.y_norm = 0.3, .desk = true});
    round_trip(proto::FocusLeave{.y_norm = 0.7});
    round_trip(proto::MouseMove{.dx = -5, .dy = 12});
    round_trip(proto::MouseButtonEvent{.button = proto::MouseButton::Right, .down = true, .x_norm = 0.5, .y_norm = 0.25});
    round_trip(proto::MouseWheel{.delta = -120, .horizontal = true});
    round_trip(proto::Key{.vk = 0x41, .scan = 0x1E, .down = true, .extended = false});
    round_trip(proto::StreamStart{.hwnd = 0x1234ABCDull, .title = "Photoshop", .width = 1600, .height = 900});
    round_trip(proto::StreamStop{.hwnd = 0x1234ABCDull});
    round_trip(proto::RemoteMouseMove{.x_norm = 0.1, .y_norm = 0.9});
    round_trip(proto::Clipboard{.seq = 3, .kind = "text", .total = 5, .offset = 0, .data = "aGVsbG8="});
    round_trip(proto::DeskMove{.x = -40, .y = 1300});
    round_trip(proto::DeskCursor{.shape = "ibeam"});
    round_trip(proto::Ping{.t = 123456789});
    round_trip(proto::Pong{.t = 123456789, .t_peer = 987654321});

    check(!proto::decode(nlohmann::json::parse(R"({"type":"no_such_type"})")), "unknown type -> nullopt");
    check(!proto::decode(nlohmann::json::parse(R"({"dx":1})")), "missing type -> nullopt");
    check(!proto::decode(nlohmann::json::parse(R"({"type":"mouse_move","dx":"oops"})")), "wrong field type -> nullopt");

    const auto partial = proto::decode(nlohmann::json::parse(R"({"type":"mouse_move","dx":3})"));
    check(partial && std::get<proto::MouseMove>(*partial).dy == 0, "missing field -> default value");

    const auto header = proto::encode(proto::FrameHeader{.seq = 7, .t_capture_us = 42, .width = 800, .height = 600});
    check(header.at("type") == "frame" && header.get<proto::FrameHeader>().seq == 7, "frame header");

    const proto::FrameHeader desk_in{.seq = 9, .width = 400, .height = 300, .layer = "desk", .x = 100, .y = 50,
                                      .desk_w = 1920, .desk_h = 1080, .rects = {{100, 50, 400, 300}, {120, 60, 10, 10}}};
    const auto desk_out = proto::encode(desk_in).get<proto::FrameHeader>();
    check(desk_out.layer == "desk" && desk_out.desk_w == 1920 && desk_out.rects.size() == 2 && desk_out.rects[1].w == 10,
          "desk frame header with rects");
    check(proto::encode(proto::FrameHeader{}).get<proto::FrameHeader>().layer.empty(), "frame header: layer defaults to window stream");

    std::printf("\n%s\n", g_failures == 0 ? "all passed" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
