// ======================================================================================
// app.cpp - the vEMstitch Workbench user interface: everything drawn inside the window.
//
// vEMstitch Workbench is a native Windows GUI (C++, Dear ImGui on Win32 + Direct3D 11) for
// the vEMstitch volume-EM tile-stitching algorithm: it stitches the tiles of one section
// into a mosaic. This file is the UI layer. The other parts are:
//   main.cpp      entry point: window, D3D11 device, ImGui context, message loop
//   engine.cpp    scans a tile folder and runs the authors' stitching code on worker
//                 threads, reporting progress as a queue of vem::Event
//   platform.cpp  Win32 helpers: UTF-8 paths, file I/O, shell dialogs
//   texture.cpp   D3D11 textures and the pixel conversions that prepare them
//
// Things to know before reading:
//  - Dear ImGui is "immediate mode": there are no retained widget objects. Every frame,
//    draw() rebuilds the whole window from the state kept in App::Impl.
//  - Threads. Everything in this file runs on the UI thread, except Loader::run(), which
//    decodes images on its own thread. The engine's worker threads never touch UI state:
//    the UI polls their event queue once per frame (process_runner). D3D11 textures are
//    created and released on the UI thread only.
//  - DPI. Sizes are written in logical pixels (96 dpi) and multiplied by S(). The app is
//    per-monitor DPI aware (app.manifest) and scales fonts and sizes itself; otherwise
//    Windows would bitmap-scale the window and it would look blurry at 125 %.
//  - Paths are UTF-8 std::string everywhere. Files are read with plat::read_file (a
//    wide-char file API) and decoded with cv::imdecode, so non-English folder names work.
//  - Actions that replace data or open a blocking dialog are queued with defer() and run
//    after the frame's widgets have been drawn.
//  - The app starts empty. The folder used last time only seeds the Open dialogs.
//    Settings live in %LOCALAPPDATA%\vEMstitchWorkbench\settings_cpp.ini.
//  - vEMstitch handles only 2 x 2 and 3 x 3 grids. Cancel only takes effect between steps,
//    because their calls cannot be interrupted (a Refine merge can take over an hour).
// ======================================================================================

// Enables the ImVec2 arithmetic operators (+, -, *) used below; must come before imgui.h.
#define IMGUI_DEFINE_MATH_OPERATORS
#include "app.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "imgui.h"
#include "imgui_internal.h"   // ImClamp and other small helpers not in the public header

#include "engine.h"
#include "platform.h"
#include "texture.h"

namespace {

using Clock = std::chrono::steady_clock;   // for durations; wall-clock times use system_clock

// --------------------------------------------------------------------------------------
// constants, palette and small helpers
// --------------------------------------------------------------------------------------

constexpr const char* kAppName = "vEMstitch Workbench";
constexpr const char* kAppVersion = "0.33";   // shown in the Log banner and in Help > About

constexpr float kFontBase = 13.0f;    // ~9.75 pt at 96 dpi; scaled by style.FontScaleDpi
constexpr float kFontSmall = 11.0f;   // tile captions, band notes, "loading" badges
constexpr float kMinZoom = 0.05f;     // image viewer zoom range: 5 % ...
constexpr float kMaxZoom = 8.0f;      // ... to 800 %
constexpr float kZoomStep = 1.25f;    // one step of the zoom buttons, shortcuts and Ctrl + wheel
constexpr int kThumbSide = 256;       // tile-map thumbnails are decoded this big at most (drawn smaller, mipmapped)
constexpr size_t kLogMax = 5000;      // the Log drops its oldest lines beyond this
// Loader slots: tell which view a decoded image belongs to. All tile-map thumbnails share
// one slot so pending ones can be dropped together when a new folder is scanned.
constexpr int kSlotTiles = 0, kSlotMosaic = 1, kSlotTileThumbs = 10;

// 0xRRGGBB (as written in a style sheet) to ImGui's packed colour.
constexpr ImU32 hexcol(unsigned rgb, unsigned a = 255) {
    return IM_COL32((rgb >> 16) & 255u, (rgb >> 8) & 255u, rgb & 255u, a);
}

// The light blue colour palette. apply_style() maps it onto ImGui's style colours, and the
// hand-drawn parts (bands, tile map, viewer, status bar) use it directly.
namespace pal {
constexpr ImU32 blue900 = hexcol(0x123a5e);
constexpr ImU32 blue700 = hexcol(0x1f5f96);
constexpr ImU32 blue500 = hexcol(0x2f83c4);
constexpr ImU32 blue300 = hexcol(0x8fc3e6);
constexpr ImU32 blue100 = hexcol(0xdceefa);
constexpr ImU32 blue050 = hexcol(0xeef7fd);
constexpr ImU32 line = hexcol(0xb9ccdb);
constexpr ImU32 line_soft = hexcol(0xdbe6ee);
constexpr ImU32 hatch = hexcol(0xcfdbe6);
constexpr ImU32 text = hexcol(0x14293a);
constexpr ImU32 muted = hexcol(0x5b7186);
constexpr ImU32 ok = hexcol(0x1f7a45);
constexpr ImU32 warn = hexcol(0x9a6212);
constexpr ImU32 bad = hexcol(0xa52626);
constexpr ImU32 surface = hexcol(0xffffff);
constexpr ImU32 panel = hexcol(0xf4f9fd);
constexpr ImU32 toolbar = hexcol(0xf4f8fb);
constexpr ImU32 window = hexcol(0xf0f4f8);
constexpr ImU32 log_bg = hexcol(0xfbfdff);
constexpr ImU32 log_fg = hexcol(0x20384c);
constexpr ImU32 check_a = hexcol(0xf7fbfe);
constexpr ImU32 check_b = hexcol(0xeef4f9);
constexpr ImU32 alert_bg = hexcol(0xfdecec);
constexpr ImU32 black = hexcol(0x000000);
constexpr ImU32 white = hexcol(0xffffff);
} // namespace pal

// Packed colour to the float form that style colours and PushStyleColor take.
ImVec4 v4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

// printf into a std::string. Short results use the stack buffer; longer ones (long paths)
// are formatted a second time into a buffer of the right size.
std::string strf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return std::string();
    if (n < (int)sizeof(buf)) return std::string(buf, (size_t)n);
    std::string out((size_t)n + 1, '\0');
    va_start(ap, fmt);
    std::vsnprintf(&out[0], out.size(), fmt, ap);
    va_end(ap);
    out.resize((size_t)n);
    return out;
}

// Strips spaces, tabs and line breaks at both ends.
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

// ASCII-only lower case; enough for settings keys and values. UTF-8 bytes pass unchanged.
std::string lower(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

// Zoom as a percentage; one decimal below 10 % so small zoom levels stay distinguishable.
std::string fmt_zoom(float z) { return z < 0.1f ? strf("%.1f%%", z * 100.0f) : strf("%.0f%%", z * 100.0f); }

// File size in B / KB / MB / GB (1024-based).
std::string fmt_bytes(std::uint64_t n) {
    const double v = double(n);
    if (v < 1024.0) return strf("%.0f B", v);
    if (v < 1024.0 * 1024.0) return strf("%.1f KB", v / 1024.0);
    if (v < 1024.0 * 1024.0 * 1024.0) return strf("%.1f MB", v / (1024.0 * 1024.0));
    return strf("%.2f GB", v / (1024.0 * 1024.0 * 1024.0));
}

// cv::Exception::what() also carries OpenCV's source file, function and line; e.err is just
// the message, which reads better in the Log.
std::string cv_error_text(const cv::Exception& e) { return e.err.empty() ? std::string(e.what()) : e.err; }

// Text colour of a Log line.
ImU32 level_color(vem::Level l) {
    switch (l) {
    case vem::Level::Ok: return pal::ok;
    case vem::Level::Warn: return pal::warn;
    case vem::Level::Error: return pal::bad;
    default: return pal::log_fg;
    }
}

// Colour of a step status in the Steps table. The engine reports "ok", "shortcut" (their
// preprocess already gave the result, or a missing tile was passed through), "failed" or
// "cancelled"; "running" and anything else are shown muted.
ImU32 status_color(const std::string& s) {
    if (s == "ok") return pal::ok;
    if (s == "shortcut") return pal::blue700;
    if (s == "failed") return pal::bad;
    if (s == "cancelled") return pal::warn;
    return pal::muted;
}

// --------------------------------------------------------------------------------------
// long texts
// --------------------------------------------------------------------------------------

// Shown under the Refine checkbox. Refine is the authors' feature re-extraction pass for the
// row merges; in their C++ it can take over an hour per section.
const char* kRefineNote =
    "The authors recommend stitching without Refine first - it is faster and more robust. "
    "Enable Refine only for sections where a seam is still visible, then re-run those.";

// --------------------------------------------------------------------------------------
// settings
// --------------------------------------------------------------------------------------
// A small key=value text file, read at startup and written by persist(). It is parsed by
// hand: unknown keys and bad or out-of-range values are ignored, so a damaged file simply
// falls back to the defaults.

struct Settings {
    std::string input, output;   // input: last dataset folder, only seeds the Open dialogs
    int pattern = 3;             // 2 or 3 (grid size; the only two vEMstitch supports)
    bool refine = false;
    float left_w = 270.0f, right_w = 390.0f, log_h = 170.0f;   // logical px
};

// %LOCALAPPDATA%\vEMstitchWorkbench\settings_cpp.ini, or next to the exe if LOCALAPPDATA is unknown.
std::string settings_file() {
    std::string base = plat::local_appdata();
    if (base.empty()) base = plat::exe_dir();
    return plat::join(plat::join(base, "vEMstitchWorkbench"), "settings_cpp.ini");
}

// The parse_* helpers return def unless the whole value is valid and inside [lo, hi].
bool parse_bool(const std::string& v, bool def) {
    const std::string s = lower(v);
    if (s == "1" || s == "true" || s == "yes" || s == "on") return true;
    if (s == "0" || s == "false" || s == "no" || s == "off") return false;
    return def;
}

int parse_int(const std::string& v, int def, int lo, int hi) {
    if (v.empty()) return def;
    char* end = nullptr;
    const long x = std::strtol(v.c_str(), &end, 10);
    if (!end || *end != '\0' || x < lo || x > hi) return def;
    return (int)x;
}

float parse_float(const std::string& v, float def, float lo, float hi) {
    if (v.empty()) return def;
    char* end = nullptr;
    const double x = std::strtod(v.c_str(), &end);
    if (!end || *end != '\0' || !std::isfinite(x) || x < lo || x > hi) return def;
    return (float)x;
}

// A missing or unreadable file, or one over 1 MB (far too big for these few keys), gives
// all defaults.
Settings load_settings() {
    Settings s;
    std::vector<unsigned char> raw;
    if (!plat::read_file(settings_file(), raw) || raw.size() > (1u << 20)) return s;
    const std::string textblob(raw.begin(), raw.end());
    size_t pos = 0;
    while (pos < textblob.size()) {
        size_t eol = textblob.find('\n', pos);
        if (eol == std::string::npos) eol = textblob.size();
        const std::string ln = trim(textblob.substr(pos, eol - pos));
        pos = eol + 1;
        // Skip blank lines, # and ; comments, and [section] headers.
        if (ln.empty() || ln[0] == '#' || ln[0] == ';' || ln[0] == '[') continue;
        const size_t eq = ln.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = lower(trim(ln.substr(0, eq)));
        const std::string val = trim(ln.substr(eq + 1));
        if (key == "input") s.input = val;
        else if (key == "output") s.output = val;
        else if (key == "pattern") s.pattern = parse_int(val, s.pattern, 2, 3);
        else if (key == "refine") s.refine = parse_bool(val, s.refine);
        else if (key == "left_w") s.left_w = parse_float(val, s.left_w, 150.0f, 900.0f);
        else if (key == "right_w") s.right_w = parse_float(val, s.right_w, 360.0f, 900.0f);
        else if (key == "log_h") s.log_h = parse_float(val, s.log_h, 60.0f, 900.0f);
    }
    return s;
}

// Rewrites the whole file (creating the folder if needed). Paths are stored as UTF-8.
bool save_settings(const Settings& s) {
    const std::string path = settings_file();
    if (!plat::make_dirs(plat::parent_dir(path))) return false;
    std::string out;
    out += "# vEMstitch Workbench (C++) settings\n";
    out += "input=" + s.input + "\n";
    out += "output=" + s.output + "\n";
    out += strf("pattern=%d\n", s.pattern);
    out += strf("refine=%d\n", s.refine ? 1 : 0);
    out += strf("left_w=%.0f\nright_w=%.0f\nlog_h=%.0f\n", s.left_w, s.right_w, s.log_h);
    return plat::write_file(path, std::vector<unsigned char>(out.begin(), out.end()));
}

// --------------------------------------------------------------------------------------
// background loader: decodes images off the UI thread and hands RGBA pixels back
// --------------------------------------------------------------------------------------
// Reading and decoding a large tile or a mosaic of several thousand pixels a side takes
// noticeable time, so it happens on this thread and the UI stays responsive. The Loader
// only produces CPU pixels (a cv::Mat in RGBA order). The UI thread turns them into D3D11
// textures in process_loader(), because textures may only be created on the UI thread.
// Every job carries a ticket; the UI ignores results whose ticket is no longer current.

enum class JobKind { Image, Thumb };   // Image: a full view image; Thumb: a tile-map thumbnail

struct Job {
    JobKind kind = JobKind::Image;
    int slot = 0;                // kSlot*: which view (or the thumbnail cache) asked
    std::uint64_t ticket = 0;    // matched against the requester's ticket when the result comes back
    std::string path;
    int max_w = 0, max_h = 0;    // the result is area-downscaled to fit this box
};

struct JobResult {
    Job job;
    bool ok = false;
    std::string error;
    cv::Mat rgba;                // CV_8UC4, R,G,B,A byte order; empty on failure
    int src_w = 0, src_h = 0;    // size of the file's image before downscaling
    std::uint64_t bytes = 0;     // file size, for the Log
};

// One worker thread with two queues. Image jobs are always taken before thumbnails, so the
// image the user just clicked is not stuck behind a batch of tile-map thumbnails.
class Loader {
public:
    explicit Loader(std::function<void()> wake) : wake_(std::move(wake)) {
        thread_ = std::thread([this]() { run(); });
    }
    // Stops the thread and joins it; a decode already in progress finishes first.
    ~Loader() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
    Loader(const Loader&) = delete;
    Loader& operator=(const Loader&) = delete;

    // A newer image request for a slot replaces an older one that has not started yet, so
    // clicking quickly through tiles decodes only the latest one. Thumbnails just queue up.
    void submit(const Job& job) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (job.kind == JobKind::Image) {
                images_.erase(std::remove_if(images_.begin(), images_.end(),
                                             [&](const Job& j) { return j.slot == job.slot; }),
                              images_.end());
                images_.push_back(job);
            } else {
                thumbs_.push_back(job);
            }
        }
        cv_.notify_one();
    }

    // Forgets thumbnail jobs of a slot that have not started (a new folder was scanned).
    void drop_thumbs(int slot) {
        std::lock_guard<std::mutex> lock(mu_);
        thumbs_.erase(std::remove_if(thumbs_.begin(), thumbs_.end(), [&](const Job& j) { return j.slot == slot; }),
                      thumbs_.end());
    }

    // Takes one finished result, if any. Never blocks; called by the UI thread.
    bool pop(JobResult& out) {
        std::lock_guard<std::mutex> lock(mu_);
        if (results_.empty()) return false;
        out = std::move(results_.front());
        results_.pop_front();
        return true;
    }

    // Anything queued, being decoded or waiting to be collected. While true the main loop
    // keeps drawing frames, so every result is picked up and shown promptly.
    bool busy() const {
        std::lock_guard<std::mutex> lock(mu_);
        return in_flight_ || !images_.empty() || !thumbs_.empty() || !results_.empty();
    }

