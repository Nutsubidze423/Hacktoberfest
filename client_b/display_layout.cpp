#include "display_layout.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <utility>

#include <nlohmann/json.hpp>

#include "kvm/config.h"

namespace kvm::layout {

namespace {

// The adapter name Temo's VirtualDisplay::find() looks for (host_a/virtual_display.cpp).
constexpr std::wstring_view kVirtualAdapter = L"Virtual Display Driver";

std::string narrow(std::wstring_view s) {
    if (s.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len, nullptr, nullptr);
    return out;
}

bool along_x(Side side) { return side == Side::Top || side == Side::Bottom; }

std::filesystem::path default_peer_file() {
    PWSTR dir = nullptr;
    std::filesystem::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &dir))) {
        out = std::filesystem::path(dir) / L"Sarke" / L"peer.json";
    }
    CoTaskMemFree(dir);
    return out;
}

std::filesystem::path executable_dir() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) {
            buf.resize(n);
            return std::filesystem::path(buf).parent_path();
        }
        buf.resize(buf.size() * 2);
    }
}

// Writes `text` next to `path` and swaps it in, so readers see the old or the new file, never half.
std::string write_atomically(const std::filesystem::path& path, const std::string& text) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    auto tmp = path;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return "can't write " + narrow(tmp.wstring());
        out << text;
        out.flush();
        if (!out) return "can't write " + narrow(tmp.wstring());
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(tmp, ec);
        return "can't replace " + narrow(path.wstring()) + " (is it read-only or open elsewhere?)";
    }
    return {};
}

}  // namespace

// ---- this PC ------------------------------------------------------------------------------------

bool is_virtual_display(const std::string& device) {
    DISPLAY_DEVICEW dd{};
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i, dd.cb = sizeof(dd)) {
        if (kVirtualAdapter == dd.DeviceString && narrow(dd.DeviceName) == device) return true;
    }
    return false;
}

std::vector<Monitor> local_monitors() {
    auto mons = display::monitors();
    std::erase_if(mons, [](const Monitor& m) { return is_virtual_display(m.name); });
    return mons;
}

int display_number(const std::string& device) {
    const auto at = device.rfind("DISPLAY");
    if (at == std::string::npos) return 0;
    int n = 0;
    for (std::size_t i = at + 7; i < device.size() && device[i] >= '0' && device[i] <= '9'; ++i) {
        n = n * 10 + (device[i] - '0');
        if (n > 999) return 0;
    }
    return n;
}

std::string computer_name() {
    wchar_t name[256];
    DWORD len = static_cast<DWORD>(std::size(name));
    if (!GetComputerNameExW(ComputerNameDnsHostname, name, &len)) return {};
    return narrow(std::wstring_view(name, len));
}

// ---- the other PC -------------------------------------------------------------------------------

std::optional<Peer> load_peer(const std::optional<std::filesystem::path>& file) {
    const auto path = file ? *file : default_peer_file();
    if (path.empty()) return std::nullopt;
    std::ifstream in(path);
    if (!in) return std::nullopt;
    const auto j = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return std::nullopt;
    Peer p;
    try {
        p.hostname = j.value("hostname", std::string{});
        p.width = j.value("screen_w", 0);
        p.height = j.value("screen_h", 0);
        p.scale = j.value("dpi_scale", 1.0);
    } catch (const nlohmann::json::exception&) {  // a field of the wrong type: treat as no file
        return std::nullopt;
    }
    if (!p.known() || p.width <= 0 || p.height <= 0 || p.width > 32768 || p.height > 32768) return std::nullopt;
    if (!(p.scale >= 0.5 && p.scale <= 8.0)) p.scale = 1.0;
    return p;
}

bool remember_peer(const Peer& peer, const std::optional<std::filesystem::path>& file) {
    const auto path = file ? *file : default_peer_file();
    if (path.empty() || !peer.known()) return false;
    const nlohmann::ordered_json j = {
        {"hostname", peer.hostname}, {"screen_w", peer.width}, {"screen_h", peer.height}, {"dpi_scale", peer.scale}};
    return write_atomically(path, j.dump(2) + "\n").empty();
}

