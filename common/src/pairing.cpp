// SPDX-License-Identifier: Apache-2.0

#include "kvm/pairing.h"

#include <algorithm>
#include <variant>
#include <vector>

#include "kvm/log.h"

namespace kvm::pairing {

namespace proto = kvm::proto;
using Clock = std::chrono::steady_clock;

bool Prompt::answer(bool yes) {
    std::lock_guard lock(mutex_);
    if (!waiting_) return false;
    answer_ = yes;
    cv_.notify_all();
    return true;
}

void Prompt::arm() {
    std::lock_guard lock(mutex_);
    answer_.reset();
    waiting_ = true;
}

std::optional<bool> Prompt::wait(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    cv_.wait_for(lock, timeout, [&] { return answer_.has_value() || cancelled_; });
    waiting_ = false;
    if (cancelled_) return std::nullopt;
    return answer_;
}

void Prompt::cancel() {
    std::lock_guard lock(mutex_);
    cancelled_ = true;
    cv_.notify_all();
}

bool AttemptLimit::allow(std::string_view address) {
    const std::string ip(
        address.substr(0, address.rfind(':') == std::string_view::npos ? address.size() : address.rfind(':')));
    const auto now = Clock::now();
    std::lock_guard lock(mutex_);
    auto& times = attempts_[ip];
    while (!times.empty() && now - times.front() > window_) times.pop_front();
    if (static_cast<int>(times.size()) >= max_) return false;
    times.push_back(now);
    return true;
}

std::string code(const tls::Stream& stream, const tls::DeviceId& mine) {
    const auto& [low, high] = std::minmax(mine, stream.peer_id());
    std::vector<std::uint8_t> context(low.begin(), low.end());
    context.insert(context.end(), high.begin(), high.end());
    const auto bytes = stream.export_keying_material("EXPORTER-sarke-pairing-v1", context, 4);
    if (bytes.size() != 4) return {};
    return tls::pairing_code(std::span<const std::uint8_t, 4>(bytes.data(), 4));
}

Result run(net::Handshaken pending, bool initiator, const tls::Credentials& me, std::string_view my_name,
           paired::Store& paired, Prompt& prompt) {
    Result result;
    result.peer_id = pending.stream->peer_id();
    result.peer_name = std::string(pending.stream->peer_name());
    const std::string the_code = code(*pending.stream, me.id());
    const std::string id_text = tls::short_id(result.peer_id);
    if (the_code.empty()) {
        log::error("pairing: can't derive the code - cancelled");
        return result;
    }
    net::Connection conn(std::move(pending));

    if (initiator) {
        conn.send(proto::PairRequest{.name = std::string(my_name)});
    } else {
        // Only pairing messages are accepted from a key that isn't paired (SECURITY.md S3, 6.3).
        const auto first = conn.incoming().pop_for(kRequestTimeout);
        const auto* request = first ? std::get_if<proto::PairRequest>(&*first) : nullptr;
        if (!request) {
            log::warn("trust: {} has the key {} ('{}'), which isn't paired with this PC - connection closed",
                      conn.peer(), id_text, result.peer_name);
            result.outcome = Outcome::NotPairing;
            return result;
        }
        if (!request->name.empty()) result.peer_name = request->name;  // shown only; the key is what counts
    }

    // The launcher shows this line to its user and answers "pair yes" / "pair no".
    prompt.arm();
    log::info("pairing: code {} with '{}' ({}) - confirm on both PCs", the_code, result.peer_name, id_text);
    const auto mine = prompt.wait(kAnswerTimeout);
    if (!mine) {
        conn.send(proto::PairConfirm{.ok = false});
        log::info("pairing: no answer here within {} s - cancelled", kAnswerTimeout.count());
        result.outcome = Outcome::Timeout;
        return result;
    }
    conn.send(proto::PairConfirm{.ok = *mine});
    if (!*mine) {
        log::info("pairing: cancelled on this PC");
        result.outcome = Outcome::Declined;
        return result;
    }

    // The other user's answer (it may have come while this one was deciding).
    std::optional<bool> theirs;
    const auto deadline = Clock::now() + kAnswerTimeout;
    while (!theirs && conn.is_open()) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if (left.count() <= 0) break;
        const auto msg = conn.incoming().pop_for(left);
        if (!msg) break;
        if (const auto* c = std::get_if<proto::PairConfirm>(&*msg)) {
            theirs = c->ok;
        } else if (const auto* r = std::get_if<proto::PairResult>(&*msg); r && !r->ok) {
            theirs = false;
        } else if (!std::holds_alternative<proto::PairRequest>(*msg) &&
                   !std::holds_alternative<proto::PairResult>(*msg)) {
            log::warn("pairing: {} sent '{}' while pairing - cancelled", conn.peer(), proto::type_of(*msg));
            return result;
        }
    }
    if (!theirs) {
        log::info("pairing: no answer from the other PC - cancelled");
        result.outcome = Outcome::Timeout;
        return result;
    }
    if (!*theirs) {
        log::info("pairing: cancelled on the other PC");
        result.outcome = Outcome::PeerDeclined;
        return result;
    }

    if (!paired.add({.id = result.peer_id, .name = result.peer_name, .paired_at = paired::now_ms()})) {
        log::error("pairing: can't save paired.json - cancelled");
        conn.send(proto::PairResult{.ok = false, .reason = "can't save"});
        return result;
    }
    conn.send(proto::PairResult{.ok = true});
    log::info("pairing: paired with '{}' ({})", result.peer_name, id_text);
    result.outcome = Outcome::Paired;
    return result;
}

}  // namespace kvm::pairing
