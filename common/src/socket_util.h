// SPDX-License-Identifier: Apache-2.0

// Socket helpers shared by net.cpp (control channel) and video.cpp (video channel).
// Private to kvm_common: not under include/, do not include from other modules.
#pragma once

#include <winsock2.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace kvm::net::detail {

// "10060 (A connection attempt failed ...)"
std::string socket_error_text(int code);
std::string last_error_text();

void set_nodelay(SOCKET s);

// Makes every blocked send()/recv() on `s` return at once, even if the peer is dead or silent:
// shutdown() plus CancelIoEx (shutdown alone may leave a blocking call waiting for the peer).
// The handle stays valid; closesocket() it after the other threads are done.
void abort_socket(SOCKET s);

// Blocking send of the whole buffer. False on error.
bool send_all(SOCKET s, const char* data, std::size_t size);

// Blocking receive of exactly `size` bytes. 0 on success, otherwise the WSA error code
// (WSAECONNRESET also for an orderly close by the peer).
int recv_exact(SOCKET s, char* data, std::size_t size);

// Bound and listening on 0.0.0.0:port. Throws std::runtime_error.
SOCKET listen_on(std::uint16_t port);

// Blocks on accept(). INVALID_SOCKET once `listen_sock` was set to INVALID_SOCKET and closed
// (see close_listener). `peer` gets "ip:port".
SOCKET accept_on(const std::atomic<SOCKET>& listen_sock, std::string& peer);

// Closes the listener so a blocked accept_on() returns. Idempotent.
void close_listener(std::atomic<SOCKET>& listen_sock);

// Connects within `timeout` (non-blocking connect + select). INVALID_SOCKET on failure (logged).
// The returned socket is in blocking mode with TCP_NODELAY set.
SOCKET connect_to(const std::string& host, std::uint16_t port, std::chrono::milliseconds timeout);

}  // namespace kvm::net::detail
