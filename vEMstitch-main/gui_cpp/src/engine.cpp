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

// engine.cpp - the stitching engine of vEMstitch Workbench (engine layer: no UI code here).
//
// It does two jobs for the UI (app.cpp), through the interface in engine.h:
//   1. scan_dataset() finds the tiles of each section in a folder. Image sizes come from the
//      file headers, so even a big folder scans quickly.
//   2. Runner stitches sections on a background thread by calling vEMstitch's own C++ building
//      blocks (preprocess / stitching_pair / stitching_rows, compiled unmodified from
//      ..\vEMstitch_c++ via algo_bridge.h) and reports every step to the UI as an Event.
//
// Why it does not simply call their three_stitching(): its rows run in an OpenMP parallel loop
// and each thread does an unsynchronised tier_list.push_back, so the rows land in the order they
// finish. The merge then joins rows that are not neighbours, gets an empty transform and
// cv::solve aborts. It failed 3 out of 3 runs with several threads and works with one. So this
// file repeats their call sequence step by step, call for call: three_stitching() for 3x3, and
// for 2x2 two_stitching(..., 0.1, refine), the overload their main.cpp uses. The deliberate
// differences (see README.md, "How a run works" and "Known issues"):
//   * rows run on their own std::threads and each writes into its fixed slot rows[r-1], so the
//     row order is guaranteed (their two_stitching gets this right with "omp ordered");
//   * tiles are located by trying several extensions (their code hard-codes .bmp) and are read
//     through Unicode-safe file APIs, so non-English folder names work;
//   * a row whose tile 1 or 2 is missing passes the present tile through, as the authors'
//     Python version does, and logs a warning (their C++ leaves the row result empty);
//   * every step runs inside try/catch, so a C++ exception from their code fails that section,
//     not the app (a hard crash such as an access violation cannot be caught this way);
//   * their std::cout / std::cerr messages are redirected into the Log while a run lasts;
//   * cancel is checked between steps, because their calls cannot be interrupted.
//
// Threads: the UI thread owns the Runner. One worker thread runs the sections one after another,
// and inside a section every grid row gets its own thread. All of them report through Sink, a
// mutex-guarded queue that the UI thread drains with Runner::poll().

#include "engine.h"
#include "algo_bridge.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <streambuf>
#include <thread>
#include <unordered_map>
#include <utility>

namespace fs = std::filesystem;

