// SPDX-License-Identifier: Apache-2.0

// A window's D3D11 flip-model swapchain with Direct2D drawing on it (roadmap P2.4b): what the viewer
// draws its picture, strip and HUD on. The picture can be a texture on the same device (a decoded
// H.264 picture, wrap()) or pixels from memory (a decoded JPEG), and either way it stays on the GPU.
//
//   auto target = kvm::SwapchainTarget::create(hwnd, gpu, why);  // gpu: GpuDevice::create() or _software()
//   if (ID2D1DeviceContext* dc = target->begin_draw()) {          // waits for the swapchain, at most a frame
//       ... draw with dc (96 dpi: one unit is one pixel) ...
//       if (!target->end_draw()) recreate();                      // the device was lost
//   }
//   target->resize(w, h);                                         // WM_SIZE
//
// Flip model (DXGI_SWAP_EFFECT_FLIP_DISCARD) with a frame-latency waitable object and a latency of
// one frame: a frame is drawn only when the swapchain can show it next, so none waits in a queue.
// Between begin_draw() and end_draw() the device's lock is held (GpuDevice::Lock): the decoder's
// threads use the same device. UI thread only.
#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <memory>
#include <string>

#include "kvm/gpu_device.h"

namespace kvm {

class SwapchainTarget {
public:
    // nullptr with `why` set if the swapchain or Direct2D on it can't be made.
    static std::unique_ptr<SwapchainTarget> create(HWND hwnd, std::shared_ptr<GpuDevice> gpu, std::string& why);
    ~SwapchainTarget();
    SwapchainTarget(const SwapchainTarget&) = delete;
    SwapchainTarget& operator=(const SwapchainTarget&) = delete;

    // Starts a frame: nullptr if it can't (the device was lost: make a new target).
    ID2D1DeviceContext* begin_draw();
    // Ends the frame and shows it. False if the device was lost.
    bool end_draw();
    // The window's client size changed (WM_SIZE).
    void resize(UINT width, UINT height);

    // A bitmap to draw a BGRA texture of this device with (it needs D3D11_BIND_SHADER_RESOURCE).
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> wrap(ID3D11Texture2D* bgra);

    const std::shared_ptr<GpuDevice>& gpu() const { return gpu_; }

private:
    SwapchainTarget() = default;
    bool bind_back_buffer();

    std::shared_ptr<GpuDevice> gpu_;
    Microsoft::WRL::ComPtr<IDXGISwapChain2> swapchain_;
    Microsoft::WRL::ComPtr<ID2D1Device> d2d_device_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> dc_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> back_buffer_;
    HANDLE frame_ready_ = nullptr;  // the swapchain's frame-latency waitable object
    std::unique_ptr<GpuDevice::Lock> lock_;
};

}  // namespace kvm
