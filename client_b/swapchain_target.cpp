// SPDX-License-Identifier: Apache-2.0

#include "swapchain_target.h"

#include <format>
#include <utility>

#include "kvm/log.h"
#include "ui_draw.h"

namespace kvm {

namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT kBuffers = 2;
constexpr UINT kFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
constexpr DWORD kFrameWaitMs = 100;  // longer than any frame; a stuck swapchain doesn't hang the UI

std::string hr_text(HRESULT hr) {
    return std::format("0x{:08X}", static_cast<std::uint32_t>(hr));
}

}  // namespace

std::unique_ptr<SwapchainTarget> SwapchainTarget::create(HWND hwnd, std::shared_ptr<GpuDevice> gpu, std::string& why) {
    if (!gpu) {
        why = "swapchain: no D3D11 device";
        return nullptr;
    }
    auto t = std::unique_ptr<SwapchainTarget>(new SwapchainTarget());
    t->gpu_ = std::move(gpu);

    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(t->gpu_->device()->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        why = "swapchain: no DXGI factory for the device";
        return nullptr;
    }
    DXGI_SWAP_CHAIN_DESC1 desc{};  // width and height 0: the window's client size
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = kBuffers;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = kFlags;
    ComPtr<IDXGISwapChain1> swapchain;
    HRESULT hr = factory->CreateSwapChainForHwnd(t->gpu_->device(), hwnd, &desc, nullptr, nullptr, &swapchain);
    if (FAILED(hr) || FAILED(swapchain.As(&t->swapchain_))) {
        why = "swapchain: CreateSwapChainForHwnd failed (" + hr_text(hr) + ")";
        return nullptr;
    }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);  // the viewer has its own full screen
    t->swapchain_->SetMaximumFrameLatency(1);
    t->frame_ready_ = t->swapchain_->GetFrameLatencyWaitableObject();

    // Direct2D on the same device, drawing into the swapchain's back buffer.
    ComPtr<ID2D1Factory1> d2d;
    if (FAILED(ui::Graphics::get().d2d()->QueryInterface(IID_PPV_ARGS(&d2d))) ||
        FAILED(d2d->CreateDevice(dxgi.Get(), &t->d2d_device_)) ||
        FAILED(t->d2d_device_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &t->dc_))) {
        why = "swapchain: Direct2D can't draw on the device";
        return nullptr;
    }
    t->dc_->SetDpi(96.0f, 96.0f);                                       // one unit = one physical pixel
    t->dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);  // opaque target
    return t;
}

SwapchainTarget::~SwapchainTarget() {
    lock_.reset();
    if (dc_) dc_->SetTarget(nullptr);
    back_buffer_.Reset();
    if (frame_ready_) CloseHandle(frame_ready_);
}

bool SwapchainTarget::bind_back_buffer() {
    ComPtr<IDXGISurface> surface;
    if (FAILED(swapchain_->GetBuffer(0, IID_PPV_ARGS(&surface)))) return false;
    const auto props =
        D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
    if (FAILED(dc_->CreateBitmapFromDxgiSurface(surface.Get(), &props, &back_buffer_))) return false;
    dc_->SetTarget(back_buffer_.Get());
    return true;
}

ID2D1DeviceContext* SwapchainTarget::begin_draw() {
    // Only draw when the swapchain can show the frame next: nothing queues behind it.
    if (frame_ready_) WaitForSingleObjectEx(frame_ready_, kFrameWaitMs, FALSE);
    lock_ = std::make_unique<GpuDevice::Lock>(*gpu_);
    if (!back_buffer_ && !bind_back_buffer()) {
        lock_.reset();
        return nullptr;
    }
    dc_->BeginDraw();
    dc_->SetTransform(D2D1::Matrix3x2F::Identity());
    return dc_.Get();
}

bool SwapchainTarget::end_draw() {
    const HRESULT drawn = dc_->EndDraw();
    // Interval 0: shown at the next composition without waiting for a vblank here (flip model never
    // tears in a window); the waitable object already keeps the queue at one frame.
    const HRESULT shown = SUCCEEDED(drawn) ? swapchain_->Present(0, 0) : drawn;
    lock_.reset();
    if (drawn == D2DERR_RECREATE_TARGET || shown == DXGI_ERROR_DEVICE_REMOVED || shown == DXGI_ERROR_DEVICE_RESET) {
        log::warn("viewer: the GPU device was lost ({}); drawing on a new one", hr_text(FAILED(drawn) ? drawn : shown));
        return false;
    }
    return true;
}

void SwapchainTarget::resize(UINT width, UINT height) {
    if (width == 0 || height == 0) return;  // minimized
    const GpuDevice::Lock lock(*gpu_);
    dc_->SetTarget(nullptr);
    back_buffer_.Reset();  // every reference to the buffers goes before ResizeBuffers
    const HRESULT hr = swapchain_->ResizeBuffers(kBuffers, width, height, DXGI_FORMAT_UNKNOWN, kFlags);
    if (FAILED(hr)) log::warn("viewer: ResizeBuffers failed ({})", hr_text(hr));
}

ComPtr<ID2D1Bitmap1> SwapchainTarget::wrap(ID3D11Texture2D* bgra) {
    ComPtr<ID2D1Bitmap1> bitmap;
    ComPtr<IDXGISurface> surface;
    if (!bgra || FAILED(bgra->QueryInterface(IID_PPV_ARGS(&surface)))) return bitmap;
    const auto props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
    dc_->CreateBitmapFromDxgiSurface(surface.Get(), &props, &bitmap);
    return bitmap;
}

}  // namespace kvm