namespace vem {

const Tile* Section::find(int row, int col) const {
    for (const Tile& t : tiles)
        if (t.row == row && t.col == col) return &t;
    return nullptr;
}

namespace {

using Clock = std::chrono::steady_clock;   // steady: step timings must not jump with the wall clock

// Tile extensions, in order of preference. A run tries them in this order, and when one cell
// exists under two extensions the scan keeps the same one the run would pick. .bmp comes first
// because it is the only format their own tool reads.
const char* const kTileExts[] = {".bmp", ".png", ".tif", ".tiff", ".jpg", ".jpeg"};
constexpr int kTileExtCount = int(sizeof(kTileExts) / sizeof(kTileExts[0]));
constexpr int kMinPattern = 2;
constexpr int kMaxPattern = 3;              // their main.cpp handles pattern 2 and 3 only
constexpr int kMaxGridIndex = 999;          // larger "row"/"col" fields are not tile indices
constexpr double kPattern2Overlap = 0.1;    // their main.cpp: two_stitching(..., 0.1, refine_flag)

// --------------------------------------------------------------------------------------------
// small helpers
// --------------------------------------------------------------------------------------------

// Seconds elapsed since t0.
double seconds_since(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

// printf-style formatting into a std::string; every log message is built with it. Short text
// fits the stack buffer; longer text is formatted a second time into a string of the exact size.
std::string strf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return std::string();
    if (n < int(sizeof buf)) return std::string(buf, size_t(n));
    std::string s(size_t(n) + 1, '\0');
    va_start(ap, fmt);
    std::vsnprintf(&s[0], s.size(), fmt, ap);
    va_end(ap);
    s.resize(size_t(n));
    return s;
}

// Paths are UTF-8 std::string everywhere; std::filesystem needs u8path on Windows. fs::path is
// wide-char there, and u8path / u8string convert explicitly from and to UTF-8, so a folder name
// in any language survives the round trip.
fs::path to_path(const std::string& utf8) { return fs::u8path(utf8); }
std::string from_path(const fs::path& p) { return p.u8string(); }

// ASCII-only case changes, used on file extensions. Bytes of UTF-8 characters are left alone.
std::string lower_ascii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

std::string upper_ascii(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    return s;
}

// True when the path has no non-ASCII bytes, i.e. cv::imread can open it (see decode_file).
bool is_ascii(const std::string& s) {
    for (unsigned char c : s)
        if (c >= 0x80) return false;
    return true;
}

// OpenCV messages end in newlines and sometimes span lines; the log wants one line.
std::string one_line(std::string s) {
    for (char& c : s)
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    while (!s.empty() && s.back() == ' ') s.pop_back();
    size_t start = 0;
    while (start < s.size() && s[start] == ' ') ++start;
    return s.substr(start);
}

// e.what() as a single log line, never empty.
std::string exception_text(const std::exception& e) {
    std::string s = one_line(e.what());
    return s.empty() ? std::string("unknown error") : s;
}

// error_code overload: a bad path or an access problem just means "no", never an exception.
bool is_regular_file(const std::string& path) {
    std::error_code ec;
    return fs::is_regular_file(to_path(path), ec);
}

// Reads a whole file into memory. std::ifstream is opened with an fs::path, which uses the
// wide-char Windows API, so non-ASCII paths work. False when the file is missing, empty or short.
bool read_bytes(const std::string& path, std::vector<uchar>& out) {
    std::ifstream f(to_path(path), std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff n = f.tellg();
    if (n <= 0) return false;
    f.seekg(0, std::ios::beg);
    out.resize(size_t(n));
    f.read(reinterpret_cast<char*>(out.data()), std::streamsize(n));
    return f.gcount() == std::streamsize(n);
}

// cv::imread with the given flags. cv::imread cannot open non-ASCII paths on Windows, so those
// (and anything imread refuses) are decoded from memory; the decoded Mat is the same.
// Never throws: an empty Mat means the file could not be read or decoded.
cv::Mat decode_file(const std::string& path, int flags) {
    cv::Mat img;
    try {
        if (is_ascii(path)) img = cv::imread(path, flags);
        if (img.empty()) {
            std::vector<uchar> buf;
            if (read_bytes(path, buf)) img = cv::imdecode(buf, flags);
        }
    } catch (...) {
        img.release();
    }
    return img;
}

// Saves img in the format named by ext (e.g. ".bmp"). Like cv::imread, cv::imwrite cannot handle
// non-ASCII paths on Windows, so the image is encoded in memory with cv::imencode and written
// with std::ofstream on the wide-char path. Returns false with a readable reason in err.
bool write_image(const fs::path& file, const cv::Mat& img, const char* ext, std::string& err) {
    try {
        std::vector<uchar> buf;
        if (!cv::imencode(ext, img, buf)) {
            err = "could not encode the image";
            return false;
        }
        std::ofstream f(file, std::ios::binary | std::ios::trunc);
        if (!f) {
            err = "cannot open the file for writing";
            return false;
        }
        f.write(reinterpret_cast<const char*>(buf.data()), std::streamsize(buf.size()));
        f.close();
        if (!f) {
            err = "write failed (disk full?)";
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        err = exception_text(e);
    } catch (...) {
        err = "unknown error";
    }
    return false;
}

// --------------------------------------------------------------------------------------------
// image size from the file header (BMP, PNG, TIFF/BigTIFF, JPEG)
// --------------------------------------------------------------------------------------------
// The scan shows each tile's pixel size. Decoding every tile of a big dataset just for that
// would be slow, so the size is read from the first bytes of the file. Anything unusual makes
// these functions return false, and the caller falls back to decoding the image.

// Little- and big-endian integer reads from a byte buffer (the formats differ in byte order).
std::uint32_t le16(const unsigned char* p) { return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8); }
std::uint32_t le32(const unsigned char* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) |
           (std::uint32_t(p[3]) << 24);
}
std::uint32_t be16(const unsigned char* p) { return (std::uint32_t(p[0]) << 8) | std::uint32_t(p[1]); }
std::uint32_t be32(const unsigned char* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) |
           std::uint32_t(p[3]);
}

// Rejects nonsense values read from a damaged or misread header.
bool plausible_size(long long w, long long h) { return w > 0 && h > 0 && w <= 1000000 && h <= 1000000; }

// TIFF: follow the offset in the header to the first image directory (IFD) and read the
// ImageWidth (tag 256) and ImageLength (tag 257) entries. little = "II" byte order; big =
// BigTIFF, which has 64-bit offsets and counts and 20-byte entries instead of 12.
bool tiff_size(std::ifstream& f, const unsigned char* hd, bool little, bool big, int& w, int& h) {
    auto u16 = [little](const unsigned char* p) { return little ? le16(p) : be16(p); };
    auto u32 = [little](const unsigned char* p) { return little ? le32(p) : be32(p); };
    auto u64 = [&](const unsigned char* p) {
        const std::uint64_t a = u32(p), b = u32(p + 4);
        return little ? (a | (b << 32)) : ((a << 32) | b);
    };
    const std::uint64_t ifd = big ? u64(hd + 8) : std::uint64_t(u32(hd + 4));
    if (ifd < 8) return false;
    f.clear();
    f.seekg(std::streamoff(ifd), std::ios::beg);
    unsigned char cnt[8] = {0};
    f.read(reinterpret_cast<char*>(cnt), big ? 8 : 2);
    if (!f) return false;
    const std::uint64_t count = big ? u64(cnt) : std::uint64_t(u16(cnt));
    if (count == 0 || count > 4096) return false;   // garbage count: do not allocate for it
    const size_t entry = big ? 20 : 12;
    std::vector<unsigned char> buf(size_t(count) * entry);
    f.read(reinterpret_cast<char*>(buf.data()), std::streamsize(buf.size()));
    if (f.gcount() != std::streamsize(buf.size())) return false;
    long long width = 0, height = 0;
    for (size_t i = 0; i < size_t(count); ++i) {
        const unsigned char* e = &buf[i * entry];
        const std::uint32_t tag = u16(e), type = u16(e + 2);
        const unsigned char* val = e + (big ? 12 : 8);
        long long v = 0;
        if (type == 3) v = u16(val);            // SHORT
        else if (type == 4) v = u32(val);       // LONG
        else if (type == 16) v = (long long)u64(val);  // LONG8 (BigTIFF)
        else continue;
        if (tag == 256) width = v;
        else if (tag == 257) height = v;
    }
    if (!plausible_size(width, height)) return false;
    w = int(width);
    h = int(height);
    return true;
}

// JPEG: walk the marker segments after the SOI marker until a start-of-frame (SOFn) segment,
// which holds the height and width. The guard bounds the walk on a corrupt file.
bool jpeg_size(std::ifstream& f, int& w, int& h) {
    f.clear();
    f.seekg(2, std::ios::beg);
    for (int guard = 0; guard < 100000; ++guard) {
        int c = f.get();
        if (c != 0xFF) return false;
        do c = f.get(); while (c == 0xFF);
        if (c == EOF) return false;
        if (c == 0x01 || (c >= 0xD0 && c <= 0xD8)) continue;   // markers without a length
        if (c == 0xD9 || c == 0xDA) return false;               // EOI / SOS before any SOF
        unsigned char lb[2];
        f.read(reinterpret_cast<char*>(lb), 2);
        if (!f) return false;
        const std::uint32_t len = be16(lb);
        if (len < 2) return false;
        // SOF0..SOF15; C4 (DHT), C8 (JPG) and CC (DAC) share the range but are not frames.
        if (c >= 0xC0 && c <= 0xCF && c != 0xC4 && c != 0xC8 && c != 0xCC) {
            unsigned char s[5];
            f.read(reinterpret_cast<char*>(s), 5);
            if (!f) return false;
            const long long height = be16(s + 1), width = be16(s + 3);
            if (!plausible_size(width, height)) return false;
            w = int(width);
            h = int(height);
            return true;
        }
        f.seekg(std::streamoff(len) - 2, std::ios::cur);
        if (!f) return false;
    }
    return false;
}

// Reads the pixel size from the file header without decoding the image. Recognises the format
// by its signature, not by the extension. False when the format is unknown or the header is
// damaged. Never throws.
bool header_size(const std::string& path, int& w, int& h) {
    try {
        std::ifstream f(to_path(path), std::ios::binary);
        if (!f) return false;
        unsigned char hd[32] = {0};
        f.read(reinterpret_cast<char*>(hd), sizeof hd);
        const std::streamsize got = f.gcount();
        if (got >= 26 && hd[0] == 'B' && hd[1] == 'M') {
            const std::uint32_t dib = le32(hd + 14);
            long long width = 0, height = 0;
            // DIB header size 12 = old OS/2 header with 16-bit sizes; 40 or more = Windows
            // BITMAPINFOHEADER or later, with signed 32-bit sizes.
            if (dib == 12) {
                width = le16(hd + 18);
                height = le16(hd + 20);
            } else if (dib >= 40) {
                width = std::int32_t(le32(hd + 18));
                height = std::int32_t(le32(hd + 22));
                if (height < 0) height = -height;   // top-down bitmap
            }
            if (!plausible_size(width, height)) return false;
            w = int(width);
            h = int(height);
            return true;
        }
        static const unsigned char png_sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        if (got >= 24 && std::memcmp(hd, png_sig, 8) == 0 && std::memcmp(hd + 12, "IHDR", 4) == 0) {
            const long long width = be32(hd + 16), height = be32(hd + 20);
            if (!plausible_size(width, height)) return false;
            w = int(width);
            h = int(height);
            return true;
        }
        if (got >= 8 && hd[0] == 'I' && hd[1] == 'I' && hd[2] == 42 && hd[3] == 0)
            return tiff_size(f, hd, true, false, w, h);
        if (got >= 8 && hd[0] == 'M' && hd[1] == 'M' && hd[2] == 0 && hd[3] == 42)
            return tiff_size(f, hd, false, false, w, h);
        if (got >= 16 && hd[0] == 'I' && hd[1] == 'I' && hd[2] == 43 && hd[3] == 0)
            return tiff_size(f, hd, true, true, w, h);
        if (got >= 16 && hd[0] == 'M' && hd[1] == 'M' && hd[2] == 0 && hd[3] == 43)
            return tiff_size(f, hd, false, true, w, h);
        if (got >= 4 && hd[0] == 0xFF && hd[1] == 0xD8) return jpeg_size(f, w, h);
    } catch (...) {
    }
    return false;
}

// --------------------------------------------------------------------------------------------
// dataset scanning helpers
// --------------------------------------------------------------------------------------------

// Position of a (lower-case) extension in kTileExts, or -1 when it is not a tile format.
// A lower rank is preferred.
int extension_rank(const std::string& ext_lower) {
    for (int i = 0; i < kTileExtCount; ++i)
        if (ext_lower == kTileExts[i]) return i;
    return -1;
}

// A row or column field of a tile name: 1 to 6 digits with a value of 1..kMaxGridIndex.
bool parse_index(const std::string& s, int& out) {
    if (s.empty() || s.size() > 6) return false;
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    if (v < 1 || v > kMaxGridIndex) return false;
    out = v;
    return true;
}

// "{section}_{row}_{col}": row and col are the last two '_'-separated fields and must be
// numeric; the section is everything before them (same as ^(.+)_(\d+)_(\d+)$).
bool parse_tile_stem(const std::string& stem, std::string& section, int& row, int& col) {
    const size_t u2 = stem.rfind('_');
    if (u2 == std::string::npos || u2 == 0) return false;
    const size_t u1 = stem.rfind('_', u2 - 1);
    if (u1 == std::string::npos || u1 == 0) return false;
    if (!parse_index(stem.substr(u1 + 1, u2 - u1 - 1), row)) return false;
    if (!parse_index(stem.substr(u2 + 1), col)) return false;
    section = stem.substr(0, u1);
    return !section.empty();
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// Natural order: "sec2" < "sec10", "165" < "1000", so sections are listed the way a person
// expects. Numbers are compared by value; equal numbers with fewer leading zeros come first.
bool natural_less(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (is_digit(a[i]) && is_digit(b[j])) {
            size_t i2 = i, j2 = j;
            while (i2 < a.size() && is_digit(a[i2])) ++i2;
            while (j2 < b.size() && is_digit(b[j2])) ++j2;
            size_t ia = i, jb = j;
            while (ia + 1 < i2 && a[ia] == '0') ++ia;
            while (jb + 1 < j2 && b[jb] == '0') ++jb;
            const size_t la = i2 - ia, lb = j2 - jb;
            if (la != lb) return la < lb;
            const int c = a.compare(ia, la, b, jb, lb);
            if (c != 0) return c < 0;
            if (i2 - i != j2 - j) return (i2 - i) < (j2 - j);
            i = i2;
            j = j2;
        } else {
            const unsigned char ca = static_cast<unsigned char>(a[i]);
            const unsigned char cb = static_cast<unsigned char>(b[j]);
            if (ca != cb) return ca < cb;
            ++i;
            ++j;
        }
    }
    return (a.size() - i) < (b.size() - j);
}

// --------------------------------------------------------------------------------------------
// event sink shared by the run worker and the row threads
// --------------------------------------------------------------------------------------------

// Everything the engine tells the UI goes through here. Any engine thread may push; only the UI
// thread pops (Runner::poll). It also holds the cancel flag, which every thread reads.
struct Sink {
    std::mutex mu;                        // guards queue and t0
    std::deque<Event> queue;
    Clock::time_point t0 = Clock::now();  // run start; reset by Runner::start
    std::atomic<bool> cancel{false};      // set by Runner::cancel, checked before each step and section

