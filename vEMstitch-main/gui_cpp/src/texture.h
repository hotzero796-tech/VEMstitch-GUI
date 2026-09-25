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

#pragma once
// texture.h - putting images on screen (UI layer).
//
// Dear ImGui draws pictures from Direct3D 11 textures. This file turns a decoded OpenCV image
// into such a texture (Texture::create), provides the CPU-side steps that prepare the pixels
// for it (to_rgba8, fit_within), and lets the viewer pick the texture sampler per image:
// sharp pixels when zoomed in, a smooth mip-mapped look when zoomed out.
//
// Threading: Texture objects must be created and destroyed on the UI thread (creation uses
// the D3D11 immediate context, which is not thread-safe). to_rgba8() and fit_within() are
// pure OpenCV and may run on any thread; the image loader thread calls them.

#include <memory>
#include <string>

#include <opencv2/core.hpp>

#include "imgui.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;

namespace ui {

// main.cpp calls textures_init() once the device exists and textures_shutdown() before it
// releases the device. init reads the device's limits and creates the two samplers below.
bool textures_init(ID3D11Device* device, ID3D11DeviceContext* context);
void textures_shutdown();
// Largest texture width or height the GPU accepts. Bigger images (a large mosaic) are
// downscaled to fit before upload.
int max_texture_side();   // 16384 on feature level 11, 8192 on 10.x

// One image on the GPU. Handed out as a shared_ptr (the image views and the thumbnail cache
// hold it). Not copyable because it owns the D3D11 objects, which the destructor releases.
class Texture {
public:
    ~Texture();
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    // rgba: CV_8UC4 in R,G,B,A byte order. src_w/src_h: size of the original image when
    // rgba is a downscaled copy (pass rgba's own size otherwise). mips: build a full mip
    // chain so strong minification (a 5818 px mosaic shown at 10 %) does not alias. The mip
    // chain is quietly skipped if the GPU cannot generate one. Returns nullptr on failure,
    // with the reason in *err.
    static std::shared_ptr<Texture> create(const cv::Mat& rgba, int src_w, int src_h, bool mips,
                                           std::string* err = nullptr);

    ImTextureID id() const;                        // what ImGui's AddImage / Image take
    int width() const { return w_; }               // size of the texture itself
    int height() const { return h_; }
    int src_width() const { return src_w_; }       // size of the original image
    int src_height() const { return src_h_; }
    bool has_mips() const { return mips_; }
    std::size_t bytes() const;                     // approximate video memory used

private:
    Texture() = default;   // only create() makes one
    ID3D11Texture2D* tex_ = nullptr;
    ID3D11ShaderResourceView* srv_ = nullptr;
    int w_ = 0, h_ = 0, src_w_ = 0, src_h_ = 0;
    bool mips_ = false;
};

// Draw-list sampler switches. Everything drawn after one of the first two uses that sampler
// until draw_sampler_reset(), which restores the backend's default render state.
void draw_sampler_nearest(ImDrawList* dl);   // point sampling, for zoom at or above 100 %
void draw_sampler_smooth(ImDrawList* dl);    // trilinear over the mip chain, for zoom below 100 %
void draw_sampler_reset(ImDrawList* dl);

// Any depth (8/16-bit integer, float) and 1-4 channels -> CV_8UC4 RGBA. Non-8-bit data is
// stretched min..max so 12/16-bit EM data is visible. Throws cv::Exception on bad input.
// Display only: the stitch always works on the original pixels.
cv::Mat to_rgba8(const cv::Mat& src);

// Area-downscale so the image fits max_w x max_h; never upscales. Works for any type.
// Returns the input itself (no copy) when it already fits.
cv::Mat fit_within(const cv::Mat& img, int max_w, int max_h);

} // namespace ui
