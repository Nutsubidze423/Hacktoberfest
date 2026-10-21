// SPDX-License-Identifier: Apache-2.0

#include "kvm/gpu_device.h"

#include <dxgi.h>

#include <format>

#include "kvm/log.h"

namespace kvm {

namespace {

std::string narrow(const wchar_t* s) {
    const int len = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string out(len > 1 ? static_cast<std::size_t>(len - 1) : 0, '\0');
    if (len > 1) WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), len, nullptr, nullptr);
    return out;
}

}  // namespace

std::shared_ptr<GpuDevice> GpuDevice::create() {
    auto gpu = std::shared_ptr<GpuDevice>(new GpuDevice());
    // VIDEO_SUPPORT: the video processor (NV12) and Media Foundation's encoders need it; BGRA_SUPPORT:
    // WGC hands out BGRA textures.
    const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                         D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr,
                                         0, D3D11_SDK_VERSION, &gpu->device_, nullptr, &gpu->context_);
    if (FAILED(hr)) {
        log::warn("gpu: no hardware D3D11 device with video support (0x{:08X})", static_cast<std::uint32_t>(hr));
        return nullptr;
    }
    if (FAILED(gpu->device_.As(&gpu->multithread_))) {
        log::warn("gpu: the device can't be shared between threads");
        return nullptr;
    }
    gpu->multithread_->SetMultithreadProtected(TRUE);

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(gpu->device_.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->GetDesc(&desc))) {
        gpu->adapter_name_ = narrow(desc.Description);
    }
    return gpu;
}

}  // namespace kvm
