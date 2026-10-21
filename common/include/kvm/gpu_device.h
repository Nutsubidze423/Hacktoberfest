// SPDX-License-Identifier: Apache-2.0

// One D3D11 device for capture, colour conversion and encode (roadmap P2.3a): frames stay on the GPU
// from Windows.Graphics.Capture to the H.264 encoder, with no copy to the CPU. The viewer decodes on
// one too (P2.4).
//
//   auto gpu = kvm::GpuDevice::create();          // nullptr: no hardware GPU (e.g. only WARP)
//   auto cap = kvm::ScreenCapture::for_window(hwnd, false, gpu);
//   kvm::Nv12Converter nv12(gpu);
//
// Its immediate context is shared by WGC's worker thread, the converter and Media Foundation's
// encoder threads, so the device is multithread-protected (ID3D10Multithread) and code that uses the
// context holds a GpuDevice::Lock.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3d10.h>
#include <wrl/client.h>

#include <memory>
#include <string>

namespace kvm {

class GpuDevice {
public:
    // The default adapter's hardware device, with video and BGRA support. nullptr (logged) when there
    // is no hardware device with video support.
    static std::shared_ptr<GpuDevice> create();

    ID3D11Device* device() const { return device_.Get(); }
    ID3D11DeviceContext* context() const { return context_.Get(); }
    const std::string& adapter_name() const { return adapter_name_; }

    // Holds the device's multithread lock: one thread at a time uses the immediate context.
    class Lock {
    public:
        explicit Lock(const GpuDevice& gpu) : mt_(gpu.multithread_.Get()) { mt_->Enter(); }
        ~Lock() { mt_->Leave(); }
        Lock(const Lock&) = delete;
        Lock& operator=(const Lock&) = delete;

    private:
        ID3D10Multithread* mt_;
    };

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D10Multithread> multithread_;
    std::string adapter_name_;
};

}  // namespace kvm