private:
    // Thread body. Every failure becomes an error text in the result: an exception that
    // escaped a std::thread would call std::terminate and end the whole program.
    void run() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait(lock, [&]() { return stop_ || !images_.empty() || !thumbs_.empty(); });
                if (stop_) return;
                std::deque<Job>& q = !images_.empty() ? images_ : thumbs_;
                job = std::move(q.front());
                q.pop_front();
                in_flight_ = true;
            }
            JobResult r;
            r.job = job;
            try {
                execute(job, r);
            } catch (const cv::Exception& e) {
                r.ok = false;
                r.error = cv_error_text(e);
            } catch (const std::bad_alloc&) {
                r.ok = false;
                r.error = "out of memory";
            } catch (const std::exception& e) {
                r.ok = false;
                r.error = e.what();
            } catch (...) {
                r.ok = false;
                r.error = "unknown error";
            }
            if (!r.ok) r.rgba.release();
            {
                std::lock_guard<std::mutex> lock(mu_);
                results_.push_back(std::move(r));
                in_flight_ = false;
            }
            if (wake_) wake_();   // the UI loop may be asleep waiting for input
        }
    }

    // Reads the file with plat::read_file (wide-char path, so any folder name works) and
    // decodes it from memory. IMREAD_UNCHANGED keeps 16-bit and multi-channel data as they
    // are; ui::to_rgba8 later stretches them for display.
    static cv::Mat decode(const std::string& path, JobResult& r) {
        std::vector<unsigned char> bytes;
        std::string err;
        if (!plat::read_file(path, bytes, &err)) {
            r.error = err;
            return cv::Mat();
        }
        r.bytes = bytes.size();
        cv::Mat img = cv::imdecode(bytes, cv::IMREAD_UNCHANGED);
        if (img.empty()) r.error = "not an image OpenCV can decode";
        return img;
    }

    // Decode, shrink to the job's box, convert to RGBA. Shrinking first means only the small
    // copy is converted; the reference to the full-size image is dropped straight after.
    static void execute(const Job& job, JobResult& r) {
        cv::Mat img = decode(job.path, r);
        if (img.empty()) return;
        r.src_w = img.cols;
        r.src_h = img.rows;
        cv::Mat small = ui::fit_within(img, job.max_w, job.max_h);
        img.release();
        r.rgba = ui::to_rgba8(small);
        r.ok = !r.rgba.empty();
        if (!r.ok) r.error = "conversion to RGBA failed";
    }

    std::function<void()> wake_;
    mutable std::mutex mu_;            // guards everything below except thread_
    std::condition_variable cv_;       // signalled on submit and on stop
    std::deque<Job> images_, thumbs_;
    std::deque<JobResult> results_;
    bool stop_ = false;
    bool in_flight_ = false;           // a job has been taken and is being decoded
    std::thread thread_;
};

// --------------------------------------------------------------------------------------
// image view state (zoom/pan maths); drawing lives in App::Impl::draw_view
// --------------------------------------------------------------------------------------

// One zoomable image canvas; the Tiles tab and the Mosaic tab each have one. The image on
// screen stays there while the next one decodes: path/label describe what is shown,
// pending_path/pending_label what has been requested. zoom is screen pixels per image
// pixel, measured against the original size (src_w/src_h), even when the texture is a
// downscaled copy of a very large mosaic.
struct ImageView {
    std::shared_ptr<ui::Texture> tex;
    int src_w = 0, src_h = 0;
    std::string path, label;          // what is shown
    std::string pending_path, pending_label, loading;   // what is being decoded
    std::string error, failed_path;   // failed_path stops the lazy loader retrying a bad file
    std::string empty_text = "No image loaded";   // centred in the canvas when there is no image
    std::uint64_t ticket = 0;         // ticket of the latest request; other results are stale
    float zoom = 1.0f, sx = 0.0f, sy = 0.0f;   // sx/sy: scroll in displayed pixels
    bool need_fit = false;            // fit on the next draw, once the canvas size is known
    ImVec2 view = ImVec2(0, 0);   // canvas size without scrollbars, from the last draw
    ImVec2 full = ImVec2(0, 0);   // canvas size including the scrollbar space

    bool has() const { return tex != nullptr && src_w > 0 && src_h > 0; }
    float dw() const { return src_w * zoom; }   // displayed width in screen pixels
    float dh() const { return src_h * zoom; }

    // Drops the image and anything pending, back to the empty state.
    void clear() {
        tex.reset();
        src_w = src_h = 0;
        path.clear();
        label.clear();
        pending_path.clear();
        pending_label.clear();
        loading.clear();
        error.clear();
        failed_path.clear();
        zoom = 1.0f;
        sx = sy = 0.0f;
        need_fit = false;
        ++ticket;   // results still in flight for this view are now stale
    }

    // Shows a freshly loaded texture. The fit is left to the next draw (need_fit) because
    // only draw_view knows the canvas size.
    void set(std::shared_ptr<ui::Texture> t, const std::string& p, const std::string& l) {
        tex = std::move(t);
        src_w = tex ? tex->src_width() : 0;
        src_h = tex ? tex->src_height() : 0;
        path = p;
        label = l;
        pending_path.clear();
        pending_label.clear();
        loading.clear();
        error.clear();
        failed_path.clear();
        sx = sy = 0.0f;
        need_fit = true;
    }

    // Top-left corner of the image inside the canvas: centred on an axis where the image is
    // smaller than the canvas, otherwise shifted by the scroll. Whole pixels, so it stays crisp.
    ImVec2 origin_local() const {
        const float x = dw() < view.x ? std::floor((view.x - dw()) * 0.5f) : -sx;
        const float y = dh() < view.y ? std::floor((view.y - dh()) * 0.5f) : -sy;
        return ImVec2(x, y);
    }

    // Keeps the scroll inside the image (zero on an axis where the image fits).
    void clamp_scroll() {
        sx = ImClamp(sx, 0.0f, std::max(0.0f, dw() - view.x));
        sy = ImClamp(sy, 0.0f, std::max(0.0f, dh() - view.y));
    }

    // Zooms so the whole image fits the canvas, leaving margin pixels spare.
    void fit(float margin) {
        if (!has()) return;
        if (full.x < 4.0f || full.y < 4.0f) {   // canvas not laid out yet: try on the next draw
            need_fit = true;
            return;
        }
        const float z = std::min((full.x - margin) / float(src_w), (full.y - margin) / float(src_h));
        zoom = ImClamp(z, kMinZoom, kMaxZoom);
        view = full;
        sx = sy = 0.0f;
        need_fit = false;
    }

    // Changes the zoom so the image point under anchor (canvas coordinates) stays under it,
    // as Ctrl + wheel does around the mouse cursor. On an axis where the image ends up
    // smaller than the canvas it is centred instead.
    void zoom_about(float nz, ImVec2 anchor) {
        nz = ImClamp(nz, kMinZoom, kMaxZoom);
        if (!has()) {
            zoom = nz;
            return;
        }
        const ImVec2 o = origin_local();
        const float ix = (anchor.x - o.x) / zoom;
        const float iy = (anchor.y - o.y) / zoom;
        zoom = nz;
        sx = dw() > view.x ? ix * nz - anchor.x : 0.0f;
        sy = dh() > view.y ? iy * nz - anchor.y : 0.0f;
        clamp_scroll();
    }

    // Zoom about the canvas centre (toolbar buttons, menu and keyboard shortcuts).
    void zoom_by(float factor) { zoom_about(zoom * factor, ImVec2(view.x * 0.5f, view.y * 0.5f)); }

    // The text of the strip under the viewer: size, zoom, file name and load state.
    std::string readout() const {
        const std::string dot = "  \xC2\xB7  ";   // a middle dot, UTF-8 encoded
        if (has()) {
            std::string s = strf("%d x %d px", src_w, src_h) + dot + fmt_zoom(zoom);
            if (!label.empty()) s += dot + label;
            if (!loading.empty()) s += dot + "loading " + loading;
            return s;
        }
        if (!loading.empty()) return "loading " + loading;
        if (!error.empty()) return "error: " + error;
        return "no image";
    }
};

// --------------------------------------------------------------------------------------
// run bookkeeping
// --------------------------------------------------------------------------------------
// The UI's own record of each section's last run, built from the engine's events in
// handle_event(). It feeds the Steps table, the status bar and the section tooltips, and is
// kept until a different folder is scanned.

// One step of a section, i.e. one row of the Steps table: a pair of tiles in a row
// ("Row 2 - tiles 1+2"), adding the next tile to a row ("Row 2 - + tile 3"), or a row
// merge ("Merge rows 1-2").
struct StepRow {
    std::string id, label, status;   // status is "running" until the StepEnd arrives
    int index = 0;                   // position in the section's step order
    double start_t = 0;              // engine time of the StepStart (s since run start), for the live timer
    bool ended = false;
    vem::StepMetrics m;              // from StepEnd; the table shows m.elapsed
    std::string error;               // shown as the row's tooltip
};

struct SectionRun {
    std::vector<StepRow> steps;      // kept in step order (Event::index)
    int total = 0;                   // expected step count: n*(n-1) row steps + (n-1) merges
    std::string status;              // queued | running | ok | failed | cancelled | not run
    std::string output, error;       // output: path of the mosaic written, if any
    int width = 0, height = 0;       // mosaic size
    double elapsed = 0;              // seconds for this section

    int done() const {
        int n = 0;
        for (const auto& s : steps) n += s.ended ? 1 : 0;
        return n;
    }
    StepRow* find(const std::string& id) {
        for (auto& s : steps)
            if (s.id == id) return &s;
        return nullptr;
    }
    // Finds the step, or inserts it at its place in step order. The rows of a section run on
    // parallel threads, so their events interleave; sorting on insert keeps the table in
    // step order whatever order the events arrive in.
    StepRow& upsert(const std::string& id, int index) {
        if (StepRow* s = find(id)) return *s;
        StepRow row;
        row.id = id;
        row.index = index;
        row.status = "running";
        auto at = std::upper_bound(steps.begin(), steps.end(), index,
                                   [](int i, const StepRow& s) { return i < s.index; });
        return *steps.insert(at, row);
    }
};

// A tile-map thumbnail, cached by tile path. tex stays null until the Loader delivers it.
struct Thumb {
    std::shared_ptr<ui::Texture> tex;
    std::uint64_t ticket = 0;
    bool failed = false;             // shown as "unreadable" in the tile map
    std::string error;
};

// One line of the Log pane.
struct LogLine {
    std::string stamp;               // "HH:MM:SS"
    vem::Level level = vem::Level::Info;
    std::string text;
};

// A queued dialog, shown one at a time by draw_modals().
//   Message: text and an OK button.
//   Confirm: text and two buttons; on_yes runs (deferred) only if the first is chosen.
//   Text:    scrollable monospace text with a Close button. Nothing opens one at present.
struct Modal {
    enum Kind { Message, Confirm, Text };
    int id = 0;                      // unique, so every dialog becomes a new ImGui popup
    Kind kind = Message;
    bool error = false;              // adds a red "Error" heading
    std::string title, body, yes_label = "Yes", no_label = "No";
    std::function<void()> on_yes;
};

} // namespace

// ======================================================================================
// App::Impl
// ======================================================================================
// All UI state and all UI code. There is one instance, owned by App. Only the UI thread
// touches it; the Loader and the engine hand data over through their own locked queues,
// which frame() drains at the start of every frame.
//
// Sections below, in order: fonts/style, log/status/modals, startup/settings, dataset,
// image loading, actions, run lifecycle, closing, frame, small widgets, main layout,
// menu bar and shortcuts, toolbar, left panel, centre viewer, right panel, status bar,
// log pane, modals, shutdown.

struct App::Impl {
    Host host;
    float scale = 1.0f;              // DPI scale: 1.0 at 96 dpi, 1.25 at 125 %
    ImFont* font_ui = nullptr;       // Segoe UI
    ImFont* font_bold = nullptr;     // Segoe UI Bold
    ImFont* font_mono = nullptr;     // Consolas: paths, numbers, the Log

    // ---- parameters and panel sizes (what the settings file stores)
    Settings cfg;                    // as loaded / last saved
    std::string input_buf, output_buf;   // text of the Input and Output fields (UTF-8)
    int pattern = 3;                 // grid size radio button: 2 or 3
    bool refine = false;
    float left_w = 270.0f, right_w = 390.0f, log_h = 170.0f;   // logical px

    // ---- helpers that run work off the UI thread
    vem::Runner runner;              // the stitch engine (engine.h)
    std::unique_ptr<Loader> loader;
    std::uint64_t ticket_seq = 0;    // source of Loader tickets

    // ---- the open dataset and the selection
    vem::ScanResult scan;
    bool have_scan = false;
    std::string dataset_label = "no dataset";   // right end of the toolbar
    std::string sel_section;         // selected section id ("" = none)
    int sel_row = 0, sel_col = 0;    // selected tile-map cell, 1-indexed

    std::map<std::string, SectionRun> runs;      // by section id
    std::map<std::string, Thumb> tile_thumbs;    // by tile path

    // ---- the centre viewer
    ImageView tiles_view, mosaic_view;
    std::string mosaic_candidate;    // mosaic file for the selected section, "" if none
    // Highlighted row of the Steps table. It follows the step that finished last until the
    // user clicks a row, which pins it.
    std::string sel_step;
    bool step_pinned = false;

    // Tab shown last frame (0 Tiles, 1 Mosaic) and a tab to switch to on the next frame
    // (-1 none). ImGui can only select a tab while the tab bar is being drawn.
    int active_tab = 0, request_tab = -1;

    // ---- the current or last run
    bool running = false, cancel_requested = false;
    Clock::time_point run_t0 = Clock::now();   // for elapsed times
    std::chrono::system_clock::time_point run_wall_t0 = std::chrono::system_clock::now();   // for log stamps
    double run_elapsed = 0;          // final duration of the last run
    bool have_run = false;           // any run started this session
    std::vector<std::string> run_sections;       // section ids of the current run
    std::string run_current, last_run_section;   // being stitched now / shown after the run
    int run_sec_index = 0, run_sec_total = 0;    // "section i/n" in the status bar
    bool worker_stop_seen = false;   // see check_worker()
    Clock::time_point worker_stop_at = Clock::now();

    std::string status_text = "Ready";   // left part of the status bar

    std::deque<LogLine> log;
    bool log_to_bottom = false;      // scroll the Log to its end on the next draw

    std::deque<Modal> modals;        // first one is on screen, the rest wait
    int modal_seq = 0;
    std::vector<std::function<void()>> deferred;   // see defer()

    // ---- quitting
    bool closing = false, quit = false;   // closing: waiting for the worker before quitting
    Clock::time_point closing_deadline = Clock::now();

    std::set<std::string> reported_errors;   // messages already shown by report_internal_error
    int error_dialogs_left = 3;

    explicit Impl(const Host& h) : host(h) {
        ImGuiIO& io = ImGui::GetIO();
        // No imgui.ini or imgui_log.txt: the layout is computed every frame and the panel
        // sizes are kept in our own settings file.
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        load_fonts();
        loader.reset(new Loader(host.wake));
        tiles_view.empty_text = "Open a dataset to begin: File > Open dataset (Ctrl+O)";
        mosaic_view.empty_text = "No mosaic for this section yet - press F9 to stitch it";
    }

    // Logical pixels (as designed at 96 dpi) to real pixels at the current DPI.
    float S(float v) const { return v * scale; }

    // ---------------------------------------------------------------- fonts / style

