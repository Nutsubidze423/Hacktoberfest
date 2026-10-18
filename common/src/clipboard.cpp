#include "kvm/clipboard.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <utility>

#include "kvm/log.h"

// Windows-only helpers used here (not yet linked by kvm_common's CMakeLists).
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")

namespace kvm::clip {
namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT kMsgApply = WM_APP + 1;  // complete content from the peer is waiting
constexpr UINT kMsgStop = WM_APP + 2;
constexpr UINT kMsgApplyFiles = WM_APP + 3;  // the peer's copied files arrived
constexpr UINT kMsgResync = WM_APP + 4;      // a peer is attached: send it what it missed
// How long an announced file copy waits for its files, and how long files that
// arrived first wait for their announcement.
constexpr auto kFilesExpectedFor = std::chrono::minutes(30);
constexpr auto kFilesUnclaimedFor = std::chrono::seconds(10);
constexpr UINT_PTR kSettleTimer = 1;
constexpr UINT_PTR kApplyTimer = 2;
// Apps often write several formats one after another; read once they're done.
constexpr UINT kSettleMs = 80;
// Another app holds the clipboard: try again every kRetryMs, kMaxTries times (~5 s).
constexpr UINT kRetryMs = 150;
constexpr int kMaxTries = 20;
// After a reconnect, give the file channel a moment to connect too (copied files go on it).
constexpr UINT kResyncMs = 500;
// One Ctrl+C in Explorer can update the clipboard twice: the same files again this soon are not resent.
constexpr auto kSameFilesFor = std::chrono::seconds(3);
// Gap between the pieces of one big copy, so mouse and keyboard messages on the
// same connection don't queue up behind it.
constexpr auto kChunkGap = std::chrono::milliseconds(4);
constexpr wchar_t kWindowClass[] = L"KvmClipboardSync";

constexpr char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// OpenClipboard with a few retries: another app may hold it for a moment.
class OpenedClipboard {
public:
    explicit OpenedClipboard(HWND owner) {
        for (int i = 0; i < 10; ++i) {
            if ((ok_ = OpenClipboard(owner) != FALSE)) return;
            Sleep(10);
        }
    }
    ~OpenedClipboard() {
        if (ok_) CloseClipboard();
    }
    OpenedClipboard(const OpenedClipboard&) = delete;
    OpenedClipboard& operator=(const OpenedClipboard&) = delete;
    explicit operator bool() const { return ok_; }

private:
    bool ok_ = false;
};

// GlobalLock / GlobalUnlock.
class Locked {
public:
    explicit Locked(HANDLE h) : h_(h), p_(h ? GlobalLock(h) : nullptr) {}
    ~Locked() {
        if (p_) GlobalUnlock(h_);
    }
    Locked(const Locked&) = delete;
    Locked& operator=(const Locked&) = delete;
    void* get() const { return p_; }
    std::size_t size() const { return p_ ? GlobalSize(h_) : 0; }

private:
    HANDLE h_;
    void* p_;
};

// "143 B", "2.0 KB", "3.1 MB" for the log.
std::string size_text(std::size_t n) {
    if (n < 1024) return std::to_string(n) + " B";
    if (n < 1024 * 1024) return std::format("{:.1f} KB", n / 1024.0);
    return std::format("{:.1f} MB", n / (1024.0 * 1024.0));
}

UINT png_format() {
    static const UINT format = RegisterClipboardFormatW(L"PNG");
    return format;
}

std::string to_utf8(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring to_wide(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Copies bytes into a movable global block for SetClipboardData. nullptr on failure.
HGLOBAL make_global(const void* data, std::size_t size) {
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, std::max<std::size_t>(size, 1));
    if (!h) return nullptr;
    {
        Locked lock(h);
        if (!lock.get()) {
            GlobalFree(h);
            return nullptr;
        }
        if (size) std::memcpy(lock.get(), data, size);
    }
    return h;
}

// SetClipboardData takes ownership only on success.
bool set_data(UINT format, HGLOBAL h) {
    if (!h) return false;
    if (SetClipboardData(format, h)) return true;
    GlobalFree(h);
    return false;
}

ComPtr<IWICImagingFactory> wic() {
    ComPtr<IWICImagingFactory> factory;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    return factory;
}

std::optional<std::vector<std::uint8_t>> encode_png(IWICImagingFactory* factory, IWICBitmapSource* source) {
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->WriteSource(source, nullptr)) || FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
        return std::nullopt;
    }
    STATSTG stat{};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) return std::nullopt;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(stat.cbSize.QuadPart));
    LARGE_INTEGER zero{};
    ULONG read = 0;
    if (FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr)) ||
        FAILED(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read)) || read != bytes.size()) {
        return std::nullopt;
    }
    return bytes;
}

