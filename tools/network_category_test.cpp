// SPDX-License-Identifier: Apache-2.0

// network_category_test: prints the networks this PC is connected to and whether Windows calls them
// Public, as the launcher's first-run hint sees them (client_b/network_category.h). Hands-on: the
// answer depends on this PC's networks, so it only fails if Windows can't be asked at all.
//
//   network_category_test.exe

#include <windows.h>

#include <cstdio>

#include "network_category.h"

int main() {
    const auto networks = kvm::connected_networks();
    for (const auto& n : networks) {
        std::wprintf(L"%ls: %ls\n", n.name.c_str(), n.is_public ? L"Public" : L"Private or Domain");
    }
    if (networks.empty()) std::printf("no connected network reported\n");
    return 0;
}
