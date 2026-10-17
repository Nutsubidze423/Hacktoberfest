#include "kvm/clipboard.h"

#include <windows.h>
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

namespace kvm::clip {
namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT kMsgApply = WM_APP + 1;  // complete content from the peer is waiting
constexpr UINT kMsgStop = WM_APP + 2;
constexpr UINT_PTR kSettleTimer = 1;
// Apps often write several formats one after another; read once they're done.
constexpr UINT kSettleMs = 80;
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
                if (GetClipboardOwner() != h) SetTimer(h, kSettleTimer, kSettleMs, nullptr);
                return 0;
            case WM_TIMER:
                if (wp == kSettleTimer) {
                    KillTimer(h, kSettleTimer);
                    if (sync) read_and_send(*sync, h);
                }
                return 0;
            case kMsgApply:
                if (sync) apply(*sync, h);
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
            if (!sync.sink_) return;  // nobody to send to
        }
        ClipboardSync::Content content;
        {
            OpenedClipboard opened(h);
            if (!opened) {
                log::warn("clipboard: could not open the clipboard (another app holds it)");
                return;
            }
            if (GetClipboardOwner() == h) return;
            if (sync.options_.text && IsClipboardFormatAvailable(CF_UNICODETEXT)) {
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
            } else if (IsClipboardFormatAvailable(CF_HDROP)) {
                log::info("clipboard: copied files are not shared yet");
            }
        }
        if (content.kind.empty() || content.bytes.empty()) return;
        if (content.bytes.size() > sync.options_.max_bytes) {
            log::warn("clipboard: copied {} of {} is too big to share (limit {})", content.kind,
                      size_text(content.bytes.size()), size_text(sync.options_.max_bytes));
            return;
        }
        sync.send_content(content);
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
        OpenedClipboard opened(h);
        if (!opened) {
            log::warn("clipboard: could not open the clipboard to paste the other PC's copy");
            return;
        }
        EmptyClipboard();  // makes this window the owner, so the change isn't echoed back
        bool ok = false;
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
        if (!ok) log::warn("clipboard: could not put the other PC's copy on the clipboard");
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
    std::lock_guard lock(sink_mutex_);
    sink_ = std::move(sink);
}

void ClipboardSync::detach() {
    latest_seq_ = 0;  // drop whatever is still queued
    std::lock_guard lock(sink_mutex_);
    sink_ = nullptr;
}

void ClipboardSync::send_content(const Content& content) {
    const std::uint32_t seq = next_seq_++;
    latest_seq_ = seq;
    for (Chunk& c : split(seq, content.kind, content.bytes)) outgoing_.push(std::move(c));
    log::info("clipboard: sharing copied {} ({})", content.kind == "text" ? "text" : "image",
              size_text(content.bytes.size()));
}

void ClipboardSync::sender_main() {
    while (auto chunk = outgoing_.pop()) {
        if (chunk->seq != latest_seq_) continue;  // a newer copy replaced it
        {
            std::lock_guard lock(sink_mutex_);
            if (sink_) sink_(*chunk);
        }
        if (chunk->offset + kChunkBytes < chunk->total) std::this_thread::sleep_for(kChunkGap);
    }
}

void ClipboardSync::receive(const Chunk& chunk) {
    const auto bytes = base64_decode(chunk.data);
    if (!bytes || (chunk.kind != "text" && chunk.kind != "png") || chunk.total > options_.max_bytes ||
        chunk.offset > chunk.total || bytes->size() > chunk.total - chunk.offset) {
        log::warn("clipboard: dropped a malformed piece from the other PC");
        return;
    }
    if ((chunk.kind == "text" && !options_.text) || (chunk.kind == "png" && !options_.images)) return;

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