    // Segoe UI (regular and bold) and Consolas from the Windows font folder, the fonts native
    // Windows programs use. A missing file falls back to ImGui's built-in font or to the
    // regular face. They are added once at the base size; ImGui 1.92 renders glyphs at
    // whatever size is asked for, so a DPI change does not reload them.
    void load_fonts() {
        ImGuiIO& io = ImGui::GetIO();
        const std::string dir = plat::fonts_dir();
        auto add = [&](const char* file) -> ImFont* {
            const std::string p = plat::join(dir, file);
            if (!plat::is_file(p)) return nullptr;
            ImFontConfig c;
            c.RasterizerMultiply = 1.1f;   // a touch darker: stb_truetype does not hint
            return io.Fonts->AddFontFromFileTTF(p.c_str(), kFontBase, &c);
        };
        font_ui = add("segoeui.ttf");
        if (!font_ui) font_ui = io.Fonts->AddFontDefault();
        font_bold = add("segoeuib.ttf");
        if (!font_bold) font_bold = font_ui;
        font_mono = add("consola.ttf");
        if (!font_mono) font_mono = font_ui;
        io.FontDefault = font_ui;
    }

    // Builds the look: spacing, borders, the palette's colours, then the DPI scale. It
    // starts from ImGui's default style every time because ScaleAllSizes() multiplies:
    // scaling an already scaled style would compound on each DPI change.
    void apply_style(float s) {
        scale = s > 0.25f ? s : 1.0f;   // ignore a nonsense scale
        ImGuiStyle& st = ImGui::GetStyle();
        st = ImGuiStyle();
        st.WindowPadding = ImVec2(6, 6);
        st.FramePadding = ImVec2(6, 3);
        st.ItemSpacing = ImVec2(6, 4);
        st.ItemInnerSpacing = ImVec2(4, 4);
        st.CellPadding = ImVec2(4, 1);
        st.IndentSpacing = 14;
        st.ScrollbarSize = 12;
        st.GrabMinSize = 10;
        st.WindowRounding = 0;
        st.ChildRounding = 0;
        st.FrameRounding = 2;
        st.PopupRounding = 2;
        st.ScrollbarRounding = 2;
        st.GrabRounding = 2;
        st.TabRounding = 2;
        st.WindowBorderSize = 1;
        st.ChildBorderSize = 1;
        st.PopupBorderSize = 1;
        st.FrameBorderSize = 1;
        st.TabBorderSize = 1;
        st.TabBarBorderSize = 1;
        st.WindowTitleAlign = ImVec2(0.0f, 0.5f);
        st.DisabledAlpha = 0.45f;

        ImVec4* c = st.Colors;
        c[ImGuiCol_Text] = v4(pal::text);
        c[ImGuiCol_TextDisabled] = v4(pal::muted);
        c[ImGuiCol_WindowBg] = v4(pal::window);
        c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_PopupBg] = v4(pal::surface);
        c[ImGuiCol_Border] = v4(pal::line);
        c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_FrameBg] = v4(pal::surface);
        c[ImGuiCol_FrameBgHovered] = v4(pal::check_a);
        c[ImGuiCol_FrameBgActive] = v4(pal::surface);
        c[ImGuiCol_TitleBg] = v4(pal::blue100);
        c[ImGuiCol_TitleBgActive] = v4(pal::blue100);
        c[ImGuiCol_TitleBgCollapsed] = v4(pal::blue100);
        c[ImGuiCol_MenuBarBg] = v4(hexcol(0xf7f9fb));
        c[ImGuiCol_ScrollbarBg] = v4(hexcol(0xf0f4f8));
        c[ImGuiCol_ScrollbarGrab] = v4(hexcol(0xc5d5e2));
        c[ImGuiCol_ScrollbarGrabHovered] = v4(hexcol(0x9fb6c9));
        c[ImGuiCol_ScrollbarGrabActive] = v4(hexcol(0x7f9ab0));
        c[ImGuiCol_CheckMark] = v4(pal::blue500);
        c[ImGuiCol_SliderGrab] = v4(pal::blue500);
        c[ImGuiCol_SliderGrabActive] = v4(pal::blue700);
        c[ImGuiCol_Button] = v4(pal::blue050);
        c[ImGuiCol_ButtonHovered] = v4(pal::blue100);
        c[ImGuiCol_ButtonActive] = v4(pal::blue300);
        c[ImGuiCol_Header] = v4(pal::blue100);
        c[ImGuiCol_HeaderHovered] = v4(pal::blue100);
        c[ImGuiCol_HeaderActive] = v4(pal::blue300);
        c[ImGuiCol_Separator] = v4(pal::line);
        c[ImGuiCol_SeparatorHovered] = v4(pal::blue300);
        c[ImGuiCol_SeparatorActive] = v4(pal::blue500);
        c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_ResizeGripHovered] = v4(pal::blue300);
        c[ImGuiCol_ResizeGripActive] = v4(pal::blue500);
        c[ImGuiCol_InputTextCursor] = v4(pal::text);
        c[ImGuiCol_Tab] = v4(hexcol(0xe4edf4));
        c[ImGuiCol_TabHovered] = v4(pal::blue100);
        c[ImGuiCol_TabSelected] = v4(pal::surface);
        c[ImGuiCol_TabSelectedOverline] = v4(pal::blue500);
        c[ImGuiCol_TabDimmed] = v4(hexcol(0xe4edf4));
        c[ImGuiCol_TabDimmedSelected] = v4(pal::surface);
        c[ImGuiCol_TabDimmedSelectedOverline] = v4(pal::blue300);
        c[ImGuiCol_PlotHistogram] = v4(pal::blue500);
        c[ImGuiCol_PlotHistogramHovered] = v4(pal::blue700);
        c[ImGuiCol_TableHeaderBg] = v4(pal::blue100);
        c[ImGuiCol_TableBorderStrong] = v4(pal::line);
        c[ImGuiCol_TableBorderLight] = v4(pal::line_soft);
        c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_TableRowBgAlt] = v4(hexcol(0xf7fbfe));
        c[ImGuiCol_TextSelectedBg] = v4(hexcol(0x2f83c4, 90));
        c[ImGuiCol_NavCursor] = v4(pal::blue500);
        c[ImGuiCol_ModalWindowDimBg] = v4(hexcol(0x14293a, 70));

        // Sizes are scaled here; fonts through FontScaleDpi, so text is rendered at the real
        // pixel size and stays sharp instead of being stretched.
        st.ScaleAllSizes(scale);
        st.FontSizeBase = kFontBase;
        st.FontScaleMain = 1.0f;
        st.FontScaleDpi = scale;
    }

    // ---------------------------------------------------------------- log / status / modals

    // Wall-clock "HH:MM:SS" of an engine event: run_wall_t0 + ev.t, where ev.t is the time
    // since the run started, stamped by the engine when the event was queued. The main loop
    // skips frames while the window is covered or minimized, so stamping a line when it is
    // drawn would bunch the stamps up at the moment the window came back.
    std::string event_stamp(const vem::Event& ev) const {
        const auto at = run_wall_t0 + std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                          std::chrono::duration<double>(ev.t));
        const std::time_t tt = std::chrono::system_clock::to_time_t(at);
        std::tm tm{};
        if (localtime_s(&tm, &tt) != 0) return plat::local_time_hms();
        return strf("%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    }

    // Adds a message to the Log, one LogLine per text line, all with the same stamp: at, or
    // the current time when at is empty. Beyond kLogMax lines the oldest are dropped.
    void log_line(vem::Level level, const std::string& msg, const std::string& at = std::string()) {
        const std::string stamp = at.empty() ? plat::local_time_hms() : at;
        size_t pos = 0;
        do {
            size_t eol = msg.find('\n', pos);
            if (eol == std::string::npos) eol = msg.size();
            std::string part = msg.substr(pos, eol - pos);
            if (!part.empty() && part.back() == '\r') part.pop_back();
            log.push_back(LogLine{stamp, level, part});
            pos = eol + 1;
        } while (pos < msg.size());
        while (log.size() > kLogMax) log.pop_front();
    }
    // Log lines stamped now, coloured by level (see level_color).
    void info(const std::string& m) { log_line(vem::Level::Info, m); }
    void okay(const std::string& m) { log_line(vem::Level::Ok, m); }
    void warn(const std::string& m) { log_line(vem::Level::Warn, m); }
    void error(const std::string& m) { log_line(vem::Level::Error, m); }

    // Queues a dialog with an OK button.
    void message(const std::string& body, bool is_error = false, const std::string& title = kAppName) {
        Modal m;
        m.id = ++modal_seq;
        m.kind = Modal::Message;
        m.error = is_error;
        m.title = title;
        m.body = body;
        modals.push_back(std::move(m));
    }

    // Queues a two-button dialog. on_yes runs (deferred) only if the first button is chosen.
    void confirm(const std::string& body, std::function<void()> on_yes, const std::string& yes = "Yes",
                 const std::string& no = "No") {
        Modal m;
        m.id = ++modal_seq;
        m.kind = Modal::Confirm;
        m.title = kAppName;
        m.body = body;
        m.yes_label = yes;
        m.no_label = no;
        m.on_yes = std::move(on_yes);
        modals.push_back(std::move(m));
    }

    // Queues an action to run after this frame's widgets have been drawn (run_deferred).
    // Used for actions that rescan, start a run or open a shell dialog. The draw code holds
    // pointers into scan.sections and runs while it draws, so those are only replaced once
    // drawing is finished; and a shell dialog, which blocks in its own message loop, never
    // opens part-way through an ImGui window.
    void defer(std::function<void()> f) { deferred.push_back(std::move(f)); }

    // ---------------------------------------------------------------- startup / settings

    // Banner lines in the Log, then the settings. The Output field gets the saved folder
    // or <exe folder>/output. The Input field stays empty: the app starts with nothing
    // loaded, and the saved input folder is only where the Open dialogs start.
    void startup() {
        okay(strf("%s %s", kAppName, kAppVersion));
        int threads = 0;
        try {
            threads = vem::hardware_threads();
        } catch (...) {
        }
        info("algorithm: vEMstitch C++");
        info(strf("threads: %d hardware thread(s)", threads));
        info("settings: " + settings_file());

        cfg = load_settings();
        pattern = cfg.pattern;
        refine = cfg.refine;
        left_w = cfg.left_w;
        right_w = cfg.right_w;
        log_h = cfg.log_h;

        const std::string exe = plat::exe_dir();
        output_buf = !trim(cfg.output).empty() ? plat::normalize(cfg.output) : plat::join(exe, "output");

        // Start empty: nothing is loaded until a dataset is opened. The folder used last time
        // is only where the Open dialogs start.
        input_buf.clear();
        status_text = "no dataset";
        info("open a dataset to begin: File > Open dataset (Ctrl+O)");
    }

    // Saves the current choices (called when a run starts, when it ends, and on exit). An
    // empty Input field does not overwrite the remembered folder, so a session in which
    // nothing was opened keeps the last folder for the Open dialogs.
    void persist() {
        if (!trim(input_buf).empty()) cfg.input = trim(input_buf);
        cfg.output = trim(output_buf);
        cfg.pattern = ImClamp(pattern, 2, 3);
        cfg.refine = refine;
        cfg.left_w = left_w;
        cfg.right_w = right_w;
        cfg.log_h = log_h;
        save_settings(cfg);
    }

    // ---------------------------------------------------------------- dataset

    // The scanned section with this id, or nullptr.
    const vem::Section* find_section(const std::string& id) const {
        for (const auto& s : scan.sections)
            if (s.id == id) return &s;
        return nullptr;
    }

    // The folder mosaics are written to: the Output field, or <exe folder>/output if empty.
    std::string output_dir() const {
        const std::string o = plat::normalize(output_buf);
        return o.empty() ? plat::join(plat::exe_dir(), "output") : o;
    }

    // Scans the Input folder for tiles named {section}_{row}_{col}.{ext} and resets the
    // selection; select_first also selects the first section and shows its first tile.
    // The scan runs right here on the UI thread; it takes image sizes from the file headers
    // where it can, so it does not decode the tiles. Refused while a stitch runs.
    void rescan(bool select_first) {
        if (running) {
            warn("a stitch is running - cancel it first");
            return;
        }
        const std::string path = plat::normalize(input_buf);
        if (path.empty()) {
            warn("no input folder set");
            return;
        }
        if (!plat::is_dir(path)) {
            error("not a folder: " + path);
            message("Not a folder:\n" + path, true);
            return;
        }
        input_buf = path;
        status_text = "scanning " + plat::file_name(path);

        vem::ScanResult result;
        try {
            result = vem::scan_dataset(path);
        } catch (const std::exception& e) {
            result = vem::ScanResult();
            result.path = path;
            result.error = e.what();
        } catch (...) {
            result = vem::ScanResult();
            result.path = path;
            result.error = "unknown error while scanning";
        }

        // Rescanning the same folder keeps the run records (Steps table, section tooltips);
        // another folder starts clean. Thumbnails are always rebuilt, as the files may have
        // changed.
        const bool same_folder = have_scan && plat::same_path(scan.path, path);
        if (!same_folder) runs.clear();
        scan = std::move(result);
        have_scan = true;
        tile_thumbs.clear();
        loader->drop_thumbs(kSlotTileThumbs);
        sel_section.clear();
        sel_row = sel_col = 0;
        tiles_view.clear();
        tiles_view.empty_text = "Select a tile in the tile map";

        if (!scan.ok || scan.sections.empty()) {
            const std::string why = scan.error.empty() ? std::string("no tiles found") : scan.error;
            error(path + ": " + why);
            status_text = "no tiles found";
            dataset_label = "no dataset";
            update_mosaic_candidate();
            return;
        }

        // Pre-select the grid size found in the folder (largest row or column number seen).
        if (scan.pattern_guess >= 2 && scan.pattern_guess <= 3) pattern = scan.pattern_guess;
        size_t tiles = 0;
        for (const auto& s : scan.sections) tiles += s.tiles.size();
        dataset_label = strf("%s  |  %d section(s), %d tile(s)", plat::file_name(path).c_str(),
                             (int)scan.sections.size(), (int)tiles);
        okay(strf("scanned %s: %d section(s), %d tile(s), pattern guess %d", path.c_str(), (int)scan.sections.size(),
                  (int)tiles, scan.pattern_guess));
        status_text = strf("%d section(s)", (int)scan.sections.size());
        if (select_first) select_section(scan.sections.front().id, true);
    }

    // Makes a section the selected one (sections table, tile map, Steps table, Mosaic tab).
    // show_first_tile also puts its first tile in the Tiles view.
    void select_section(const std::string& id, bool show_first_tile) {
        const vem::Section* s = find_section(id);
        if (!s) return;
        const bool changed = sel_section != id;
        sel_section = id;
        if (changed) {
            info(strf("section %s: %dx%d, %d tile(s)%s", s->id.c_str(), s->rows, s->cols, (int)s->tiles.size(),
                      s->missing.empty() ? "" : strf(", %d missing", (int)s->missing.size()).c_str()));
            // The Steps highlight belongs to the old section, unless this is the section
            // being stitched right now, whose highlight is still following the run.
            if (!(running && id == run_current)) {
                sel_step.clear();
                step_pinned = false;
            }
        }
        if (show_first_tile && !s->tiles.empty()) {
            const vem::Tile& t = s->tiles.front();
            sel_row = t.row;
            sel_col = t.col;
            show_tile(t, false);
        }
        update_mosaic_candidate();
    }

    // Loads a tile into the Tiles view (in the background); switch_tab brings that tab forward.
    void show_tile(const vem::Tile& t, bool switch_tab) {
        if (!plat::is_file(t.path)) {
            error("tile file not found: " + t.path);
            return;
        }
        request_image(tiles_view, kSlotTiles, t.path, strf("%s  (r%d c%d)", t.file.c_str(), t.row, t.col));
        if (switch_tab) request_tab = 0;
    }

    // Works out which mosaic file belongs to the selected section: this session's run
    // output if there is one, else {section}-res.bmp in the output folder (the name and
    // place vEMstitch's own tool uses), e.g. from an earlier session. It does not load
    // anything: the Mosaic tab loads the file when it is shown (lazy_loads). A view that
    // shows a different file is cleared.
    void update_mosaic_candidate() {
        std::string c;
        if (!sel_section.empty()) {
            auto it = runs.find(sel_section);
            if (it != runs.end() && !it->second.output.empty() && plat::is_file(it->second.output)) {
                c = it->second.output;
            } else {
                const std::string p = plat::join(output_dir(), sel_section + "-res.bmp");
                if (plat::is_file(p)) c = p;
            }
        }
        mosaic_candidate = c;
        if (c.empty()) {
            if (mosaic_view.has() || !mosaic_view.pending_path.empty() || !mosaic_view.error.empty()) mosaic_view.clear();
        } else if ((!mosaic_view.path.empty() && !plat::same_path(mosaic_view.path, c)) &&
                   !plat::same_path(mosaic_view.pending_path, c)) {
            mosaic_view.clear();   // showing another section's mosaic; the Mosaic tab reloads lazily
        }
    }

    // ---------------------------------------------------------------- image loading

    // Asks the Loader for a full image for a view. The size limit is the largest texture the
    // GPU accepts, so a bigger mosaic comes back downscaled (process_loader logs a warning).
    // The view keeps showing its current image until the new one arrives.
    void request_image(ImageView& v, int slot, const std::string& path, const std::string& label) {
        Job j;
        j.kind = JobKind::Image;
        j.slot = slot;
        j.ticket = ++ticket_seq;
        j.path = path;
        j.max_w = j.max_h = ui::max_texture_side();
        v.ticket = j.ticket;
        v.pending_path = path;
        v.pending_label = label;
        v.loading = plat::file_name(path);
        v.error.clear();
        loader->submit(j);
    }

    // Queues a thumbnail once per path. The tile map calls this every frame for every cell;
    // the cache entry is made straight away so the same tile is not requested again.
    void request_thumb(std::map<std::string, Thumb>& cache, int slot, const std::string& path, int max_w, int max_h) {
        if (path.empty() || cache.count(path)) return;
        Thumb t;
        t.ticket = ++ticket_seq;
        cache[path] = t;
        Job j;
        j.kind = JobKind::Thumb;
        j.slot = slot;
        j.ticket = t.ticket;
        j.path = path;
        j.max_w = max_w;
        j.max_h = max_h;
        loader->submit(j);
    }

    // The view a full-image result belongs to (thumbnails have no view).
    ImageView* view_for_slot(int slot) {
        if (slot == kSlotTiles) return &tiles_view;
        if (slot == kSlotMosaic) return &mosaic_view;
        return nullptr;
    }

    // Collects decoded images from the Loader and turns them into GPU textures, which must
    // happen on the UI thread. At most 64 results per frame, so a burst of thumbnails cannot
    // stall one frame. A result nobody is waiting for any more is dropped: its ticket no
    // longer matches (the user asked for something else since), or its thumbnail entry was
    // cleared by a rescan.
    void process_loader() {
        JobResult r;
        int budget = 64;
        while (budget-- > 0 && loader->pop(r)) {
            if (r.job.kind == JobKind::Thumb) {
                auto& cache = tile_thumbs;
                auto it = cache.find(r.job.path);
                if (it == cache.end() || it->second.ticket != r.job.ticket) continue;
                if (!r.ok) {
                    it->second.failed = true;
                    it->second.error = r.error;
                    warn("thumbnail failed for " + plat::file_name(r.job.path) + ": " + r.error);
                    continue;
                }
                std::string err;
                it->second.tex = ui::Texture::create(r.rgba, r.src_w, r.src_h, true, &err);
                if (!it->second.tex) {
                    it->second.failed = true;
                    it->second.error = err;
                }
                continue;
            }
            ImageView* v = view_for_slot(r.job.slot);
            if (!v || v->ticket != r.job.ticket) continue;
            if (!r.ok) {
                v->loading.clear();
                v->pending_path.clear();
                v->error = r.error;
                v->failed_path = r.job.path;
                error("could not read " + plat::file_name(r.job.path) + ": " + r.error);
                continue;
            }
            std::string err;
            // mips = true: a mipmapped texture stays smooth when a large image is zoomed far out.
            auto tex = ui::Texture::create(r.rgba, r.src_w, r.src_h, true, &err);
            r.rgba.release();   // the GPU has its own copy now
            if (!tex) {
                v->loading.clear();
                v->pending_path.clear();
                v->error = err;
                v->failed_path = r.job.path;
                error("could not show " + plat::file_name(r.job.path) + ": " + err);
                continue;
            }
            const std::string label = v->pending_label;
            v->set(tex, r.job.path, label);
            if (r.job.slot == kSlotMosaic) {
                info(strf("mosaic %s  %d x %d  (%s)", plat::file_name(r.job.path).c_str(), r.src_w, r.src_h,
                          fmt_bytes(r.bytes).c_str()));
                if (tex->width() != r.src_w || tex->height() != r.src_h)
                    warn(strf("mosaic shown downscaled to %d x %d (largest texture is %d px)", tex->width(),
                              tex->height(), ui::max_texture_side()));
                if (!running) status_text = "ready";
            }
        }
    }

    // The mosaic is only decoded when the Mosaic tab is on screen, since it is a large image.
    // Called every frame: loads the candidate unless it is already shown, already loading,
    // or failed before (failed_path stops a broken file being retried every frame).
    void lazy_loads() {
        if (active_tab == 1 && !mosaic_candidate.empty() && !plat::same_path(mosaic_view.path, mosaic_candidate) &&
            !plat::same_path(mosaic_view.pending_path, mosaic_candidate) &&
            !plat::same_path(mosaic_view.failed_path, mosaic_candidate)) {
            if (!running) status_text = "loading mosaic";
            request_image(mosaic_view, kSlotMosaic, mosaic_candidate, plat::file_name(mosaic_candidate));
        }
    }

    // ---------------------------------------------------------------- actions
    // What the menu, toolbar and shortcuts call. The two Open dialogs start in the current
    // Input folder, else the folder used last time (settings), else the program folder.

    // Open dataset: pick any tile file; its folder becomes the dataset.
    void browse_input() {
        if (running) {
            warn("a stitch is running - cancel it first");
            return;
        }
        const std::string initial = plat::is_dir(input_buf) ? input_buf
                                  : (plat::is_dir(cfg.input) ? cfg.input : plat::exe_dir());
        const std::string path = plat::pick_image_file(host.hwnd, "Open a tile (its folder is loaded)", initial);
        if (path.empty()) return;
        open_image_file(path);
    }

    // Open folder: the original folder picker, for when you know the tile folder.
    void browse_input_folder() {
        if (running) {
            warn("a stitch is running - cancel it first");
            return;
        }
        const std::string initial = plat::is_dir(input_buf) ? input_buf
                                  : (plat::is_dir(cfg.input) ? cfg.input : plat::exe_dir());
        const std::string path = plat::pick_folder(host.hwnd, "Select the tile folder", initial);
        if (path.empty()) return;
        input_buf = plat::normalize(path);
        rescan(true);
    }

    // Loads the file's folder and selects the file when it is a tile. Anything else gets an
    // explanation, so an empty tile map is never the only answer.
    void open_image_file(const std::string& picked) {
        const std::string file = plat::normalize(picked);
        input_buf = plat::parent_dir(file);
        rescan(false);
        if (have_scan && scan.ok) {
            for (const auto& s : scan.sections) {
                for (const auto& t : s.tiles) {
                    if (!plat::same_path(t.path, file)) continue;
                    select_section(s.id, false);
                    sel_row = t.row;
                    sel_col = t.col;
                    show_tile(t, true);
                    return;
                }
            }
        }

        // Not a tile. Count its pages to tell a multi-page stack (e.g. a TIFF of whole slices)
        // from a single image. This only shapes the explanation, so any failure is ignored.
        // Unlike the Loader, imcount/imread take the path as a narrow string; the manifest's
        // UTF-8 code page is what is meant to let them open non-English names.
        const std::string name = plat::file_name(file);
        size_t pages = 1;
        int w = 0, h = 0;
        try {
            pages = cv::imcount(file);
            const cv::Mat first = cv::imread(file, cv::IMREAD_GRAYSCALE);
            w = first.cols;
            h = first.rows;
        } catch (...) {
        }

        std::string body;
        if (pages > 1) {
            body = strf("%s is an image stack: %d slices", name.c_str(), (int)pages) +
                   (w > 0 ? strf(" of %d x %d.", w, h) : std::string(".")) +
                   "\n\nvEMstitch joins overlapping tiles of one section into a mosaic. A stack of "
                   "whole slices has no tiles to join, so there is nothing in it to stitch."
                   "\n\nTiles are separate files named {section}_{row}_{col}.tif, for example "
                   "165_1_1.tif, 165_1_2.tif ... for a 3 x 3 grid.";
            warn(strf("%s: image stack (%d slices), not a tile set - nothing to stitch", name.c_str(), (int)pages));
        } else {
            body = name + " is not named like a tile, so it is not part of a tile set."
                   "\n\nTiles are named {section}_{row}_{col}.{ext}, for example 165_1_1.tif: "
                   "section 165, row 1, column 1.";
            warn(name + ": name does not follow {section}_{row}_{col}.{ext}");
        }
        if (have_scan && scan.ok && !scan.sections.empty()) {
            body += strf("\n\nThis folder does contain %d tile section(s); they are listed on the left.",
                         (int)scan.sections.size());
            select_section(scan.sections.front().id, true);
        }
        message(body);
    }

    // Picks the output folder. That also changes which saved mosaic the Mosaic tab can show.
    void browse_output() {
        const std::string initial = plat::is_dir(output_buf) ? output_buf : plat::exe_dir();
        const std::string path = plat::pick_folder(host.hwnd, "Select the output folder", initial);
        if (path.empty()) return;
        output_buf = plat::normalize(path);
        update_mosaic_candidate();
    }

    // File > Open output folder: creates it if needed, then opens it in Explorer.
    void open_output_folder() {
        const std::string path = output_dir();
        std::string err;
        if (!plat::make_dirs(path, &err)) {
            message("Cannot create:\n" + path + "\n\n" + err, true);
            return;
        }
        if (!plat::open_in_explorer(path, &err)) {
            error("could not open " + path + ": " + err);
            message("Could not open:\n" + path + "\n\n" + err, true);
        }
    }

    void clear_log() { log.clear(); }

    // The view on the visible tab; the zoom commands act on it.
    ImageView& active_view() {
        if (active_tab == 1) return mosaic_view;
        return tiles_view;
    }

    void zoom_in() { active_view().zoom_by(kZoomStep); }
    void zoom_out() { active_view().zoom_by(1.0f / kZoomStep); }
    void zoom_fit() { active_view().fit(S(16)); }

    // ---------------------------------------------------------------- run lifecycle
    // run() checks and asks  ->  start_run() hands the job to the engine  ->  every frame
    // process_runner() passes the engine's events to handle_event()  ->  the RunEnd event
    // calls finish_run(). check_worker() covers a worker that ends without a RunEnd.
    //
    // The engine does not call their three_stitching(): its OpenMP row loop pushes results
    // into a shared list without a lock, so rows land in the order they finish and the
    // merge fails on any multi-core machine. It calls their building blocks in the same
    // order instead, with each row's result in a fixed slot (see engine.cpp).

    // F9 / Run stitch (all = false: the selected section) or Shift+F9 / Run all. Checks the
    // dataset, the grid size and the output folder, then asks before running sections whose
    // grid differs from the Pattern setting or that have missing tiles. The confirmations
    // are chained: grid mismatch first, then missing tiles, then start_run().
    void run(bool all) {
        if (running) {
            warn("a stitch is already running");
            return;
        }
        if (!have_scan || scan.sections.empty()) {
            message("Open a dataset first (File > Open dataset...).");
            return;
        }
        // The Input field was edited but not rescanned: scan again so the run matches the
        // folder on screen, and keep the selected section if it is still there.
        if (!plat::same_path(input_buf, scan.path)) {
            info("the input folder changed since the last scan - rescanning");
            const std::string keep = sel_section;
            rescan(false);
            if (!have_scan || scan.sections.empty()) return;
            if (find_section(keep)) select_section(keep, false);
            else select_section(scan.sections.front().id, true);
        }
        std::vector<const vem::Section*> targets;
        if (all) {
            for (const auto& s : scan.sections) targets.push_back(&s);
        } else if (const vem::Section* s = find_section(sel_section)) {
            targets.push_back(s);
        }
        if (targets.empty()) {
            message("Select a section first.");
            return;
        }
        if (pattern != 2 && pattern != 3) {
            message("vEMstitch stitches a 2 x 2 or a 3 x 3 grid only.", true);
            return;
        }
        const std::string out = output_dir();
        if (trim(output_buf).empty()) output_buf = out;
        std::string err;
        if (!plat::make_dirs(out, &err)) {
            error("output folder unusable: " + err);
            message("Cannot create the output folder:\n" + out + "\n\n" + err, true);
            return;
        }

        // Collect the sections that need a warning; at most 8 of each are listed by name.
        std::vector<std::string> ids;
        std::string mismatch, missing;
        int n_mismatch = 0, n_missing = 0;
        for (const vem::Section* s : targets) {
            ids.push_back(s->id);
            if (s->rows != pattern || s->cols != pattern) {
                ++n_mismatch;
                if (n_mismatch <= 8) mismatch += strf("\n  %s: %d x %d", s->id.c_str(), s->rows, s->cols);
            }
            if (!s->missing.empty()) {
                ++n_missing;
                if (n_missing <= 8) {
                    std::string cells;
                    for (size_t i = 0; i < s->missing.size() && i < 8; ++i)
                        cells += strf("%s%d,%d", i ? "  " : "", s->missing[i].first, s->missing[i].second);
                    if (s->missing.size() > 8) cells += "  ...";
                    missing += strf("\n  %s: %d missing (%s)", s->id.c_str(), (int)s->missing.size(), cells.c_str());
                }
            }
        }
        if (n_mismatch > 8) mismatch += strf("\n  ... and %d more", n_mismatch - 8);
        if (n_missing > 8) missing += strf("\n  ... and %d more", n_missing - 8);

        // A missing tile does not stop the run. Their C++ would leave that row empty (a bug);
        // the engine passes the present tile through instead, as the authors' Python version
        // does, and logs a warning.
        std::function<void()> go = [this, ids, out]() { start_run(ids, out); };
        std::function<void()> after_mismatch = go;
        if (n_missing > 0) {
            const std::string body = "Tiles are missing:" + missing +
                                     "\n\nA missing tile is passed over with a warning.\nRun anyway?";
            after_mismatch = [this, body, go]() { confirm(body, go, "Run anyway", "Cancel"); };
        }
        if (n_mismatch > 0) {
            const std::string body = strf("The pattern is set to %d, but these sections have a different grid:", pattern) +
                                     mismatch + "\n\nRun anyway?";
            confirm(body, after_mismatch, "Run anyway", "Cancel");
        } else {
            after_mismatch();
        }
    }

    // Starts the engine's worker thread and resets the run records. runner.start() returns
    // at once; the work happens on the worker and progress arrives as events. Every listed
    // section starts as "queued" with its expected step count: n*(n-1) row steps plus n-1
    // row merges, i.e. 8 for 3 x 3 and 3 for 2 x 2.
    void start_run(const std::vector<std::string>& ids, const std::string& out) {
        if (running) return;   // a second confirmation dialog could land here during a run
        vem::RunConfig rc;
        rc.input = scan.path;
        rc.output = out;
        rc.sections = ids;
        rc.pattern = pattern;
        rc.refine = refine;

        std::string why;
        bool started = false;
        try {
            started = runner.start(rc, scan, &why);
        } catch (const std::exception& e) {
            why = e.what();
        } catch (...) {
            why = "unknown error";
        }
        if (!started) {
            if (why.empty()) why = "the engine refused the run";
            error("the run could not start: " + why);
            message("The run could not start.\n\n" + why, true);
            return;
        }

        running = true;
        have_run = true;
        cancel_requested = false;
        worker_stop_seen = false;
        // Taken just after the engine started its own run clock, so run_wall_t0 + ev.t is
        // the wall-clock time of an event to within a few milliseconds.
        run_t0 = Clock::now();
        run_wall_t0 = std::chrono::system_clock::now();
        run_elapsed = 0;
        run_sections = ids;
        run_current.clear();
        last_run_section = ids.front();
        run_sec_index = 0;
        run_sec_total = (int)ids.size();
        const int total = pattern * (pattern - 1) + (pattern - 1);
        for (const auto& id : ids) {
            SectionRun r;
            r.status = "queued";
            r.total = total;
            runs[id] = r;
        }
        sel_step.clear();
        step_pinned = false;
        log_to_bottom = true;
        info(std::string(68, '-'));   // a divider line in the Log between runs
        status_text = ids.size() == 1 ? "running section " + ids.front()
                                      : strf("running %d sections", (int)ids.size());
        persist();
    }

    // Esc / Cancel. Only sets a flag the engine checks between steps: their calls cannot be
    // interrupted, so the run ends once the step(s) in progress finish, which for a Refine
    // merge can take a long time. Returns immediately.
    void cancel() {
        if (!running || cancel_requested) return;
        cancel_requested = true;
        try {
            runner.cancel();
        } catch (...) {
        }
        status_text = "cancelling - finishing the step(s) in progress";
    }

    // Drains the engine's event queue without blocking. At most 4000 events per frame, so a
    // flood of Log lines cannot hold up one frame; the rest wait for the next. A failure
    // while handling one event is logged and the others still go through.
    void process_runner() {
        vem::Event ev;
        int budget = 4000;
        while (budget-- > 0) {
            bool got = false;
            try {
                got = runner.poll(ev);
            } catch (...) {
                got = false;
            }
            if (!got) break;
            try {
                handle_event(ev);
            } catch (const std::exception& e) {
                error(std::string("event handling failed: ") + e.what());
            }
        }
    }

    // Applies one engine event to the run records and the Log. Log events include the lines
    // their code prints with std::cout / std::cerr, which the engine captures during a run
    // (a windowed program has no console) and tags with the step that printed them.
    void handle_event(const vem::Event& ev) {
        switch (ev.type) {
        case vem::EventType::Log:
            log_line(ev.level, ev.msg, event_stamp(ev));
            break;
        case vem::EventType::SectionStart: {
            // The run moves on to a section: select it, so its tiles and steps are on screen.
            run_current = ev.section;
            last_run_section = ev.section;
            run_sec_index = ev.sections_total > 0 ? ev.sections_done + 1 : run_sec_index + 1;
            if (ev.sections_total > 0) run_sec_total = ev.sections_total;
            SectionRun& r = runs[ev.section];
            r.steps.clear();
            r.status = "running";
            if (ev.total > 0) r.total = ev.total;
            if (!ev.msg.empty()) log_line(ev.level, ev.msg, event_stamp(ev));
            if (sel_section != ev.section && find_section(ev.section)) select_section(ev.section, true);
            sel_step.clear();
            step_pinned = false;
            status_text = "running section " + ev.section;
            break;
        }
        case vem::EventType::StepStart: {
            SectionRun& r = runs[ev.section];
            StepRow& row = r.upsert(ev.step_id, ev.index);
            row.label = ev.label.empty() ? ev.step_id : ev.label;
            row.status = "running";
            row.start_t = ev.t;
            row.ended = false;
            if (ev.total > 0) r.total = ev.total;
            break;
        }
        case vem::EventType::StepEnd: {
            SectionRun& r = runs[ev.section];
            StepRow& row = r.upsert(ev.step_id, ev.index);
            if (!ev.label.empty()) row.label = ev.label;
            if (row.label.empty()) row.label = ev.step_id;
            row.status = ev.status.empty() ? std::string("ok") : ev.status;
            row.ended = true;
            row.m = ev.metrics;
            row.error = ev.error;
            if (ev.total > 0) r.total = ev.total;
            // Highlight the step that just finished, unless the user has picked a row.
            if (!step_pinned && ev.section == sel_section) sel_step = row.id;
            break;
        }
        case vem::EventType::SectionEnd: {
            SectionRun& r = runs[ev.section];
            r.status = ev.status.empty() ? std::string("ok") : ev.status;
            r.output = ev.output_path;
            r.error = ev.error;
            r.width = ev.width;
            r.height = ev.height;
            r.elapsed = ev.elapsed;
            if (ev.sections_total > 0) run_sec_total = ev.sections_total;
            // The engine normally ends every step it starts. Should one still be open (an event
            // lost), close it with the section's outcome so no row is left "running".
            for (auto& s : r.steps)
                if (!s.ended) {
                    s.ended = true;
                    s.status = r.status == "cancelled" ? "cancelled" : "failed";
                }
            if (!ev.msg.empty()) log_line(ev.level, ev.msg, event_stamp(ev));
            // If this section is on screen, load its new mosaic and switch to the Mosaic tab.
            const bool have_output = r.status == "ok" && !r.output.empty() && plat::is_file(r.output);
            if (ev.section == sel_section) {
                if (have_output) {
                    mosaic_candidate = r.output;
                    request_image(mosaic_view, kSlotMosaic, r.output, plat::file_name(r.output));
                    request_tab = 1;
                } else {
                    update_mosaic_candidate();
                }
            }
            if (r.status == "ok")
                status_text = strf("section %s done: %d x %d in %.1f s", ev.section.c_str(), r.width, r.height, r.elapsed);
            else
                status_text = "section " + ev.section + " " + r.status;
            break;
        }
        case vem::EventType::RunEnd:
            finish_run(ev.status, ev.msg, ev.level, ev.sections_done, ev.sections_total, ev.elapsed, event_stamp(ev));
            break;
        }
    }

    // The run is over: called for the engine's RunEnd event, or by check_worker() when the
    // worker ended without one. Settles everything still open: sections still queued become
    // "not run", and running sections and steps become "cancelled" or "failed".
    void finish_run(const std::string& status, const std::string& msg, vem::Level level, int done, int total,
                    double elapsed, const std::string& at = std::string()) {
        if (!running) return;
        running = false;
        run_elapsed = elapsed > 0 ? elapsed : std::chrono::duration<double>(Clock::now() - run_t0).count();
        const bool cancelled = cancel_requested || status == "cancelled";
        for (const auto& id : run_sections) {
            auto it = runs.find(id);
            if (it == runs.end()) continue;
            if (it->second.status == "queued") it->second.status = "not run";
            if (it->second.status == "running") it->second.status = cancelled ? "cancelled" : "failed";
            for (auto& s : it->second.steps)
                if (!s.ended) {
                    s.ended = true;
                    s.status = cancelled ? "cancelled" : "failed";
                }
        }
        if (!msg.empty()) log_line(level, msg, at);
        if (total > 0) run_sec_total = total;
        if (done > 0) run_sec_index = done;
        if (cancelled)
            status_text = "cancelled";
        else if (status == "ok")
            status_text = strf("done - %d section(s) in %.1f s", run_sec_total, run_elapsed);
        else
            status_text = "finished with errors - see the log";
        cancel_requested = false;
        run_current.clear();
        update_mosaic_candidate();
        persist();
    }

    // Safety net, called every frame during a run. If the worker thread has finished but no
    // RunEnd event came (the engine drops an event it cannot queue, e.g. when out of
    // memory), the run is ended as failed after 1.5 s so the UI does not stay "running".
    void check_worker() {
        if (!running) return;
        bool alive = true;
        try {
            alive = runner.running();
        } catch (...) {
            alive = false;
        }
        if (alive) {
            worker_stop_seen = false;
            return;
        }
        // The worker queues RunEnd before it flags itself done; allow the queue to drain.
        if (!worker_stop_seen) {
            worker_stop_seen = true;
            worker_stop_at = Clock::now();
            return;
        }
        if (Clock::now() - worker_stop_at > std::chrono::milliseconds(1500))
            finish_run("failed", "the stitch worker stopped without reporting the end of the run", vem::Level::Error, 0,
                       0, 0);
    }

    // ---------------------------------------------------------------- closing

    // Close button, Alt+F4 (both arrive as WM_CLOSE in main.cpp) or File > Exit. When idle
    // the app quits at once. During a run it asks first.
    void request_close() {
        if (quit || closing) return;
        if (!running) {
            quit = true;
            return;
        }
        // Only one "quit?" dialog at a time, however often the close button is pressed.
        for (const auto& m : modals)
            if (m.kind == Modal::Confirm && m.yes_label == "Cancel and quit") return;
        confirm("A stitch is still running.\n\nCancel it and quit?", [this]() { begin_closing(); }, "Cancel and quit",
                "Keep running");
    }

    // Cancels the run and gives the worker up to 5 s to reach the end of its step. If it is
    // still inside their code after that, the app quits anyway: main.cpp then leaves the
    // worker running and terminates the process once settings are saved.
    void begin_closing() {
        closing = true;
        cancel();
        closing_deadline = Clock::now() + std::chrono::seconds(5);
        warn("closing: waiting up to 5 s for the stitch to stop");
        status_text = "closing - waiting for the worker";
    }

    // Called every frame while closing. Waits at most 15 ms per frame for the worker, so the
    // window keeps redrawing (and the Log keeps updating) while it waits.
    void tick_closing() {
        if (!closing || quit) return;
        bool done = false;
        try {
            done = runner.join_for(15);
        } catch (...) {
            done = true;
        }
        if (done) {
            quit = true;
        } else if (Clock::now() >= closing_deadline) {
            warn("the worker is still inside their code; exiting anyway");
            quit = true;
        }
    }

    // ---------------------------------------------------------------- frame

    // One frame. First take in what the background threads delivered (decoded images,
    // engine events) and advance the run and closing state, so this frame draws the newest
    // state; then draw; then run the actions the widgets queued with defer().
    void frame() {
        process_loader();
        process_runner();
        check_worker();
        tick_closing();
        lazy_loads();
        draw();
        run_deferred();
    }

    // Runs the actions queued during this frame. The list is swapped out first, so anything
    // an action itself defers waits for the next frame instead of changing the list while
    // it is being walked. Each action is guarded, so one failure is reported and the rest
    // still run.
    void run_deferred() {
        std::vector<std::function<void()>> todo;
        todo.swap(deferred);
        for (auto& f : todo) {
            try {
                f();
            } catch (const std::exception& e) {
                report_internal_error(e.what());
            } catch (...) {
                report_internal_error("unknown exception");
            }
        }
    }

    // Every internal error goes to the Log. A dialog is shown only for a message not seen
    // before, and at most three times per session, so a fault that repeats every frame
    // cannot bury the window in dialogs.
    void report_internal_error(const std::string& what) {
        error("internal error: " + what);
        if (reported_errors.insert(what).second && error_dialogs_left > 0) {
            --error_dialogs_left;
            message("An internal error occurred:\n\n" + what + "\n\nThe application keeps running; details are in the log.",
                    true);
        }
    }

    // While true, main.cpp keeps producing frames instead of sleeping until input: the
    // engine does not wake the loop, so a running stitch is followed by polling each frame.
    bool wants_continuous() const {
        return running || closing || (loader && loader->busy());
    }

    // ---------------------------------------------------------------- small widgets

    // Text in one colour.
    void text_col(ImU32 col, const std::string& s) {
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextUnformatted(s.c_str());
        ImGui::PopStyleColor();
    }

    // Right-aligned text in a table cell.
    void text_right(ImU32 col, const std::string& s) {
        // Keep one cell padding clear on the right: flush text is clipped by the table's edge
        // in the last column.
        const float w = ImGui::CalcTextSize(s.c_str()).x;
        const float a = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().CellPadding.x;
        if (a > w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + a - w);
        text_col(col, s);
    }

    // Blue-100 caption band with a bold blue-900 label and an optional right-hand note
    // (a count such as "8" or "3/8"). Drawn with the draw list across the full panel width;
    // a Dummy then reserves its space in the layout.
    void band(const std::string& title, const std::string& right = std::string(), ImU32 right_col = pal::muted) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
        ImGui::PushFont(font_bold, 0.0f);
        const float fh = ImGui::GetFontSize();
        const float h = std::floor(fh + S(6));
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), pal::blue100);
        dl->AddRectFilled(ImVec2(p.x, p.y + h - 1), ImVec2(p.x + w, p.y + h), pal::line);
        float right_w = 0.0f;
        if (!right.empty()) {
            ImGui::PushFont(font_mono, kFontSmall);
            const ImVec2 ts = ImGui::CalcTextSize(right.c_str());
            right_w = ts.x + S(12);
            dl->AddText(ImVec2(p.x + w - S(6) - ts.x, p.y + std::floor((h - ts.y) * 0.5f)), right_col, right.c_str());
            ImGui::PopFont();
        }
        dl->PushClipRect(p, ImVec2(p.x + w - right_w, p.y + h), true);
        dl->AddText(ImVec2(p.x + S(6), p.y + std::floor((h - fh) * 0.5f)), pal::blue900, title.c_str());
        dl->PopClipRect();
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(w, h));
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
    }

    // Height band() will take, for layout sums done before drawing it.
    float band_height() const { return std::floor(ImGui::GetFontSize() + S(6)); }

    // Toolbar button with a minimum width. Its tooltip shows even while it is disabled, so a
    // greyed-out button still says what it does. True only when clicked while enabled.
    bool tool_button(const char* label, bool enabled, const char* tip, float min_w = 0.0f) {
        const float w = std::max(ImGui::CalcTextSize(label, nullptr, true).x + S(22), min_w > 0.0f ? min_w : S(62));
        ImGui::BeginDisabled(!enabled);
        const bool r = ImGui::Button(label, ImVec2(w, 0));
        ImGui::EndDisabled();
        if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", tip);
        return r && enabled;
    }

    // A thin vertical line between toolbar groups.
    void tool_sep() {
        ImGui::SameLine(0, S(6));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetFrameHeight();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + S(2)), ImVec2(p.x + 1, p.y + h - S(2)), pal::line);
        ImGui::Dummy(ImVec2(1, h));
        ImGui::SameLine(0, S(6));
    }

    // Strong blue with white text for the selected table row. Pushes 4 colours; the caller
    // pops 4.
    void push_selected_row_colors() {
        ImGui::PushStyleColor(ImGuiCol_Header, v4(pal::blue500));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, v4(pal::blue500));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, v4(pal::blue700));
        ImGui::PushStyleColor(ImGuiCol_Text, v4(pal::white));
    }

    // Table header row in bold dark blue. (PushFont size 0 keeps the current size.)
    void table_headers() {
        ImGui::PushFont(font_bold, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, v4(pal::blue900));
        ImGui::TableHeadersRow();
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }

    // Draggable divider. value is in logical px; sign flips the drag direction for panes
    // anchored at the far side (right panel, bottom log). The drag is divided by the DPI
    // scale so the edge stays under the mouse. lo/hi bound the stored value; draw() may
    // still shrink a pane further when the window is small.
    void splitter(const char* id, ImVec2 pos, ImVec2 size, bool vertical, float* value, float sign, float lo, float hi) {
        if (size.x < 1 || size.y < 1) return;
        ImGui::SetCursorScreenPos(pos);
        ImGui::InvisibleButton(id, size);
        const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
        if (hovered || active) ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
        if (active) {
            const ImGuiIO& io = ImGui::GetIO();
            const float d = vertical ? io.MouseDelta.x : io.MouseDelta.y;
            *value = ImClamp(*value + sign * d / scale, lo, hi);
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 col = active ? pal::blue500 : (hovered ? pal::blue300 : pal::line);
        if (vertical) {
            const float x = std::floor(pos.x + size.x * 0.5f);
            dl->AddRectFilled(ImVec2(x, pos.y), ImVec2(x + 1, pos.y + size.y), col);
        } else {
            const float y = std::floor(pos.y + size.y * 0.5f);
            dl->AddRectFilled(ImVec2(pos.x, y), ImVec2(pos.x + size.x, y + 1), col);
        }
    }

    // ---------------------------------------------------------------- main layout

    // The whole window is one borderless ImGui window covering the viewport, cut into
    // fixed regions that are computed here every frame:
    //
    //   menu bar
    //   toolbar
    //   left | centre | right      (vertical splitters between them)
    //   ---------------------      (horizontal splitter)
    //   status bar
    //   log
    //
    // Panel sizes are stored in logical px. As far as the window allows, the centre keeps
    // 300 and the body 220 logical px: the side panels and the log shrink first.
    void draw() {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        const ImGuiWindowFlags wf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                    ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_MenuBar |
                                    ImGuiWindowFlags_NoScrollWithMouse;
        ImGui::Begin("##workbench", nullptr, wf);
        ImGui::PopStyleVar(3);

        draw_menu_bar();
        handle_shortcuts();

        // Everything below the menu bar. Heights and positions are floored to whole pixels
        // so lines and borders stay crisp.
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float W = std::max(avail.x, 200.0f), H = std::max(avail.y, 200.0f);

        const float tb_h = std::floor(ImGui::GetFrameHeight() + S(8));
        const float split = std::floor(S(5));
        const float status_h = std::floor(ImGui::GetFrameHeight() + S(4));

        // Vertical: the log takes its stored height (at most 55 % of the window), but gives
        // way when the body would drop under 220 logical px.
        float log_px = ImClamp(S(log_h), S(70), std::max(S(70), H * 0.55f));
        float body_h = H - tb_h - split - status_h - log_px;
        if (body_h < S(220)) {
            log_px = std::max(S(50), log_px - (S(220) - body_h));
            body_h = H - tb_h - split - status_h - log_px;
        }
        body_h = std::max(body_h, S(60));

        // Horizontal: side panels at their stored widths (capped at 35 % / 42 % of the
        // window); if the centre would drop under 300 logical px, each side panel gives up
        // half of the shortfall, down to its own minimum.
        float lw = ImClamp(S(left_w), S(190), std::max(S(190), W * 0.35f));
        float rw = ImClamp(S(right_w), S(270), std::max(S(270), W * 0.42f));
        float cw = W - lw - rw - 2 * split;
        if (cw < S(300)) {
            const float lack = S(300) - cw;
            rw = std::max(S(240), rw - lack * 0.5f);
            lw = std::max(S(170), lw - lack * 0.5f);
            cw = std::max(S(80), W - lw - rw - 2 * split);
        }

        const float y_body = origin.y + tb_h;
        draw_toolbar(origin, ImVec2(W, tb_h));

        draw_left(ImVec2(origin.x, y_body), ImVec2(lw, body_h));
        splitter("##split_left", ImVec2(origin.x + lw, y_body), ImVec2(split, body_h), true, &left_w, 1.0f, 170.0f,
                 700.0f);
        draw_centre(ImVec2(origin.x + lw + split, y_body), ImVec2(cw, body_h));
        splitter("##split_right", ImVec2(origin.x + lw + split + cw, y_body), ImVec2(split, body_h), true, &right_w,
                 -1.0f, 240.0f, 800.0f);
        draw_right(ImVec2(origin.x + W - rw, y_body), ImVec2(rw, body_h));

        const float y_split = y_body + body_h;
        splitter("##split_bottom", ImVec2(origin.x, y_split), ImVec2(W, split), false, &log_h, -1.0f, 50.0f, 900.0f);
        draw_status(ImVec2(origin.x, y_split + split), ImVec2(W, status_h));
        draw_log(ImVec2(origin.x, y_split + split + status_h), ImVec2(W, H - (y_split + split + status_h - origin.y)));

        draw_modals();
        ImGui::End();
    }

    // ---------------------------------------------------------------- menu bar / shortcuts

    // File, Process, View, Help. Items that open a dialog, rescan or start a run are
    // deferred (see defer()); items unavailable during a run are greyed out.
    void draw_menu_bar() {
        if (!ImGui::BeginMenuBar()) return;
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open dataset...", "Ctrl+O", false, !running)) defer([this]() { browse_input(); });
            if (ImGui::MenuItem("Open folder...", "Ctrl+Shift+O", false, !running)) defer([this]() { browse_input_folder(); });
            if (ImGui::MenuItem("Rescan", "F5", false, !running)) defer([this]() { rescan(true); });
            ImGui::Separator();
            if (ImGui::MenuItem("Open output folder")) defer([this]() { open_output_folder(); });
            ImGui::Separator();
            if (ImGui::MenuItem("Clear log")) clear_log();
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4")) request_close();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Process")) {
            const bool can_run = !running && have_scan && !scan.sections.empty();
            if (ImGui::MenuItem("Run", "F9", false, can_run && !sel_section.empty())) defer([this]() { run(false); });
            if (ImGui::MenuItem("Run all sections", "Shift+F9", false, can_run)) defer([this]() { run(true); });
            if (ImGui::MenuItem("Cancel", "Esc", false, running && !cancel_requested)) cancel();
            ImGui::Separator();
            ImGui::MenuItem("Refine", nullptr, &refine, !running);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Tiles", "Ctrl+1", active_tab == 0)) request_tab = 0;
            if (ImGui::MenuItem("Mosaic", "Ctrl+2", active_tab == 1)) request_tab = 1;
            ImGui::Separator();
            const bool img = active_view().has();
            if (ImGui::MenuItem("Zoom in", "Ctrl+=", false, img)) zoom_in();
            if (ImGui::MenuItem("Zoom out", "Ctrl+-", false, img)) zoom_out();
            if (ImGui::MenuItem("Fit", "Ctrl+0", false, img)) zoom_fit();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("About...")) show_about();
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    // Help > About: version, algorithm, thread count, settings file and program folder.
    void show_about() {
        int threads = 0;
        try {
            threads = vem::hardware_threads();
        } catch (...) {
        }
        std::string body;
        body += strf("%s %s\n\n", kAppName, kAppVersion);
        body += "Algorithm   vEMstitch C++\n";
        body += strf("Threads     %d hardware thread(s)\n", threads);
        body += "Settings    " + settings_file() + "\n";
        body += "Program     " + plat::exe_dir();
        Modal m;
        m.id = ++modal_seq;
        m.kind = Modal::Message;
        m.title = std::string("About ") + kAppName;
        m.body = body;
        modals.push_back(std::move(m));
    }

    // Keyboard shortcuts (the same ones the menus list). Ignored while a dialog, menu or
    // other popup is open. RouteGlobal: they work wherever the keyboard focus is.
    void handle_shortcuts() {
        if (!modals.empty()) return;
        if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) return;
        const ImGuiInputFlags g = ImGuiInputFlags_RouteGlobal;
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, g) && !running) defer([this]() { browse_input(); });
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_O, g) && !running) defer([this]() { browse_input_folder(); });
        if (ImGui::Shortcut(ImGuiKey_F5, g) && !running) defer([this]() { rescan(true); });
        // F9 is not blocked during a run: run() then logs that a stitch is already running.
        if (ImGui::Shortcut(ImGuiKey_F9, g)) defer([this]() { run(false); });
        if (ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_F9, g)) defer([this]() { run(true); });
        // Esc while typing in a field only ends the edit; it must not cancel the run.
        if (!ImGui::IsAnyItemActive() && ImGui::Shortcut(ImGuiKey_Escape, g)) cancel();
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_1, g)) request_tab = 0;
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_2, g)) request_tab = 1;
        // Zoom in: Ctrl+=, Ctrl++ (Shift+= on a US keyboard) or Ctrl + keypad plus.
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Equal, g) ||
            ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Equal, g) ||
            ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_KeypadAdd, g))
            zoom_in();
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Minus, g) ||
            ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_KeypadSubtract, g))
            zoom_out();
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_0, g) || ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Keypad0, g))
            zoom_fit();
    }

    // ---------------------------------------------------------------- toolbar

    // Open, Rescan | Run stitch (the main action, dark blue), Run all, Cancel | zoom out,
    // zoom %, zoom in, Fit | dataset name and counts, right-aligned (full path as tooltip).
    void draw_toolbar(ImVec2 pos, ImVec2 size) {
        ImGui::SetCursorScreenPos(pos);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(pal::toolbar));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(4), S(4)));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(3), S(4)));
        ImGui::BeginChild("##toolbar", size, ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        {   // 1 px line under the toolbar
            const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
            dl->AddRectFilled(ImVec2(wp.x, wp.y + ws.y - 1), ImVec2(wp.x + ws.x, wp.y + ws.y), pal::line);
        }
        const bool can_run = !running && have_scan && !scan.sections.empty();

        if (tool_button("Open", !running, "Open dataset... (Ctrl+O): pick any tile; its folder is loaded")) defer([this]() { browse_input(); });
        ImGui::SameLine();
        if (tool_button("Rescan", !running, "Rescan the input folder (F5)")) defer([this]() { rescan(true); });
        tool_sep();

        ImGui::PushFont(font_bold, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, v4(pal::blue700));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, v4(pal::blue500));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, v4(pal::blue900));
        ImGui::PushStyleColor(ImGuiCol_Border, v4(pal::blue900));
        ImGui::PushStyleColor(ImGuiCol_Text, v4(pal::white));
        const bool run_clicked = tool_button("Run stitch", can_run && !sel_section.empty(),
                                             "Stitch the selected section (F9)", S(86));
        ImGui::PopStyleColor(5);
        ImGui::PopFont();
        if (run_clicked) defer([this]() { run(false); });
        ImGui::SameLine();
        if (tool_button("Run all", can_run, "Stitch every section in the dataset (Shift+F9)"))
            defer([this]() { run(true); });
        ImGui::SameLine();
        if (tool_button("Cancel", running && !cancel_requested, "Cancel after the step(s) in progress (Esc)")) cancel();
        tool_sep();

        ImageView& v = active_view();
        const bool img = v.has();
        if (tool_button("-", img, "Zoom out (Ctrl+-)", S(26))) zoom_out();
        ImGui::SameLine();
        {   // zoom readout: fixed width, so the buttons after it do not move as it changes
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = S(54), h = ImGui::GetFrameHeight();
            ImGui::Dummy(ImVec2(w, h));
            ImGui::PushFont(font_mono, 0.0f);
            const std::string z = img ? fmt_zoom(v.zoom) : std::string("-");
            const ImVec2 ts = ImGui::CalcTextSize(z.c_str());
            dl->AddText(ImVec2(p.x + std::floor((w - ts.x) * 0.5f), p.y + std::floor((h - ts.y) * 0.5f)), pal::blue900,
                        z.c_str());
            ImGui::PopFont();
        }
        ImGui::SameLine();
        if (tool_button("+", img, "Zoom in (Ctrl+=)", S(26))) zoom_in();
        ImGui::SameLine(0, S(6));
        if (tool_button("Fit", img, "Fit the image to the view (Ctrl+0, or double-click the image)", S(44)))
            zoom_fit();

        // Dataset label at the right end; left out when the window is too narrow for it.
        ImGui::SameLine();
        ImGui::PushFont(font_mono, 0.0f);
        const float tw = ImGui::CalcTextSize(dataset_label.c_str()).x;
        const float a = ImGui::GetContentRegionAvail().x;
        if (a > tw + S(8)) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + a - tw - S(4));
            ImGui::AlignTextToFramePadding();
            text_col(pal::muted, dataset_label);
            if (have_scan && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", scan.path.c_str());
        }
        ImGui::PopFont();

        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }

    // ---------------------------------------------------------------- left: sections + tile map

    // The sections table on top, the selected section's tile map below.
    void draw_left(ImVec2 pos, ImVec2 size) {
        ImGui::SetCursorScreenPos(pos);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(pal::surface));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::BeginChild("##left", size, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();

        band("Sections", have_scan ? strf("%d", (int)scan.sections.size()) : std::string());
        // Table height: room for 7 rows at most, else 40 % of the panel, but at least 3
        // rows. The tile map gets the rest.
        const float row_h = ImGui::GetTextLineHeight() + ImGui::GetStyle().CellPadding.y * 2.0f;
        const float table_h = std::min(row_h * 7.0f + S(3), std::max(row_h * 3.0f, size.y * 0.4f));
        draw_sections_table(table_h);
        ImGui::Dummy(ImVec2(1, S(3)));

        const vem::Section* s = find_section(sel_section);
        if (s)
            band("Tiles - section " + s->id, strf("%d/%d  %dx%d", (int)s->tiles.size(), s->rows * s->cols, s->rows, s->cols));
        else
            band("Tiles");
        draw_tile_map(s);

        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    // One row per section: id, grid (rows x cols), "complete" or "N missing". Clicking a row
    // selects the section; the tooltip shows the status of its last run.
    void draw_sections_table(float height) {
        const ImGuiTableFlags tf = ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersOuterH |
                                   ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit |
                                   ImGuiTableFlags_NoSavedSettings;
        if (!ImGui::BeginTable("##sections", 3, tf, ImVec2(-FLT_MIN, height))) return;
        ImGui::TableSetupScrollFreeze(0, 1);   // header row stays visible while scrolling
        ImGui::TableSetupColumn("Section", ImGuiTableColumnFlags_WidthFixed, S(84));
        ImGui::TableSetupColumn("Grid", ImGuiTableColumnFlags_WidthFixed, S(46));
        ImGui::TableSetupColumn("Tiles", ImGuiTableColumnFlags_WidthStretch);
        table_headers();
        if (!have_scan || scan.sections.empty()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            text_col(pal::muted, "no dataset");
        }
        for (size_t i = 0; i < scan.sections.size(); ++i) {
            const vem::Section& s = scan.sections[i];
            const bool sel = s.id == sel_section;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID((int)i);
            if (sel) push_selected_row_colors();
            if (ImGui::Selectable(s.id.c_str(), sel, ImGuiSelectableFlags_SpanAllColumns) && !sel)
                select_section(s.id, true);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
                auto it = runs.find(s.id);
                if (it != runs.end() && !it->second.status.empty())
                    ImGui::SetTooltip("section %s - last run: %s", s.id.c_str(), it->second.status.c_str());
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(strf("%dx%d", s.rows, s.cols).c_str());
            ImGui::TableSetColumnIndex(2);
            const bool complete = s.complete && s.missing.empty();
            const std::string state = complete ? std::string("complete") : strf("%d missing", (int)s.missing.size());
            if (sel)   // the selected row keeps the white text pushed above
                ImGui::TextUnformatted(state.c_str());
            else
                text_col(complete ? pal::ok : pal::warn, state);
            if (sel) ImGui::PopStyleColor(4);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // The selected section as a grid of clickable cells laid out like the tiles themselves
    // (row 1 at the top). A present tile shows its thumbnail, requested the first time the
    // cell is drawn; a missing one is hatched. Clicking a cell shows that tile. Cell size
    // follows the panel width, between 40 and 120 logical px.
    void draw_tile_map(const vem::Section* s) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(pal::panel));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(4), S(4)));
        ImGui::BeginChild("##tilemap", ImVec2(-FLT_MIN, -FLT_MIN), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::PopStyleVar();
        if (!s || s->rows <= 0 || s->cols <= 0) {
            text_col(pal::muted, have_scan ? "No section selected" : "Open a dataset (Ctrl+O)");
            ImGui::EndChild();
            ImGui::PopStyleColor();
            return;
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float gap = S(4), border = std::max(1.0f, std::floor(S(2)));
        ImGui::PushFont(font_ui, kFontSmall);
        const float cap_h = ImGui::GetFontSize() + S(3);
        const float avail = ImGui::GetContentRegionAvail().x;
        const float box = std::floor(ImClamp((avail - gap * (s->cols - 1)) / s->cols - 2 * border, S(40), S(120)));
        const float cw = box + 2 * border, ch = box + cap_h + 2 * border;   // cell incl. caption

        // Thumbnails (up to 256 px, mipmapped) are drawn much smaller than that; trilinear
        // sampling keeps them smooth.
        ui::draw_sampler_smooth(dl);
        for (int r = 1; r <= s->rows; ++r) {
            for (int c = 1; c <= s->cols; ++c) {
                const vem::Tile* t = s->find(r, c);
                const ImVec2 p(start.x + (c - 1) * (cw + gap), start.y + (r - 1) * (ch + gap));
                // An invisible button per cell does the clicking and hovering; the look is
                // drawn with the draw list below.
                ImGui::SetCursorScreenPos(p);
                ImGui::PushID(r * 1000 + c);
                const bool clicked = ImGui::InvisibleButton("##cell", ImVec2(cw, ch));
                const bool hovered = ImGui::IsItemHovered();
                if (t && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                    ImGui::SetTooltip("%s\n%d x %d px, %s", t->file.c_str(), t->w, t->h, fmt_bytes(t->bytes).c_str());
                ImGui::PopID();
                if (clicked) {
                    sel_row = r;
                    sel_col = c;
                    if (t)
                        show_tile(*t, true);
                    else
                        warn(strf("tile %d,%d is missing from this section", r, c));
                }
                const bool selected = sel_row == r && sel_col == c;
                const ImU32 frame = selected ? pal::blue500 : (hovered ? pal::blue300 : pal::line_soft);
                dl->AddRectFilled(p, ImVec2(p.x + cw, p.y + ch), pal::surface);
                const ImVec2 b0(p.x + border, p.y + border), b1(b0.x + box, b0.y + box);
                if (t) {
                    // Black box, thumbnail centred in it with its aspect ratio kept.
                    dl->AddRectFilled(b0, b1, pal::black);
                    request_thumb(tile_thumbs, kSlotTileThumbs, t->path, kThumbSide, kThumbSide);
                    const Thumb& th = tile_thumbs[t->path];
                    if (th.tex) {
                        const float k = std::min(box / th.tex->width(), box / th.tex->height());
                        const ImVec2 sz(std::floor(th.tex->width() * k), std::floor(th.tex->height() * k));
                        const ImVec2 i0(b0.x + std::floor((box - sz.x) * 0.5f), b0.y + std::floor((box - sz.y) * 0.5f));
                        dl->AddImage(th.tex->id(), i0, i0 + sz);
                    } else {
                        const char* msg = th.failed ? "unreadable" : "loading";
                        const ImVec2 ts = ImGui::CalcTextSize(msg);
                        dl->AddText(ImVec2(b0.x + (box - ts.x) * 0.5f, b0.y + (box - ts.y) * 0.5f),
                                    th.failed ? pal::bad : pal::muted, msg);
                    }
                } else {
                    // Missing tile: diagonal hatching with a "missing" label.
                    dl->AddRectFilled(b0, b1, pal::panel);
                    dl->PushClipRect(b0, b1, true);
                    const float step = S(8);
                    for (float k = 0; k < box * 2; k += step)
                        dl->AddLine(ImVec2(b0.x + k, b0.y), ImVec2(b0.x + k - box, b0.y + box), pal::hatch, 1.0f);
                    dl->PopClipRect();
                    const ImVec2 ts = ImGui::CalcTextSize("missing");
                    const ImVec2 tp(b0.x + std::floor((box - ts.x) * 0.5f), b0.y + std::floor((box - ts.y) * 0.5f));
                    dl->AddRectFilled(tp - ImVec2(S(3), S(1)), tp + ts + ImVec2(S(3), S(1)), pal::panel);
                    dl->AddText(tp, pal::muted, "missing");
                }
                dl->AddRect(p, ImVec2(p.x + cw, p.y + ch), frame, 0.0f, 0, border);
                const std::string cap = strf("%d,%d", r, c);
                const ImVec2 ts = ImGui::CalcTextSize(cap.c_str());
                dl->AddText(ImVec2(p.x + std::floor((cw - ts.x) * 0.5f), b1.y + std::floor((cap_h - ts.y) * 0.5f)),
                            t ? pal::blue900 : pal::muted, cap.c_str());
            }
        }
        ui::draw_sampler_reset(dl);
        ImGui::PopFont();
        // Reserve the grid's full size so the child window scrolls when the grid is larger.
        ImGui::SetCursorScreenPos(start);
        ImGui::Dummy(ImVec2(s->cols * cw + (s->cols - 1) * gap, s->rows * ch + (s->rows - 1) * gap));
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    // ---------------------------------------------------------------- centre: views

    // Two tabs, Tiles (one raw tile) and Mosaic (the stitched result), each an image
    // canvas, and a readout strip along the bottom for the visible one.
    void draw_centre(ImVec2 pos, ImVec2 size) {
        ImGui::SetCursorScreenPos(pos);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(4), S(4)));
        ImGui::BeginChild("##centre", size, ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();

        const float readout_h = std::floor(ImGui::GetTextLineHeight() + S(8));
        const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
        const float pad = S(4);

        if (ImGui::BeginTabBar("##views")) {
            // "###id": the part after ### is the ImGui ID, independent of the visible label.
            static const char* names[2] = {"Tiles###tab_tiles", "Mosaic###tab_mosaic"};
            for (int i = 0; i < 2; ++i) {
                // Apply a pending tab switch (request_tab) while the tab bar is being drawn.
                const ImGuiTabItemFlags f = request_tab == i ? ImGuiTabItemFlags_SetSelected : 0;
                if (ImGui::BeginTabItem(names[i], nullptr, f)) {
                    active_tab = i;
                    ImVec2 a = ImGui::GetContentRegionAvail();
                    a.y -= readout_h + S(2);
                    a.x = std::max(a.x, 1.0f);
                    a.y = std::max(a.y, 1.0f);
                    if (i == 0) draw_view(tiles_view, "##view_tiles", a);
                    else draw_view(mosaic_view, "##view_mosaic", a);
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }
        request_tab = -1;   // a switch request is used once

        // readout line: W x H px . zoom . file
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 r0(wp.x + pad, wp.y + ws.y - pad - readout_h), r1(wp.x + ws.x - pad, wp.y + ws.y - pad);
        dl->AddRectFilled(r0, r1, pal::blue050);
        dl->AddRect(r0, r1, pal::line_soft);
        ImGui::PushFont(font_mono, 0.0f);
        const std::string ro = active_view().readout();
        dl->PushClipRect(r0, r1, true);
        dl->AddText(ImVec2(r0.x + S(6), r0.y + std::floor((readout_h - ImGui::GetFontSize()) * 0.5f)), pal::muted,
                    ro.c_str());
        dl->PopClipRect();
        ImGui::PopFont();

        ImGui::EndChild();
    }

    // Zoomable, pannable image canvas with a checkerboard, thin scrollbars and point
    // sampling above 100 %. It is drawn by hand with the draw list rather than as an ImGui
    // scrolling child, so zoom and scroll live entirely in ImageView and the scrollbars are
    // our own too. Mouse: wheel scrolls, Shift + wheel scrolls sideways, Ctrl + wheel zooms
    // around the cursor, drag pans, double-click fits.
    void draw_view(ImageView& v, const char* id, ImVec2 size) {
        size.x = std::max(std::floor(size.x), 1.0f);
        size.y = std::max(std::floor(size.y), 1.0f);
        const ImGuiIO& io = ImGui::GetIO();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float bar = std::floor(S(10));   // scrollbar thickness
        v.full = size;
        if (v.need_fit && v.has()) v.fit(S(16));

        // Which scrollbars are needed, and the canvas size left over. A scrollbar takes room
        // from the other direction, so needing one can make the other necessary too.
        auto layout = [&](bool& nh, bool& nv) {
            nh = v.has() && v.dw() > size.x;
            nv = v.has() && v.dh() > size.y;
            if (nh && !nv) nv = v.dh() > size.y - bar;
            if (nv && !nh) nh = v.dw() > size.x - bar;
            v.view = ImVec2(std::max(1.0f, size.x - (nv ? bar : 0)), std::max(1.0f, size.y - (nh ? bar : 0)));
        };
        bool need_h = false, need_v = false;
        layout(need_h, need_v);

        ImGui::PushID(id);
        ImGui::SetCursorScreenPos(origin);
        ImGui::InvisibleButton("##canvas", v.view, ImGuiButtonFlags_MouseButtonLeft);
        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();
        if (hovered) {
            // Claim the mouse wheel while over the canvas, so it drives the image and does
            // not also scroll a window around it.
            ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
            ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelX);
        }
        if (v.has()) {
            const float step = S(60);   // screen px per wheel notch
            if (hovered && io.MouseWheel != 0.0f) {
                if (io.KeyCtrl) {
                    v.zoom_about(v.zoom * std::pow(kZoomStep, io.MouseWheel), io.MousePos - origin);
                    layout(need_h, need_v);
                } else if (io.KeyShift) {
                    v.sx -= io.MouseWheel * step;
                } else {
                    v.sy -= io.MouseWheel * step;
                }
            }
            if (hovered && io.MouseWheelH != 0.0f) v.sx -= io.MouseWheelH * step;
            if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f)) {
                v.sx -= io.MouseDelta.x;
                v.sy -= io.MouseDelta.y;
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            }
            if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                v.fit(S(16));
                layout(need_h, need_v);
            }
            v.clamp_scroll();
        }

        // Canvas background: a light checkerboard, as image viewers use, so the edge of the
        // image is always clear.
        const ImVec2 c0 = origin, c1 = origin + v.view;
        dl->PushClipRect(c0, c1, true);
        dl->AddRectFilled(c0, c1, pal::check_a);
        {
            const float cell = std::max(4.0f, std::floor(S(16)));
            const int nx = (int)std::ceil(v.view.x / cell), ny = (int)std::ceil(v.view.y / cell);
            for (int y = 0; y < ny; ++y)
                for (int x = (y & 1); x < nx; x += 2)
                    dl->AddRectFilled(ImVec2(c0.x + x * cell, c0.y + y * cell),
                                      ImVec2(c0.x + (x + 1) * cell, c0.y + (y + 1) * cell), pal::check_b);
        }
        if (v.has()) {
            const ImVec2 o = v.origin_local();
            const ImVec2 p0(std::floor(c0.x + o.x), std::floor(c0.y + o.y));
            const ImVec2 p1(p0.x + v.dw(), p0.y + v.dh());
            // Sampler choice uses screen px per texture px. The texture can be smaller than
            // the file (a downscaled mosaic), so this is not always the same as zoom.
            // 100 % or more: nearest sampling, for sharp pixels. Less: trilinear over the mip
            // chain, so fine detail does not shimmer (the backend's default sampler if the
            // texture has no mips).
            const float texel_zoom = v.dw() / float(v.tex->width());
            bool custom = false;
            if (texel_zoom >= 0.999f) {
                ui::draw_sampler_nearest(dl);
                custom = true;
            } else if (v.tex->has_mips()) {
                ui::draw_sampler_smooth(dl);
                custom = true;
            }
            dl->AddImage(v.tex->id(), p0, p1);
            if (custom) ui::draw_sampler_reset(dl);
            dl->AddRect(p0 - ImVec2(1, 1), p1 + ImVec2(1, 1), pal::line);
            // The next image is still decoding: a small badge over the current one.
            if (!v.loading.empty()) {
                ImGui::PushFont(font_ui, kFontSmall);
                const std::string m = "loading " + v.loading + " ...";
                const ImVec2 ts = ImGui::CalcTextSize(m.c_str());
                const ImVec2 q(c0.x + S(6), c0.y + S(6));
                dl->AddRectFilled(q, q + ts + ImVec2(S(10), S(4)), pal::blue050);
                dl->AddRect(q, q + ts + ImVec2(S(10), S(4)), pal::line);
                dl->AddText(q + ImVec2(S(5), S(2)), pal::blue900, m.c_str());
                ImGui::PopFont();
            }
        } else {
            // No image: a centred message (loading, error or the view's empty text).
            std::string m = v.empty_text;
            ImU32 col = pal::muted;
            if (!v.loading.empty()) {
                m = "loading " + v.loading + " ...";
            } else if (!v.error.empty()) {
                m = "could not show the image: " + v.error;
                col = pal::bad;
            }
            const ImVec2 ts = ImGui::CalcTextSize(m.c_str());
            dl->AddText(ImVec2(c0.x + std::max(S(8), std::floor((v.view.x - ts.x) * 0.5f)),
                               c0.y + std::floor((v.view.y - ts.y) * 0.5f)),
                        col, m.c_str());
        }
        dl->PopClipRect();
        dl->AddRect(c0 - ImVec2(0, 0), c1, pal::line);

        // Scrollbars, in the same colours as ImGui's own (see apply_style). Pressing on the
        // track outside the thumb jumps the thumb's centre there; dragging then moves the
        // image in proportion.
        const ImU32 track = hexcol(0xf0f4f8), grab = hexcol(0xc5d5e2), grab_hot = hexcol(0x9fb6c9);
        if (need_h) {
            const ImVec2 b0(c0.x, c1.y), b1(c1.x, c1.y + bar);
            ImGui::SetCursorScreenPos(b0);
            ImGui::InvisibleButton("##hbar", b1 - b0);
            const float range = std::max(1.0f, v.dw() - v.view.x);
            const float thumb = std::max(v.view.x * v.view.x / v.dw(), S(24));
            const float travel = std::max(1.0f, v.view.x - thumb);
            if (ImGui::IsItemActive()) {
                if (ImGui::IsItemActivated()) {
                    const float t0 = b0.x + v.sx / range * travel;
                    if (io.MousePos.x < t0 || io.MousePos.x > t0 + thumb)
                        v.sx = (io.MousePos.x - b0.x - thumb * 0.5f) / travel * range;
                } else {
                    v.sx += io.MouseDelta.x * range / travel;
                }
                v.clamp_scroll();
            }
            const float t0 = b0.x + v.sx / range * travel;
            dl->AddRectFilled(b0, b1, track);
            dl->AddRectFilled(ImVec2(t0, b0.y + 2), ImVec2(t0 + thumb, b1.y - 2),
                              ImGui::IsItemHovered() || ImGui::IsItemActive() ? grab_hot : grab, S(2));
        }
        if (need_v) {
            const ImVec2 b0(c1.x, c0.y), b1(c1.x + bar, c1.y);
            ImGui::SetCursorScreenPos(b0);
            ImGui::InvisibleButton("##vbar", b1 - b0);
            const float range = std::max(1.0f, v.dh() - v.view.y);
            const float thumb = std::max(v.view.y * v.view.y / v.dh(), S(24));
            const float travel = std::max(1.0f, v.view.y - thumb);
            if (ImGui::IsItemActive()) {
                if (ImGui::IsItemActivated()) {
                    const float t0 = b0.y + v.sy / range * travel;
                    if (io.MousePos.y < t0 || io.MousePos.y > t0 + thumb)
                        v.sy = (io.MousePos.y - b0.y - thumb * 0.5f) / travel * range;
                } else {
                    v.sy += io.MouseDelta.y * range / travel;
                }
                v.clamp_scroll();
            }
            const float t0 = b0.y + v.sy / range * travel;
            dl->AddRectFilled(b0, b1, track);
            dl->AddRectFilled(ImVec2(b0.x + 2, t0), ImVec2(b1.x - 2, t0 + thumb),
                              ImGui::IsItemHovered() || ImGui::IsItemActive() ? grab_hot : grab, S(2));
        }
        if (need_h && need_v) dl->AddRectFilled(ImVec2(c1.x, c1.y), ImVec2(c1.x + bar, c1.y + bar), track);

        // Everything above was placed at absolute positions; reserve the whole area once
        // so the parent's layout knows its size.
        ImGui::PopID();
        ImGui::SetCursorScreenPos(origin);
        ImGui::Dummy(size);
    }

    // ---------------------------------------------------------------- right: parameters + steps

    // Parameters on top; the Steps table of the selected section fills the rest.
    void draw_right(ImVec2 pos, ImVec2 size) {
        ImGui::SetCursorScreenPos(pos);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(pal::surface));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::BeginChild("##right", size, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
        const float top = ImGui::GetCursorScreenPos().y;

        band("Parameters");
        draw_parameters();

        const float used = ImGui::GetCursorScreenPos().y - top;
        const float table_h = std::max(S(40), size.y - used - band_height());

        const SectionRun* r = nullptr;
        auto it = runs.find(sel_section);
        if (it != runs.end()) r = &it->second;
        band("Steps", r && r->total > 0 ? strf("%d/%d", r->done(), r->total) : std::string());
        draw_steps_table(r, table_h);

        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    // A path text field bound directly to a std::string. ImGui edits a char buffer; the
    // resize callback grows the string as the text grows (the pattern of ImGui's
    // misc/cpp/imgui_stdlib). *committed is set on Enter or when the field loses focus after
    // an edit, so callers act once per edit instead of on every keystroke. The full path
    // shows as a tooltip, since the field is often narrower than the path.
    bool input_path(const char* id, std::string& value, bool* committed) {
        struct Cb {
            static int resize(ImGuiInputTextCallbackData* d) {
                if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
                    auto* s = static_cast<std::string*>(d->UserData);
                    s->resize((size_t)d->BufTextLen);
                    d->Buf = &(*s)[0];
                }
                return 0;
            }
        };
        if (value.capacity() < 260) value.reserve(260);   // MAX_PATH, so typing rarely has to grow it
        ImGui::PushFont(font_mono, 0.0f);
        const bool enter = ImGui::InputText(id, &value[0], value.capacity() + 1,
                                            ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_EnterReturnsTrue,
                                            Cb::resize, &value);
        ImGui::PopFont();
        if (committed) *committed = enter || ImGui::IsItemDeactivatedAfterEdit();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) && !ImGui::IsItemActive() && !value.empty())
            ImGui::SetTooltip("%s", value.c_str());
        return enter;
    }

    // Input and Output folders, Pattern (2 x 2 or 3 x 3) and Refine: the options of vEMstitch
    // itself. All disabled while a stitch runs; the run has already taken its settings.
    void draw_parameters() {
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(S(3), S(2)));
        ImGui::Dummy(ImVec2(1, S(2)));
        ImGui::Indent(S(6));
        const float right_pad = S(6);
        ImGui::BeginDisabled(running);
        // A 3-column table (label | field | browse button) keeps the rows aligned.
        if (ImGui::BeginTable("##form", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadOuterX,
                              ImVec2(ImGui::GetContentRegionAvail().x - right_pad, 0))) {
            ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("b", ImGuiTableColumnFlags_WidthFixed);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Input");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-FLT_MIN);
            bool committed = false;
            input_path("##input", input_buf, &committed);
            // A typed-in folder is scanned once the edit is finished (and only if it changed).
            if (committed && !plat::same_path(input_buf, scan.path)) defer([this]() { rescan(true); });
            ImGui::TableSetColumnIndex(2);
            if (ImGui::Button("...##browse_in")) defer([this]() { browse_input_folder(); });
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Browse... for the tile folder");

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Output");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-FLT_MIN);
            committed = false;
            input_path("##output", output_buf, &committed);
            if (committed) update_mosaic_candidate();   // a saved mosaic may exist in the new folder
            ImGui::TableSetColumnIndex(2);
            if (ImGui::Button("...##browse_out")) defer([this]() { browse_output(); });
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Browse... for the output folder");

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Pattern");
            ImGui::TableSetColumnIndex(1);
            ImGui::RadioButton("2 x 2", &pattern, 2);
            ImGui::SameLine();
            ImGui::RadioButton("3 x 3", &pattern, 3);
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            text_col(pal::muted, "tiles per section");
            ImGui::EndTable();
        }
        ImGui::Dummy(ImVec2(1, S(2)));
        ImGui::Checkbox("Refine (feature re-extraction)", &refine);
        ImGui::EndDisabled();
        ImGui::Dummy(ImVec2(1, S(2)));
        ImGui::PushStyleColor(ImGuiCol_Text, v4(pal::muted));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - right_pad);
        ImGui::TextUnformatted(kRefineNote);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::Unindent(S(6));
        ImGui::Dummy(ImVec2(1, S(4)));
        ImGui::PopStyleVar();
    }

    // The user clicked a Steps row: highlight it and stop following the latest step.
    void select_step(const StepRow& s) {
        sel_step = s.id;
        step_pinned = true;
    }

    // One row per step of the selected section's last run: label, status, time. A step in
    // progress shows a live seconds counter (only for the section being stitched); a failed
    // step's error is in its row's tooltip. With no rows, a hint is drawn over the table.
    void draw_steps_table(const SectionRun* r, float height) {
        const ImVec2 tpos = ImGui::GetCursorScreenPos();
        const ImGuiTableFlags tf = ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersOuterH |
                                   ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit |
                                   ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_Resizable;
        const float row_h = ImGui::GetTextLineHeight() + ImGui::GetStyle().CellPadding.y * 2.0f;
        if (ImGui::BeginTable("##steps", 3, tf, ImVec2(-FLT_MIN, height))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Step", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, S(72));
            ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, S(64));
            table_headers();
            // Time since the run started, on the UI's clock. StepRow::start_t is on the
            // engine's clock, which started a few milliseconds earlier: close enough for a
            // whole-second counter.
            const double now_t = std::chrono::duration<double>(Clock::now() - run_t0).count();
            if (r) {
                for (const auto& s : r->steps) {
                    const bool sel = s.id == sel_step;
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushID(s.id.c_str());
                    if (sel) push_selected_row_colors();
                    if (ImGui::Selectable(s.label.c_str(), sel, ImGuiSelectableFlags_SpanAllColumns))
                        select_step(s);
                    if (!s.error.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                        ImGui::SetTooltip("%s", s.error.c_str());
                    // One cell: white text on the selected row, else the given colour.
                    auto cell = [&](int col, const std::string& txt, ImU32 c, bool right) {
                        ImGui::TableSetColumnIndex(col);
                        const ImU32 cc = sel ? pal::white : c;
                        if (right)
                            text_right(cc, txt);
                        else
                            text_col(cc, txt);
                    };
                    cell(1, s.status, status_color(s.status), false);
                    ImGui::PushFont(font_mono, 0.0f);
                    if (s.ended)
                        cell(2, strf("%.1f s", s.m.elapsed), pal::text, true);
                    else if (running && run_current == sel_section)
                        cell(2, strf("%.0f s", std::max(0.0, now_t - s.start_t)), pal::muted, true);
                    else
                        cell(2, "", pal::text, true);
                    ImGui::PopFont();
                    if (sel) ImGui::PopStyleColor(4);
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
        if (!r || r->steps.empty()) {
            std::string hint;
            if (!have_scan || scan.sections.empty())
                hint = "No dataset loaded.";
            else if (r && r->status == "queued")
                hint = "Queued - waits for the sections before it.";
            else if (running && run_current == sel_section)
                hint = "Starting...";
            else if (!sel_section.empty())
                hint = "No steps yet - press F9 to stitch section " + sel_section + ".";
            ImGui::GetWindowDrawList()->AddText(ImVec2(tpos.x + S(6), tpos.y + row_h + S(6)), pal::muted, hint.c_str());
        }
    }

    void draw_status(ImVec2 pos, ImVec2 size) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p1 = pos + size;
        dl->AddRectFilled(pos, p1, pal::blue100);
        dl->AddRectFilled(pos, ImVec2(p1.x, pos.y + 1), pal::line);

        const std::string sec = running ? run_current : last_run_section;
        const SectionRun* r = nullptr;
        auto it = runs.find(sec);
        if (have_run && it != runs.end()) r = &it->second;

        std::string text = status_text;
        if (running && !cancel_requested && r) {
            std::string labels;
            int n = 0;
            for (const auto& s : r->steps)
                if (!s.ended) {
                    if (n++) labels += ", ";
                    labels += s.label;
                }
            if (n) text = "section " + sec + ": " + labels;
        }

        const double elapsed = running ? std::chrono::duration<double>(Clock::now() - run_t0).count() : run_elapsed;
        ImGui::PushFont(font_mono, 0.0f);
        const float fh = ImGui::GetFontSize();
        const float y = pos.y + std::floor((size.y - fh) * 0.5f) + 0.5f;
        float x = p1.x - S(8);
        auto right_text = [&](const std::string& s, float min_w) {
            const float w = std::max(ImGui::CalcTextSize(s.c_str()).x, min_w);
            x -= w;
            const float tw = ImGui::CalcTextSize(s.c_str()).x;
            dl->AddText(ImVec2(x + w - tw, y), pal::blue900, s.c_str());
            x -= S(14);
        };
        right_text(have_run ? strf("%.1f s", elapsed) : std::string(), ImGui::CalcTextSize("0000.0 s").x);
        right_text(run_sec_total > 0 ? strf("section %d/%d", std::max(1, run_sec_index), run_sec_total) : std::string(),
                   ImGui::CalcTextSize("section 0/0").x);
        const int done = r ? r->done() : 0, total = r ? r->total : 0;
        right_text(total > 0 ? strf("step %d/%d", std::min(done, total), total) : std::string(),
                   ImGui::CalcTextSize("step 00/00").x);
        ImGui::PopFont();

        const float bar_w = S(170), bar_h = std::floor(size.y - S(10));
        x -= bar_w;
        ImGui::SetCursorScreenPos(ImVec2(x, pos.y + std::floor((size.y - bar_h) * 0.5f)));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, v4(pal::surface));
        ImGui::PushStyleColor(ImGuiCol_Border, v4(pal::line));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
        ImGui::ProgressBar(total > 0 ? float(std::min(done, total)) / float(total) : 0.0f, ImVec2(bar_w, bar_h), "");
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);

        const ImVec2 t0(pos.x + S(8), pos.y), t1(x - S(12), p1.y);
        if (t1.x > t0.x) {
            dl->PushClipRect(t0, t1, true);
            const float uy = pos.y + std::floor((size.y - ImGui::GetFontSize()) * 0.5f);
            dl->AddText(ImVec2(t0.x, uy), pal::blue900, text.c_str());
            dl->PopClipRect();
        }
    }

    void draw_log(ImVec2 pos, ImVec2 size) {
        if (size.y < 4) return;
        ImGui::SetCursorScreenPos(pos);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(pal::log_bg));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::BeginChild("##logpane", size, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
        band("Log", log.empty() ? std::string() : strf("%d", (int)log.size()));

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(6), S(3)));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8), 0));
        ImGui::BeginChild("##log", ImVec2(-FLT_MIN, -FLT_MIN), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::PushFont(font_mono, 0.0f);
        ImGuiListClipper clipper;
        clipper.Begin((int)log.size());
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const LogLine& l = log[(size_t)i];
                text_col(pal::muted, l.stamp);
                ImGui::SameLine();
                text_col(level_color(l.level), l.text);
            }
        }
        clipper.End();
        ImGui::PopFont();
        // Tolerance of a line plus a scrollbar: the first long path adds a horizontal scrollbar,
        // which shrinks the view by more than 1 px and would otherwise stop the log following.
        const float slack = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ScrollbarSize;
        if (log_to_bottom || ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - slack) ImGui::SetScrollHereY(1.0f);
        log_to_bottom = false;
        if (ImGui::BeginPopupContextWindow("##logmenu")) {
            if (ImGui::MenuItem("Copy log")) {
                std::string all;
                for (const auto& l : log) all += l.stamp + "  " + l.text + "\n";
                ImGui::SetClipboardText(all.c_str());
            }
            if (ImGui::MenuItem("Clear log")) clear_log();
            ImGui::EndPopup();
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(2);

        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    // ---------------------------------------------------------------- modals

    void draw_modals() {
        if (modals.empty()) return;
        Modal& m = modals.front();
        const std::string pid = m.title + "###modal" + std::to_string(m.id);
        if (!ImGui::IsPopupOpen(pid.c_str())) ImGui::OpenPopup(pid.c_str());
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGuiWindowFlags f = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse;
        if (m.kind == Modal::Text) {
            const ImVec2 vs = ImGui::GetMainViewport()->Size;
            ImGui::SetNextWindowSize(ImVec2(std::min(S(760), vs.x - S(40)), std::min(S(580), vs.y - S(40))),
                                     ImGuiCond_Appearing);
        } else {
            f |= ImGuiWindowFlags_AlwaysAutoResize;
        }
        bool done = false, yes = false;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(10)));
        if (ImGui::BeginPopupModal(pid.c_str(), nullptr, f)) {
            const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
            if (m.kind == Modal::Text) {
                ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(pal::surface));
                ImGui::BeginChild("##modal_text", ImVec2(0, -(ImGui::GetFrameHeightWithSpacing() + S(4))),
                                  ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
                ImGui::PushFont(font_mono, 0.0f);
                ImGui::TextUnformatted(m.body.c_str());
                ImGui::PopFont();
                ImGui::EndChild();
                ImGui::PopStyleColor();
                const float bw = S(84);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - bw);
                if (ImGui::Button("Close", ImVec2(bw, 0))) done = true;
                if (focused && (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_Enter))) done = true;
            } else {
                if (m.error) {
                    ImGui::PushFont(font_bold, 0.0f);
                    text_col(pal::bad, "Error");
                    ImGui::PopFont();
                }
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + S(500));
                ImGui::TextUnformatted(m.body.c_str());
                ImGui::PopTextWrapPos();
                ImGui::Dummy(ImVec2(S(320), S(8)));
                const float bw = S(96);
                if (m.kind == Modal::Confirm) {
                    const float total = bw * 2 + ImGui::GetStyle().ItemSpacing.x;
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - total));
                    ImGui::PushFont(font_bold, 0.0f);
                    if (ImGui::Button(m.yes_label.c_str(), ImVec2(bw, 0))) done = yes = true;
                    ImGui::PopFont();
                    ImGui::SameLine();
                    if (ImGui::Button(m.no_label.c_str(), ImVec2(bw, 0))) done = true;
                    if (focused && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)))
                        done = yes = true;
                    if (focused && ImGui::IsKeyPressed(ImGuiKey_Escape)) done = true;
                } else {
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - bw));
                    if (ImGui::Button("OK", ImVec2(bw, 0))) done = true;
                    if (focused && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) ||
                                    ImGui::IsKeyPressed(ImGuiKey_Escape)))
                        done = true;
                }
            }
            if (done) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
        if (done) {
            std::function<void()> cb;
            if (yes) cb = std::move(m.on_yes);
            modals.pop_front();
            if (cb) defer(std::move(cb));
        }
    }

    // ---------------------------------------------------------------- shutdown

    void shutdown() {
        persist();
        loader.reset();
        tiles_view.clear();
        mosaic_view.clear();
        tile_thumbs.clear();
    }
};

// ======================================================================================
// App
// ======================================================================================

App::App(const Host& host) : d(new Impl(host)) {}
App::~App() = default;

void App::set_dpi_scale(float scale) { d->apply_style(scale); }
void App::startup() { d->startup(); }
void App::frame() { d->frame(); }
void App::report_internal_error(const std::string& what) { d->report_internal_error(what); }
bool App::wants_continuous_frames() const { return d->wants_continuous(); }
void App::request_close() { d->request_close(); }
bool App::should_quit() const { return d->quit; }
void App::shutdown() { d->shutdown(); }

bool App::worker_alive() const {
    try {
        return d->runner.running();
    } catch (...) {
        return false;
    }
}

const char* App::name() { return kAppName; }
const char* App::version() { return kAppVersion; }

void App::clear_color(float out_rgba[4]) {
    const ImVec4 c = ImGui::ColorConvertU32ToFloat4(pal::window);
    out_rgba[0] = c.x;
    out_rgba[1] = c.y;
    out_rgba[2] = c.z;
    out_rgba[3] = 1.0f;
}