    void push(Event e) {
        try {
            std::lock_guard<std::mutex> lock(mu);
            e.t = seconds_since(t0);        // stamped under the lock so queue order == time order
            queue.push_back(std::move(e));
        } catch (...) {
            // Out of memory while queueing an event: dropping it beats terminating a worker.
        }
    }

    // Queues a Log line. section tags it with a section id (empty = about the whole run).
    // Never throws, because it is also called from catch blocks and row threads.
    void log(Level level, const std::string& msg, const std::string& section = std::string()) {
        try {
            Event e;
            e.type = EventType::Log;
            e.level = level;
            e.msg = msg;
            e.section = section;
            push(std::move(e));
        } catch (...) {
        }
    }
};

// --------------------------------------------------------------------------------------------
// vEMstitch's own console output
// --------------------------------------------------------------------------------------------
// Their code prints a few messages with std::cout / std::cerr ("feature number = ...",
// "Exception caught: ..."). A windowed app has no console, so during a run both streams are
// pointed at LineCapture, which hands every finished line to the Log. Their functions run on
// several row threads at once, so each thread's text is collected separately and tagged with
// the step that thread is running.

// thread_local because several row threads run their code at once: each thread knows which
// step it is in, and a line printed on that thread is tagged with it.
thread_local std::string t_step_label;   // set by run_step while their code runs on this thread
thread_local std::string t_section;

// A streambuf that splits the text written to it into lines and sends each finished line to
// the Log. The unfinished line is kept per thread (lines_), so text from two row threads that
// print at the same time is never mixed; mu_ guards that map.
class LineCapture : public std::streambuf {
public:
    LineCapture(Sink& sink, Level level) : sink_(sink), level_(level) {}

protected:
    // The stream calls overflow for single characters and xsputn for blocks of text.
    int_type overflow(int_type ch) override {
        if (traits_type::eq_int_type(ch, traits_type::eof())) return traits_type::not_eof(ch);
        const char c = traits_type::to_char_type(ch);
        put(&c, 1);
        return ch;
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        put(s, n);
        return n;
    }
    int sync() override {       // std::endl and flush: emit any partial line
        std::string line;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = lines_.find(std::this_thread::get_id());
            if (it != lines_.end()) line.swap(it->second);
        }
        emit(line);
        return 0;
    }

private:
    void put(const char* s, std::streamsize n) {
        std::vector<std::string> done;
        {
            std::lock_guard<std::mutex> lock(mu_);
            std::string& cur = lines_[std::this_thread::get_id()];
            for (std::streamsize i = 0; i < n; ++i) {
                if (s[i] == '\n') {
                    done.push_back(std::move(cur));
                    cur.clear();
                } else if (s[i] != '\r') {
                    cur += s[i];
                }
            }
        }
        for (const std::string& line : done) emit(line);   // outside mu_: Sink takes its own lock
    }
    // Sends one line to the Log as "vEMstitch [step]: text", so the user can see that it came
    // from their code and which step printed it.
    void emit(const std::string& text) {
        if (text.empty()) return;
        const std::string where = t_step_label.empty() ? std::string() : " [" + t_step_label + "]";
        sink_.log(level_, "vEMstitch" + where + ": " + text, t_section);
    }

    Sink& sink_;
    Level level_;
    std::mutex mu_;
    std::unordered_map<std::thread::id, std::string> lines_;
};

// Points std::cout / std::cerr at the Log for its lifetime, then restores them. It lives on the
// worker thread for the whole run (Runner::Impl::run_all), so the streams are restored even if
// the run ends with an exception. cout lines are logged as Info and cerr lines as Warn. The swap
// is process-wide: anything else printed during a run lands in the Log too.
class ConsoleToLog {
public:
    explicit ConsoleToLog(Sink& sink) : out_(sink, Level::Info), err_(sink, Level::Warn) {
        old_out_ = std::cout.rdbuf(&out_);   // rdbuf() also clears any error state
        old_err_ = std::cerr.rdbuf(&err_);
    }
    ~ConsoleToLog() {
        // flush() makes LineCapture::sync emit this thread's unfinished line, if any.
        std::cout.flush();
        std::cerr.flush();
        std::cout.rdbuf(old_out_);
        std::cerr.rdbuf(old_err_);
    }
    ConsoleToLog(const ConsoleToLog&) = delete;
    ConsoleToLog& operator=(const ConsoleToLog&) = delete;

private:
    LineCapture out_, err_;
    std::streambuf* old_out_ = nullptr;
    std::streambuf* old_err_ = nullptr;
};

// --------------------------------------------------------------------------------------------
// one section
// --------------------------------------------------------------------------------------------

// How a step or a row ended. Cancelled also covers "stopped because another row failed".
enum class Outcome { NotRun, Ok, Failed, Cancelled };

// What a row thread leaves in its fixed slot: the stitched row (res) and the mask their code
// returns with it (mass), which the merge passes back to them as that row's mask.
struct RowResult {
    Outcome outcome = Outcome::NotRun;
    cv::Mat res, mass;
    std::string error;
};