// ---- sides --------------------------------------------------------------------------------------

bool side_accepted(Side side) {
    return kVerticalSidesAccepted || side == Side::Left || side == Side::Right;
}

Side client_side_for(Role role, Side peer_side) {
    return role == Role::Host ? peer_side : display::opposite(peer_side);
}

Side peer_side_from(Role role, Side client_side) {
    return role == Role::Host ? client_side : display::opposite(client_side);
}

Side nearest_side(const Rect& around, display::Point p, bool vertical) {
    const double half_w = std::max(1, around.width()) / 2.0;
    const double half_h = std::max(1, around.height()) / 2.0;
    const double dx = (p.x - (around.left + half_w)) / half_w;
    const double dy = (p.y - (around.top + half_h)) / half_h;
    if (vertical && std::abs(dy) > std::abs(dx)) return dy < 0 ? Side::Top : Side::Bottom;
    return dx < 0 ? Side::Left : Side::Right;
}

// ---- geometry -----------------------------------------------------------------------------------

// Same rule as kvm::display::OuterEdge (common/src/dpi.cpp): every edge pixel of every monitor with
// no other monitor directly beyond it.
std::vector<Rect> outer_edge_pieces(const std::vector<Monitor>& mons, Side side) {
    std::vector<Rect> out;
    for (const Monitor& m : mons) {
        const Rect& r = m.rect;
        if (r.width() <= 0 || r.height() <= 0) continue;
        int fixed = 0;
        int from = along_x(side) ? r.left : r.top;
        const int to = along_x(side) ? r.right : r.bottom;
        switch (side) {
            case Side::Left: fixed = r.left; break;
            case Side::Right: fixed = r.right - 1; break;
            case Side::Top: fixed = r.top; break;
            case Side::Bottom: fixed = r.bottom - 1; break;
        }
        std::vector<std::pair<int, int>> covered;
        for (const Monitor& n : mons) {
            if (&n == &m) continue;
            const Rect& o = n.rect;
            bool touches = false;
            switch (side) {
                case Side::Left: touches = o.right == r.left; break;
                case Side::Right: touches = o.left == r.right; break;
                case Side::Top: touches = o.bottom == r.top; break;
                case Side::Bottom: touches = o.top == r.bottom; break;
            }
            if (!touches) continue;
            const int lo = std::max(along_x(side) ? o.left : o.top, from);
            const int hi = std::min(along_x(side) ? o.right : o.bottom, to);
            if (lo < hi) covered.emplace_back(lo, hi);
        }
        std::sort(covered.begin(), covered.end());
        const auto piece = [&](int a, int b) {
            out.push_back(along_x(side) ? Rect{a, fixed, b, fixed + 1} : Rect{fixed, a, fixed + 1, b});
        };
        for (const auto& [lo, hi] : covered) {
            if (lo > from) piece(from, lo);
            from = std::max(from, hi);
        }
        if (from < to) piece(from, to);
    }
    std::sort(out.begin(), out.end(), [&](const Rect& a, const Rect& b) {
        return along_x(side) ? (a.left != b.left ? a.left < b.left : a.top < b.top)
                             : (a.top != b.top ? a.top < b.top : a.left < b.left);
    });
    return out;
}

long long outer_edge_length(const std::vector<Monitor>& mons, Side side) {
    long long total = 0;
    for (const Rect& r : outer_edge_pieces(mons, side)) total += along_x(side) ? r.width() : r.height();
    return total;
}

