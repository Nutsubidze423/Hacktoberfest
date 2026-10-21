// SPDX-License-Identifier: Apache-2.0

// The Windows category of the networks this PC is connected to (Settings > Network & internet >
// the network > Public / Private). Sarke's firewall rules open its ports on Private and Domain
// networks only (installer/sarke.iss), so on a Public one the other PCs can't reach it: the
// launcher says how to change that.
//
//   for (const auto& n : kvm::connected_networks()) if (n.is_public) ...
#pragma once

#include <string>
#include <vector>

namespace kvm {

struct ConnectedNetwork {
    std::wstring name;  // what Windows shows, e.g. the Wi-Fi's name
    bool is_public = false;
};

// Empty if Windows can't say (the Network List Manager isn't available). Call from a thread that
// may initialise COM (it does so itself, apartment-threaded, and undoes it).
std::vector<ConnectedNetwork> connected_networks();

}  // namespace kvm
