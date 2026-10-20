// SPDX-License-Identifier: Apache-2.0

#include "capture.h"

// Classic COM headers must come before C++/WinRT so winrt::com_ptr / as<> work with them.
#include <unknwn.h>
#include <inspectable.h>

#include <d3d11.h>
#include <dxgi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include "kvm/log.h"
#include "kvm/net.h"

namespace kvm {
namespace {

namespace wgc = winrt::Windows::Graphics::Capture;
namespace d3d = winrt::Windows::Graphics::DirectX::Direct3D11;
using winrt::Windows::Foundation::Metadata::ApiInformation;

constexpr auto kPixelFormat = winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized;
constexpr int kFramePoolBuffers = 2;
constexpr wchar_t kSessionClass[] = L"Windows.Graphics.Capture.GraphicsCaptureSession";

// WinRT needs a multithreaded apartment. Keeping one alive for the whole process lets WGC be used
// from any thread, including threads that never called CoInitializeEx.
void ensure_mta() {
    static const bool ok = [] {
        CO_MTA_USAGE_COOKIE cookie{};
        return SUCCEEDED(CoIncrementMTAUsage(&cookie));
    }();
    (void)ok;
}

std::runtime_error capture_error(const char* what, const winrt::hresult_error& e) {
    return std::runtime_error(std::format("{}: 0x{:08X} {}", what, static_cast<std::uint32_t>(e.code().value),
                                          winrt::to_string(e.message())));
}

winrt::com_ptr<ID3D11Device> create_d3d_device() {
    winrt::com_ptr<ID3D11Device> device;
    // WGC hands out BGRA textures; BGRA_SUPPORT is required for them. WARP is a CPU fallback for
    // machines without a usable GPU driver (e.g. inside some VMs).
    for (const auto type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
        const HRESULT hr = D3D11CreateDevice(nullptr, type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                             D3D11_SDK_VERSION, device.put(), nullptr, nullptr);
        if (SUCCEEDED(hr)) return device;
    }
    throw std::runtime_error("D3D11CreateDevice failed (no hardware or WARP device)");
}

d3d::IDirect3DDevice to_winrt_device(const winrt::com_ptr<ID3D11Device>& device) {
    const auto dxgi = device.as<IDXGIDevice>();
    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
    return inspectable.as<d3d::IDirect3DDevice>();
}

winrt::com_ptr<ID3D11Texture2D> to_texture(const d3d::IDirect3DSurface& surface) {
    const auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::check_hresult(access->GetInterface(winrt::guid_of<ID3D11Texture2D>(), texture.put_void()));
    return texture;
}

// Without this Windows draws a yellow border around the captured window / monitor. Unpackaged apps
// are allowed to turn it off; older Windows versions don't have the property at all.
void try_disable_border(const wgc::GraphicsCaptureSession& session) {
    if (!ApiInformation::IsPropertyPresent(kSessionClass, L"IsBorderRequired")) return;
    // Asked once per process: desk mode starts a capture for every window it shows.
    static const bool allowed = [] {
        bool ok = false;
        // The access request is async and .get() must not block an STA thread, so wait on a helper thread.
        std::thread([&ok] {
            try {
                const auto status =
                    wgc::GraphicsCaptureAccess::RequestAccessAsync(wgc::GraphicsCaptureAccessKind::Borderless).get();
                ok = status == winrt::Windows::Security::Authorization::AppCapabilityAccess::AppCapabilityAccessStatus::Allowed;
            } catch (const winrt::hresult_error&) {
            }
        }).join();
        if (!ok) log::warn("capture: borderless capture not allowed, Windows will draw a border");
        return ok;
    }();
    if (!allowed) return;
    try {
        session.IsBorderRequired(false);
    } catch (const winrt::hresult_error& e) {
        log::warn("capture: can't disable the capture border: {}", winrt::to_string(e.message()));
    }
}

}  // namespace

struct ScreenCapture::Impl {
    wgc::GraphicsCaptureItem item{nullptr};
    bool capture_cursor = false;

