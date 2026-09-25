// vEMstitch Workbench - a desktop interface for the vEMstitch tile stitcher.
// Copyright (C) 2026 Abdulrahman Ali
//
// This program is free software: you can redistribute it and/or modify it under the terms
// of the GNU General Public License as published by the Free Software Foundation, either
// version 3 of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
// without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with this program.
// If not, see <https://www.gnu.org/licenses/>.

// texture.cpp - Direct3D 11 textures and samplers for image display (UI layer).
// See texture.h for the threading rules: everything that touches D3D11 runs on the UI thread.

#include "texture.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <opencv2/imgproc.hpp>

#include "imgui_impl_dx11.h"

namespace ui {

namespace {

// Set by textures_init(). The device and context belong to main.cpp; only the samplers are ours.
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
// Our own samplers. The backend's default linear sampler reads only the top mip level
// (MaxLOD 0), so a mip chain would never be used without g_smooth.
ID3D11SamplerState* g_point = nullptr;
ID3D11SamplerState* g_smooth = nullptr;
int g_max_side = 8192;          // largest texture side for the device's feature level
bool g_mip_autogen = false;     // the GPU can generate mips for RGBA8 textures

// ---------------------------------------------------------------- draw callbacks

// The backend publishes its device context through Renderer_RenderState only while it is
// inside ImGui_ImplDX11_RenderDrawData(), which is exactly when these callbacks run.
void bind_sampler(ID3D11SamplerState* s) {
    auto* rs = static_cast<ImGui_ImplDX11_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
    if (rs && rs->DeviceContext && s) rs->DeviceContext->PSSetSamplers(0, 1, &s);
}
void cb_point(const ImDrawList*, const ImDrawCmd*) { bind_sampler(g_point); }
void cb_smooth(const ImDrawList*, const ImDrawCmd*) { bind_sampler(g_smooth); }

// Error text for a failed D3D11 call, with a plain message for running out of video memory.
std::string hr_text(const char* what, HRESULT hr) {
    char buf[96];
    if (hr == E_OUTOFMEMORY)
        std::snprintf(buf, sizeof(buf), "%s: out of video memory", what);
    else
        std::snprintf(buf, sizeof(buf), "%s failed (HRESULT 0x%08lX)", what, (unsigned long)hr);
    return buf;
}

} // namespace

// ---------------------------------------------------------------- setup

bool textures_init(ID3D11Device* device, ID3D11DeviceContext* context) {
    textures_shutdown();
    g_device = device;
    g_context = context;
    if (!device || !context) return false;

    // Largest texture per D3D11 feature level. main.cpp only asks for 10.0 or higher, so
    // the 4096 branch is a safety net.
    const D3D_FEATURE_LEVEL fl = device->GetFeatureLevel();
    if (fl >= D3D_FEATURE_LEVEL_11_0)
        g_max_side = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;   // 16384
    else if (fl >= D3D_FEATURE_LEVEL_10_0)
        g_max_side = 8192;
    else
        g_max_side = 4096;

    UINT support = 0;
    g_mip_autogen = SUCCEEDED(device->CheckFormatSupport(DXGI_FORMAT_R8G8B8A8_UNORM, &support)) &&
                    (support & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN) != 0;

    // g_point: nearest pixel, top level only, for zoom at or above 100 %.
    // g_smooth: trilinear across all mip levels, for zoom below 100 %.
    // Both clamp at the edges so the border pixels do not bleed in from the opposite side.
    D3D11_SAMPLER_DESC desc = {};
    desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    desc.MinLOD = 0.0f;
    desc.MaxLOD = 0.0f;
    device->CreateSamplerState(&desc, &g_point);
    desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    desc.MaxLOD = D3D11_FLOAT32_MAX;
    device->CreateSamplerState(&desc, &g_smooth);
    return g_point != nullptr && g_smooth != nullptr;
}

void textures_shutdown() {
    if (g_point) g_point->Release();
    if (g_smooth) g_smooth->Release();
    g_point = g_smooth = nullptr;
    g_device = nullptr;
    g_context = nullptr;
}

int max_texture_side() { return g_max_side; }

// ---------------------------------------------------------------- Texture

Texture::~Texture() {
    if (srv_) srv_->Release();
    if (tex_) tex_->Release();
}

// The DX11 backend takes the shader resource view as the texture id.
ImTextureID Texture::id() const { return (ImTextureID)(intptr_t)srv_; }

// 4 bytes per pixel; a full mip chain adds about a third (1/4 + 1/16 + ...).
std::size_t Texture::bytes() const {
    const std::size_t base = std::size_t(w_) * std::size_t(h_) * 4u;
    return mips_ ? base + base / 3u : base;
}

std::shared_ptr<Texture> Texture::create(const cv::Mat& rgba, int src_w, int src_h, bool mips, std::string* err) {
    auto fail = [err](const std::string& why) -> std::shared_ptr<Texture> {
        if (err) *err = why;
        return nullptr;
    };
    if (!g_device || !g_context) return fail("no graphics device");
    if (rgba.empty() || rgba.type() != CV_8UC4) return fail("internal: texture pixels must be 8-bit RGBA");
    if (rgba.cols > g_max_side || rgba.rows > g_max_side) return fail("image is larger than the largest texture");

    // Work on one contiguous block of pixels: a non-continuous Mat (such as a sub-view of a
    // larger one) is copied first.
    const cv::Mat px = rgba.isContinuous() ? rgba : rgba.clone();
    std::shared_ptr<Texture> t(new Texture());
    t->w_ = px.cols;
    t->h_ = px.rows;
    t->src_w_ = src_w > 0 ? src_w : px.cols;
    t->src_h_ = src_h > 0 ? src_h : px.rows;
    t->mips_ = mips && g_mip_autogen && (px.cols > 1 || px.rows > 1);   // a 1 x 1 image has no mips

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = (UINT)px.cols;
    desc.Height = (UINT)px.rows;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;

    HRESULT hr = E_FAIL;
    if (t->mips_) {
        // Autogenerated mips need a render-target-capable texture filled after creation.
        // MipLevels 0 means "the full chain down to 1 x 1".
        desc.MipLevels = 0;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        hr = g_device->CreateTexture2D(&desc, nullptr, &t->tex_);
        if (FAILED(hr)) t->mips_ = false;   // fall back to a single level below
    }
    if (!t->mips_) {
        // Single level: the pixels go in with the creation call and the texture never changes.
        desc.MipLevels = 1;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = 0;
        D3D11_SUBRESOURCE_DATA init = {};
        init.pSysMem = px.data;
        init.SysMemPitch = (UINT)px.step[0];
        hr = g_device->CreateTexture2D(&desc, &init, &t->tex_);
        if (FAILED(hr)) return fail(hr_text("CreateTexture2D", hr));
    }

    // View over every mip level (MipLevels -1 = all), so g_smooth can use them.
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MostDetailedMip = 0;
    sd.Texture2D.MipLevels = (UINT)-1;
    hr = g_device->CreateShaderResourceView(t->tex_, &sd, &t->srv_);
    if (FAILED(hr)) return fail(hr_text("CreateShaderResourceView", hr));

    // Mip path: upload the top level, then let the GPU compute the smaller ones. This uses
    // the immediate context, which is why textures are made on the UI thread only.
    if (t->mips_) {
        g_context->UpdateSubresource(t->tex_, 0, nullptr, px.data, (UINT)px.step[0], 0);
        g_context->GenerateMips(t->srv_);
    }
    return t;
}

// ---------------------------------------------------------------- sampler switches

void draw_sampler_nearest(ImDrawList* dl) {
    if (dl && g_point) dl->AddCallback(cb_point, nullptr);
}

void draw_sampler_smooth(ImDrawList* dl) {
    if (dl && g_smooth) dl->AddCallback(cb_smooth, nullptr);
}

// Prefers the backend's full render-state reset; falls back to just its linear sampler if
// the reset callback is not provided.
void draw_sampler_reset(ImDrawList* dl) {
    if (!dl) return;
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    if (pio.DrawCallback_ResetRenderState)
        dl->AddCallback(pio.DrawCallback_ResetRenderState, nullptr);
    else if (pio.DrawCallback_SetSamplerLinear)
        dl->AddCallback(pio.DrawCallback_SetSamplerLinear, nullptr);
}

// ---------------------------------------------------------------- CPU-side pixel preparation

cv::Mat to_rgba8(const cv::Mat& src) {
    CV_Assert(!src.empty());
    cv::Mat img = src;
    // 2 channels or more than 4 have no colour meaning here: show the first channel.
    if (img.channels() == 2 || img.channels() > 4) {
        cv::Mat first;
        cv::extractChannel(img, first, 0);
        img = first;
    }
    // Stretch the image's own min..max to 0..255 (one range for all channels), so 12-bit data
    // stored in 16 bits, or float data, uses the full grey range.
    if (img.depth() != CV_8U) {
        double lo = 0.0, hi = 0.0;
        cv::minMaxLoc(img.reshape(1), &lo, &hi);
        const double scale = hi > lo ? 255.0 / (hi - lo) : 1.0;
        cv::Mat eight;
        img.convertTo(eight, CV_MAKETYPE(CV_8U, img.channels()), scale, -lo * scale);
        img = eight;
    }
    // OpenCV stores colour as BGR(A); the texture wants RGBA with opaque alpha.
    cv::Mat out;
    switch (img.channels()) {
    case 1:
        cv::cvtColor(img, out, cv::COLOR_GRAY2RGBA);
        break;
    case 3:
        cv::cvtColor(img, out, cv::COLOR_BGR2RGBA);
        break;
    default: {
        // Drop the alpha channel: a transparent EM image would just look empty.
        cv::Mat bgr;
        cv::cvtColor(img, bgr, cv::COLOR_BGRA2BGR);
        cv::cvtColor(bgr, out, cv::COLOR_BGR2RGBA);
        break;
    }
    }
    return out;
}

// INTER_AREA averages the source pixels under each output pixel, which avoids the moire
// that plain resampling gives when shrinking a lot.
cv::Mat fit_within(const cv::Mat& img, int max_w, int max_h) {
    if (img.empty() || max_w <= 0 || max_h <= 0) return img;
    if (img.cols <= max_w && img.rows <= max_h) return img;
    const double s = std::min(double(max_w) / img.cols, double(max_h) / img.rows);
    const int w = std::min(max_w, std::max(1, (int)std::lround(img.cols * s)));
    const int h = std::min(max_h, std::max(1, (int)std::lround(img.rows * s)));
    cv::Mat out;
    cv::resize(img, out, cv::Size(w, h), 0, 0, cv::INTER_AREA);
    return out;
}

} // namespace ui
