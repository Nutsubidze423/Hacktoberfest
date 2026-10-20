// SPDX-License-Identifier: Apache-2.0

#include "pairing_dialog.h"

#include <commctrl.h>

#include <format>
#include <iterator>

#pragma comment(lib, "comctl32.lib")
// TaskDialogIndirect exists only in Common Controls 6: every program that links this asks for it.
#pragma comment( \
    linker,      \
    "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace kvm {

namespace {

constexpr int kPairButton = 1001;
constexpr int kUnpairButton = 1002;

}  // namespace

std::optional<bool> PairingDialog::ask(HWND owner, std::wstring_view peer_name, std::wstring_view code,
                                       std::chrono::seconds timeout) {
    if (hwnd_) return std::nullopt;  // one question at a time
    closed_by_app_ = false;
    deadline_ = std::chrono::steady_clock::now() + timeout;

    const std::wstring title = std::format(L"Sarke: pair with {}?", peer_name);
    const std::wstring instruction = std::format(L"Does the other PC show {}?", code);
    const std::wstring content = std::format(
        L"Pair with \"{}\" only if you started pairing on that PC and it shows exactly this code.\n\n"
        L"A paired PC can control this one with its mouse and keyboard, see the windows you share and send you "
        L"files. You can unpair it later.",
        peer_name);
    footer_ = std::format(L"Cancels itself in {} s.", timeout.count());
    const std::wstring pair_label = L"Pair\nThe other PC shows " + std::wstring(code);
    const TASKDIALOG_BUTTON buttons[] = {{kPairButton, pair_label.c_str()}};

    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.hwndParent = owner;
    config.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_CALLBACK_TIMER | TDF_SIZE_TO_CONTENT;
    config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    config.pszWindowTitle = title.c_str();
    config.pszMainIcon = TD_SHIELD_ICON;
    config.pszMainInstruction = instruction.c_str();
    config.pszContent = content.c_str();
    config.cButtons = static_cast<UINT>(std::size(buttons));
    config.pButtons = buttons;
    config.nDefaultButton = IDCANCEL;  // a stray Enter must never pair
    config.pszFooter = footer_.c_str();
    config.pszFooterIcon = TD_INFORMATION_ICON;
    config.pfCallback = &PairingDialog::callback;
    config.lpCallbackData = reinterpret_cast<LONG_PTR>(this);

    int pressed = IDCANCEL;
    const HRESULT hr = TaskDialogIndirect(&config, &pressed, nullptr, nullptr);
    hwnd_ = nullptr;
    if (closed_by_app_) return std::nullopt;
    return SUCCEEDED(hr) && pressed == kPairButton;
}

void PairingDialog::close() {
    if (!hwnd_) return;
    closed_by_app_ = true;
    SendMessageW(hwnd_, TDM_CLICK_BUTTON, IDCANCEL, 0);
}

bool confirm_unpair(HWND owner, std::wstring_view peer_name) {
    const std::wstring title = std::format(L"Sarke: unpair {}?", peer_name);
    const std::wstring instruction = std::format(L"Unpair \"{}\"?", peer_name);
    const std::wstring content = std::format(
        L"Both PCs forget each other. If they are connected now, the connection ends.\n\n"
        L"To use \"{}\" with this PC again, pair again: both PCs show a code to compare.",
        peer_name);
    const std::wstring unpair_label = L"Unpair\n" + std::wstring(peer_name) + L" can't connect until you pair again";
    const TASKDIALOG_BUTTON buttons[] = {{kUnpairButton, unpair_label.c_str()}};

    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.hwndParent = owner;
    config.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    config.pszWindowTitle = title.c_str();
    config.pszMainIcon = TD_WARNING_ICON;
    config.pszMainInstruction = instruction.c_str();
    config.pszContent = content.c_str();
    config.cButtons = static_cast<UINT>(std::size(buttons));
    config.pButtons = buttons;
    config.nDefaultButton = IDCANCEL;
    int pressed = IDCANCEL;
    return SUCCEEDED(TaskDialogIndirect(&config, &pressed, nullptr, nullptr)) && pressed == kUnpairButton;
}

HRESULT CALLBACK PairingDialog::callback(HWND hwnd, UINT msg, WPARAM, LPARAM, LONG_PTR ref) {
    auto* self = reinterpret_cast<PairingDialog*>(ref);
    switch (msg) {
    case TDN_CREATED:
        self->hwnd_ = hwnd;
        SetForegroundWindow(hwnd);  // the user is looking at this PC: the question must be in front
        break;
    case TDN_DESTROYED: self->hwnd_ = nullptr; break;
    case TDN_TIMER: {
        const auto left =
            std::chrono::duration_cast<std::chrono::seconds>(self->deadline_ - std::chrono::steady_clock::now());
        if (left.count() <= 0) {
            SendMessageW(hwnd, TDM_CLICK_BUTTON, IDCANCEL, 0);
            break;
        }
        self->footer_ = std::format(L"Cancels itself in {} s.", left.count());
        SendMessageW(hwnd, TDM_UPDATE_ELEMENT_TEXT, TDE_FOOTER, reinterpret_cast<LPARAM>(self->footer_.c_str()));
        break;
    }
    default: break;
    }
    return S_OK;
}

}  // namespace kvm
