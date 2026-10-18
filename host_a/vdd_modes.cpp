#include "vdd_modes.h"

#include <windows.h>

#include <shellapi.h>

#include <chrono>
#include <fstream>
#include <iterator>
#include <regex>
#include <sstream>
#include <string>
#include <thread>

#include "kvm/log.h"
#include "virtual_display.h"

namespace kvm::vdd_modes {

namespace fs = std::filesystem;

namespace {

constexpr auto kRestartTimeout = std::chrono::seconds(45);  // includes the time the user takes for UAC
constexpr auto kComeBackTimeout = std::chrono::seconds(15);

std::string utf8(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

bool read_file(std::string& out) {
    std::ifstream in(settings_path(), std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool has_mode(const std::string& xml, int width, int height) {
    static const std::regex mode(R"(<width>\s*(\d+)\s*</width>\s*<height>\s*(\d+)\s*</height>)");
    for (auto it = std::sregex_iterator(xml.begin(), xml.end(), mode); it != std::sregex_iterator(); ++it) {
        if (std::stoi((*it)[1]) == width && std::stoi((*it)[2]) == height) return true;
    }
    return false;
}

std::string base64(const std::string& bytes) {
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::uint32_t n = (static_cast<std::uint8_t>(bytes[i]) << 16) |
                                (i + 1 < bytes.size() ? static_cast<std::uint8_t>(bytes[i + 1]) << 8 : 0) |
                                (i + 2 < bytes.size() ? static_cast<std::uint8_t>(bytes[i + 2]) : 0);
        out += kAlphabet[(n >> 18) & 63];
        out += kAlphabet[(n >> 12) & 63];
        out += i + 1 < bytes.size() ? kAlphabet[(n >> 6) & 63] : '=';
        out += i + 2 < bytes.size() ? kAlphabet[n & 63] : '=';
    }
    return out;
}

// powershell -EncodedCommand wants base64 of UTF-16LE: no quoting problems on the command line.
std::wstring encoded_command(const std::wstring& script) {
    const std::string utf16le(reinterpret_cast<const char*>(script.data()), script.size() * sizeof(wchar_t));
    const std::string b64 = base64(utf16le);
    return std::wstring(b64.begin(), b64.end());
}

}  // namespace

fs::path settings_path() {
    return L"C:\\VirtualDisplayDriver\\vdd_settings.xml";
}

bool listed(int width, int height) {
    std::string xml;
    return read_file(xml) && has_mode(xml, width, height);
}

bool add(int width, int height) {
    std::string xml;
    if (!read_file(xml)) {
        log::warn("vdd: can't read {}", utf8(settings_path()));
        return false;
    }
    if (has_mode(xml, width, height)) return true;
    const auto end = xml.find("</resolutions>");
    if (end == std::string::npos) {
        log::warn("vdd: {} has no <resolutions> list", utf8(settings_path()));
        return false;
    }
    const std::string nl = xml.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    const auto line_start = xml.rfind('\n', end);
    const std::size_t at = line_start == std::string::npos ? end : line_start + 1;
    std::ostringstream block;
    block << "        <resolution>" << nl << "            <width>" << width << "</width>" << nl << "            <height>"
          << height << "</height>" << nl << "            <refresh_rate>60</refresh_rate>" << nl << "        </resolution>"
          << nl;
    xml.insert(at, block.str());

    std::ofstream out(settings_path(), std::ios::binary | std::ios::trunc);
    out << xml;
    if (!out) {
        log::warn("vdd: can't write {}", utf8(settings_path()));
        return false;
    }
    log::info("vdd: added {}x{} to {}", width, height, utf8(settings_path()));
    return true;
}

bool restart_driver() {
    // pnputil restarts just that device (Windows 10 2004+); disable + enable is the fallback.
    const std::wstring script =
        L"$d = Get-PnpDevice -PresentOnly | Where-Object { $_.FriendlyName -eq 'Virtual Display Driver' };"
        L"if (-not $d) { exit 2 };"
        L"foreach ($x in $d) {"
        L"  pnputil /restart-device \"$($x.InstanceId)\" | Out-Null;"
        L"  if ($LASTEXITCODE -ne 0) {"
        L"    Disable-PnpDevice -InstanceId $x.InstanceId -Confirm:$false;"
        L"    Enable-PnpDevice -InstanceId $x.InstanceId -Confirm:$false } };"
        L"exit 0";
    const std::wstring params = L"-NoProfile -NonInteractive -WindowStyle Hidden -EncodedCommand " + encoded_command(script);

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = L"runas";
    info.lpFile = L"powershell.exe";
    info.lpParameters = params.c_str();
    info.nShow = SW_HIDE;
    log::info("vdd: asking Windows for permission to restart the virtual display driver (once per new resolution)");
    if (!ShellExecuteExW(&info) || !info.hProcess) {
        const DWORD err = GetLastError();
        log::warn("vdd: driver restart {} ({})", err == ERROR_CANCELLED ? "refused" : "failed to start", err);
        return false;
    }
    const auto ms = static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(kRestartTimeout).count());
    const DWORD waited = WaitForSingleObject(info.hProcess, ms);
    DWORD code = 1;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);
    if (waited != WAIT_OBJECT_0 || code != 0) {
        log::warn("vdd: driver restart didn't finish (wait {}, exit code {})", waited, code);
        return false;
    }
    log::info("vdd: driver restarted");
    return true;
}

bool ensure(int width, int height) {
    if (width <= 0 || height <= 0) return false;
    if (!fs::exists(settings_path())) return false;  // not the driver we know: leave it alone
    if (listed(width, height)) return false;
    if (!add(width, height)) return false;
    if (!restart_driver()) {
        log::warn("vdd: {}x{} will be used after the next restart of the PC (or run tools\\vdd_modes.ps1 as admin)",
                  width, height);
        return false;
    }
    // Wait for the device to come back offering the new mode.
    const auto deadline = std::chrono::steady_clock::now() + kComeBackTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (const auto vdd = VirtualDisplay::find()) {
            const SIZE m = vdd->best_mode(width, height);
            if (m.cx == width && m.cy == height) {
                log::info("vdd: {}x{} is available now", width, height);
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    log::warn("vdd: the driver came back without {}x{}", width, height);
    return true;  // restarted all the same: the caller must find() again
}

}  // namespace kvm::vdd_modes
