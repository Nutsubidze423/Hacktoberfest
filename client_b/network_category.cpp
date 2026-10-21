// SPDX-License-Identifier: Apache-2.0

#include "network_category.h"

#include <windows.h>
#include <netlistmgr.h>
#include <wrl/client.h>

#include <memory>
#include <utility>

#include "kvm/log.h"

namespace kvm {

namespace {

using Microsoft::WRL::ComPtr;

// CoInitializeEx for this scope; undone only if it was this call that initialised COM.
class ComScope {
public:
    ComScope() : hr_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComScope() {
        if (SUCCEEDED(hr_)) CoUninitialize();
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    // RPC_E_CHANGED_MODE: the thread already has COM in another mode, which works as well.
    bool ok() const { return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE; }

private:
    HRESULT hr_;
};

}  // namespace

std::vector<ConnectedNetwork> connected_networks() {
    std::vector<ConnectedNetwork> out;
    const ComScope com;
    if (!com.ok()) return out;
    ComPtr<INetworkListManager> manager;
    if (FAILED(CoCreateInstance(__uuidof(NetworkListManager), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&manager)))) {
        log::warn("launcher: can't ask Windows which networks are Public");
        return out;
    }
    ComPtr<IEnumNetworks> networks;
    if (FAILED(manager->GetNetworks(NLM_ENUM_NETWORK_CONNECTED, &networks))) return out;
    for (;;) {
        ComPtr<INetwork> network;
        ULONG fetched = 0;
        if (networks->Next(1, &network, &fetched) != S_OK || fetched == 0) break;
        NLM_NETWORK_CATEGORY category = NLM_NETWORK_CATEGORY_PRIVATE;
        if (FAILED(network->GetCategory(&category))) continue;
        ConnectedNetwork entry;
        BSTR raw = nullptr;
        if (SUCCEEDED(network->GetName(&raw))) {
            const std::unique_ptr<OLECHAR, void (*)(BSTR)> name(raw, &SysFreeString);
            if (name) entry.name.assign(name.get(), SysStringLen(name.get()));
        }
        entry.is_public = category == NLM_NETWORK_CATEGORY_PUBLIC;
        out.push_back(std::move(entry));
    }
    return out;
}

}  // namespace kvm