// The clipboard's bitmap (synthesized from CF_DIB / CF_DIBV5 by Windows) as PNG.
// Alpha is dropped: most apps leave it 0 in clipboard bitmaps.
std::optional<std::vector<std::uint8_t>> bitmap_to_png(HBITMAP bitmap) {
    const auto factory = wic();
    ComPtr<IWICBitmap> wic_bitmap;
    if (!factory || !bitmap ||
        FAILED(factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha, &wic_bitmap))) {
        return std::nullopt;
    }
    return encode_png(factory.Get(), wic_bitmap.Get());
}

struct Pixels {
    UINT width = 0;
    UINT height = 0;
    std::vector<std::uint8_t> bgra;  // top-down rows
};

std::optional<Pixels> decode_png(std::span<const std::uint8_t> png) {
    const auto factory = wic();
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(png.data(), static_cast<UINT>(png.size())));
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    Pixels p;
    if (!factory || !stream ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom)) ||
        FAILED(converter->GetSize(&p.width, &p.height)) || p.width == 0 || p.height == 0) {
        return std::nullopt;
    }
    const UINT stride = p.width * 4;
    p.bgra.resize(static_cast<std::size_t>(stride) * p.height);
    if (FAILED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(p.bgra.size()), p.bgra.data()))) {
        return std::nullopt;
    }
    return p;
}

std::vector<std::filesystem::path> dropped_files(HANDLE drop) {
    std::vector<std::filesystem::path> paths;
    Locked lock(drop);
    if (!lock.get()) return paths;
    auto* hdrop = static_cast<HDROP>(drop);
    const UINT n = DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < n; ++i) {
        const UINT len = DragQueryFileW(hdrop, i, nullptr, 0);
        std::wstring path(len, L'\0');
        DragQueryFileW(hdrop, i, path.data(), len + 1);
        paths.emplace_back(std::move(path));
    }
    return paths;
}

// Adds up sizes and file counts (folders recursively); false as soon as a limit is passed.
bool within_limits(const std::vector<std::filesystem::path>& items, std::uint64_t max_bytes, std::size_t max_count,
                   std::uint64_t& bytes, std::size_t& count) {
    namespace fs = std::filesystem;
    bytes = 0;
    count = 0;
    std::error_code ec;
    const auto add = [&](std::uint64_t size) { bytes += size; return ++count <= max_count && bytes <= max_bytes; };
    for (const auto& item : items) {
        if (fs::is_directory(item, ec)) {
            for (auto it = fs::recursive_directory_iterator(item, fs::directory_options::skip_permission_denied, ec);
                 !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (it->is_regular_file(ec) && !add(it->file_size(ec))) return false;
            }
        } else if (!add(fs::file_size(item, ec))) {
            return false;
        }
    }
    return true;
}

// CF_HDROP for Explorer's paste, wide paths.
HGLOBAL make_hdrop(const std::vector<std::filesystem::path>& paths) {
    std::wstring list;
    for (const auto& p : paths) {
        list += p.wstring();
        list += L'\0';
    }
    list += L'\0';
    std::vector<std::uint8_t> buffer(sizeof(DROPFILES) + list.size() * sizeof(wchar_t));
    DROPFILES header{};
    header.pFiles = sizeof(DROPFILES);
    header.fWide = TRUE;
    std::memcpy(buffer.data(), &header, sizeof(header));
    std::memcpy(buffer.data() + sizeof(header), list.data(), list.size() * sizeof(wchar_t));
    return make_global(buffer.data(), buffer.size());
}

