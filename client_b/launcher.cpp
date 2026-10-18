#include "launcher.h"

#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <exception>
#include <format>
#include <fstream>
#include <mutex>
#include <regex>
#include <set>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

#include "display_settings.h"
#include "kvm/config.h"
#include "kvm/discovery.h"
#include "kvm/log.h"
#include "kvm/protocol.h"
#include "toast.h"
#include "tray_icon.h"
#include "ui_tokens_tmp.h"

#pragma comment(lib, "shell32.lib")

namespace kvm::launcher {

namespace fs = std::filesystem;
using layout::Role;

namespace {

constexpr wchar_t kWindowClass[] = L"SarkeLauncherWindow";
constexpr wchar_t kMutexName[] = L"Local\\Sarke.Launcher";
constexpr UINT kMsgLines = WM_APP + 20;     // the reader queued lines
constexpr UINT kMsgReaderDone = WM_APP + 21;  // the app's output closed (it exited)
constexpr UINT kMsgDevices = WM_APP + 22;     // the scanner updated the PCs on the network
constexpr UINT kMsgRemoteShow = WM_APP + 30;     // from a second sarke.exe
constexpr UINT kMsgRemoteQuit = WM_APP + 31;     // sarke.exe --quit
constexpr UINT kMsgRemoteRestart = WM_APP + 32;  // sarke.exe --restart
constexpr UINT_PTR kKillTimer = 1;           // the app didn't stop after Ctrl+Break: end it
constexpr UINT_PTR kConnectTimer = 2;        // the client didn't reach the PC it was told to: host again
constexpr UINT kStopGraceMs = 9000;          // the app waits up to 4.5 s to bring windows home
constexpr UINT kConnectTimeoutMs = 15000;
constexpr auto kProblemToastGap = std::chrono::seconds(10);
// The device list: every PC running Sarke as the host answers discovery (UDP 50503).
constexpr auto kScanEvery = std::chrono::milliseconds(2500);
constexpr auto kScanWait = std::chrono::milliseconds(1200);
constexpr auto kForgetAfter = std::chrono::seconds(8);         // not seen for this long: off the list
constexpr auto kAutoRetryBlock = std::chrono::seconds(30);     // after an auto-connect that failed

// Menu ids.
enum : UINT {
    kCmdHeader = 1,
    kCmdDisplays = 10,
    kCmdQualityBase = 20,  // + index into kQualities
    kCmdFpsBase = 30,      // + index into kFrameRates
    kCmdDesk = 40,
    kCmdNotifyConnected = 50,
    kCmdNotifyLost = 51,
    kCmdOpenFiles = 60,
    kCmdOpenConfig = 61,
    kCmdShowLog = 62,
    kCmdRoleHost = 70,
    kCmdRoleClient = 71,
    kCmdRestart = 80,
    kCmdQuit = 90,
    kCmdDeviceBase = 100,  // + index into menu_devices_: connect to that PC
    kCmdAutoBase = 200,    // + index into menu_auto_: toggle "Connect automatically"
    kMaxMenuDevices = 50,
};
struct Choice {
    int value;
    const wchar_t* text;
};
constexpr Choice kQualities[] = {{50, L"Low (50)"}, {75, L"Medium (75)"}, {90, L"High (90)"}, {95, L"Best (95)"}};
constexpr Choice kFrameRates[] = {{15, L"15 fps"}, {30, L"30 fps"}, {60, L"60 fps"}, {0, L"No limit"}};

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

std::string narrow(std::wstring_view s) {
    if (s.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len, nullptr, nullptr);
    return out;
}

fs::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR dir = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &dir))) out = dir;
    CoTaskMemFree(dir);
    return out;
}

fs::path sarke_dir() { return known_folder(FOLDERID_LocalAppData) / L"Sarke"; }

fs::path launcher_dir() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) {
            buf.resize(n);
            return fs::path(buf).parent_path();
        }
        buf.resize(buf.size() * 2);
    }
}

const wchar_t* role_word(Role r) { return r == Role::Host ? L"host" : L"client"; }

// Computer names compare without case (Windows treats them that way).
std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool same_name(std::string_view a, std::string_view b) { return lower(a) == lower(b); }

// This PC's name as kvm_host announces it (main.cpp's hello).
std::string computer_name() {
    wchar_t name[256]{};
    DWORD len = 256;
    if (!GetComputerNameExW(ComputerNameDnsHostname, name, &len)) return {};
    return narrow(std::wstring_view(name, len));
}

std::wstring scale_text(double scale) { return std::format(L"{}%", std::lround(scale * 100)); }

}  // namespace

// ---------------------------------------------------------------------------------------------
// Log lines