// What a step body hands back to run_step: the output image and mask, plus how it was made
// (path and shortcut end up in the StepEnd event).
struct StepWork {           // what a step body hands back
    cv::Mat res, mass;
    std::string path = "stitch";
    bool shortcut = false;
};

// Stitches one section: the rows in parallel, then the row merges top to bottom, then writes
// {section}-res.bmp. The worker creates one per section. Nothing escapes it: any failure only
// fails this section.
//
// Steps of an N x N section, in index order (what the Steps table lists):
//   for each row r:  "Row r - tiles 1+2", then "Row r - + tile k" for k = 3..N   (N-1 per row)
//   then:            "Merge rows 1-2", "Merge rows 1-3", ...                      (N-1 merges)
// so total_ = N*(N-1) + (N-1): 8 steps for 3x3, 3 for 2x2.
class SectionJob {
public:
    SectionJob(Sink& sink, const RunConfig& cfg, const Section* scanned, const std::string& id)
        : sink_(sink), cfg_(cfg), scanned_(scanned), id_(id), n_(cfg.pattern),
          total_(cfg.pattern * (cfg.pattern - 1) + (cfg.pattern - 1)), pattern2_(cfg.pattern == 2) {}

    // Runs the section and emits SectionStart ... SectionEnd. Returns "ok", "failed" or
    // "cancelled". Never throws.
    std::string run(int section_index, int sections_total);

private:
    Sink& sink_;
    const RunConfig& cfg_;     // owned by Runner::Impl, which outlives the job
    const Section* scanned_;   // the scan entry for this section, nullptr when unavailable
    std::string id_;
    int n_;
    int total_;
    bool pattern2_;
    fs::path out_dir_;
    std::atomic<bool> abort_{false};    // a row failed: the other rows stop at their next step
    std::atomic<int> steps_done_{0};    // steps ended so far, on any thread

    // True when the user cancelled or another row of this section failed.
    bool stop_requested() const { return sink_.cancel.load() || abort_.load(); }
    void log(Level level, const std::string& msg) { sink_.log(level, msg, id_); }

    // 0-based place of each step in the list above; the UI sorts the Steps table by it.
    int pair_index(int r) const { return (r - 1) * (n_ - 1); }
    int fold_index(int r, int k) const { return (r - 1) * (n_ - 1) + (k - 2); }
    int merge_index(int m) const { return n_ * (n_ - 1) + (m - 1); }

    std::string execute(std::string& error, std::string& output, int& width, int& height);
    std::string locate_tile(int row, int col) const;
    cv::Mat load_tile(int row, int col);
    Outcome run_step(const std::string& step_id, const std::string& label, int index,
                     const std::function<void(StepWork&)>& body, cv::Mat& res, cv::Mat& mass,
                     std::string& error);
    RowResult run_row(int r);
    std::string summarise(const StepMetrics& m) const;
};

// Finds the file of tile (row, col). The scan's path is used when it still exists; otherwise
// {section}_{row}_{col} is tried with each extension in kTileExts order, lower then upper case.
// Empty string = the tile is missing.
std::string SectionJob::locate_tile(int row, int col) const {
    if (scanned_) {
        const Tile* t = scanned_->find(row, col);
        if (t && is_regular_file(t->path)) return t->path;
    }
    const fs::path dir = to_path(cfg_.input);
    const std::string stem = id_ + "_" + std::to_string(row) + "_" + std::to_string(col);
    for (int i = 0; i < kTileExtCount; ++i) {
        const std::string exts[2] = {kTileExts[i], upper_ascii(kTileExts[i])};
        for (const std::string& ext : exts) {
            const std::string candidate = from_path(dir / to_path(stem + ext));
            if (is_regular_file(candidate)) return candidate;
        }
    }
    return std::string();
}

// cv::imread(path) (default colour) followed by cvtColor(BGR2GRAY), exactly as they read tiles,
// so their code gets the same grey pixels it would get from its own reader. The read goes
// through decode_file, so non-ASCII paths work. A file that cannot be decoded or converted is
// treated like a missing tile, with a warning. Empty Mat = missing.
cv::Mat SectionJob::load_tile(int row, int col) {
    const std::string path = locate_tile(row, col);
    if (path.empty()) return cv::Mat();
    cv::Mat img = decode_file(path, cv::IMREAD_COLOR);
    if (img.empty()) {
        log(Level::Warn, strf("could not decode %s; tile %d,%d is treated as missing",
                              from_path(to_path(path).filename()).c_str(), row, col));
        return cv::Mat();
    }
    try {
        cv::cvtColor(img, img, cv::COLOR_BGR2GRAY);
    } catch (const std::exception& e) {
        log(Level::Warn, strf("could not convert tile %d,%d to grey (%s); treated as missing", row, col,
                              exception_text(e).c_str()));
        return cv::Mat();
    }
    return img;
}

// The tail of a step's Log line, e.g. "2048x1768, stitch".
std::string SectionJob::summarise(const StepMetrics& m) const {
    return strf("%dx%d, %s", m.out_w, m.out_h, m.path.c_str());
}

// Runs one step: sends StepStart, runs body (the calls into their code), then sends StepEnd with
// the time, output size and status, and logs one line. body runs inside try/catch, so an
// exception from their code (cv::Exception or any other) becomes a failed step with its message
// instead of ending the app. On success res/mass receive the output; on failure they are left
// as they were and error says what went wrong. Called on row threads and on the worker thread.
Outcome SectionJob::run_step(const std::string& step_id, const std::string& label, int index,
                             const std::function<void(StepWork&)>& body, cv::Mat& res, cv::Mat& mass,
                             std::string& error) {
    // Cancel is checked here, before the step starts. Once body is inside their code it cannot
    // be interrupted, so a cancel only takes effect at the next step.
    if (stop_requested()) return Outcome::Cancelled;

    Event start;
    start.type = EventType::StepStart;
    start.section = id_;
    start.step_id = step_id;
    start.label = label;
    start.index = index;
    start.total = total_;
    start.msg = label;
    sink_.push(start);

    StepWork w;
    std::string err;
    const auto t = Clock::now();
    t_step_label = label;       // anything their code prints now is tagged with this step
    t_section = id_;
    try {
        body(w);
        if (w.res.empty()) err = "the step produced an empty image";
    } catch (const cv::Exception& e) {
        err = exception_text(e);
    } catch (const std::exception& e) {
        err = exception_text(e);
    } catch (...) {
        err = "unknown exception";
    }
    t_step_label.clear();       // their code is done: later output on this thread is not this step's
    t_section.clear();
    const double took = seconds_since(t);

    StepMetrics m;
    m.path = w.path;
    m.elapsed = took;
    if (err.empty()) {
        m.out_w = w.res.cols;
        m.out_h = w.res.rows;
    }
    // A passthrough is shown as a warning: a tile was missing, so the mosaic is incomplete.
    const bool passthrough = w.path == "passthrough";
    const std::string status = !err.empty() ? "failed" : (w.shortcut ? "shortcut" : "ok");

    Event end;
    end.type = EventType::StepEnd;
    end.section = id_;
    end.step_id = step_id;
    end.label = label;
    end.index = index;
    end.total = total_;
    end.status = status;
    end.metrics = m;
    end.error = err;
    end.elapsed = m.elapsed;
    end.level = !err.empty() ? Level::Error : (passthrough ? Level::Warn : (w.shortcut ? Level::Info : Level::Ok));
    end.msg = err.empty() ? strf("%s: %s in %.2f s - %s", label.c_str(), status.c_str(), m.elapsed,
                                 summarise(m).c_str())
                          : strf("%s failed after %.2f s: %s", label.c_str(), m.elapsed, err.c_str());
    sink_.push(end);
    ++steps_done_;
    log(end.level, end.msg);

    if (!err.empty()) {
        error = label + " failed: " + err;
        return Outcome::Failed;
    }
    res = w.res;
    mass = w.mass;
    return Outcome::Ok;
}