    winrt::com_ptr<ID3D11Device> d3d_device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    d3d::IDirect3DDevice device{nullptr};

    wgc::Direct3D11CaptureFramePool pool{nullptr};
    wgc::GraphicsCaptureSession session{nullptr};
    winrt::Windows::Graphics::SizeInt32 pool_size{};
    winrt::com_ptr<ID3D11Texture2D> staging;
    D3D11_TEXTURE2D_DESC staging_desc{};

    wgc::Direct3D11CaptureFramePool::FrameArrived_revoker frame_arrived;
    wgc::GraphicsCaptureItem::Closed_revoker closed;
    FrameCallback on_frame;
    ClosedCallback on_closed;

    // Serializes the WGC callbacks with each other and with stop(); the D3D11 immediate context is
    // not thread-safe either.
    std::mutex mutex;
    bool started = false;
    bool stopped = false;

    void handle_frame();
    void handle_closed();
    void copy_to_cpu(const winrt::com_ptr<ID3D11Texture2D>& texture, int width, int height, CapturedFrame& out);
};

void ScreenCapture::Impl::handle_frame() {
    // Taken first, so the time doesn't include the copy below. Same clock as B's latency math.
    const std::int64_t arrived_us = steady_now_us();
    std::lock_guard lock(mutex);
    if (stopped) return;
    try {
        const auto frame = pool.TryGetNextFrame();
        if (!frame) return;

        const auto size = frame.ContentSize();
        if (size.Width <= 0 || size.Height <= 0) return;
        if (size.Width != pool_size.Width || size.Height != pool_size.Height) {
            // The window was resized: recreate the pool at the new size and skip this frame.
            pool_size = size;
            pool.Recreate(device, kPixelFormat, kFramePoolBuffers, size);
            return;
        }

        CapturedFrame out;
        out.t_capture_us = arrived_us;
        copy_to_cpu(to_texture(frame.Surface()), size.Width, size.Height, out);
        on_frame(std::move(out));
    } catch (const winrt::hresult_error& e) {
        log::error("capture: frame failed: 0x{:08X} {}", static_cast<std::uint32_t>(e.code().value),
                   winrt::to_string(e.message()));
    } catch (const std::exception& e) {
        log::error("capture: frame failed: {}", e.what());
    }
}

void ScreenCapture::Impl::copy_to_cpu(const winrt::com_ptr<ID3D11Texture2D>& texture, int width, int height,
                                      CapturedFrame& out) {
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (!staging || staging_desc.Width != desc.Width || staging_desc.Height != desc.Height ||
        staging_desc.Format != desc.Format) {
        D3D11_TEXTURE2D_DESC s{};
        s.Width = desc.Width;
        s.Height = desc.Height;
        s.MipLevels = 1;
        s.ArraySize = 1;
        s.Format = desc.Format;
        s.SampleDesc.Count = 1;
        s.Usage = D3D11_USAGE_STAGING;
        s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging = nullptr;
        winrt::check_hresult(d3d_device->CreateTexture2D(&s, nullptr, staging.put()));
        staging_desc = s;
    }

    context->CopyResource(staging.get(), texture.get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    winrt::check_hresult(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));

    // The pool texture can be larger than the content right after a resize.
    out.width = std::min(width, static_cast<int>(desc.Width));
    out.height = std::min(height, static_cast<int>(desc.Height));
    out.stride = out.width * 4;
    const size_t row_bytes = static_cast<size_t>(out.stride);
    out.bgra.resize(row_bytes * static_cast<size_t>(out.height));
    const auto* src = static_cast<const std::uint8_t*>(mapped.pData);
    for (int y = 0; y < out.height; ++y) {
        std::memcpy(out.bgra.data() + row_bytes * static_cast<size_t>(y), src + static_cast<size_t>(mapped.RowPitch) * y,
                    row_bytes);
    }
    context->Unmap(staging.get(), 0);
}

void ScreenCapture::Impl::handle_closed() {
    ClosedCallback callback;
    {
        std::lock_guard lock(mutex);
        if (stopped) return;
        callback = on_closed;
    }
    if (callback) callback();
}

ScreenCapture::ScreenCapture(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

ScreenCapture::~ScreenCapture() {
    stop();
}

bool ScreenCapture::is_supported() {
    ensure_mta();
    try {
        return wgc::GraphicsCaptureSession::IsSupported();
    } catch (const winrt::hresult_error&) {
        return false;
    }
}

std::unique_ptr<ScreenCapture> ScreenCapture::for_window(HWND hwnd, bool capture_cursor) {
    if (!IsWindow(hwnd)) throw std::runtime_error("capture: not a window (closed?)");
    ensure_mta();
    auto impl = std::make_unique<Impl>();
    impl->capture_cursor = capture_cursor;
    try {
        const auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        winrt::check_hresult(
            interop->CreateForWindow(hwnd, winrt::guid_of<wgc::IGraphicsCaptureItem>(), winrt::put_abi(impl->item)));
    } catch (const winrt::hresult_error& e) {
        throw capture_error("capture: can't capture this window", e);
    }
    return std::unique_ptr<ScreenCapture>(new ScreenCapture(std::move(impl)));
}

std::unique_ptr<ScreenCapture> ScreenCapture::for_monitor(HMONITOR monitor, bool capture_cursor) {
    if (!monitor) throw std::runtime_error("capture: null monitor");
    ensure_mta();
    auto impl = std::make_unique<Impl>();
    impl->capture_cursor = capture_cursor;
    try {
        const auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        winrt::check_hresult(interop->CreateForMonitor(monitor, winrt::guid_of<wgc::IGraphicsCaptureItem>(),
                                                       winrt::put_abi(impl->item)));
    } catch (const winrt::hresult_error& e) {
        throw capture_error("capture: can't capture this monitor", e);
    }
    return std::unique_ptr<ScreenCapture>(new ScreenCapture(std::move(impl)));
}

void ScreenCapture::start(LatestSlot<CapturedFrame>& out, ClosedCallback on_closed) {
    start([&out](CapturedFrame&& frame) { out.put(std::move(frame)); }, std::move(on_closed));
}

void ScreenCapture::start(FrameCallback on_frame, ClosedCallback on_closed) {
    auto& s = *impl_;
    std::lock_guard lock(s.mutex);
    if (s.started) throw std::runtime_error("capture: already started");
    s.on_frame = std::move(on_frame);
    s.on_closed = std::move(on_closed);
    try {
        s.d3d_device = create_d3d_device();
        s.d3d_device->GetImmediateContext(s.context.put());
        s.device = to_winrt_device(s.d3d_device);

        s.pool_size = s.item.Size();
        // Free-threaded: FrameArrived runs on a WGC worker thread, no message loop needed.
        s.pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(s.device, kPixelFormat, kFramePoolBuffers,
                                                                     s.pool_size);
        s.session = s.pool.CreateCaptureSession(s.item);
        if (ApiInformation::IsPropertyPresent(kSessionClass, L"IsCursorCaptureEnabled")) {
            s.session.IsCursorCaptureEnabled(s.capture_cursor);
        }
        try_disable_border(s.session);

        s.frame_arrived = s.pool.FrameArrived(winrt::auto_revoke, [impl = &s](auto&&, auto&&) { impl->handle_frame(); });
        s.closed = s.item.Closed(winrt::auto_revoke, [impl = &s](auto&&, auto&&) { impl->handle_closed(); });
        s.session.StartCapture();
        s.started = true;
    } catch (const winrt::hresult_error& e) {
        throw capture_error("capture: can't start", e);
    }
}

void ScreenCapture::stop() {
    if (!impl_) return;
    auto& s = *impl_;
    {
        // Waits for a running callback to finish; later callbacks see `stopped` and return.
        std::lock_guard lock(s.mutex);
        if (!s.started || s.stopped) return;
        s.stopped = true;
    }
    s.frame_arrived.revoke();
    s.closed.revoke();
    try {
        s.session.Close();
        s.pool.Close();
    } catch (const winrt::hresult_error&) {
    }
}

}  // namespace kvm