Event parse_line(std::string_view line) {
    static const std::regex kLine(R"(^\d\d:\d\d:\d\d\.\d{3} \[(\w+)\] (.*)$)");
    static const std::regex kHostConnected(R"(^client '(.*)' connected from (.+): screen (\d+)x(\d+), dpi scale ([0-9.]+)$)");
    static const std::regex kHostEnded(R"(^client '(.*)' session ended$)");
    static const std::regex kClientLinkUp(R"(^connected to host (.+)$)");
    static const std::regex kClientHello(R"(^host '(.*)': screen (\d+)x(\d+), dpi scale ([0-9.]+)$)");
    static const std::regex kRtt(R"(^link: rtt ([0-9.]+) ms)");

    std::string s(line);
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    std::smatch m;
    if (!std::regex_match(s, m, kLine)) return std::monostate{};
    const std::string level = m[1];
    const std::string msg = m[2];
    std::smatch g;
    if (std::regex_match(msg, g, kHostConnected)) {
        return Connected{g[1], g[2], std::stoi(g[3]), std::stoi(g[4]), std::stod(g[5])};
    }
    if (std::regex_match(msg, g, kClientHello)) {
        return Connected{g[1], {}, std::stoi(g[2]), std::stoi(g[3]), std::stod(g[4])};
    }
    if (std::regex_match(msg, g, kHostEnded)) return SessionEnded{};
    if (msg.starts_with("reconnecting in ")) return SessionEnded{};
    if (std::regex_match(msg, g, kClientLinkUp)) return LinkUp{g[1]};
    if (msg.starts_with("looking for a KVM host") || msg.starts_with("no KVM host found yet")) return Searching{};
    if (std::regex_search(msg, g, kRtt)) return Rtt{std::stod(g[1])};
    if (level == "error") return Problem{msg};
    return std::monostate{};
}

// ---------------------------------------------------------------------------------------------
// Settings

namespace {

fs::path settings_file(const std::optional<fs::path>& file) { return file ? *file : sarke_dir() / L"settings.json"; }

}  // namespace

Settings load_settings(const std::optional<fs::path>& file) {
    Settings s;
    std::ifstream in(settings_file(file));
    if (!in) return s;
    const auto j = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return s;
    if (const auto it = j.find("role"); it != j.end() && it->is_string()) {
        if (*it == "host") s.role = Role::Host;
        if (*it == "client") s.role = Role::Client;
    }
    if (const auto it = j.find("notify_connected"); it != j.end() && it->is_boolean()) s.notify_connected = *it;
    if (const auto it = j.find("notify_lost"); it != j.end() && it->is_boolean()) s.notify_lost = *it;
    if (const auto it = j.find("auto_connect"); it != j.end() && it->is_array()) {
        for (const auto& name : *it) {
            if (name.is_string() && !name.get<std::string>().empty()) s.auto_connect.push_back(name.get<std::string>());
        }
    }
    return s;
}

bool auto_connect_now(std::string_view self, std::string_view peer, std::chrono::steady_clock::duration seen_for) {
    return lower(self) < lower(peer) || seen_for >= kAutoConnectYield;
}