// One row: tiles 1+2, then + tile k for k = 3..N, as in their three_stitching row loop body.
// Normally runs on the row's own thread (see execute); returns the result for the row's slot.
// Never throws. Missing tiles: their C++ leaves the row result empty when tile 1 or 2 is missing;
// here the present tile is passed through instead, as the authors' Python version does, and a
// warning is logged. With both tiles missing the row fails.
RowResult SectionJob::run_row(int r) {
    RowResult out;
    try {
        cv::Mat img_1 = load_tile(r, 1);
        cv::Mat img_2 = load_tile(r, 2);
        if (img_1.empty() && img_2.empty())
            log(Level::Error, strf("row %d: tiles %s_%d_1 and %s_%d_2 are both missing", r, id_.c_str(), r,
                                   id_.c_str(), r));
        else if (img_1.empty() || img_2.empty())
            log(Level::Warn, strf("row %d: tile %s_%d_%d is missing, passing tile %d,%d through", r,
                                  id_.c_str(), r, img_1.empty() ? 1 : 2, r, img_1.empty() ? 2 : 1));

        // Step "tiles 1+2". The stitching part is their row body copied call for call, including
        // the empty masks given to preprocess and the all-ones CV_64FC1 masks given to
        // stitching_pair.
        const auto first_pair = [&](StepWork& w) {
            if (img_1.empty() && img_2.empty())
                throw std::runtime_error(strf("tiles %s_%d_1 and %s_%d_2 are both missing", id_.c_str(), r,
                                              id_.c_str(), r));
            if (img_1.empty() || img_2.empty()) {
                // Pass the present tile through, with the same all-ones mask their code builds
                // for a lone tile.
                const cv::Mat& present = img_1.empty() ? img_2 : img_1;
                w.res = present;
                w.mass = cv::Mat::ones(present.size(), CV_64FC1);
                w.path = "passthrough";
                w.shortcut = true;
                return;
            }
            cv::Mat stitching_res, mass;
            cv::Mat im1_mask, im2_mask;
            cv::Mat img_1_mask, img_2_mask;
            cv::Mat stitching_res_temp, mass_temp;
            std::string mode = "r";
            bool process_flag = false;
            im1_mask = cv::Mat();
            im2_mask = cv::Mat();
            std::tie(stitching_res_temp, mass_temp, process_flag) =
                preprocess(img_1, img_2, im1_mask, im2_mask, mode);
            if (process_flag) {
                img_1_mask = cv::Mat::ones(img_1.size(), CV_64FC1);
                img_2_mask = cv::Mat::ones(img_2.size(), CV_64FC1);
                // 2x2 uses the overlap overload, as their two_stitching(..., 0.1, refine) does;
                // 3x3 uses the plain one, as their three_stitching does.
                if (pattern2_)
                    std::tie(stitching_res, mass, std::ignore) =
                        stitching_pair(img_1, img_2, img_1_mask, img_2_mask, mode, kPattern2Overlap);
                else
                    std::tie(stitching_res, mass, std::ignore) =
                        stitching_pair(img_1, img_2, img_1_mask, img_2_mask, mode);
                stitching_res.convertTo(stitching_res, CV_8UC1);   // back to 8-bit, as theirs
                w.path = "stitch";
            } else {
                // preprocess found a nearly blank overlap and joined the tiles itself.
                stitching_res = stitching_res_temp;
                mass = mass_temp;
                stitching_res.convertTo(stitching_res, CV_8UC1);
                w.path = "preprocess-shortcut";
                w.shortcut = true;
            }
            w.res = stitching_res;
            w.mass = mass;
        };

        cv::Mat stitching_res, mass;
        std::string error;
        Outcome o = run_step(strf("r%dc1+2", r), strf("Row %d - tiles 1+2", r), pair_index(r), first_pair,
                             stitching_res, mass, error);
        if (o != Outcome::Ok) {
            out.outcome = o;
            out.error = error;
            return out;
        }
        img_1.release();            // the tiles are no longer needed: free them early
        img_2.release();

        // Once a tile is missing, later tiles of the row are not loaded and the row is passed
        // through to its end, as their code does when tile 3 is missing (it keeps the row as is).
        bool row_broken = false;
        for (int k = 3; k <= n_; ++k) {
            cv::Mat img_k;
            if (!row_broken) {
                img_k = load_tile(r, k);
                if (img_k.empty()) {
                    row_broken = true;
                    log(Level::Warn, strf("row %d: tile %s_%d_%d is missing, the rest of the row is passed "
                                          "through unchanged", r, id_.c_str(), r, k));
                }
            }
            // Step "+ tile k": fold tile k onto the right of the row so far, as their code does
            // for tile 3.
            const auto fold = [&](StepWork& w) {
                if (img_k.empty()) {
                    w.res = stitching_res;
                    w.mass = mass;
                    w.path = "passthrough";
                    w.shortcut = true;
                    return;
                }
                // Header copies (no pixel copy) of the row's running result, used exactly like
                // their stitching_res / mass variables in three_stitching. Their functions take
                // cv::Mat& and may reassign what they get (preprocess can convert im1 to double
                // in place), so the step works on these copies; the row's own stitching_res /
                // mass are only written by run_step, once the step has succeeded.
                cv::Mat res_k = stitching_res, mass_k = mass;
                cv::Mat stitching_res_temp, mass_temp;
                // The mask types are copied exactly from their code even though they differ:
                // the tile 1+2 masks above are CV_64FC1, their tile-3 mask has the image's type.
                cv::Mat img_k_mask = cv::Mat::ones(img_k.size(), img_k.type());   // CV_8U, as theirs
                std::string mode = "r";
                bool process_flag = false;
                cv::Mat none_mask = cv::Mat();
                std::tie(stitching_res_temp, mass_temp, process_flag) =
                    preprocess(res_k, img_k, mass_k, none_mask, mode);
                if (process_flag) {
                    if (pattern2_)
                        std::tie(res_k, mass_k, std::ignore) =
                            stitching_pair(res_k, img_k, mass_k, img_k_mask, mode, kPattern2Overlap);
                    else
                        std::tie(res_k, mass_k, std::ignore) =
                            stitching_pair(res_k, img_k, mass_k, img_k_mask, mode);
                    res_k.convertTo(res_k, CV_8UC1);
                    w.path = "stitch";
                } else {
                    res_k = stitching_res_temp;
                    mass_k = mass_temp;
                    res_k.convertTo(res_k, CV_8UC1);
                    w.path = "preprocess-shortcut";
                    w.shortcut = true;
                }
                w.res = res_k;
                w.mass = mass_k;
            };
            o = run_step(strf("r%dc+%d", r, k), strf("Row %d - + tile %d", r, k), fold_index(r, k), fold,
                         stitching_res, mass, error);
            if (o != Outcome::Ok) {
                out.outcome = o;
                out.error = error;
                return out;
            }
        }
        out.outcome = Outcome::Ok;
        out.res = stitching_res;
        out.mass = mass;
    } catch (const std::exception& e) {
        // Safety net for errors outside run_step, e.g. out of memory while loading a tile.
        out.outcome = Outcome::Failed;
        out.error = strf("row %d: %s", r, exception_text(e).c_str());
    } catch (...) {
        out.outcome = Outcome::Failed;
        out.error = strf("row %d: unknown exception", r);
    }
    return out;
}

