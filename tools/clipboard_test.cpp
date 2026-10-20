// SPDX-License-Identifier: Apache-2.0

// Checks common/clipboard: base64 + chunking, then the real clipboard: copied
// text and images are sent, the peer's content is pasted here (text, CF_DIB +
// "PNG") without being echoed back, a newer copy cancels an older transfer,
// size limit, detach; a copy made while detached (or cut off) goes on the next
// attach, a busy clipboard is retried, the same files copied twice go once.
// Uses this PC's clipboard; text that was on it is put back at the end.
// Usage: clipboard_test.exe   (exit code 0 = all passed)

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "kvm/clipboard.h"

namespace clip = kvm::clip;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what.c_str());
    if (!ok) {
        ++g_failures;
    }
}

std::vector<std::uint8_t> bytes_of(std::string_view s) {
    return {s.begin(), s.end()};
}

// ---- chunks this PC sent to the "peer"
std::mutex g_mu;
std::vector<clip::Chunk> g_sent;

bool sink(const clip::Chunk& c) {
    std::lock_guard l(g_mu);
    g_sent.push_back(c);
    return true;
}

std::vector<clip::Chunk> take_sent() {
    std::lock_guard l(g_mu);
    return std::exchange(g_sent, {});
}

struct Content {
    std::uint32_t seq = 0;
    std::string kind;
    std::vector<std::uint8_t> bytes;
};

// The last content that arrived complete, as the peer would put it together.
std::optional<Content> last_complete(const std::vector<clip::Chunk>& chunks) {
    std::map<std::uint32_t, Content> parts;
    std::map<std::uint32_t, std::size_t> got;
    std::optional<Content> done;
    for (const auto& c : chunks) {
        auto& p = parts[c.seq];
        if (p.bytes.size() != c.total) p = {c.seq, c.kind, std::vector<std::uint8_t>(c.total)};
        const auto b = clip::base64_decode(c.data).value_or(std::vector<std::uint8_t>{});
        std::copy(b.begin(), b.end(), p.bytes.begin() + c.offset);
        if ((got[c.seq] += b.size()) == c.total) done = p;
    }
    return done;
}

std::optional<Content> wait_content(int ms = 3000) {
    std::vector<clip::Chunk> all;
    for (int i = 0; i < ms / 20; ++i) {
        auto more = take_sent();
        all.insert(all.end(), more.begin(), more.end());
        if (auto c = last_complete(all)) {
            Sleep(150);  // anything else still coming?
            auto rest = take_sent();
            all.insert(all.end(), rest.begin(), rest.end());
            return last_complete(all);
        }
        Sleep(20);
    }
    return std::nullopt;
}

// ---- driving the real clipboard from a window of our own
HWND g_owner = nullptr;

bool open_clip() {
    for (int i = 0; i < 50; ++i) {
        if (OpenClipboard(g_owner)) return true;
        Sleep(10);
    }
    return false;
}

HGLOBAL global_copy(const void* data, std::size_t n) {
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n);
    std::memcpy(GlobalLock(h), data, n);
    GlobalUnlock(h);
    return h;
}

void set_text(const std::wstring& text, bool with_image = false);

std::vector<std::uint8_t> make_dib(int w, int h) {
    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(bih);
    bih.biWidth = w;
    bih.biHeight = h;  // bottom-up
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;
    std::vector<std::uint8_t> dib(sizeof(bih) + static_cast<std::size_t>(w) * h * 4);
    std::memcpy(dib.data(), &bih, sizeof(bih));
    std::uint8_t* px = dib.data() + sizeof(bih);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            std::uint8_t* p = px + (static_cast<std::size_t>(y) * w + x) * 4;
            p[0] = static_cast<std::uint8_t>(x * 6);        // B
            p[1] = static_cast<std::uint8_t>(y * 8);        // G
            p[2] = static_cast<std::uint8_t>(255 - x * 5);  // R
            p[3] = 0;
        }
    }
    return dib;
}

