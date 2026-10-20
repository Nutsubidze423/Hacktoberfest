// SPDX-License-Identifier: Apache-2.0

#include "kvm/file_transfer.h"

#include <winsock2.h>
#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <map>

#include <nlohmann/json.hpp>

#include "kvm/log.h"
#include "socket_util.h"

namespace kvm::files {

namespace fs = std::filesystem;
using namespace kvm::net::detail;

namespace {

constexpr std::size_t kPrefixBytes = 8;  // u32 header_len + u32 payload_len
constexpr std::uint32_t kMaxHeaderBytes = 64 * 1024;
constexpr std::size_t kMaxRelativePath = 1024;

std::string utf8(const fs::path& p) {
    const auto s = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

fs::path from_utf8(std::string_view s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string megabytes(std::uint64_t bytes) {
    return std::format("{:.1f} MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
}

// "CON", "nul.txt", "COM1.log"...: Windows opens the device instead of a file with that name.
bool is_device_name(std::string_view name) {
    std::string stem(name.substr(0, name.find('.')));
    std::transform(stem.begin(), stem.end(), stem.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    for (const std::string_view device : {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"}) {
        if (stem == device) return true;
    }
    return stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) &&
           std::isdigit(static_cast<unsigned char>(stem[3]));
}

// The first name in `dir` that's free: "cat.jpg" -> "cat (2).jpg", "project" -> "project (2)".
fs::path free_name(const fs::path& dir, const fs::path& name, bool is_file) {
    std::error_code ec;
    if (!fs::exists(dir / name, ec)) return name;
    const fs::path stem = is_file ? name.stem() : name;
    const fs::path ext = is_file ? name.extension() : fs::path();
    for (int n = 2;; ++n) {
        fs::path candidate = stem;
        candidate += std::format(L" ({})", n);
        candidate += ext;
        if (!fs::exists(dir / candidate, ec)) return candidate;
    }
}

bool send_message(SOCKET s, const nlohmann::json& header, const char* payload = nullptr, std::size_t size = 0) {
    const std::string h = header.dump();
    std::string head(kPrefixBytes + h.size(), '\0');
    const u_long lens[2] = {htonl(static_cast<u_long>(h.size())), htonl(static_cast<u_long>(size))};
    std::memcpy(head.data(), lens, kPrefixBytes);
    std::memcpy(head.data() + kPrefixBytes, h.data(), h.size());
    return send_all(s, head.data(), head.size()) && (size == 0 || send_all(s, payload, size));
}

struct Message {
    nlohmann::json header;
    std::vector<char> payload;
};

// 0 on success, a WSA error code from the socket, or -1 for a malformed message.
int recv_message(SOCKET s, Message& out) {
    std::array<u_long, 2> lens{};
    if (const int err = recv_exact(s, reinterpret_cast<char*>(lens.data()), kPrefixBytes); err != 0) return err;
    const std::uint32_t header_len = ntohl(lens[0]);
    const std::uint32_t payload_len = ntohl(lens[1]);
    if (header_len == 0 || header_len > kMaxHeaderBytes || payload_len > kChunkBytes) return -1;
    std::string header(header_len, '\0');
    if (const int err = recv_exact(s, header.data(), header_len); err != 0) return err;
    out.header = nlohmann::json::parse(header, nullptr, /*allow_exceptions=*/false);
    if (out.header.is_discarded() || !out.header.is_object()) return -1;
    out.payload.resize(payload_len);
    if (payload_len != 0) {
        if (const int err = recv_exact(s, out.payload.data(), payload_len); err != 0) return err;
    }
    return 0;
}

// The drag image a transfer_begin carries (header image_w/h/x/y, payload = its pixels), if any.
std::optional<DragImage> drag_image_from(const Message& m) {
    const auto number = [&](const char* key) -> std::optional<int> {
        const auto it = m.header.find(key);
        if (it == m.header.end() || !it->is_number_integer()) return std::nullopt;
        return it->get<int>();
    };
    const auto w = number("image_w"), h = number("image_h"), x = number("image_x"), y = number("image_y");
    if (!w || !h || !x || !y || *w <= 0 || *h <= 0 || *w > 1024 || *h > 1024 ||
        m.payload.size() != static_cast<std::size_t>(*w) * static_cast<std::size_t>(*h) * 4) {
        return std::nullopt;
    }
    DragImage image{.width = *w, .height = *h, .cursor_x = *x, .cursor_y = *y};
    image.bgra.assign(m.payload.begin(), m.payload.end());
    return image;
}

}  // namespace

fs::path default_target() {
    fs::path downloads;
    PWSTR known = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &known))) downloads = known;
    CoTaskMemFree(known);
    if (downloads.empty()) {
        wchar_t* profile = nullptr;
        std::size_t length = 0;
        if (_wdupenv_s(&profile, &length, L"USERPROFILE") == 0 && profile) downloads = fs::path(profile) / L"Downloads";
        std::free(profile);
    }
    return downloads / L"KVM";
}

std::optional<fs::path> safe_destination(const fs::path& root, std::string_view relative) {
    if (relative.empty() || relative.size() > kMaxRelativePath || relative.front() == '/') return std::nullopt;
    fs::path out = root;
    for (std::size_t start = 0;;) {
        const std::size_t end = relative.find('/', start);
        const std::string_view part = relative.substr(start, end == std::string_view::npos ? end : end - start);
        if (part.empty() || part == "." || part == "..") return std::nullopt;
        for (const unsigned char c : part) {
            // Control characters and what Windows forbids in names; ':' also covers drive letters and
            // alternate data streams, '\' covers "\\?\" and backslash paths.
            if (c < 32 || std::strchr("<>:\"\\|?*", c) != nullptr) return std::nullopt;
        }
        if (part.back() == '.' || part.back() == ' ') return std::nullopt;  // Windows strips them: "a." is "a"
        if (is_device_name(part)) return std::nullopt;
        out /= from_utf8(part);
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    // Belt and braces: the result must still be below root.
    std::wstring base = root.lexically_normal().native();
    if (!base.empty() && base.back() != L'\\') base += L'\\';
    if (!out.lexically_normal().native().starts_with(base)) return std::nullopt;
    return out;
}

FileTransfer::FileTransfer(SOCKET sock, std::string peer, Callbacks callbacks, fs::path target)
    : sock_(sock), peer_(std::move(peer)), callbacks_(std::move(callbacks)), target_(std::move(target)) {
    set_nodelay(sock_);
    receiver_ = std::thread([this] { receive_loop(); });
    sender_ = std::thread([this] { send_loop(); });
}

FileTransfer::~FileTransfer() {
    close();
    for (auto* t : {&receiver_, &sender_}) {
        if (t->joinable() && t->get_id() != std::this_thread::get_id()) t->join();
    }
    closesocket(sock_);
}

void FileTransfer::close() {
    if (open_.exchange(false)) net::detail::abort_socket(sock_);  // wakes the blocked recv/send at once
    outgoing_.close();
}

bool FileTransfer::send(std::vector<fs::path> items, bool drag, std::optional<DragImage> image) {
    if (!drag) image.reset();
    return open_ && outgoing_.push(Outgoing{std::move(items), drag, std::move(image)});
}

void FileTransfer::send_loop() {
    std::vector<char> buffer(kChunkBytes);
    std::uint64_t next_id = 1;
    while (auto outgoing = outgoing_.pop()) {
        auto* items = &outgoing->items;
        nlohmann::json begin = {{"type", "transfer_begin"}, {"drag", true}};
        const DragImage* image = outgoing->image ? &*outgoing->image : nullptr;
        if (image && (image->bgra.size() > kChunkBytes ||
                      image->bgra.size() != static_cast<std::size_t>(image->width) * image->height * 4)) {
            image = nullptr;  // too big for one message (or malformed): the other PC draws its own
        }
        if (image) {
            begin["image_w"] = image->width;
            begin["image_h"] = image->height;
            begin["image_x"] = image->cursor_x;
            begin["image_y"] = image->cursor_y;
        }
        if (outgoing->drag &&
            !send_message(sock_, begin, image ? reinterpret_cast<const char*>(image->bgra.data()) : nullptr,
                          image ? image->bgra.size() : 0)) {
            if (open_) log::warn("files: sending to {} failed: {}", peer_, last_error_text());
            close();
            return;
        }
        struct Entry {
            fs::path file;
            std::string wire;  // relative path with '/', as the receiver sees it
            std::uint64_t size;
        };
        std::vector<Entry> entries;
        std::uint64_t total = 0;
        for (fs::path item : *items) {
            if (!item.has_filename()) item = item.parent_path();  // "C:\work\project\" -> "...\project"
            std::error_code ec;
            if (fs::is_regular_file(item, ec)) {
                entries.push_back({item, utf8(item.filename()), fs::file_size(item, ec)});
            } else if (fs::is_directory(item, ec)) {
                for (auto it =
                         fs::recursive_directory_iterator(item, fs::directory_options::skip_permission_denied, ec);
                     !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
                    if (!it->is_regular_file(ec)) continue;
                    entries.push_back(
                        {it->path(), utf8(item.filename() / it->path().lexically_relative(item)), it->file_size(ec)});
                }
            } else {
                log::warn("files: {} not found, skipped", utf8(item));
            }
        }
        for (const auto& e : entries) total += e.size;

        std::uint64_t sent = 0;
        std::size_t count = 0;
        for (const auto& e : entries) {
            std::ifstream in(e.file, std::ios::binary);
            if (!in) {
                log::warn("files: can't read {}, skipped", utf8(e.file));
                continue;
            }
            const std::uint64_t id = next_id++;
            if (!send_message(sock_, {{"type", "file_begin"}, {"id", id}, {"path", e.wire}, {"size", e.size}})) break;
            std::uint64_t offset = 0;
            bool ok = true;
            while (ok) {
                in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const auto n = static_cast<std::size_t>(in.gcount());
                if (n == 0) break;  // end of file (an empty file sends no chunks)
                ok = send_message(sock_, {{"type", "file_chunk"}, {"id", id}, {"offset", offset}}, buffer.data(), n);
                offset += n;
                sent += n;
                if (ok && callbacks_.on_send_progress) callbacks_.on_send_progress(sent, total);
            }
            if (!ok || !send_message(sock_, {{"type", "file_end"}, {"id", id}})) break;
            ++count;
        }
        if (!open_ || !send_message(sock_, {{"type", "transfer_end"}, {"count", count}})) {
            if (open_) log::warn("files: sending to {} failed: {}", peer_, last_error_text());
            close();
            return;
        }
        if (callbacks_.on_send_progress) callbacks_.on_send_progress(total, total);
        log::info("files: sent {} file(s), {} to {}", count, megabytes(sent), peer_);
    }
}

void FileTransfer::receive_loop() {
    struct Incoming {
        std::ofstream out;
        fs::path path;
        std::uint64_t size = 0;
        std::uint64_t written = 0;
        bool skip = false;  // unsafe path or write error: chunks are read and dropped
    };
    std::map<std::uint64_t, Incoming> files;
    std::map<std::string, fs::path> tops;  // top-level name as sent -> name used here, this transfer
    std::vector<fs::path> saved;           // this transfer's top-level entries as saved
    std::size_t received = 0;
    std::uint64_t received_bytes = 0;
    bool drag = false;          // this transfer continues a drag here
    fs::path target = target_;  // where this transfer goes: target_, or a fresh temp folder for a drag
    std::uint32_t drags = 0;

    const auto discard = [](Incoming& f) {
        if (!f.out.is_open()) return;
        f.out.close();
        std::error_code ec;
        fs::remove(f.path, ec);
    };

    Message m;
    while (open_) {
        if (const int err = recv_message(sock_, m); err != 0) {
            if (err == -1) {
                log::error("files: malformed message from {}", peer_);
            } else if (open_) {
                log::info("files: {} disconnected", peer_);
            }
            break;
        }
        try {
            const std::string type = m.header.value("type", "");
            if (type == "transfer_begin") {
                drag = m.header.value("drag", false);
                if (drag) {
                    // A fresh folder per drag: the drop on this PC copies from it, names never clash.
                    target = fs::temp_directory_path() / L"Sarke" / L"drag" /
                             std::format(L"{}-{}", GetCurrentProcessId(), ++drags);
                    std::error_code ec;
                    fs::remove_all(target, ec);
                    if (callbacks_.on_drag_begin) callbacks_.on_drag_begin(drag_image_from(m));
                }
            } else if (type == "file_begin") {
                const auto id = m.header.at("id").get<std::uint64_t>();
                const auto rel = m.header.at("path").get<std::string>();
                Incoming f;
                f.size = m.header.at("size").get<std::uint64_t>();
                // The top-level file or folder gets a free name here; its contents follow it.
                const std::size_t slash = rel.find('/');
                const std::string top = rel.substr(0, slash);
                const std::string rest = slash == std::string::npos ? std::string() : rel.substr(slash + 1);
                auto mapped = tops.find(top);
                if (mapped == tops.end()) {
                    std::error_code ec;
                    fs::create_directories(target, ec);
                    const auto checked = safe_destination(target, top);
                    const fs::path name = checked ? free_name(target, checked->filename(), rest.empty()) : fs::path();
                    if (!name.empty()) saved.push_back(target / name);
                    mapped = tops.emplace(top, name).first;  // empty name = unsafe, skip everything below it
                }
                std::optional<fs::path> dest;
                if (!mapped->second.empty()) {
                    dest = rest.empty() ? std::optional(target / mapped->second)
                                        : safe_destination(target / mapped->second, rest);
                }
                if (!dest) {
                    log::warn("files: unsafe path \"{}\" from {}, skipped", rel.substr(0, 200), peer_);
                    f.skip = true;
                } else {
                    std::error_code ec;
                    fs::create_directories(dest->parent_path(), ec);
                    f.path = *dest;
                    f.out.open(f.path, std::ios::binary | std::ios::trunc);
                    if (!f.out) {
                        log::warn("files: can't write {}, skipped", utf8(f.path));
                        f.skip = true;
                    }
                }
                discard(files[id]);  // a reused id replaces an unfinished file
                files[id] = std::move(f);
            } else if (type == "file_chunk") {
                const auto it = files.find(m.header.at("id").get<std::uint64_t>());
                if (it == files.end()) throw std::runtime_error("chunk for an unknown file");
                auto& f = it->second;
                if (f.skip) continue;
                const auto offset = m.header.at("offset").get<std::uint64_t>();
                if (offset != f.written || f.written + m.payload.size() > f.size) {
                    log::warn("files: {} arrived out of order or too long, dropped", utf8(f.path));
                    discard(f);
                    f.skip = true;
                    continue;
                }
                f.out.write(m.payload.data(), static_cast<std::streamsize>(m.payload.size()));
                f.written += m.payload.size();
            } else if (type == "file_end") {
                const auto it = files.find(m.header.at("id").get<std::uint64_t>());
                if (it == files.end()) throw std::runtime_error("end of an unknown file");
                auto& f = it->second;
                if (!f.skip) {
                    f.out.close();
                    if (f.written == f.size && f.out) {
                        ++received;
                        received_bytes += f.written;
                    } else {
                        log::warn("files: {} is incomplete ({} of {} bytes), deleted", utf8(f.path), f.written, f.size);
                        std::error_code ec;
                        fs::remove(f.path, ec);
                    }
                }
                files.erase(it);
            } else if (type == "transfer_end") {
                for (auto& [id, f] : files) discard(f);
                files.clear();
                log::info("files: received {} file(s), {} from {} into {}{}", received, megabytes(received_bytes),
                          peer_, utf8(target), drag ? " (drag)" : "");
                if (!saved.empty()) {
                    if (drag && callbacks_.on_drag_received) {
                        callbacks_.on_drag_received(saved);
                    } else if (callbacks_.on_received) {
                        callbacks_.on_received(saved);
                    }
                }
                tops.clear();
                saved.clear();
                received = 0;
                received_bytes = 0;
                drag = false;
                target = target_;
            } else {
                log::warn("files: unknown message \"{}\" from {}, ignored", type.substr(0, 50), peer_);
            }
        } catch (const std::exception& e) {
            log::error("files: bad message from {}: {}", peer_, e.what());
            break;
        }
    }
    for (auto& [id, f] : files) discard(f);  // never leave half a file behind
    close();
}

FileServer::FileServer(std::uint16_t port) : listen_sock_(listen_on(port)) {}

FileServer::~FileServer() {
    stop();
}

std::unique_ptr<FileTransfer> FileServer::accept(Callbacks callbacks, fs::path target) {
    std::string peer;
    const SOCKET s = accept_on(listen_sock_, peer);
    if (s == INVALID_SOCKET) return nullptr;
    return std::make_unique<FileTransfer>(s, std::move(peer), std::move(callbacks), std::move(target));
}

void FileServer::stop() {
    close_listener(listen_sock_);
}

std::unique_ptr<FileTransfer> connect_files(const std::string& host, std::uint16_t port, Callbacks callbacks,
                                            fs::path target, std::chrono::milliseconds timeout) {
    const SOCKET s = connect_to(host, port, timeout);
    if (s == INVALID_SOCKET) return nullptr;
    return std::make_unique<FileTransfer>(s, host + ":" + std::to_string(port), std::move(callbacks),
                                          std::move(target));
}

}  // namespace kvm::files