Rect peer_rect(const std::vector<Monitor>& mons, Side side, int peer_w, int peer_h) {
    const Rect box = display::virtual_desktop(mons);
    // Centre on the span of the crossing edge (not the whole desktop): with an L-shaped desktop
    // that's where the cursor really goes over.
    int lo = std::numeric_limits<int>::max();
    int hi = std::numeric_limits<int>::min();
    for (const Rect& r : outer_edge_pieces(mons, side)) {
        lo = std::min(lo, along_x(side) ? r.left : r.top);
        hi = std::max(hi, along_x(side) ? r.right : r.bottom);
    }
    if (lo > hi) {  // no monitors
        lo = along_x(side) ? box.left : box.top;
        hi = along_x(side) ? box.right : box.bottom;
    }
    const int mid = lo + (hi - lo) / 2;
    switch (side) {
        case Side::Left: return {box.left - peer_w, mid - peer_h / 2, box.left, mid - peer_h / 2 + peer_h};
        case Side::Right: return {box.right, mid - peer_h / 2, box.right + peer_w, mid - peer_h / 2 + peer_h};
        case Side::Top: return {mid - peer_w / 2, box.top - peer_h, mid - peer_w / 2 + peer_w, box.top};
        default: return {mid - peer_w / 2, box.bottom, mid - peer_w / 2 + peer_w, box.bottom + peer_h};
    }
}

// ---- config.json --------------------------------------------------------------------------------

ConfigState read_config(const std::optional<std::filesystem::path>& path) {
    ConfigState st;
    if (path) {
        st.path = *path;
    } else if (auto found = find_config_file()) {
        st.path = *found;
    } else {
        st.path = executable_dir() / L"config.json";
    }
    std::error_code ec;
    st.exists = std::filesystem::is_regular_file(st.path, ec);
    if (!st.exists) return st;

    std::ifstream in(st.path);
    if (!in) {
        st.error = "can't open " + narrow(st.path.wstring());
        return st;
    }
    const auto j = nlohmann::ordered_json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) {
        st.error = narrow(st.path.filename().wstring()) + " is not valid JSON - fix or delete it";
        return st;
    }
    if (const auto it = j.find("client_side"); it != j.end() && it->is_string()) st.client_side = it->get<std::string>();
    if (const auto it = j.find("host_ip"); it != j.end() && it->is_string()) st.host_ip = it->get<std::string>();
    return st;
}

std::string save_client_side(const std::filesystem::path& path, Side client_side) {
    nlohmann::ordered_json j = nlohmann::ordered_json::object();
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec)) {
        std::ifstream in(path);
        if (!in) return "can't open " + narrow(path.wstring());
        j = nlohmann::ordered_json::parse(in, nullptr, /*allow_exceptions=*/false);
        // Never overwrite a file we can't read: the user's other settings are in it.
        if (j.is_discarded() || !j.is_object()) return narrow(path.filename().wstring()) + " is not valid JSON - fix or delete it";
    }
    j["client_side"] = std::string(display::to_string(client_side));
    return write_atomically(path, j.dump(2) + "\n");
}

Role guess_role(const std::string& host_ip) {
    in_addr v4{};
    in6_addr v6{};
    sockaddr_storage addr{};
    int addr_len = 0;
    if (inet_pton(AF_INET, host_ip.c_str(), &v4) == 1) {
        if ((ntohl(v4.s_addr) >> 24) == 127) return Role::Host;
        auto* a = reinterpret_cast<sockaddr_in*>(&addr);
        a->sin_family = AF_INET;
        a->sin_addr = v4;
        addr_len = sizeof(sockaddr_in);
    } else if (inet_pton(AF_INET6, host_ip.c_str(), &v6) == 1) {
        if (IN6_IS_ADDR_LOOPBACK(&v6)) return Role::Host;
        auto* a = reinterpret_cast<sockaddr_in6*>(&addr);
        a->sin6_family = AF_INET6;
        a->sin6_addr = v6;
        addr_len = sizeof(sockaddr_in6);
    } else {
        return host_ip.empty() || host_ip == "localhost" ? Role::Host : Role::Client;
    }
    // Binding to an address only works if one of this PC's adapters has it.
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return Role::Client;
    bool local = false;
    const SOCKET s = socket(addr.ss_family, SOCK_DGRAM, IPPROTO_UDP);
    if (s != INVALID_SOCKET) {
        local = bind(s, reinterpret_cast<const sockaddr*>(&addr), addr_len) == 0;
        closesocket(s);
    }
    WSACleanup();
    return local ? Role::Host : Role::Client;
}

}  // namespace kvm::layout