void set_text(const std::wstring& text, bool with_image) {
    if (!open_clip()) return;
    EmptyClipboard();
    SetClipboardData(CF_UNICODETEXT, global_copy(text.c_str(), (text.size() + 1) * sizeof(wchar_t)));
    if (with_image) {
        const auto dib = make_dib(8, 8);
        SetClipboardData(CF_DIB, global_copy(dib.data(), dib.size()));
    }
    CloseClipboard();
}

void set_image(const std::vector<std::uint8_t>& dib) {
    if (!open_clip()) return;
    EmptyClipboard();
    SetClipboardData(CF_DIB, global_copy(dib.data(), dib.size()));
    CloseClipboard();
}

void set_files(const std::vector<std::filesystem::path>& paths) {
    std::wstring list;
    for (const auto& p : paths) list += p.wstring() + L'\0';
    list += L'\0';
    std::vector<std::uint8_t> buf(sizeof(DROPFILES) + list.size() * sizeof(wchar_t));
    DROPFILES df{};
    df.pFiles = sizeof(DROPFILES);
    df.fWide = TRUE;
    std::memcpy(buf.data(), &df, sizeof(df));
    std::memcpy(buf.data() + sizeof(df), list.data(), list.size() * sizeof(wchar_t));
    if (!open_clip()) return;
    EmptyClipboard();
    SetClipboardData(CF_HDROP, global_copy(buf.data(), buf.size()));
    CloseClipboard();
}