// 32-bit bottom-up CF_DIB, which every app that pastes images understands.
HGLOBAL make_dib(const Pixels& p) {
    BITMAPINFOHEADER header{};
    header.biSize = sizeof(header);
    header.biWidth = static_cast<LONG>(p.width);
    header.biHeight = static_cast<LONG>(p.height);
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_RGB;
    header.biSizeImage = static_cast<DWORD>(p.bgra.size());
    std::vector<std::uint8_t> dib(sizeof(header) + p.bgra.size());
    std::memcpy(dib.data(), &header, sizeof(header));
    const std::size_t stride = static_cast<std::size_t>(p.width) * 4;
    for (UINT y = 0; y < p.height; ++y) {
        std::memcpy(dib.data() + sizeof(header) + (p.height - 1 - y) * stride, p.bgra.data() + y * stride, stride);
    }
    return make_global(dib.data(), dib.size());
}

}  // namespace

// ---------------------------------------------------------------------------

std::string base64_encode(std::span<const std::uint8_t> bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const std::uint32_t v = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
        out += kBase64[(v >> 18) & 63];
        out += kBase64[(v >> 12) & 63];
        out += kBase64[(v >> 6) & 63];
        out += kBase64[v & 63];
    }
    if (i + 1 == bytes.size()) {
        const std::uint32_t v = bytes[i] << 16;
        out += kBase64[(v >> 18) & 63];
        out += kBase64[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == bytes.size()) {
        const std::uint32_t v = (bytes[i] << 16) | (bytes[i + 1] << 8);
        out += kBase64[(v >> 18) & 63];
        out += kBase64[(v >> 12) & 63];
        out += kBase64[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> base64_decode(std::string_view text) {
    if (text.size() % 4 != 0) return std::nullopt;
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<std::uint8_t> out;
    out.reserve(text.size() / 4 * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        const bool last = i + 4 == text.size();
        const int pad = last ? (text[i + 3] == '=') + (text[i + 2] == '=') : 0;
        int v[4];
        for (int k = 0; k < 4; ++k) {
            v[k] = (k >= 4 - pad) ? 0 : value(text[i + k]);
            if (v[k] < 0) return std::nullopt;
        }
        const std::uint32_t n = (v[0] << 18) | (v[1] << 12) | (v[2] << 6) | v[3];
        out.push_back(static_cast<std::uint8_t>(n >> 16));
        if (pad < 2) out.push_back(static_cast<std::uint8_t>(n >> 8));
        if (pad < 1) out.push_back(static_cast<std::uint8_t>(n));
    }
    return out;
}

std::vector<Chunk> split(std::uint32_t seq, std::string_view kind, std::span<const std::uint8_t> bytes) {
    std::vector<Chunk> chunks;
    std::size_t offset = 0;
    do {
        const std::size_t n = std::min(kChunkBytes, bytes.size() - offset);
        chunks.push_back({seq, std::string(kind), static_cast<std::uint32_t>(bytes.size()),
                          static_cast<std::uint32_t>(offset), base64_encode(bytes.subspan(offset, n))});
        offset += n;
    } while (offset < bytes.size());
    return chunks;
}

// ---------------------------------------------------------------------------

// Runs on the clipboard thread.
struct ClipboardThread {
    static ClipboardSync* self(HWND h) { return reinterpret_cast<ClipboardSync*>(GetWindowLongPtrW(h, GWLP_USERDATA)); }

    static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            SetWindowLongPtrW(h, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        }
        ClipboardSync* sync = self(h);
        switch (msg) {
            case WM_CLIPBOARDUPDATE:
                // Our own write (content from the peer) must not travel back.
                if (GetClipboardOwner() != h) {
                    if (sync) sync->read_tries_ = 0;
                    SetTimer(h, kSettleTimer, kSettleMs, nullptr);
                }
                return 0;
            case WM_TIMER:
                if (wp == kSettleTimer) {
                    KillTimer(h, kSettleTimer);
                    if (sync) read_and_send(*sync, h);
                } else if (wp == kApplyTimer) {
                    KillTimer(h, kApplyTimer);
                    if (sync) apply(*sync, h);
                }
                return 0;
            case kMsgApply:
                if (sync) {
                    sync->apply_tries_ = 0;
                    apply(*sync, h);
                }
                return 0;
            case kMsgResync:
                // Copied while the peer was away, or its sending broke off: it goes now.
                if (sync && GetClipboardSequenceNumber() != sync->synced_clip_seq_ && GetClipboardOwner() != h) {
                    log::info("clipboard: the other PC is back - sending what was copied meanwhile");
                    sync->read_tries_ = 0;
                    SetTimer(h, kSettleTimer, kResyncMs, nullptr);
                }
                return 0;
            case kMsgApplyFiles:
                if (sync) apply_files(*sync, h);
                return 0;
            case kMsgStop:
                RemoveClipboardFormatListener(h);
                PostQuitMessage(0);
                return 0;
            default:
                return DefWindowProcW(h, msg, wp, lp);
        }
    }

    static void read_and_send(ClipboardSync& sync, HWND h) {
        {
            std::lock_guard lock(sync.sink_mutex_);
            if (!sync.sink_) return;  // nobody to send to now: it goes on the next attach() (kMsgResync)
        }
        ClipboardSync::Content content;
        std::vector<std::filesystem::path> files;
        std::uint32_t clip_seq = 0;
        {
            OpenedClipboard opened(h);
            if (!opened) {
                if (++sync.read_tries_ <= kMaxTries) {
                    SetTimer(h, kSettleTimer, kRetryMs, nullptr);  // another app holds it: try again soon
                } else {
                    log::warn("clipboard: could not open the clipboard (another app holds it)");
                }
                return;
            }
            if (GetClipboardOwner() == h) return;
            clip_seq = GetClipboardSequenceNumber();  // can't change while we hold it open
            if (clip_seq == sync.synced_clip_seq_) return;  // the peer has this one already
            if (sync.options_.files && IsClipboardFormatAvailable(CF_HDROP)) {
                files = dropped_files(GetClipboardData(CF_HDROP));
            } else if (sync.options_.text && IsClipboardFormatAvailable(CF_UNICODETEXT)) {
                Locked text(GetClipboardData(CF_UNICODETEXT));
                if (const auto* w = static_cast<const wchar_t*>(text.get())) {
                    const std::size_t max_chars = text.size() / sizeof(wchar_t);
                    content = {"text", {}};
                    const std::string utf8 = to_utf8({w, wcsnlen(w, max_chars)});
                    content.bytes.assign(utf8.begin(), utf8.end());
                }
            } else if (sync.options_.images && png_format() && IsClipboardFormatAvailable(png_format())) {
                Locked png(GetClipboardData(png_format()));
                if (png.get()) {
                    const auto* p = static_cast<const std::uint8_t*>(png.get());
                    content = {"png", std::vector<std::uint8_t>(p, p + png.size())};
                }
            } else if (sync.options_.images && IsClipboardFormatAvailable(CF_BITMAP)) {
                if (auto png = bitmap_to_png(static_cast<HBITMAP>(GetClipboardData(CF_BITMAP)))) {
                    content = {"png", std::move(*png)};
                } else {
                    log::warn("clipboard: could not convert the copied image");
                }
            }
        }
        if (!files.empty()) {
            const auto now = std::chrono::steady_clock::now();
            if (files == sync.last_files_ && now - sync.last_files_at_ < kSameFilesFor) {
                sync.synced_clip_seq_ = clip_seq;  // the same Ctrl+C updating the clipboard once more
                return;
            }
            if (send_files(sync, files, clip_seq)) {
                sync.last_files_ = std::move(files);
                sync.last_files_at_ = now;
            }
            return;
        }
        if (content.kind.empty() || content.bytes.empty()) {
            sync.synced_clip_seq_ = clip_seq;  // nothing we share (e.g. a format we don't know): don't retry it
            return;
        }
        if (content.bytes.size() > sync.options_.max_bytes) {
            log::warn("clipboard: copied {} of {} is too big to share (limit {})", content.kind,
                      size_text(content.bytes.size()), size_text(sync.options_.max_bytes));
            sync.synced_clip_seq_ = clip_seq;
            return;
        }
        sync.send_content(content, clip_seq);
    }

    // Files copied here: announce them to the peer, then hand them to the file channel.
    // False if they were not shared.
    static bool send_files(ClipboardSync& sync, std::vector<std::filesystem::path> files, std::uint32_t clip_seq) {
        ClipboardSync::FilesCopied callback;
        {
            std::lock_guard lock(sync.incoming_mutex_);
            callback = sync.on_files_copied_;
        }
        if (!callback) {
            log::info("clipboard: files were copied, but there is no file channel to send them");
            return false;
        }
        std::uint64_t bytes = 0;
        std::size_t count = 0;
        if (!within_limits(files, sync.options_.max_file_bytes, sync.options_.max_file_count, bytes, count)) {
            log::warn("clipboard: copied files are too big to share (limit {} / {} files) - drag them to the edge instead",
                      size_text(static_cast<std::size_t>(sync.options_.max_file_bytes)), sync.options_.max_file_count);
            sync.synced_clip_seq_ = clip_seq;
            return false;
        }
        std::string names;
        for (const auto& f : files) names += to_utf8(f.filename().wstring()) + "\n";
        sync.send_content({"files", std::vector<std::uint8_t>(names.begin(), names.end())}, clip_seq);
        log::info("clipboard: sharing {} copied file(s), {}, over the file channel", count, size_text(static_cast<std::size_t>(bytes)));
        callback(std::move(files));
        return true;
    }

    // The peer's copied files arrived: put them on this clipboard for Explorer's paste.
    static void apply_files(ClipboardSync& sync, HWND h) {
        std::optional<std::vector<std::filesystem::path>> files;
        {
            std::lock_guard lock(sync.incoming_mutex_);
            files = std::exchange(sync.files_to_apply_, std::nullopt);
        }
        if (!files || files->empty()) return;
        bool ok = false;
        {
            OpenedClipboard opened(h);
            if (!opened) {
                log::warn("clipboard: could not open the clipboard to paste the other PC's files");
                return;
            }
            EmptyClipboard();  // makes this window the owner, so the change isn't echoed back
            const DWORD copy = DROPEFFECT_COPY;  // paste copies them out of Downloads\KVM, never moves
            ok = set_data(CF_HDROP, make_hdrop(*files));
            set_data(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT), make_global(&copy, sizeof(copy)));
        }
        if (ok) {
            sync.synced_clip_seq_ = GetClipboardSequenceNumber();  // the peer has these: not sent back on reconnect
            log::info("clipboard: {} file(s) from the other PC are on the clipboard - Ctrl+V in Explorer to paste",
                      files->size());
        } else {
            log::warn("clipboard: could not put the other PC's files on the clipboard");
        }
    }

    static void apply(ClipboardSync& sync, HWND h) {
        std::optional<ClipboardSync::Content> content;
        {
            std::lock_guard lock(sync.incoming_mutex_);
            content = std::exchange(sync.to_apply_, std::nullopt);
        }
        if (!content) return;

        std::optional<Pixels> pixels;
        if (content->kind == "png" && !(pixels = decode_png(content->bytes))) {
            log::warn("clipboard: received an image that could not be decoded");
            return;
        }
        bool ok = false;
        {
            OpenedClipboard opened(h);
            if (!opened) {
                {
                    std::lock_guard lock(sync.incoming_mutex_);
                    if (!sync.to_apply_) sync.to_apply_ = std::move(content);  // unless a newer copy came meanwhile
                }
                if (++sync.apply_tries_ <= kMaxTries) {
                    SetTimer(h, kApplyTimer, kRetryMs, nullptr);  // another app holds the clipboard: try again soon
                } else {
                    log::warn("clipboard: could not open the clipboard to paste the other PC's copy");
                }
                return;
            }
            EmptyClipboard();  // makes this window the owner, so the change isn't echoed back
            if (content->kind == "text") {
                const std::wstring w = to_wide({reinterpret_cast<const char*>(content->bytes.data()), content->bytes.size()});
                ok = set_data(CF_UNICODETEXT, make_global(w.c_str(), (w.size() + 1) * sizeof(wchar_t)));
                log::info("clipboard: text from the other PC ({} characters)", w.size());
            } else {
                const bool dib = set_data(CF_DIB, make_dib(*pixels));
                const bool png = set_data(png_format(), make_global(content->bytes.data(), content->bytes.size()));
                ok = dib || png;
                log::info("clipboard: image from the other PC ({}x{}, {})", pixels->width, pixels->height,
                          size_text(content->bytes.size()));
            }
        }
        if (ok) {
            sync.synced_clip_seq_ = GetClipboardSequenceNumber();  // the peer has this: not sent back on reconnect
        } else {
            log::warn("clipboard: could not put the other PC's copy on the clipboard");
        }
    }
};

ClipboardSync::ClipboardSync(ClipboardOptions options) : options_(options) {}

ClipboardSync::~ClipboardSync() {
    stop();
}

bool ClipboardSync::start() {
    if (clipboard_thread_.joinable()) return started_ > 0;
    started_ = 0;
    clipboard_thread_ = std::thread(&ClipboardSync::clipboard_thread_main, this);
    started_.wait(0);
    if (started_ < 0) {
        clipboard_thread_.join();
        return false;
    }
    sender_ = std::thread(&ClipboardSync::sender_main, this);
    log::info("clipboard: sharing {}{}", options_.text ? "text" : "", options_.images ? (options_.text ? " and images" : "images") : "");
    return true;
}

void ClipboardSync::stop() {
    if (clipboard_thread_.joinable()) {
        if (HWND h = static_cast<HWND>(hwnd_.load())) PostMessageW(h, kMsgStop, 0, 0);
        clipboard_thread_.join();
    }
    outgoing_.close();
    if (sender_.joinable()) sender_.join();
}

void ClipboardSync::attach(Sink sink) {
    {
        std::lock_guard lock(sink_mutex_);
        sink_ = std::move(sink);
    }
    if (HWND h = static_cast<HWND>(hwnd_.load())) PostMessageW(h, kMsgResync, 0, 0);
}

void ClipboardSync::detach() {
    latest_seq_ = 0;  // drop whatever is still queued
    std::lock_guard lock(sink_mutex_);
    sink_ = nullptr;
}

void ClipboardSync::send_content(const Content& content, std::uint32_t clip_seq) {
    const std::uint32_t seq = next_seq_++;
    sending_ = (static_cast<std::uint64_t>(seq) << 32) | clip_seq;
    latest_seq_ = seq;
    for (Chunk& c : split(seq, content.kind, content.bytes)) outgoing_.push(std::move(c));
    if (content.kind != "files") {
        log::info("clipboard: sharing copied {} ({})", content.kind == "text" ? "text" : "image",
                  size_text(content.bytes.size()));
    }
}

void ClipboardSync::set_on_files_copied(FilesCopied callback) {
    std::lock_guard lock(incoming_mutex_);
    on_files_copied_ = std::move(callback);
}

bool ClipboardSync::files_received(const std::vector<std::filesystem::path>& saved) {
    if (!options_.files || saved.empty()) return false;
    std::lock_guard lock(incoming_mutex_);
    const auto now = std::chrono::steady_clock::now();
    if (now < files_expected_until_) {
        files_expected_until_ = {};
        files_to_apply_ = saved;
        if (HWND h = static_cast<HWND>(hwnd_.load())) PostMessageW(h, kMsgApplyFiles, 0, 0);
        return true;
    }
    // Maybe the announcement is still on its way (it travels on the control channel).
    files_unclaimed_ = saved;
    files_unclaimed_at_ = now;
    return false;
}

void ClipboardSync::sender_main() {
    std::uint32_t current = 0;  // the copy being sent...
    bool intact = false;        // ...and every piece of it so far went out
    while (auto chunk = outgoing_.pop()) {
        if (chunk->seq != latest_seq_) continue;  // a newer copy replaced it
        if (chunk->seq != current) {
            current = chunk->seq;
            intact = chunk->offset == 0;
        }
        bool sent = false;
        {
            // Checked again under the lock: after detach() (latest_seq_ = 0) no piece goes to the next
            // connection, which would get only the rest of a copy.
            std::lock_guard lock(sink_mutex_);
            if (sink_ && chunk->seq == latest_seq_) sent = sink_(*chunk);
        }
        intact = intact && sent;
        const bool last = chunk->offset + kChunkBytes >= chunk->total;
        if (last && intact) {
            const std::uint64_t s = sending_;
            if ((s >> 32) == chunk->seq) synced_clip_seq_ = static_cast<std::uint32_t>(s);  // the peer has it all
        }
        if (!last) std::this_thread::sleep_for(kChunkGap);
    }
}

void ClipboardSync::receive(const Chunk& chunk) {
    const auto bytes = base64_decode(chunk.data);
    if (!bytes || (chunk.kind != "text" && chunk.kind != "png" && chunk.kind != "files") ||
        chunk.total > options_.max_bytes || chunk.offset > chunk.total || bytes->size() > chunk.total - chunk.offset) {
        log::warn("clipboard: dropped a malformed piece from the other PC");
        return;
    }
    if ((chunk.kind == "text" && !options_.text) || (chunk.kind == "png" && !options_.images) ||
        (chunk.kind == "files" && !options_.files)) {
        return;
    }

    std::lock_guard lock(incoming_mutex_);
    if (chunk.seq != incoming_seq_ || chunk.kind != incoming_kind_ || incoming_.size() != chunk.total) {
        incoming_seq_ = chunk.seq;  // a new copy: start over
        incoming_kind_ = chunk.kind;
        incoming_.assign(chunk.total, 0);
        incoming_received_ = 0;
    }
    std::copy(bytes->begin(), bytes->end(), incoming_.begin() + chunk.offset);
    incoming_received_ += bytes->size();
    if (incoming_received_ < incoming_.size()) return;

    if (incoming_kind_ == "files") {
        // The peer copied files; they come over the file channel (files_received).
        const auto names = static_cast<std::size_t>(std::count(incoming_.begin(), incoming_.end(), '\n'));
        incoming_.clear();
        incoming_seq_ = 0;
        incoming_kind_.clear();
        incoming_received_ = 0;
        const auto now = std::chrono::steady_clock::now();
        log::info("clipboard: the other PC copied {} file(s), they arrive over the file channel", names);
        if (!files_unclaimed_.empty() && now - files_unclaimed_at_ < kFilesUnclaimedFor) {
            files_to_apply_ = std::exchange(files_unclaimed_, {});  // they were faster than this message
            if (HWND h = static_cast<HWND>(hwnd_.load())) PostMessageW(h, kMsgApplyFiles, 0, 0);
        } else {
            files_expected_until_ = now + kFilesExpectedFor;
        }
        return;
    }

    to_apply_ = Content{incoming_kind_, std::move(incoming_)};
    incoming_.clear();
    incoming_seq_ = 0;
    incoming_kind_.clear();
    incoming_received_ = 0;
    if (HWND h = static_cast<HWND>(hwnd_.load())) PostMessageW(h, kMsgApply, 0, 0);
}

void ClipboardSync::clipboard_thread_main() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &ClipboardThread::wnd_proc;
    wc.hInstance = inst;
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);  // fails harmlessly if already registered
    HWND h = CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, this);
    if (!h || !AddClipboardFormatListener(h)) {
        log::error("clipboard: could not watch the clipboard ({})", GetLastError());
        if (h) DestroyWindow(h);
        if (SUCCEEDED(com)) CoUninitialize();
        started_ = -1;
        started_.notify_all();
        return;
    }
    synced_clip_seq_ = GetClipboardSequenceNumber();  // what was copied before we started isn't shared
    hwnd_ = h;
    started_ = 1;
    started_.notify_all();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);

    hwnd_ = nullptr;
    DestroyWindow(h);
    if (SUCCEEDED(com)) CoUninitialize();
}

}  // namespace kvm::clip