// The section itself: checks the pattern and the output folder, runs the rows in parallel,
// merges them top to bottom and writes {section}-res.bmp. Returns "ok", "failed" or
// "cancelled"; error / output / width / height describe the result. May throw: run() catches.
std::string SectionJob::execute(std::string& error, std::string& output, int& width, int& height) {
    if (n_ < kMinPattern || n_ > kMaxPattern) {
        error = strf("pattern must be between %d and %d, got %d", kMinPattern, kMaxPattern, n_);
        return "failed";
    }

    std::error_code ec;
    out_dir_ = to_path(cfg_.output);
    {
        std::error_code aec;
        const fs::path abs = fs::absolute(out_dir_, aec);
        if (!aec) out_dir_ = abs.lexically_normal();   // absolute, native separators, for the Log
    }
    fs::create_directories(out_dir_, ec);
    if (!fs::is_directory(out_dir_, ec)) {
        error = "cannot create the output folder " + cfg_.output;
        return "failed";
    }
    log(Level::Info, strf("section %s: %dx%d grid, refine=%s", id_.c_str(), n_, n_, cfg_.refine ? "on" : "off"));
    {
        std::error_code ec2;
        const fs::path in_abs = fs::absolute(to_path(cfg_.input), ec2);
        log(Level::Info, "input " + (ec2 ? cfg_.input : from_path(in_abs.lexically_normal())));
        log(Level::Info, "output " + from_path(out_dir_));
    }
    // A tile grid that does not match the chosen pattern is run anyway, with a warning: extra
    // rows and columns are ignored and absent tiles are treated as missing.
    if (scanned_) {
        if (scanned_->rows > n_ || scanned_->cols > n_)
            log(Level::Warn, strf("section %s has a %dx%d tile grid; pattern %d uses only the first %dx%d",
                                  id_.c_str(), scanned_->rows, scanned_->cols, n_, n_, n_));
        else if (scanned_->rows < n_ || scanned_->cols < n_)
            log(Level::Warn, strf("section %s has a %dx%d tile grid but pattern %d expects %dx%d",
                                  id_.c_str(), scanned_->rows, scanned_->cols, n_, n_, n_));
    }

    // ---- rows: one thread per row, each writing only its own slot ----
    // This is the fix for the race in their three_stitching: its OpenMP row loop pushes each
    // finished row into one shared vector, unsynchronised, so rows land in the order they finish.
    std::vector<RowResult> rows(static_cast<size_t>(n_));
    const int saved_omp_threads = omp_get_max_threads();
    // Rows run concurrently, as their three_stitching/two_stitching do, but each thread writes
    // only its own slot, so the rows cannot land out of order.
    {
        // Their code has OpenMP loops of its own. Each row thread gets an equal share of the
        // cores for them, so N rows at once do not oversubscribe the machine.
        const int per_row = std::max(1, omp_get_num_procs() / n_);
        log(Level::Info, strf("rows run concurrently: %d threads, %d OpenMP threads each", n_, per_row));
        std::vector<std::thread> threads;
        for (int r = 1; r <= n_; ++r) {
            RowResult* slot = &rows[size_t(r - 1)];
            const auto row_body = [this, r, per_row, slot]() {
                try {
                    omp_set_num_threads(per_row);
                    *slot = run_row(r);
                    if (slot->outcome == Outcome::Failed) {
                        // The section has failed anyway: stop the other rows at their next step.
                        abort_ = true;
                        log(Level::Error, strf("row %d failed; the other rows stop after their current step", r));
                    }
                } catch (...) {
                    // run_row does not throw; this is a last guard, because an exception that
                    // escapes a std::thread function calls std::terminate and ends the app.
                    abort_ = true;
                    slot->outcome = Outcome::Failed;
                    try {
                        slot->error = strf("row %d: unexpected error in the row thread", r);
                    } catch (...) {
                    }
                }
            };
            try {
                threads.emplace_back(row_body);
            } catch (const std::exception& e) {
                // No thread available: run the row here on the worker thread instead, before
                // the next row is started.
                log(Level::Warn, strf("could not start a thread for row %d (%s); running it here", r,
                                      exception_text(e).c_str()));
                row_body();
            }
        }
        for (std::thread& t : threads)
            if (t.joinable()) t.join();
        omp_set_num_threads(saved_omp_threads);   // put the OpenMP thread count back as it was
    }

    // A failed row fails the section; any other row that did not finish was cancelled.
    for (const RowResult& row : rows)
        if (row.outcome == Outcome::Failed) {
            error = row.error.empty() ? std::string("a row failed") : row.error;
            return "failed";
        }
    for (const RowResult& row : rows)
        if (row.outcome != Outcome::Ok) {
            log(Level::Warn, strf("cancelled after %d of %d steps", steps_done_.load(), total_));
            return "cancelled";
        }

    // ---- merge top to bottom, as their while (tier_list.size() >= 2) loop ----
    // Merge m joins the block of rows 1..m with row m+1 through their stitching_rows ("d" =
    // down). This part runs in order on the worker thread, as in their code. Refine only
    // affects these calls and can make one merge take over an hour.
    std::vector<cv::Mat> tier_list, tier_mask_list;
    for (RowResult& row : rows) {
        tier_list.push_back(row.res);
        tier_mask_list.push_back(row.mass);
        row.res.release();          // drop the slot's reference; tier_list now holds the row
        row.mass.release();
    }
    int m = 0;
    while (tier_list.size() >= 2) {
        ++m;
        const auto merge = [&](StepWork& w) {
            cv::Mat im1 = tier_list[0];
            cv::Mat im2 = tier_list[1];
            cv::Mat im1_mask = tier_mask_list[0];
            cv::Mat im2_mask = tier_mask_list[1];
            std::string mode = "d";
            cv::Mat stitching_res, mass, overlap_mass;
            // 2x2: the overlap overload, as their two_stitching(..., 0.1, refine) does.
            if (pattern2_)
                std::tie(stitching_res, mass, overlap_mass) =
                    stitching_rows(im1, im2, im1_mask, im2_mask, mode, kPattern2Overlap, cfg_.refine);
            else
                std::tie(stitching_res, mass, overlap_mass) =
                    stitching_rows(im1, im2, im1_mask, im2_mask, mode, cfg_.refine);
            stitching_res.convertTo(stitching_res, CV_8UC1);
            w.res = stitching_res;
            w.mass = mass;
            w.path = "stitch";
        };
        cv::Mat stitching_res, mass;
        const Outcome o = run_step(strf("merge%d", m), strf("Merge rows 1-%d", m + 1), merge_index(m), merge,
                                   stitching_res, mass, error);
        if (o == Outcome::Failed) return "failed";
        if (o != Outcome::Ok) {
            log(Level::Warn, strf("cancelled after %d of %d steps", steps_done_.load(), total_));
            return "cancelled";
        }
        tier_list[1] = stitching_res;
        tier_mask_list[1] = mass;
        tier_list.erase(tier_list.begin());
        tier_mask_list.erase(tier_mask_list.begin());
    }

    // A cancel that arrived during the last merge still counts: the mosaic is not written.
    if (sink_.cancel.load()) {
        log(Level::Warn, strf("cancelled after %d of %d steps; the mosaic was not written",
                              steps_done_.load(), total_));
        return "cancelled";
    }

    // Same file name and format as their tool writes, but saved through write_image so a
    // non-ASCII output folder works.
    cv::Mat final_res = tier_list[0];
    final_res.convertTo(final_res, CV_8UC1);
    const fs::path out_file = out_dir_ / to_path(id_ + "-res.bmp");
    std::string werr;
    if (!write_image(out_file, final_res, ".bmp", werr)) {
        error = "could not write " + from_path(out_file) + ": " + werr;
        return "failed";
    }
    output = from_path(out_file);
    width = final_res.cols;
    height = final_res.rows;
    log(Level::Ok, strf("section %s done: %dx%d -> %s", id_.c_str(), width, height, output.c_str()));
    return "ok";
}

