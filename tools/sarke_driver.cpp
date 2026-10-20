// SPDX-License-Identifier: Apache-2.0

// sarke_driver.exe — installs the Virtual Display Driver (MikeTheTech's signed IddCx driver,
// installer\vdd) that desk mode uses, the way devcon does: adds the driver package and creates its
// root device (Root\MttVDD). The Sarke installer runs it elevated; it can also be run by hand.
//
//   sarke_driver.exe install <folder with mttvdd.inf>   exit 0 ok, 3010 ok but restart Windows, else failed
//   sarke_driver.exe remove                             removes the device if Sarke created it
//   sarke_driver.exe status                             exit 0 if the device is there
//
// A device that was there already (the user installed the driver themselves) is kept on remove.

#include <windows.h>

#include <cfgmgr32.h>
#include <devguid.h>
#include <newdev.h>
#include <setupapi.h>

#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "newdev.lib")
#pragma comment(lib, "advapi32.lib")

namespace {

constexpr wchar_t kHardwareId[] = L"Root\\MttVDD";
constexpr wchar_t kStateKey[] = L"Software\\Sarke";
constexpr wchar_t kCreatedValue[] = L"CreatedVirtualDisplay";  // 1 = this tool created the device
constexpr int kRestartNeeded = 3010;                           // ERROR_SUCCESS_REBOOT_REQUIRED

void say(const wchar_t* format, auto... args) {
    std::fwprintf(stdout, format, args...);
    std::fputwc(L'\n', stdout);
    std::fflush(stdout);
}

std::wstring last_error() {
    const DWORD err = GetLastError();
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   err, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring out = std::to_wstring(err) + L" " + (text ? text : L"");
    LocalFree(text);
    while (!out.empty() && (out.back() == L'\n' || out.back() == L'\r')) out.pop_back();
    return out;
}

bool has_hardware_id(HDEVINFO set, SP_DEVINFO_DATA& dev) {
    wchar_t ids[1024]{};
    if (!SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, nullptr, reinterpret_cast<BYTE*>(ids),
                                           sizeof(ids) - 2 * sizeof(wchar_t), nullptr)) {
        return false;
    }
    for (const wchar_t* id = ids; *id; id += std::wcslen(id) + 1) {
        if (_wcsicmp(id, kHardwareId) == 0) return true;
    }
    return false;
}

// Calls `each` for every display device with our hardware id (present or not); stops when it returns false.
template <typename Each>
void for_each_device(Each each) {
    const HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, 0);
    if (set == INVALID_HANDLE_VALUE) return;
    SP_DEVINFO_DATA dev{sizeof(dev)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
        if (has_hardware_id(set, dev) && !each(set, dev)) break;
    }
    SetupDiDestroyDeviceInfoList(set);
}

bool device_exists() {
    bool found = false;
    for_each_device([&](HDEVINFO, SP_DEVINFO_DATA&) {
        found = true;
        return false;
    });
    return found;
}

void remember_created(bool created) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kStateKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        return;
    }
    if (created) {
        const DWORD one = 1;
        RegSetValueExW(key, kCreatedValue, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
    } else {
        RegDeleteValueW(key, kCreatedValue);
    }
    RegCloseKey(key);
}

bool we_created_it() {
    DWORD value = 0, size = sizeof(value);
    return RegGetValueW(HKEY_LOCAL_MACHINE, kStateKey, kCreatedValue, RRF_RT_REG_DWORD, nullptr, &value, &size) ==
               ERROR_SUCCESS &&
           value == 1;
}

// The root device the driver binds to, as devcon install creates it.
bool create_device(const std::wstring& inf) {
    GUID cls{};
    wchar_t cls_name[MAX_CLASS_NAME_LEN]{};
    if (!SetupDiGetINFClassW(inf.c_str(), &cls, cls_name, MAX_CLASS_NAME_LEN, nullptr)) {
        say(L"sarke_driver: can't read %ls: %ls", inf.c_str(), last_error().c_str());
        return false;
    }
    const HDEVINFO set = SetupDiCreateDeviceInfoList(&cls, nullptr);
    if (set == INVALID_HANDLE_VALUE) return false;
    SP_DEVINFO_DATA dev{sizeof(dev)};
    bool ok = SetupDiCreateDeviceInfoW(set, cls_name, &cls, nullptr, nullptr, DICD_GENERATE_ID, &dev);
    if (ok) {
        std::wstring ids(kHardwareId);
        ids.push_back(L'\0');
        ids.push_back(L'\0');  // REG_MULTI_SZ
        ok = SetupDiSetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, reinterpret_cast<const BYTE*>(ids.data()),
                                               static_cast<DWORD>(ids.size() * sizeof(wchar_t))) &&
             SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &dev);
    }
    if (!ok) say(L"sarke_driver: creating the device failed: %ls", last_error().c_str());
    SetupDiDestroyDeviceInfoList(set);
    return ok;
}

int install(const std::filesystem::path& folder) {
    const std::wstring inf = (folder / L"mttvdd.inf").wstring();
    if (!std::filesystem::exists(inf)) {
        say(L"sarke_driver: %ls not found", inf.c_str());
        return 2;
    }
    if (device_exists()) {
        // Keep the device that runs (and whatever driver it has); just make sure the package is known.
        if (!SetupCopyOEMInfW(inf.c_str(), nullptr, SPOST_PATH, 0, nullptr, 0, nullptr, nullptr)) {
            say(L"sarke_driver: adding the driver package failed: %ls", last_error().c_str());
        }
        say(L"sarke_driver: the Virtual Display Driver is installed already");
        return 0;
    }
    if (!create_device(inf)) return 1;
    BOOL restart = FALSE;
    if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, kHardwareId, inf.c_str(), INSTALLFLAG_FORCE, &restart)) {
        say(L"sarke_driver: installing the driver failed: %ls", last_error().c_str());
        // Don't leave a device without a driver behind.
        for_each_device([](HDEVINFO set, SP_DEVINFO_DATA& dev) {
            SetupDiCallClassInstaller(DIF_REMOVE, set, &dev);
            return true;
        });
        return 1;
    }
    remember_created(true);
    say(L"sarke_driver: the Virtual Display Driver is installed%ls", restart ? L" (restart Windows to finish)" : L"");
    return restart ? kRestartNeeded : 0;
}

int remove() {
    if (!we_created_it()) {
        say(L"sarke_driver: the Virtual Display Driver wasn't installed by Sarke, it stays");
        return 0;
    }
    int removed = 0;
    for_each_device([&](HDEVINFO set, SP_DEVINFO_DATA& dev) {
        if (SetupDiCallClassInstaller(DIF_REMOVE, set, &dev)) ++removed;
        return true;
    });
    remember_created(false);
    say(L"sarke_driver: removed %d Virtual Display Driver device(s)", removed);
    return 0;
}

int status() {
    const bool there = device_exists();
    say(L"sarke_driver: the Virtual Display Driver is %ls", there ? L"installed" : L"not installed");
    return there ? 0 : 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const std::wstring_view command = argc > 1 ? argv[1] : L"";
    if (command == L"install" && argc > 2) return install(argv[2]);
    if (command == L"remove") return remove();
    if (command == L"status") return status();
    say(L"usage: sarke_driver.exe install <folder with mttvdd.inf> | remove | status");
    return 2;
}