bool save_settings(const Settings& s, const std::optional<fs::path>& file) {
    const fs::path path = settings_file(file);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    nlohmann::ordered_json j = nlohmann::ordered_json::object();
    if (s.role) j["role"] = *s.role == Role::Host ? "host" : "client";
    j["notify_connected"] = s.notify_connected;
    j["notify_lost"] = s.notify_lost;
    j["auto_connect"] = s.auto_connect;
    fs::path tmp = path;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << j.dump(2) << "\n";
        if (!out) return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

std::optional<fs::path> find_app(Role role, const fs::path& dir) {
    const bool host = role == Role::Host;
    const wchar_t* names[] = {host ? L"sarke_host.exe" : L"sarke_client.exe", host ? L"kvm_host.exe" : L"kvm_client.exe"};
    const fs::path places[] = {dir, dir.parent_path() / (host ? L"host_a" : L"client_b")};
    std::error_code ec;
    for (const auto& place : places) {
        for (const auto* name : names) {
            if (fs::is_regular_file(place / name, ec)) return place / name;
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------------
// The launcher

namespace {

class App {
public:
    explicit App(Options options) : options_(std::move(options)) {}
    ~App();

    int run();

private:
    enum class Link { Starting, Waiting, Linked, Lost, Stopped };

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle_message(UINT msg, WPARAM wp, LPARAM lp);

    // A PC on the network, as the scanner sees it.
    struct Device {
        discovery::Host host;
        std::chrono::steady_clock::time_point first_seen;
        std::chrono::steady_clock::time_point last_seen;
    };
    // The PC this one connects to as the client (chosen in the list, or automatically).
    struct Target {
        std::string name;
        std::string ip;
        bool automatic = false;
    };

    void decide_role();
    void switch_role(Role want, const std::wstring& why = {});
    void keep_peer_side(Role from, Role to);
    void scan_loop(std::stop_token stop, HWND notify);
    void on_devices();
    void maybe_auto_connect();
    void connect_to(const discovery::Host& host, bool automatic);
    void disconnect();
    void pick_device();  // the Displays window's Connect button
    std::vector<TrayIcon::MenuItem> device_items();
    bool auto_listed(std::string_view name) const;
    void toggle_auto(const std::string& name);
    bool start_app();
    void stop_app(bool restart);
    void end_app_now();
    void on_reader_done();
    void reader_loop(HANDLE read);
    void on_lines();
    void handle(const Event& e);

    void set_link(Link link);
    void update_tray();
    std::vector<TrayIcon::MenuItem> build_menu();
    void on_command(UINT id);
    void change_config(std::string_view key, layout::ConfigValue value, const std::wstring& what);
    void restart(const std::wstring& why);
    void notify(std::wstring_view title, std::wstring_view text, Toast::Kind kind);
    std::optional<Config> current_config() const;

    Options options_;
    Settings settings_;
    Role role_ = Role::Host;
    fs::path config_path_;
    fs::path log_path_;
    std::ofstream log_;

    HWND hwnd_ = nullptr;
    TrayIcon tray_;
    DisplaySettings displays_;

    HANDLE job_ = nullptr;
    HANDLE process_ = nullptr;
    DWORD pid_ = 0;
    HANDLE stdin_write_ = nullptr;
    std::thread reader_;
    std::mutex lines_mutex_;
    std::deque<std::string> lines_;
    bool prompt_answered_ = false;

    Link link_ = Link::Stopped;
    bool stopping_ = false;
    bool restart_after_stop_ = false;
    bool quitting_ = false;
    std::string peer_name_;
    std::string peer_address_;
    layout::Peer peer_;
    std::optional<double> rtt_ms_;
    std::wstring last_problem_;
    std::chrono::steady_clock::time_point last_problem_toast_{};

    std::string self_name_;
    std::optional<Target> target_;
    std::vector<Device> devices_;            // UI thread's copy of the scan
    std::vector<discovery::Host> menu_devices_;  // what the open menu's device ids stand for
    std::vector<std::string> menu_auto_;     // what its "Connect automatically" ids stand for
    std::set<std::string> suppressed_;       // lower-case names the user disconnected from: no auto-connect until they go away
    std::chrono::steady_clock::time_point auto_block_until_{};
    std::mutex scan_mutex_;
    std::vector<Device> scanned_;            // written by the scanner thread
    std::jthread scanner_;                   // last: stops before the members it uses go away
};

App::~App() {
    scanner_.request_stop();
    if (scanner_.joinable()) scanner_.join();
    if (process_) end_app_now();
    if (reader_.joinable()) reader_.join();
    if (job_) CloseHandle(job_);
    if (hwnd_) DestroyWindow(hwnd_);
}

int App::run() {
    // One launcher per user session: a second start shows the first one's Display arrangement.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        const UINT what = options_.command == Command::Quit      ? kMsgRemoteQuit
                          : options_.command == Command::Restart ? kMsgRemoteRestart
                                                                 : kMsgRemoteShow;
        if (const HWND other = FindWindowW(kWindowClass, nullptr)) PostMessageW(other, what, 0, 0);
        CloseHandle(mutex);
        return 0;
    }
    if (options_.command != Command::Run) {  // --quit / --restart with nothing running
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &App::wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);
    // A hidden top-level window: receives the reader's messages, timers and session-end messages.
    hwnd_ = CreateWindowExW(0, kWindowClass, L"Sarke", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr,
                            GetModuleHandleW(nullptr), this);
    if (!hwnd_) return 1;

    // Kill the app if the launcher dies (it cleans up on Ctrl+Break, which is what Quit sends first).
    job_ = CreateJobObjectW(nullptr, nullptr);
    if (job_) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }

    settings_ = load_settings();
    const auto cfg = layout::read_config(options_.config_path);
    config_path_ = cfg.path;
    if (const auto err = layout::ensure_config(config_path_); !err.empty()) log::warn("launcher: {}", err);

    tray_.create(ui::kAppName, {.open = [this] {
                                    displays_.show({.config_path = config_path_, .role = role_, .app_restarts = true});
                                },
                                .quit = [this] { on_command(kCmdQuit); }});
    tray_.set_menu([this] { return build_menu(); }, [this](UINT id) { on_command(id); });
    displays_.set_applied_handler([this] { restart(L"to use the new side"); });
    // The Displays window's Connect button lists the PCs on the network; Disconnect makes this the host.
    displays_.set_role_change_handler([this](Role r) {
        if (r == Role::Client) {
            pick_device();
        } else {
            disconnect();
        }
    });

    self_name_ = computer_name();
    scanner_ = std::jthread([this, hwnd = hwnd_](std::stop_token stop) { scan_loop(stop, hwnd); });
    decide_role();
    if (start_app()) {
        notify(L"Sarke is running", std::format(L"{} · settings are in the tray", role_ == Role::Host ? L"Host" : L"Client"),
               Toast::Kind::Info);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return 0;
}

void App::decide_role() {
    const auto last = settings_.role;
    // Every PC starts as the host: ready for another one to join, and listed on the other PCs (only
    // hosts answer discovery). The user picks a PC to connect to ("Connect to" in the tray, or the
    // Displays window's Connect button), and only PCs marked "Connect automatically" are joined on
    // their own. --client still starts as the client (it finds the host by itself).
    role_ = options_.role.value_or(Role::Host);
    if (role_ == Role::Host) log::info("launcher: starting as the host; pick a PC under \"Connect to\" to join it");
    if (last && *last != role_) keep_peer_side(*last, role_);  // the client last time: the side stays put
    settings_.role = role_;
    save_settings(settings_);
}

void App::switch_role(Role want, const std::wstring& why) {
    if (want == Role::Host) target_.reset();
    if (want == role_) return;
    KillTimer(hwnd_, kConnectTimer);
    keep_peer_side(role_, want);
    role_ = want;
    settings_.role = want;
    save_settings(settings_);
    displays_.follow_role(want);
    peer_name_.clear();
    restart(!why.empty() ? why : want == Role::Client ? L"Connecting to the other PC" : L"This PC is the host now");
}

// ---- the PCs on the network ---------------------------------------------------------------------

void App::scan_loop(std::stop_token stop, HWND notify) {
    while (!stop.stop_requested()) {
        const auto started = std::chrono::steady_clock::now();
        auto hosts = discovery::find(kScanWait);
        std::erase_if(hosts, [&](const discovery::Host& h) {
            return h.protocol_version != proto::kProtocolVersion || same_name(h.name, self_name_);
        });
        const auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard lock(scan_mutex_);
            for (const auto& h : hosts) {
                const auto it = std::find_if(scanned_.begin(), scanned_.end(),
                                             [&](const Device& d) { return same_name(d.host.name, h.name); });
                if (it == scanned_.end()) {
                    scanned_.push_back({.host = h, .first_seen = now, .last_seen = now});
                } else {
                    it->host = h;  // its IP or busy state may have changed
                    it->last_seen = now;
                }
            }
            std::erase_if(scanned_, [&](const Device& d) { return now - d.last_seen > kForgetAfter; });
            std::sort(scanned_.begin(), scanned_.end(),
                      [](const Device& a, const Device& b) { return lower(a.host.name) < lower(b.host.name); });
        }
        PostMessageW(notify, kMsgDevices, 0, 0);
        while (!stop.stop_requested() && std::chrono::steady_clock::now() < started + kScanEvery) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void App::on_devices() {
    {
        std::lock_guard lock(scan_mutex_);
        devices_ = scanned_;
    }
    // A PC the user disconnected from may connect automatically again once it has gone away.
    std::erase_if(suppressed_, [&](const std::string& name) {
        return std::none_of(devices_.begin(), devices_.end(), [&](const Device& d) { return lower(d.host.name) == name; });
    });
    maybe_auto_connect();
}

bool App::auto_listed(std::string_view name) const {
    return std::any_of(settings_.auto_connect.begin(), settings_.auto_connect.end(),
                       [&](const std::string& n) { return same_name(n, name); });
}

void App::maybe_auto_connect() {
    // Only an idle host joins on its own: nobody is connected to it and it isn't switching.
    if (role_ != Role::Host || link_ == Link::Linked || !process_ || stopping_ || quitting_) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < auto_block_until_) return;
    for (const auto& d : devices_) {
        if (d.host.busy || !auto_listed(d.host.name) || suppressed_.contains(lower(d.host.name))) continue;
        if (!auto_connect_now(self_name_, d.host.name, now - d.first_seen)) continue;
        log::info("launcher: {} is on the network - connecting automatically", d.host.name);
        connect_to(d.host, /*automatic=*/true);
        return;
    }
}

void App::connect_to(const discovery::Host& host, bool automatic) {
    target_ = Target{.name = host.name, .ip = host.ip, .automatic = automatic};
    const std::wstring why = L"Connecting to " + widen(host.name);
    if (role_ == Role::Client) {
        peer_name_.clear();
        restart(why);  // another PC than now: kvm_client starts again with the new --host
    } else {
        switch_role(Role::Client, why);
    }
}

void App::disconnect() {
    if (target_) suppressed_.insert(lower(target_->name));  // don't join it again by itself right away
    switch_role(Role::Host);
}

void App::toggle_auto(const std::string& name) {
    auto& list = settings_.auto_connect;
    if (auto_listed(name)) {
        std::erase_if(list, [&](const std::string& n) { return same_name(n, name); });
        notify(L"Connect automatically: off", widen(name) + L" is joined only when you pick it.", Toast::Kind::Info);
    } else {
        list.push_back(name);
        suppressed_.erase(lower(name));
        notify(L"Connect automatically: on", L"Sarke joins " + widen(name) + L" whenever it is on the network.",
               Toast::Kind::Info);
    }
    save_settings(settings_);
    maybe_auto_connect();
}

std::vector<TrayIcon::MenuItem> App::device_items() {
    using Item = TrayIcon::MenuItem;
    std::vector<Item> m;
    menu_devices_.clear();
    menu_auto_.clear();
    const bool linked_to_target = role_ == Role::Client && target_;
    for (const auto& d : devices_) {
        if (menu_devices_.size() >= kMaxMenuDevices) break;
        const bool current = linked_to_target && same_name(target_->name, d.host.name);
        std::wstring text = widen(d.host.name) + ui::kSeparator + widen(d.host.ip);
        if (current) {
            text += std::wstring(ui::kSeparator) + (link_ == Link::Linked ? L"connected" : L"connecting…");
        } else if (d.host.busy) {
            text += std::wstring(ui::kSeparator) + L"busy (another PC is connected)";
        }
        m.push_back({.id = kCmdDeviceBase + static_cast<UINT>(menu_devices_.size()), .text = text, .checked = current,
                     .radio = true, .enabled = !current && !d.host.busy});
        menu_devices_.push_back(d.host);
    }
    if (linked_to_target && std::none_of(devices_.begin(), devices_.end(),
                                         [&](const Device& d) { return same_name(d.host.name, target_->name); })) {
        // Our host missed the last scan (a lost answer): it is still ours, so list it.
        m.push_back({.text = widen(target_->name) + ui::kSeparator + widen(target_->ip) + ui::kSeparator +
                             (link_ == Link::Linked ? L"connected" : L"connecting…"),
                     .checked = true, .radio = true, .enabled = false});
    }
    if (m.empty()) m.push_back({.text = L"No other PC found yet — start Sarke there, same Wi-Fi", .enabled = false});

    // "Connect automatically": the PCs seen now plus the ones remembered.
    std::vector<std::string> names;
    for (const auto& d : devices_) names.push_back(d.host.name);
    if (target_) names.push_back(target_->name);
    for (const auto& n : settings_.auto_connect) names.push_back(n);
    for (const auto& n : names) {
        if (std::none_of(menu_auto_.begin(), menu_auto_.end(), [&](const std::string& x) { return same_name(x, n); }) &&
            menu_auto_.size() < kMaxMenuDevices) {
            menu_auto_.push_back(n);
        }
    }
    if (!menu_auto_.empty()) {
        m.push_back({.separator = true});
        m.push_back({.text = L"Connect automatically when it's on the network:", .enabled = false});
        for (std::size_t i = 0; i < menu_auto_.size(); ++i) {
            m.push_back({.id = kCmdAutoBase + static_cast<UINT>(i), .text = widen(menu_auto_[i]),
                         .checked = auto_listed(menu_auto_[i])});
        }
    }
    return m;
}

void App::pick_device() {
    const UINT cmd = tray_.pick(device_items());
    if (cmd != 0) on_command(cmd);
}

// config.json client_side says where the client stands next to the host, so the same value means
// the opposite side once this PC swaps roles. Rewrites it for `to`, so the other PC stays on the side
// of this PC the user set in the Displays window.
void App::keep_peer_side(Role from, Role to) {
    if (from == to) return;
    const auto cfg = layout::read_config(config_path_);
    if (!cfg.error.empty()) {
        log::warn("launcher: can't keep the other PC's side: {}", cfg.error);
        return;
    }
    // Missing client_side: the apps use "right".
    const auto client_side = cfg.client_side.empty() ? std::optional(display::Side::Right) : display::parse_side(cfg.client_side);
    if (!client_side) return;  // invalid: the Displays window asks for a side
    const auto peer_side = layout::peer_side_from(from, *client_side);
    const auto now = layout::client_side_for(to, peer_side);
    if (const auto error = layout::save_client_side(config_path_, now); !error.empty()) {
        log::warn("launcher: can't keep the other PC's side: {}", error);
        return;
    }
    log::info("launcher: the other PC stays on the {} - client_side \"{}\" for the {}", display::to_string(peer_side),
              display::to_string(now), to == Role::Host ? "host" : "client");
}

bool App::start_app() {
    if (process_) return true;
    const auto exe = find_app(role_, launcher_dir());
    if (!exe) {
        set_link(Link::Stopped);
        notify(L"Sarke can't start", std::format(L"{} not found next to sarke.exe", role_ == Role::Host ? L"kvm_host.exe" : L"kvm_client.exe"),
               Toast::Kind::Lost);
        return false;
    }
    if (const auto running = layout::running_role()) {
        // Started by hand in a console: two copies would fight over ports, hooks and hotkeys.
        set_link(Link::Stopped);
        notify(L"Sarke is already running",
               std::format(L"Close the {} console window, then choose Restart in the Sarke menu.",
                           *running == Role::Host ? L"kvm_host" : L"kvm_client"),
               Toast::Kind::Lost);
        return false;
    }

    // Log: the app's output, this run only (the previous one is kept as .old.log).
    const fs::path logs = sarke_dir() / L"logs";
    std::error_code ec;
    fs::create_directories(logs, ec);
    log_path_ = logs / (std::wstring(role_word(role_)) + L".log");
    fs::path old = log_path_;
    old.replace_extension(L".old.log");
    log_.close();
    fs::rename(log_path_, old, ec);
    log_.open(log_path_, std::ios::binary | std::ios::trunc);

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE out_read = nullptr, out_write = nullptr, in_read = nullptr;
    if (!CreatePipe(&out_read, &out_write, &sa, 0) || !CreatePipe(&in_read, &stdin_write_, &sa, 0)) {
        notify(L"Sarke can't start", L"Could not create the pipes for the app.", Toast::Kind::Lost);
        return false;
    }
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stdin_write_, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmd = std::format(L"\"{}\" --config \"{}\"", exe->wstring(), config_path_.wstring());
    // The PC picked in the list: connect to exactly that one (not the first host that answers).
    if (role_ == Role::Client && target_) cmd += L" --host \"" + widen(target_->ip) + L"\"";
    for (const auto& arg : options_.app_args) cmd += L" \"" + arg + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = out_write;
    si.hStdError = out_write;
    si.hStdInput = in_read;
    PROCESS_INFORMATION pi{};
    // Own console (hidden) and process group: Ctrl+Break reaches only the app.
    const BOOL ok = CreateProcessW(exe->c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | CREATE_SUSPENDED, nullptr,
                                   exe->parent_path().c_str(), &si, &pi);
    CloseHandle(out_write);
    CloseHandle(in_read);
    if (!ok) {
        CloseHandle(out_read);
        CloseHandle(stdin_write_);
        stdin_write_ = nullptr;
        notify(L"Sarke can't start", std::format(L"Starting {} failed (error {}).", exe->filename().wstring(), GetLastError()),
               Toast::Kind::Lost);
        set_link(Link::Stopped);
        return false;
    }
    if (job_) AssignProcessToJobObject(job_, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    pid_ = pi.dwProcessId;
    prompt_answered_ = false;
    stopping_ = false;
    if (reader_.joinable()) reader_.join();
    reader_ = std::thread([this, out_read] { reader_loop(out_read); });
    log::info("launcher: started {} (pid {})", narrow(exe->wstring()), pid_);
    set_link(role_ == Role::Host ? Link::Waiting : Link::Starting);
    // A client that can't reach its PC (gone, or it became a client too) goes back to being the host.
    if (role_ == Role::Client && target_) SetTimer(hwnd_, kConnectTimer, kConnectTimeoutMs, nullptr);
    return true;
}

void App::reader_loop(HANDLE read) {
    std::string pending;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(read, buf, sizeof(buf), &n, nullptr) && n > 0) {
        pending.append(buf, n);
        std::vector<std::string> lines;
        for (std::size_t at; (at = pending.find('\n')) != std::string::npos;) {
            lines.push_back(pending.substr(0, at + 1));
            pending.erase(0, at + 1);
        }
        // The client asks which host when several answer ("Connect to [1]: ", no newline).
        if (pending.find("Connect to [") != std::string::npos) {
            lines.push_back(pending + "\n");
            lines.push_back("\x01prompt");
            pending.clear();
        }
        if (!lines.empty()) {
            {
                std::lock_guard lock(lines_mutex_);
                for (auto& l : lines) lines_.push_back(std::move(l));
            }
            PostMessageW(hwnd_, kMsgLines, 0, 0);
        }
    }
    CloseHandle(read);
    PostMessageW(hwnd_, kMsgReaderDone, 0, 0);
}

void App::on_lines() {
    std::deque<std::string> lines;
    {
        std::lock_guard lock(lines_mutex_);
        lines.swap(lines_);
    }
    for (const auto& line : lines) {
        if (line == "\x01prompt") {
            if (!prompt_answered_ && stdin_write_) {  // the first one: the same pick the console defaults to
                DWORD written = 0;
                WriteFile(stdin_write_, "1\r\n", 3, &written, nullptr);
                prompt_answered_ = true;
            }
            continue;
        }
        if (log_) log_.write(line.data(), static_cast<std::streamsize>(line.size())).flush();
        handle(parse_line(line));
    }
}

void App::handle(const Event& e) {
    std::visit(
        [this](const auto& ev) {
            using T = std::decay_t<decltype(ev)>;
            if constexpr (std::is_same_v<T, Connected>) {
                const bool was_linked = link_ == Link::Linked;
                KillTimer(hwnd_, kConnectTimer);
                peer_name_ = ev.name;
                if (!ev.address.empty()) peer_address_ = ev.address;
                peer_ = {.hostname = ev.name, .width = ev.width, .height = ev.height, .scale = ev.scale};
                displays_.set_peer(peer_, /*connected=*/true);  // also saved in peer.json
                set_link(Link::Linked);
                if (!was_linked && settings_.notify_connected) {
                    std::wstring detail = std::format(L"{}{}{}{}{}", ev.width, ui::kTimes, ev.height, ui::kSeparator, scale_text(ev.scale));
                    if (!peer_address_.empty()) detail = widen(peer_address_) + ui::kSeparator + detail;
                    notify(L"Connected to " + widen(ev.name), detail, Toast::Kind::Connected);
                }
            } else if constexpr (std::is_same_v<T, LinkUp>) {
                peer_address_ = ev.address;
            } else if constexpr (std::is_same_v<T, SessionEnded>) {
                // The client keeps retrying its PC; if it doesn't come back, this PC is the host again.
                if (role_ == Role::Client && target_ && link_ == Link::Linked) {
                    SetTimer(hwnd_, kConnectTimer, kConnectTimeoutMs, nullptr);
                }
                if (link_ == Link::Linked) {
                    set_link(Link::Lost);
                    if (settings_.notify_lost) {
                        const std::wstring who = peer_name_.empty() ? std::wstring(L"the other PC") : widen(peer_name_);
                        notify(L"Connection lost",
                               role_ == Role::Host ? L"Waiting for " + who + L" to come back." : L"Reconnecting to " + who + L"\u2026",
                               Toast::Kind::Lost);
                    }
                }
            } else if constexpr (std::is_same_v<T, Searching>) {
                if (link_ != Link::Linked) set_link(Link::Waiting);
            } else if constexpr (std::is_same_v<T, Rtt>) {
                rtt_ms_ = ev.ms;
                update_tray();
            } else if constexpr (std::is_same_v<T, Problem>) {
                last_problem_ = widen(ev.text);
                const auto now = std::chrono::steady_clock::now();
                if (now - last_problem_toast_ > kProblemToastGap) {
                    last_problem_toast_ = now;
                    notify(L"Sarke has a problem", last_problem_, Toast::Kind::Lost);
                }
            }
        },
        e);
}

void App::stop_app(bool restart_after) {
    restart_after_stop_ = restart_after;
    if (!process_) {
        on_reader_done();
        return;
    }
    if (stopping_) return;
    stopping_ = true;
    // Ctrl+Break: the app's console handler stops it the normal way (windows home, VDD detached).
    bool sent = false;
    if (AttachConsole(pid_)) {
        SetConsoleCtrlHandler(nullptr, TRUE);
        sent = GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid_) != FALSE;
        FreeConsole();
        SetConsoleCtrlHandler(nullptr, FALSE);
    }
    if (!sent) {
        log::warn("launcher: Ctrl+Break failed, ending the app");
        end_app_now();
        return;
    }
    SetTimer(hwnd_, kKillTimer, kStopGraceMs, nullptr);
}

void App::end_app_now() {
    if (process_) TerminateProcess(process_, 1);
}

void App::on_reader_done() {
    KillTimer(hwnd_, kKillTimer);
    DWORD code = 0;
    if (process_) {
        WaitForSingleObject(process_, 3000);
        GetExitCodeProcess(process_, &code);
        CloseHandle(process_);
        process_ = nullptr;
    }
    if (stdin_write_) {
        CloseHandle(stdin_write_);
        stdin_write_ = nullptr;
    }
    if (reader_.joinable()) reader_.join();
    on_lines();  // whatever was still queued
    const bool expected = stopping_;
    stopping_ = false;
    pid_ = 0;
    if (log_) log_.close();
    rtt_ms_.reset();
    set_link(Link::Stopped);

    if (quitting_) {
        PostQuitMessage(0);
        return;
    }
    if (restart_after_stop_) {
        restart_after_stop_ = false;
        start_app();
        return;
    }
    if (!expected) {
        notify(L"Sarke stopped",
               last_problem_.empty() ? std::format(L"The {} exited (code {}). Restart it from the Sarke menu.", role_word(role_), code)
                                     : last_problem_,
               Toast::Kind::Lost);
    }
}

void App::set_link(Link link) {
    link_ = link;
    if (link != Link::Linked) rtt_ms_.reset();
    update_tray();
}

void App::update_tray() {
    std::wstring word = std::wstring(role_ == Role::Host ? L"Host" : L"Client") + ui::kSeparator;
    switch (link_) {
    case Link::Starting: word += L"Starting"; break;
    case Link::Waiting:
        word += role_ == Role::Host ? L"Waiting for the other PC"
                : target_          ? L"Connecting to " + widen(target_->name)
                                   : L"Looking for the host";
        break;
    case Link::Linked:
        word += L"Linked to " + widen(peer_name_);
        if (rtt_ms_) word += std::format(L"{}{} ms", ui::kSeparator, std::lround(*rtt_ms_));
        break;
    case Link::Lost: word += L"Connection lost"; break;
    case Link::Stopped: word += L"Stopped"; break;
    }
    tray_.set_state_word(word);
}

std::optional<Config> App::current_config() const {
    try {
        return load_config(config_path_);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::vector<TrayIcon::MenuItem> App::build_menu() {
    using Item = TrayIcon::MenuItem;
    const auto cfg = current_config();
    std::vector<Item> m;
    std::wstring header = std::format(L"Sarke{}This PC is the {}", ui::kSeparator, role_word(role_));
    m.push_back({.id = kCmdHeader, .text = header, .enabled = false});
    m.push_back({.separator = true});
    m.push_back({.id = kCmdDisplays, .text = L"Display arrangement\u2026", .bold = true});

    Item quality{.text = L"Window quality"};
    bool matched = false;
    for (std::size_t i = 0; i < std::size(kQualities); ++i) {
        const bool on = cfg && cfg->jpeg_quality == kQualities[i].value;
        matched = matched || on;
        quality.submenu.push_back({.id = kCmdQualityBase + static_cast<UINT>(i), .text = kQualities[i].text, .checked = on, .radio = true});
    }
    if (cfg && !matched) quality.submenu.push_back({.text = std::format(L"Custom ({})", cfg->jpeg_quality), .checked = true, .radio = true, .enabled = false});
    m.push_back(std::move(quality));

    Item fps{.text = L"Frame rate"};
    for (std::size_t i = 0; i < std::size(kFrameRates); ++i) {
        fps.submenu.push_back({.id = kCmdFpsBase + static_cast<UINT>(i), .text = kFrameRates[i].text,
                               .checked = cfg && cfg->max_fps == kFrameRates[i].value, .radio = true});
    }
    m.push_back(std::move(fps));
    m.push_back({.id = kCmdDesk, .text = L"Desk mode (the other PC as a monitor)", .checked = cfg && cfg->desk});

    Item notes{.text = L"Notifications"};
    notes.submenu.push_back({.id = kCmdNotifyConnected, .text = L"When the other PC connects", .checked = settings_.notify_connected});
    notes.submenu.push_back({.id = kCmdNotifyLost, .text = L"When the connection drops", .checked = settings_.notify_lost});
    m.push_back(std::move(notes));
    m.push_back({.separator = true});

    m.push_back({.id = kCmdOpenFiles, .text = L"Open received files"});
    m.push_back({.id = kCmdOpenConfig, .text = L"Open config.json"});
    m.push_back({.id = kCmdShowLog, .text = L"Show log", .enabled = !log_path_.empty()});
    // Every PC runs as a host and lists the others; picking one makes this PC its client.
    m.push_back({.text = L"Connect to", .submenu = device_items()});
    if (role_ == Role::Client) {
        const std::wstring who = target_ ? widen(target_->name) : std::wstring(L"the other PC");
        m.push_back({.id = kCmdRoleHost, .text = L"Disconnect from " + who + L" (make this PC the host)"});
    }
    m.push_back({.separator = true});
    m.push_back({.id = kCmdRestart, .text = process_ ? L"Restart Sarke" : L"Start Sarke"});
    m.push_back({.id = kCmdQuit, .text = L"Quit Sarke"});
    return m;
}

void App::on_command(UINT id) {
    if (id >= kCmdQualityBase && id < kCmdQualityBase + std::size(kQualities)) {
        const auto& q = kQualities[id - kCmdQualityBase];
        change_config("jpeg_quality", q.value, std::wstring(L"Window quality: ") + q.text);
        return;
    }
    if (id >= kCmdFpsBase && id < kCmdFpsBase + std::size(kFrameRates)) {
        const auto& f = kFrameRates[id - kCmdFpsBase];
        change_config("max_fps", f.value, std::wstring(L"Frame rate: ") + f.text);
        return;
    }
    if (id >= kCmdDeviceBase && id < kCmdDeviceBase + menu_devices_.size()) {
        const discovery::Host host = menu_devices_[id - kCmdDeviceBase];
        log::info("launcher: connecting to {} ({}) - picked by the user", host.name, host.ip);
        connect_to(host, /*automatic=*/false);
        return;
    }
    if (id >= kCmdAutoBase && id < kCmdAutoBase + menu_auto_.size()) {
        toggle_auto(menu_auto_[id - kCmdAutoBase]);
        return;
    }
    switch (id) {
    case kCmdDisplays:
        displays_.show({.config_path = config_path_, .role = role_, .app_restarts = true});
        break;
    case kCmdDesk: {
        const auto cfg = current_config();
        const bool on = !(cfg && cfg->desk);
        change_config("desk", on, on ? L"Desk mode on" : L"Desk mode off");
        break;
    }
    case kCmdNotifyConnected:
        settings_.notify_connected = !settings_.notify_connected;
        save_settings(settings_);
        break;
    case kCmdNotifyLost:
        settings_.notify_lost = !settings_.notify_lost;
        save_settings(settings_);
        break;
    case kCmdOpenFiles: {
        // Where the apps put received files (common/src/file_transfer.cpp).
        const fs::path dir = known_folder(FOLDERID_Downloads) / L"KVM";
        std::error_code ec;
        fs::create_directories(dir, ec);
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    }
    case kCmdOpenConfig:
        ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + config_path_.wstring() + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        break;
    case kCmdShowLog:
        if (!log_path_.empty()) ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + log_path_.wstring() + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        break;
    case kCmdRoleHost:
        disconnect();
        break;
    case kCmdRoleClient:
        pick_device();
        break;
    case kCmdRestart:
        if (process_) {
            restart(L"");
        } else {
            start_app();
        }
        break;
    case kCmdQuit:
        quitting_ = true;
        if (process_) {
            stop_app(false);
        } else {
            PostQuitMessage(0);
        }
        break;
    default:
        break;
    }
}

void App::change_config(std::string_view key, layout::ConfigValue value, const std::wstring& what) {
    if (const auto err = layout::save_config_key(config_path_, key, value); !err.empty()) {
        notify(L"Couldn't change the setting", widen(err), Toast::Kind::Lost);
        return;
    }
    restart(what.empty() ? std::wstring() : what);
}

void App::restart(const std::wstring& why) {
    notify(L"Restarting Sarke", why.empty() ? L"Back in a moment." : why + L" \u2014 back in a moment.", Toast::Kind::Info);
    stop_app(/*restart=*/true);
}

void App::notify(std::wstring_view title, std::wstring_view text, Toast::Kind kind) {
    tray_.notify(title, text, kind);
}

LRESULT CALLBACK App::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* self = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->handle_message(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT App::handle_message(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case kMsgLines:
        on_lines();
        return 0;
    case kMsgReaderDone:
        on_reader_done();
        return 0;
    case kMsgDevices:
        on_devices();
        return 0;
    case kMsgRemoteQuit:
        on_command(kCmdQuit);
        return 0;
    case kMsgRemoteRestart:
        on_command(kCmdRestart);
        return 0;
    case kMsgRemoteShow:  // a second launcher was started
        displays_.show({.config_path = config_path_, .role = role_, .app_restarts = true});
        notify(L"Sarke is already running", L"It lives in the tray (^ on the taskbar).", Toast::Kind::Info);
        return 0;
    case WM_TIMER:
        if (wp == kKillTimer) {  // didn't stop in time
            KillTimer(hwnd_, kKillTimer);
            log::warn("launcher: the app didn't stop within {} ms, ending it", kStopGraceMs);
            end_app_now();
        } else if (wp == kConnectTimer) {
            KillTimer(hwnd_, kConnectTimer);
            if (role_ == Role::Client && link_ != Link::Linked && target_) {
                const std::wstring who = widen(target_->name);
                log::info("launcher: {} didn't answer - this PC is the host again", target_->name);
                if (target_->automatic) auto_block_until_ = std::chrono::steady_clock::now() + kAutoRetryBlock;
                notify(L"Couldn't connect to " + who, L"It may have gone off the network. This PC is the host again.",
                       Toast::Kind::Lost);
                switch_role(Role::Host, L"This PC is the host again");
            }
        }
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wp && process_) {  // Windows is signing out: the app gets its own logoff event; wait for it
            WaitForSingleObject(process_, 5000);
        }
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        hwnd_ = nullptr;
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace

int run(const Options& options) {
    App app(options);
    return app.run();
}

}  // namespace kvm::launcher