// Wraps execute(): sends SectionStart, turns any exception into a "failed" status, logs the
// failure, and always sends SectionEnd, so the UI never keeps a section marked as running.
std::string SectionJob::run(int section_index, int sections_total) {
    const auto t0 = Clock::now();
    {
        Event e;
        e.type = EventType::SectionStart;
        e.section = id_;
        e.total = total_;
        e.sections_done = section_index;
        e.sections_total = sections_total;
        e.msg = strf("section %s (%d of %d)", id_.c_str(), section_index + 1, sections_total);
        sink_.push(std::move(e));
    }

    std::string status = "failed", error, output;
    int width = 0, height = 0;
    try {
        status = execute(error, output, width, height);
    } catch (const std::exception& e) {
        status = "failed";
        error = exception_text(e);
    } catch (...) {
        status = "failed";
        error = "unknown exception";
    }
    const double elapsed = seconds_since(t0);
    if (status == "failed")
        log(Level::Error, "section " + id_ + " failed: " + (error.empty() ? std::string("unknown error") : error));

    Event e;
    e.type = EventType::SectionEnd;
    e.section = id_;
    e.total = total_;
    e.status = status;
    e.error = status == "failed" ? error : std::string();
    e.output_path = output;
    e.width = width;
    e.height = height;
    e.elapsed = elapsed;
    e.sections_done = section_index + 1;
    e.sections_total = sections_total;
    e.level = status == "ok" ? Level::Ok : (status == "cancelled" ? Level::Warn : Level::Error);
    e.msg = status == "ok" ? strf("section %s: ok, %dx%d in %.1f s", id_.c_str(), width, height, elapsed)
                           : strf("section %s: %s after %.1f s", id_.c_str(), status.c_str(), elapsed);
    sink_.push(std::move(e));
    return status;
}

// --------------------------------------------------------------------------------------------
// runner helpers
// --------------------------------------------------------------------------------------------

// True when a and b name the same existing folder (fs::equivalent, so letter case, separators
// and relative paths do not matter). Runner::start uses it to decide whether the UI's scan
// belongs to the run's input folder.
bool same_folder(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return false;
    std::error_code ec;
    const bool eq = fs::equivalent(to_path(a), to_path(b), ec);
    return !ec && eq;
}

}  // namespace

// ------------------------------------------------------------------------------------------------
// scan_dataset
// ------------------------------------------------------------------------------------------------

// See engine.h. Lists the folder once (no subfolders), keeps the files whose names parse as
// tiles, groups them by section and (row, col), then builds one Section per id in natural order.
ScanResult scan_dataset(const std::string& folder) {
    ScanResult result;
    result.path = folder;
    try {
        if (folder.empty()) {
            result.error = "no input directory selected";
            return result;
        }
        std::error_code ec;
        fs::path root = to_path(folder);
        const fs::path abs = fs::absolute(root, ec);
        if (!ec) root = abs.lexically_normal();
        result.path = from_path(root);
        if (!fs::is_directory(root, ec)) {
            result.error = "not a directory: " + result.path;
            return result;
        }

        // section id -> (row, col) -> the file found for that cell and its extension rank
        struct Found { int rank; Tile tile; };
        std::map<std::string, std::map<std::pair<int, int>, Found>> grouped;

        fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
        if (ec) {
            result.error = "cannot list " + result.path + ": " + ec.message();
            return result;
        }
        const fs::directory_iterator end;
        while (it != end) {
            const fs::directory_entry entry = *it;
            std::error_code fec;
            if (entry.is_regular_file(fec)) {
                const std::string name = from_path(entry.path().filename());
                const size_t dot = name.rfind('.');
                if (dot != std::string::npos && dot > 0) {
                    const int rank = extension_rank(lower_ascii(name.substr(dot)));
                    std::string section;
                    int row = 0, col = 0;
                    if (rank >= 0 && parse_tile_stem(name.substr(0, dot), section, row, col)) {
                        auto& cells = grouped[section];
                        const auto key = std::make_pair(row, col);
                        const auto existing = cells.find(key);
                        // Same cell under two extensions: keep the one the run would pick.
                        if (existing == cells.end() || rank < existing->second.rank) {
                            Tile t;
                            t.row = row;
                            t.col = col;
                            t.file = name;
                            t.path = from_path(entry.path());
                            const std::uintmax_t size = entry.file_size(fec);
                            t.bytes = fec ? 0 : std::uint64_t(size);
                            // Header first; decode the whole image only when that fails.
                            int w = 0, h = 0;
                            if (!header_size(t.path, w, h)) {
                                const cv::Mat img = decode_file(t.path, cv::IMREAD_UNCHANGED);
                                w = img.cols;
                                h = img.rows;
                            }
                            t.w = w;
                            t.h = h;
                            cells[key] = Found{rank, t};
                        }
                    }
                }
            }
            // increment(ec) rather than ++it: a listing error must not throw. The tiles found
            // so far are kept and the error is reported.
            it.increment(ec);
            if (ec) {
                result.error = "listing " + result.path + " stopped early: " + ec.message();
                break;
            }
        }

        std::vector<std::string> ids;
        for (const auto& kv : grouped) ids.push_back(kv.first);
        std::sort(ids.begin(), ids.end(), natural_less);

        int guess = 0;
        for (const std::string& id : ids) {
            const auto& cells = grouped[id];
            Section s;
            s.id = id;
            for (const auto& kv : cells) {
                s.rows = std::max(s.rows, kv.first.first);
                s.cols = std::max(s.cols, kv.first.second);
                s.tiles.push_back(kv.second.tile);   // std::map order is (row, col)
            }
            // Every cell inside the rows x cols rectangle without a file is missing.
            for (int r = 1; r <= s.rows; ++r)
                for (int c = 1; c <= s.cols; ++c)
                    if (cells.find(std::make_pair(r, c)) == cells.end()) s.missing.emplace_back(r, c);
            s.complete = s.missing.empty();
            guess = std::max(guess, std::max(s.rows, s.cols));
            result.sections.push_back(std::move(s));
        }

        if (result.sections.empty()) {
            if (result.error.empty())
                result.error = "no tiles matching {section}_{row}_{col}.{bmp,png,tif,tiff,jpg,jpeg} in " + result.path;
            return result;
        }
        // The largest grid seen, clamped to the grids vEMstitch supports.
        result.pattern_guess = std::min(kMaxPattern, std::max(kMinPattern, guess));
        result.ok = true;
    } catch (const std::exception& e) {
        result.ok = false;
        result.sections.clear();
        result.error = "scan failed: " + exception_text(e);
    } catch (...) {
        result.ok = false;
        result.sections.clear();
        result.error = "scan failed: unknown exception";
    }
    return result;
}

// ------------------------------------------------------------------------------------------------
// Runner
// ------------------------------------------------------------------------------------------------

// Runner's state, shared by the UI thread and the worker. done / done_mu / done_cv exist so the
// UI can ask whether the worker has finished and wait for it with a time limit (join_for):
// std::thread alone has no way to do either.
struct Runner::Impl {
    Sink sink;
    std::thread worker;
    std::mutex done_mu;                 // guards done
    std::condition_variable done_cv;    // signalled by finish()
    bool done = true;                   // true while no run is in progress
    RunConfig cfg;                      // written by start() before the worker starts, then read-only
    std::vector<Section> scanned;   // scan entries of the requested sections (same folder only)

