// Wire protocol between host A and client B. This is the frozen team contract:
// change it only in a PR from the core/net owner, and update the table in CLAUDE.md with it.
//
// Port 50500 (control + input): [u32 len][JSON], big-endian length.
// Port 50501 (video):           [u32 header_len][u32 payload_len][header JSON][payload], big-endian.
//
// Every control message is a JSON object with a "type" field. Use encode()/decode() below
// instead of building JSON by hand.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>

namespace kvm::proto {

inline constexpr std::uint16_t kControlPort = 50500;
inline constexpr std::uint16_t kVideoPort = 50501;
// Bump when a message changes incompatibly; A rejects a hello with a different version.
inline constexpr int kProtocolVersion = 1;
// Upper bound for one framed message, guards against garbage lengths.
inline constexpr std::uint32_t kMaxControlMessageBytes = 64 * 1024;
inline constexpr std::uint32_t kMaxFramePayloadBytes = 32 * 1024 * 1024;

enum class MouseButton { Left, Right, Middle, X1, X2 };

NLOHMANN_JSON_SERIALIZE_ENUM(MouseButton, {
    {MouseButton::Left, "left"},
    {MouseButton::Right, "right"},
    {MouseButton::Middle, "middle"},
    {MouseButton::X1, "x1"},
    {MouseButton::X2, "x2"},
})

// B -> A, first message after connecting.
struct Hello {
    static constexpr std::string_view kType = "hello";
    int protocol_version = kProtocolVersion;
    int screen_w = 0;  // physical pixels
    int screen_h = 0;
    double dpi_scale = 1.0;
    std::string hostname;
};

// A -> B: control moves to B. y_norm is where the cursor crossed the edge (0..1).
struct FocusEnter {
    static constexpr std::string_view kType = "focus_enter";
    double y_norm = 0.5;
};

// Control returns to A. A -> B when A takes it back (its own decision, Ctrl+Alt+Shift+Q, send
// failure); B -> A when B's cursor leaves through the edge facing A (only B knows that).
// y_norm is the height of that edge crossing (0..1).
struct FocusLeave {
    static constexpr std::string_view kType = "focus_leave";
    double y_norm = 0.5;
};

// A -> B: relative mouse movement in physical pixels.
struct MouseMove {
    static constexpr std::string_view kType = "mouse_move";
    int dx = 0;
    int dy = 0;
};

// Both directions. x_norm/y_norm are used only B -> A (position inside the streamed window).
struct MouseButtonEvent {
    static constexpr std::string_view kType = "mouse_button";
    MouseButton button = MouseButton::Left;
    bool down = false;
    double x_norm = 0.0;
    double y_norm = 0.0;
};

// Both directions. delta in WHEEL_DELTA units (120 = one notch).
struct MouseWheel {
    static constexpr std::string_view kType = "mouse_wheel";
    int delta = 0;
    bool horizontal = false;
};

// Both directions.
struct Key {
    static constexpr std::string_view kType = "key";
    std::uint16_t vk = 0;
    std::uint16_t scan = 0;
    bool down = false;
    bool extended = false;
};

// A -> B: a window stream starts. Size is the captured client area in physical pixels.
struct StreamStart {
    static constexpr std::string_view kType = "stream_start";
    std::uint64_t hwnd = 0;
    std::string title;
    int width = 0;
    int height = 0;
};

// A -> B: the window stream stopped.
struct StreamStop {
    static constexpr std::string_view kType = "stream_stop";
    std::uint64_t hwnd = 0;
};

// B -> A: pointer position inside the streamed window, normalized 0..1.
struct RemoteMouseMove {
    static constexpr std::string_view kType = "remote_mouse_move";
    double x_norm = 0.0;
    double y_norm = 0.0;
};

// Both directions. t is the sender's monotonic clock in microseconds.
struct Ping {
    static constexpr std::string_view kType = "ping";
    std::int64_t t = 0;
};

// Reply to Ping: t is echoed back, t_peer is the replier's clock when it answered
// (lets the sender estimate the clock offset between A and B).
struct Pong {
    static constexpr std::string_view kType = "pong";
    std::int64_t t = 0;
    std::int64_t t_peer = 0;
};

// Video channel header (port 50501), followed by `payload_len` bytes of encoded image.
struct FrameHeader {
    static constexpr std::string_view kType = "frame";
    std::uint32_t seq = 0;
    std::int64_t t_capture_us = 0;  // A's monotonic clock
    int width = 0;
    int height = 0;
    std::string codec = "jpeg";
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Hello, protocol_version, screen_w, screen_h, dpi_scale, hostname)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(FocusEnter, y_norm)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(FocusLeave, y_norm)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(MouseMove, dx, dy)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(MouseButtonEvent, button, down, x_norm, y_norm)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(MouseWheel, delta, horizontal)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Key, vk, scan, down, extended)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(StreamStart, hwnd, title, width, height)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(StreamStop, hwnd)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(RemoteMouseMove, x_norm, y_norm)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Ping, t)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Pong, t, t_peer)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(FrameHeader, seq, t_capture_us, width, height, codec)

// Any message that travels on the control channel (port 50500).
using Message = std::variant<Hello, FocusEnter, FocusLeave, MouseMove, MouseButtonEvent, MouseWheel,
                             Key, StreamStart, StreamStop, RemoteMouseMove, Ping, Pong>;

// Serializes a message struct (including FrameHeader) to JSON with its "type" field set.
template <class M>
nlohmann::json encode(const M& msg) {
    nlohmann::json j = msg;
    j["type"] = M::kType;
    return j;
}

inline nlohmann::json encode(const Message& msg) {
    return std::visit([](const auto& m) { return encode(m); }, msg);
}

// The "type" string of a message, e.g. for logging.
inline std::string_view type_of(const Message& msg) {
    return std::visit([](const auto& m) { return std::decay_t<decltype(m)>::kType; }, msg);
}

// Parses a control-channel message. Returns nullopt for unknown types or malformed fields;
// never throws.
inline std::optional<Message> decode(const nlohmann::json& j) {
    const auto it = j.find("type");
    if (it == j.end() || !it->is_string()) {
        return std::nullopt;
    }
    const auto& type = it->get_ref<const std::string&>();

    std::optional<Message> out;
    try {
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            (void)((type == std::variant_alternative_t<I, Message>::kType
                        ? (out.emplace(std::in_place_index<I>, j.get<std::variant_alternative_t<I, Message>>()), true)
                        : false) ||
                   ...);
        }(std::make_index_sequence<std::variant_size_v<Message>>{});
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
    return out;
}

// Visitor helper: std::visit(kvm::proto::Overloaded{[](const Hello&) {...}, ...}, msg);
template <class... Fs>
struct Overloaded : Fs... {
    using Fs::operator()...;
};

}  // namespace kvm::proto
