// SPDX-License-Identifier: Apache-2.0

// The question the launcher asks when two PCs pair (docs/SECURITY.md 6.3): "Does the other PC show
// 042 917?" with Pair and Cancel, a Windows task dialog. Cancel is the default and it cancels
// itself when the time runs out, because a wrong "yes" would let another PC control this one.
//
//   kvm::PairingDialog dialog;
//   const auto yes = dialog.ask(owner, L"DESKTOP-1", L"042 917", 60s);   // modal
//   ... meanwhile, from the same thread (a message handled during the modal loop):
//   dialog.close();   // the app ended the pairing: ask() returns nullopt
//
// UI thread only. Needs Common Controls 6 (sarke.exe's manifest).
#pragma once

#include <windows.h>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace kvm {

class PairingDialog {
public:
    // Shows the dialog until the user answers, the time runs out or close() is called. true = Pair,
    // false = Cancel or the time ran out, nullopt = closed by close() (nothing to answer).
    std::optional<bool> ask(HWND owner, std::wstring_view peer_name, std::wstring_view code,
                            std::chrono::seconds timeout);
    void close();
    bool open() const { return hwnd_ != nullptr; }

private:
    static HRESULT CALLBACK callback(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LONG_PTR ref);

    HWND hwnd_ = nullptr;
    bool closed_by_app_ = false;
    std::chrono::steady_clock::time_point deadline_{};
    std::wstring footer_;
};

}  // namespace kvm