    void run_all();
    // Marks the worker as done and wakes join_for. It is the last thing the worker does, after
    // RunEnd is queued, so by the time running() turns false RunEnd is already in the queue.
    void finish() {
        {
            std::lock_guard<std::mutex> lock(done_mu);
            done = true;
        }
        done_cv.notify_all();
    }
};

// The worker thread's body: sends vEMstitch's console output to the Log, runs the requested
// sections one after another, then queues RunEnd with the totals. A failed section does not
// stop the run (the next one still runs); a cancel does.
void Runner::Impl::run_all() {
    ConsoleToLog console(sink);   // vEMstitch's own messages go to the Log while the run lasts
    const auto t0 = Clock::now();
    const int count = int(cfg.sections.size());
    int ended = 0, ok = 0, failed = 0;
    bool cancelled = false;
    for (int i = 0; i < count; ++i) {
        if (sink.cancel.load()) {       // cancel between sections
            cancelled = true;
            break;
        }
        const std::string& id = cfg.sections[size_t(i)];
        const Section* entry = nullptr;
        for (const Section& s : scanned)
            if (s.id == id) entry = &s;
        SectionJob job(sink, cfg, entry, id);
        const std::string status = job.run(i, count);
        ++ended;
        if (status == "ok") {
            ++ok;
        } else if (status == "failed") {
            ++failed;
        } else {
            cancelled = true;
            break;
        }
    }

    Event e;
    e.type = EventType::RunEnd;
    e.status = cancelled ? "cancelled" : (failed > 0 ? "failed" : "ok");
    e.level = cancelled ? Level::Warn : (failed > 0 ? Level::Error : Level::Ok);
    e.sections_done = ended;
    e.sections_total = count;
    e.elapsed = seconds_since(t0);
    e.msg = strf("run %s: %d ok, %d failed, %d not run, %.1f s", e.status.c_str(), ok, failed,
                 count - ok - failed, e.elapsed);
    sink.push(std::move(e));
}

Runner::Runner() : d(new Impl) {}

// Cancels, then waits for the worker. It stops at its next step boundary, which can take as
// long as the step in progress (see engine.h for why main.cpp avoids this mid-run).
Runner::~Runner() {
    try {
        if (d) {
            d->sink.cancel = true;
            if (d->worker.joinable()) d->worker.join();
        }
    } catch (...) {
    }
}

// Validates everything up front, so a bad request is refused with a reason on the UI thread
// instead of failing later on the worker. Then resets the queue clock and cancel flag and starts
// the worker. Never throws.
bool Runner::start(const RunConfig& cfg, const ScanResult& scan, std::string* why_not) {
    const auto refuse = [why_not](const std::string& why) {
        if (why_not) *why_not = why;
        return false;
    };
    try {
        if (running()) return refuse("a run is already in progress");
        // The previous run has finished (running() is false), but its std::thread object must be
        // joined before a new thread is assigned to it (assigning to a joinable std::thread calls
        // std::terminate). The old thread is already exiting, so this returns quickly.
        if (d->worker.joinable()) d->worker.join();   // previous run has finished

        std::error_code ec;
        if (cfg.input.empty()) return refuse("no input folder selected");
        if (!fs::is_directory(to_path(cfg.input), ec)) return refuse("input folder does not exist: " + cfg.input);
        if (cfg.output.empty()) return refuse("no output folder selected");
        if (cfg.pattern < kMinPattern || cfg.pattern > kMaxPattern)
            return refuse(strf("pattern must be between %d and %d, got %d", kMinPattern, kMaxPattern, cfg.pattern));
        if (cfg.sections.empty()) return refuse("no section selected");

        // The UI's scan is only trusted when it is of the run's input folder. The entries are
        // copied, so the worker never reads the UI's ScanResult.
        std::vector<Section> scanned;
        const bool scan_matches = same_folder(scan.path, cfg.input);
        for (const std::string& id : cfg.sections) {
            if (id.empty()) return refuse("empty section id");
            if (!scan_matches) continue;
            const Section* found = nullptr;
            for (const Section& s : scan.sections)
                if (s.id == id) found = &s;
            if (!found) return refuse("section " + id + " is not in the scanned dataset - rescan the input folder");
            scanned.push_back(*found);
        }

        {
            std::lock_guard<std::mutex> lock(d->sink.mu);   // Sink::push reads t0 under this lock
            d->sink.t0 = Clock::now();
        }
        d->sink.cancel = false;
        d->cfg = cfg;
        d->scanned = std::move(scanned);
        {
            std::lock_guard<std::mutex> lock(d->done_mu);
            d->done = false;
        }
        d->sink.log(Level::Info, strf("run started: %d section(s), pattern %d, refine=%s",
                                      int(cfg.sections.size()), cfg.pattern, cfg.refine ? "on" : "off"));
        if (!scan_matches)
            d->sink.log(Level::Warn, "the dataset scan is for a different folder; tiles are located by name in " +
                                         cfg.input);

        Impl* impl = d.get();
        try {
            d->worker = std::thread([impl]() {
                try {
                    impl->run_all();
                } catch (...) {
                    // Last guard: an exception escaping the thread function would end the app.
                    // Still queue a RunEnd, so the UI leaves its running state.
                    try {
                        Event e;
                        e.type = EventType::RunEnd;
                        e.status = "failed";
                        e.level = Level::Error;
                        e.msg = "run failed: unexpected error in the worker";
                        e.sections_total = int(impl->cfg.sections.size());
                        impl->sink.push(std::move(e));
                    } catch (...) {
                    }
                }
                impl->finish();
            });
        } catch (const std::exception& e) {
            d->finish();            // no worker: mark the runner idle again
            return refuse("could not start the worker thread: " + exception_text(e));
        }
        if (why_not) why_not->clear();
        return true;
    } catch (const std::exception& e) {
        return refuse("could not start the run: " + exception_text(e));
    } catch (...) {
        return refuse("could not start the run: unknown exception");
    }
}

// Only sets the flag; the worker and row threads see it before their next step or section.
// The step in progress runs to its end, because their calls cannot be interrupted.
void Runner::cancel() {
    if (!running()) return;
    if (d->sink.cancel.exchange(true)) return;   // already requested
    d->sink.log(Level::Warn, "cancel requested: the run stops once the step(s) in progress finish");
}

bool Runner::running() const {
    std::lock_guard<std::mutex> lock(d->done_mu);
    return !d->done;
}

// Called by the UI thread every frame: pops the oldest event, or returns false at once.
bool Runner::poll(Event& out) {
    std::lock_guard<std::mutex> lock(d->sink.mu);
    if (d->sink.queue.empty()) return false;
    out = std::move(d->sink.queue.front());
    d->sink.queue.pop_front();
    return true;
}

// Waits up to the given time for the worker to report done, then joins the (exiting) thread.
// The UI calls it with a short timeout each frame while closing, so the window stays responsive.
bool Runner::join_for(int milliseconds) {
    {
        std::unique_lock<std::mutex> lock(d->done_mu);
        if (!d->done_cv.wait_for(lock, std::chrono::milliseconds(std::max(0, milliseconds)),
                                 [this] { return d->done; }))
            return false;
    }
    try {
        if (d->worker.joinable()) d->worker.join();
    } catch (...) {
    }
    return true;
}

// Version of the OpenCV library in use; falls back to the version compiled against.
std::string opencv_version() {
    try {
        return cv::getVersionString();
    } catch (...) {
        return CV_VERSION;
    }
}

// Logical CPU count. hardware_concurrency() may return 0 when it cannot tell, so at least 1.
int hardware_threads() {
    const unsigned n = std::thread::hardware_concurrency();
    return n > 0 ? int(n) : 1;
}

}  // namespace vem