std::vector<std::filesystem::path> get_files() {
    std::vector<std::filesystem::path> out;
    if (!open_clip()) return out;
    if (HANDLE h = GetClipboardData(CF_HDROP)) {
        auto* drop = static_cast<HDROP>(GlobalLock(h));
        if (drop) {
            const UINT n = DragQueryFileW(static_cast<HDROP>(h), 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; ++i) {
                wchar_t path[MAX_PATH * 4];
                DragQueryFileW(static_cast<HDROP>(h), i, path, static_cast<UINT>(std::size(path)));
                out.emplace_back(path);
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

std::optional<std::wstring> get_text() {
    if (!open_clip()) return std::nullopt;
    std::optional<std::wstring> out;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* w = static_cast<const wchar_t*>(GlobalLock(h))) {
            out = w;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

std::optional<std::vector<std::uint8_t>> get_format(UINT format) {
    if (!open_clip()) return std::nullopt;
    std::optional<std::vector<std::uint8_t>> out;
    if (HANDLE h = GetClipboardData(format)) {
        if (const auto* p = static_cast<const std::uint8_t*>(GlobalLock(h))) {
            out.emplace(p, p + GlobalSize(h));
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

bool wait_until(const std::function<bool()>& cond, int ms = 3000) {
    for (int i = 0; i < ms / 20; ++i) {
        if (cond()) return true;
        Sleep(20);
    }
    return cond();
}

std::wstring wide(std::string_view s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

void base64_and_chunks() {
    std::printf("-- base64 and chunks\n");
    const std::pair<const char*, const char*> vectors[] = {
        {"", ""}, {"f", "Zg=="}, {"fo", "Zm8="}, {"foo", "Zm9v"}, {"foob", "Zm9vYg=="}, {"foobar", "Zm9vYmFy"}};
    bool ok = true;
    for (const auto& [plain, enc] : vectors) {
        ok = ok && clip::base64_encode(bytes_of(plain)) == enc && clip::base64_decode(enc) == bytes_of(plain);
    }
    check(ok, "RFC 4648 test vectors");
    std::mt19937 rng(42);
    std::vector<std::uint8_t> random(1001);
    for (auto& b : random) b = static_cast<std::uint8_t>(rng());
    check(clip::base64_decode(clip::base64_encode(random)) == random, "1001 random bytes round-trip");
    check(!clip::base64_decode("abc") && !clip::base64_decode("@@@@") && !clip::base64_decode("Zg==Zg=="),
          "malformed base64 rejected");

    check(clip::split(1, "text", {}).size() == 1, "empty content -> one chunk");
    std::vector<std::uint8_t> big(100000);
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<std::uint8_t>(i * 7);
    const auto chunks = clip::split(9, "png", big);
    bool sizes = chunks.size() == 4;
    std::vector<std::uint8_t> joined;
    for (const auto& c : chunks) {
        sizes = sizes && c.seq == 9 && c.kind == "png" && c.total == big.size() && c.offset == joined.size() &&
                c.data.size() <= 43692;
        const auto b = clip::base64_decode(c.data).value();
        joined.insert(joined.end(), b.begin(), b.end());
    }
    check(sizes, "100 KB -> 4 chunks, each under 44 KB of base64");
    check(joined == big, "chunks join back to the same bytes");
}

}  // namespace

int main() {
    base64_and_chunks();

    std::printf("\n-- the real clipboard\n");
    // The window that owns our test copies needs a thread that pumps messages:
    // Windows sends the previous owner WM_DESTROYCLIPBOARD and waits for it.
    std::atomic<DWORD> owner_tid{0};
    std::thread owner_thread([&] {
        g_owner = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
        owner_tid = GetCurrentThreadId();
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
        DestroyWindow(g_owner);
    });
    while (!owner_tid) Sleep(1);
    const auto saved = get_text();

    {
        clip::ClipboardSync sync;
        check(sync.start(), "start(): watching the clipboard");
        sync.attach(sink);

        // -- copy text here -> sent to the peer
        const std::string hello = "Hello KVM \xE2\x80\x94 \xE1\x83\xA5\xE1\x83\x90\xE1\x83\xA0\xE1\x83\x97\xE1\x83\xA3\xE1\x83\x9A\xE1\x83\x98 \xE2\x9C\x93";
        set_text(wide(hello));
        const auto sent = wait_content();
        check(sent && sent->kind == "text" && sent->bytes == bytes_of(hello), "copied text (UTF-8, Georgian) is sent");

        // -- text from the peer -> pasted here, not echoed back
        const std::string from_peer = "from the other PC \xE2\x9C\x93";
        for (const auto& c : clip::split(1001, "text", bytes_of(from_peer))) sync.receive(c);
        check(wait_until([&] { return get_text() == wide(from_peer); }), "peer's text is on this clipboard");
        Sleep(400);
        check(take_sent().empty(), "...and is not echoed back");

        // -- copy an image here -> PNG to the peer
        const auto dib = make_dib(40, 30);
        set_image(dib);
        const auto png = wait_content();
        const bool is_png = png && png->kind == "png" && png->bytes.size() > 8 && png->bytes[0] == 0x89 &&
                            png->bytes[1] == 'P' && png->bytes[2] == 'N' && png->bytes[3] == 'G';
        check(is_png, "copied image is sent as PNG (" + std::to_string(png ? png->bytes.size() : 0) + " bytes)");

        // -- that PNG from the peer -> CF_DIB + "PNG" here, same pixels
        if (is_png) {
            set_text(L"something else");
            wait_content();
            for (const auto& c : clip::split(1002, "png", png->bytes)) sync.receive(c);
            const UINT png_format = RegisterClipboardFormatW(L"PNG");
            check(wait_until([&] { return IsClipboardFormatAvailable(CF_DIB) && IsClipboardFormatAvailable(png_format); }),
                  "peer's image is on this clipboard as CF_DIB and PNG");
            const auto got = get_format(CF_DIB);
            bool same = got && got->size() >= dib.size();
            if (same) {
                BITMAPINFOHEADER a{}, b{};
                std::memcpy(&a, dib.data(), sizeof(a));
                std::memcpy(&b, got->data(), sizeof(b));
                same = b.biWidth == a.biWidth && std::abs(b.biHeight) == a.biHeight && b.biBitCount == 32;
                for (std::size_t i = 0; same && i < dib.size() - sizeof(a); i += 4) {
                    for (int k = 0; k < 3; ++k) same = same && got->at(sizeof(b) + i + k) == dib[sizeof(a) + i + k];
                }
            }
            check(same, "pixels survive copy -> PNG -> paste exactly (40x30)");
            Sleep(400);
            check(take_sent().empty(), "...and the image is not echoed back");
        }

        // -- text and an image together: only the text goes
        set_text(L"text wins", /*with_image=*/true);
        const auto both = wait_content();
        check(both && both->kind == "text" && both->bytes == bytes_of("text wins"), "text + image copied -> text is sent");

        // -- big text in many chunks
        std::string big;
        for (int i = 0; big.size() < 200000; ++i) big += "line " + std::to_string(i) + " of a long copied text\r\n";
        set_text(wide(big));
        const auto got_big = wait_content(6000);
        check(got_big && got_big->bytes == bytes_of(big), "200 KB text arrives complete");

        // -- a newer copy cancels the rest of an older one
        std::string huge(3 * 1024 * 1024, 'x');
        set_text(wide(huge));
        Sleep(400);  // the first pieces are on their way
        set_text(L"newer copy");
        const auto newest = wait_content(8000);
        check(newest && newest->bytes == bytes_of("newer copy"), "newer copy arrives");
        Sleep(300);
        const auto rest = take_sent();
        check(rest.empty(), "...and the older 3 MB copy stopped (" + std::to_string(rest.size()) + " pieces after)");

        // -- malformed input from the peer is ignored
        sync.receive({.seq = 5, .kind = "text", .total = 4, .offset = 0, .data = "!!!!"});
        sync.receive({.seq = 6, .kind = "exe", .total = 4, .offset = 0, .data = "Zm9v"});
        sync.receive({.seq = 7, .kind = "text", .total = 2, .offset = 0, .data = "Zm9vYmFy"});
        Sleep(200);
        check(get_text() == L"newer copy", "malformed pieces from the peer are ignored");

        // -- copied files: announced to the peer, then handed to the file channel
        namespace fs = std::filesystem;
        const fs::path dir = fs::temp_directory_path() / "kvm_clipboard_test";
        fs::remove_all(dir);
        fs::create_directories(dir / "folder");
        std::ofstream(dir / "a.txt") << "hello";
        std::ofstream(dir / "folder" / "b.txt") << "abc";
        std::vector<fs::path> handed;
        sync.set_on_files_copied([&](std::vector<fs::path> p) {
            std::lock_guard l(g_mu);
            handed = std::move(p);
        });
        set_files({dir / "a.txt", dir / "folder"});
        const auto announced = wait_content();
        check(announced && announced->kind == "files" && announced->bytes == bytes_of("a.txt\nfolder\n"),
              "copied files are announced to the peer (names)");
        check(wait_until([&] {
                  std::lock_guard l(g_mu);
                  return handed.size() == 2 && handed[0] == dir / "a.txt" && handed[1] == dir / "folder";
              }),
              "...and handed to the file channel");

        // -- the peer copied files: when they arrive they go on this clipboard
        for (const auto& c : clip::split(2001, "files", bytes_of("x.txt\n"))) sync.receive(c);
        check(sync.files_received({dir / "a.txt"}), "announced files from the peer are claimed");
        check(wait_until([&] { return get_files() == std::vector<fs::path>{dir / "a.txt"}; }),
              "...and are on this clipboard as files (Ctrl+V in Explorer)");
        const auto effect = get_format(RegisterClipboardFormatW(L"Preferred DropEffect"));
        check(effect && effect->size() >= 4 && (*effect)[0] == DROPEFFECT_COPY, "...as a copy, never a move");
        Sleep(400);
        check(take_sent().empty(), "...and are not echoed back");

        // -- files the peer dragged (no announcement) stay off the clipboard
        set_text(L"marker");
        wait_content();
        check(!sync.files_received({dir / "folder"}), "dragged files are not claimed");
        Sleep(200);
        check(get_text() == L"marker", "...and the clipboard is unchanged");

        // -- ...unless their announcement comes right after them
        for (const auto& c : clip::split(2002, "files", bytes_of("folder\n"))) sync.receive(c);
        check(wait_until([&] { return get_files() == std::vector<fs::path>{dir / "folder"}; }),
              "files that arrive before their announcement are still claimed");
        Sleep(300);
        take_sent();

        // -- one Ctrl+C in Explorer can update the clipboard twice: the files go once
        int handovers = 0;
        sync.set_on_files_copied([&](std::vector<fs::path>) {
            std::lock_guard l(g_mu);
            ++handovers;
        });
        set_files({dir / "a.txt"});
        const auto first_copy = wait_content();
        set_files({dir / "a.txt"});
        Sleep(500);
        const auto second_copy = take_sent();
        {
            std::lock_guard l(g_mu);
            check(first_copy && first_copy->kind == "files" && second_copy.empty() && handovers == 1,
                  "the same files copied twice at once are shared once (" + std::to_string(handovers) + ")");
        }

        // -- detached: nothing is sent...
        sync.detach();
        set_text(L"copied while away");
        Sleep(500);
        check(take_sent().empty(), "detached: copies are not sent");

        // -- ...until the peer is back: then the copy it missed goes
        sync.attach(sink);
        const auto missed = wait_content();
        check(missed && missed->kind == "text" && missed->bytes == bytes_of("copied while away"),
              "attached again: the copy made while detached is sent");

        // -- attached again with nothing new: nothing is sent
        sync.detach();
        sync.attach(sink);
        Sleep(900);
        check(take_sent().empty(), "attached again with nothing new: nothing is sent");

        // -- the peer's copy, then a reconnect: not sent back
        for (const auto& c : clip::split(3001, "text", bytes_of("peer copy"))) sync.receive(c);
        check(wait_until([&] { return get_text() == L"peer copy"; }), "peer's text is on this clipboard");
        sync.detach();
        sync.attach(sink);
        Sleep(900);
        check(take_sent().empty(), "...and a reconnect doesn't send it back");

        // -- a copy whose sending failed (the connection broke) goes again on reconnect
        sync.attach([](const clip::Chunk&) { return false; });
        set_text(L"cut off");
        Sleep(500);
        sync.detach();
        take_sent();
        sync.attach(sink);
        const auto again = wait_content();
        check(again && again->bytes == bytes_of("cut off"), "a copy whose sending failed goes again on reconnect");

        // -- another app holds the clipboard when the peer's copy comes: it goes on once it's free
        check(open_clip(), "holding the clipboard like another app");
        for (const auto& c : clip::split(3002, "text", bytes_of("waited"))) sync.receive(c);
        Sleep(700);
        CloseClipboard();
        check(wait_until([&] { return get_text() == L"waited"; }), "...the peer's copy is pasted once it's free");
        Sleep(300);
        check(take_sent().empty(), "...and is not echoed back");
        sync.detach();
    }

    // -- size limit
    {
        clip::ClipboardSync limited({.max_bytes = 1024});
        check(limited.start(), "start with max_bytes = 1 KB");
        limited.attach(sink);
        set_text(std::wstring(2000, L'y'));
        Sleep(600);
        check(take_sent().empty(), "copy over the limit is not sent");
        set_text(L"small enough");
        const auto ok = wait_content();
        check(ok && ok->bytes == bytes_of("small enough"), "copy under the limit is sent");
    }

    // -- file limit
    {
        namespace fs = std::filesystem;
        const fs::path dir = fs::temp_directory_path() / "kvm_clipboard_test";
        clip::ClipboardSync limited({.max_file_bytes = 4});
        check(limited.start(), "start with max_file_bytes = 4");
        limited.attach(sink);
        bool handed = false;
        limited.set_on_files_copied([&](std::vector<fs::path>) { handed = true; });
        set_files({dir / "a.txt"});  // 5 bytes
        Sleep(600);
        check(take_sent().empty() && !handed, "copied files over the limit are not shared");
        fs::remove_all(dir);
    }

    if (saved) set_text(*saved);
    PostThreadMessageW(owner_tid, WM_QUIT, 0, 0);
    owner_thread.join();
    std::printf("\n%s\n", g_failures == 0 ? "all passed" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
