// SPDX-License-Identifier: Apache-2.0

// Pairing two PCs by comparing a code (docs/SECURITY.md 6.3).
//
// It runs on a TLS connection whose peer isn't paired yet. The PC whose user picked the other one
// starts it (pair_request). Both PCs derive the same 6-digit code from their own TLS session: a relay
// in between would have two sessions and two different codes. Each PC logs
//   pairing: code 042 917 with 'DESKTOP-1' (1A2B-3C4D-5E6F-7081) - confirm on both PCs
// and the launcher asks its user, who answers with "pair yes" / "pair no" on the app's stdin
// (kvm/launcher_commands.h). Each PC stores the other in paired.json only when both said yes.
//
//   kvm::pairing::Prompt prompt;   // the launcher-command handler calls prompt.answer(...)
//   auto result = kvm::pairing::run(std::move(pending), /*initiator=*/true, me, name, paired, prompt);
#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "kvm/net.h"
#include "kvm/paired.h"
#include "kvm/tls.h"

namespace kvm::pairing {

inline constexpr auto kAnswerTimeout = std::chrono::seconds(60);
inline constexpr auto kRequestTimeout = std::chrono::seconds(10);  // responder: pair_request must come by then

// This PC's user's answer, given by the launcher ("pair yes" / "pair no").
class Prompt {
public:
    // True if a question was waiting for it (arm() was called and the answer isn't in yet).
    bool answer(bool yes);
    // Opens a question: call it before the question is shown (logged), so even an instant answer counts.
    // An earlier, stale answer is forgotten.
    void arm();
    // Waits for the answer to the armed question. nullopt after `timeout` or after cancel().
    std::optional<bool> wait(std::chrono::milliseconds timeout);
    void cancel();

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool waiting_ = false;
    bool cancelled_ = false;
    std::optional<bool> answer_;
};

// Pairing prompts can be started by anyone on the network: at most `max` attempts per peer address
// within `window` (SECURITY.md S9), so a stranger can't flood the user with prompts.
class AttemptLimit {
public:
    explicit AttemptLimit(int max = 3, std::chrono::seconds window = std::chrono::minutes(10))
        : max_(max), window_(window) {}
    // Counts an attempt from `address` ("ip" or "ip:port": the port is ignored). False = too many.
    bool allow(std::string_view address);

private:
    int max_;
    std::chrono::seconds window_;
    std::mutex mutex_;
    std::map<std::string, std::deque<std::chrono::steady_clock::time_point>, std::less<>> attempts_;
};

// The code both PCs show for this TLS session (SECURITY.md 6.3): the first 4 bytes of the RFC 8446
// exporter, label "EXPORTER-sarke-pairing-v1", context = both device IDs (the smaller first).
std::string code(const tls::Stream& stream, const tls::DeviceId& mine);

enum class Outcome {
    Paired,        // both users confirmed; the peer is in paired.json
    Declined,      // this PC's user said no
    PeerDeclined,  // the other user said no
    Timeout,       // an answer didn't come in time
    NotPairing,    // responder: the peer sent something other than pair_request
    Failed,        // the connection dropped, or saving failed
};

struct Result {
    Outcome outcome = Outcome::Failed;
    tls::DeviceId peer_id{};
    std::string peer_name;
};

// Runs the exchange on `pending` (TLS done, its key not paired). `initiator`: this PC's user picked
// the other PC; it sends pair_request. Closes the connection when done; a paired client then
// reconnects normally.
Result run(net::Handshaken pending, bool initiator, const tls::Credentials& me, std::string_view my_name,
           paired::Store& paired, Prompt& prompt);

}  // namespace kvm::pairing
