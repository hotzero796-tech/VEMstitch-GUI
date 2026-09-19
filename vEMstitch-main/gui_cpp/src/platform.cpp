// platform.cpp - Win32 implementation of platform.h (platform layer).
//
// The only place in the app that converts between the app's UTF-8 paths and the UTF-16
// strings Windows uses. Every file-system call here goes through the wide-char ("W") API
// (or std::filesystem with a wide path) so non-English folder names work whatever the
// system's code page is. Nothing here keeps state between calls.

#include "platform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cstdio>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace plat {

// ---------------------------------------------------------------- UTF-8 / UTF-16

// Two calls each: the first asks for the output length, the second converts.
std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out((size_t)n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), &out[0], n);
    return out;
}

std::string narrow(const std::wstring& wide) {
    if (wide.empty()) return std::string();
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out((size_t)n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), &out[0], n, nullptr, nullptr);
    return out;
}

// ---------------------------------------------------------------- well-known folders

// GetModuleFileNameW silently truncates when the buffer is too small (it then returns the
// buffer size), so the buffer grows until the whole path fits.
std::string exe_dir() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = ::GetModuleFileNameW(nullptr, &buf[0], (DWORD)buf.size());
        if (n == 0) return std::string();
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    const size_t slash = buf.find_last_of(L"\\/");
    if (slash != std::wstring::npos) buf.resize(slash);
    return narrow(buf);
}

// Path of a shell known folder, or "". The returned buffer must be freed with
// CoTaskMemFree even when the call fails.
static std::string known_folder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    std::string out;
    if (SUCCEEDED(::SHGetKnownFolderPath(id, 0, nullptr, &raw)) && raw) out = narrow(raw);
    if (raw) ::CoTaskMemFree(raw);
    return out;
}

// Asks the shell first; the environment variable is only a fallback.
std::string local_appdata() {
    std::string p = known_folder(FOLDERID_LocalAppData);
    if (!p.empty()) return p;
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return narrow(buf);
    return std::string();
}

// Where App looks for its fonts (Segoe UI, Consolas). Falls back to the Windows folder
// plus Fonts, then to the usual path.
std::string fonts_dir() {
    std::string p = known_folder(FOLDERID_Fonts);
    if (!p.empty()) return p;
    wchar_t buf[MAX_PATH] = {};
    const UINT n = ::GetWindowsDirectoryW(buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return narrow(buf) + "\\Fonts";
    return "C:\\Windows\\Fonts";
}

// ---------------------------------------------------------------- paths

bool is_dir(const std::string& path) {
    if (path.empty()) return false;
    const DWORD a = ::GetFileAttributesW(widen(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool is_file(const std::string& path) {
    if (path.empty()) return false;
    const DWORD a = ::GetFileAttributesW(widen(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Creates the folder and any missing parents. The fs::path is built from a wide string so
// the conversion never depends on the code page. Success is judged by whether the folder
// exists afterwards, not by the error code.
bool make_dirs(const std::string& path, std::string* err) {
    if (path.empty()) {
        if (err) *err = "empty path";
        return false;
    }
    if (is_dir(path)) return true;
    std::error_code ec;
    fs::create_directories(fs::path(widen(path)), ec);
    if (is_dir(path)) return true;
    // std::error_code::message() is in the ANSI code page on MSVC; ask Windows for the text instead.
    if (err) *err = ec ? error_text((unsigned long)ec.value()) : std::string("could not create the folder");
    return false;
}

std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    const char last = a.back();
    if (last == '\\' || last == '/') return a + b;
    return a + "\\" + b;
}

// Trailing separators are ignored, so "D:\data\" gives "data".
std::string file_name(const std::string& path) {
    std::string p = path;
    while (p.size() > 1 && (p.back() == '\\' || p.back() == '/')) p.pop_back();
    const size_t slash = p.find_last_of("\\/");
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

// The parent of "C:\data" is "C:\", not "C:" (which would mean the current folder on
// drive C).
std::string parent_dir(const std::string& path) {
    std::string p = path;
    while (p.size() > 1 && (p.back() == '\\' || p.back() == '/')) p.pop_back();
    const size_t slash = p.find_last_of("\\/");
    if (slash == std::string::npos) return std::string();
    if (slash == 2 && p.size() > 1 && p[1] == ':') return p.substr(0, 3);   // "C:\"
    return p.substr(0, slash);
}

// Cleans up a path typed or pasted by the user: strips surrounding spaces, tabs and quotes
// (Explorer's "Copy as path" adds quotes), turns / into \, makes it absolute and removes
// trailing separators except on a drive root. The folder does not have to exist.
std::string normalize(const std::string& path) {
    std::string trimmed = path;
    while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t' || trimmed.back() == '"'))
        trimmed.pop_back();
    size_t start = 0;
    while (start < trimmed.size() && (trimmed[start] == ' ' || trimmed[start] == '\t' || trimmed[start] == '"'))
        ++start;
    trimmed = trimmed.substr(start);
    if (trimmed.empty()) return std::string();
    std::wstring w = widen(trimmed);
    for (auto& c : w)
        if (c == L'/') c = L'\\';
    // First call: required size including the terminator. Second call: length without it.
    const DWORD need = ::GetFullPathNameW(w.c_str(), 0, nullptr, nullptr);
    if (need > 0) {
        std::wstring full(need, L'\0');
        const DWORD got = ::GetFullPathNameW(w.c_str(), need, &full[0], nullptr);
        if (got > 0 && got < need) {
            full.resize(got);
            w = full;
        }
    }
    while (w.size() > 3 && w.back() == L'\\') w.pop_back();
    return narrow(w);
}

// ASCII lower-casing is enough for image extensions. A leading dot (".hidden") is a name,
// not an extension.
std::string lower_ext(const std::string& path) {
    const std::string name = file_name(path);
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot == 0) return std::string();
    std::string ext = name.substr(dot);
    for (auto& c : ext)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return ext;
}

// Windows file names are case-insensitive, so paths are compared after normalize() with an
// ordinal, case-insensitive comparison (not locale-dependent).
bool same_path(const std::string& a, const std::string& b) {
    const std::wstring wa = widen(normalize(a));
    const std::wstring wb = widen(normalize(b));
    if (wa.empty() || wb.empty()) return false;
    return ::CompareStringOrdinal(wa.c_str(), (int)wa.size(), wb.c_str(), (int)wb.size(), TRUE) == CSTR_EQUAL;
}

// ---------------------------------------------------------------- whole-file I/O

// Reads the whole file into memory: images for cv::imdecode, which takes a byte buffer, and
// the settings file. 64-bit seek and tell, so sizes over 2 GB are right. Running out of
// memory is reported, not thrown.
bool read_file(const std::string& path, std::vector<unsigned char>& out, std::string* err) {
    out.clear();
    FILE* f = nullptr;
    if (_wfopen_s(&f, widen(path).c_str(), L"rb") != 0 || !f) {
        if (err) *err = "cannot open " + path;
        return false;
    }
    bool ok = false;
    if (_fseeki64(f, 0, SEEK_END) == 0) {
        const long long size = _ftelli64(f);
        if (size >= 0 && _fseeki64(f, 0, SEEK_SET) == 0) {
            try {
                out.resize((size_t)size);
                ok = size == 0 || std::fread(out.data(), 1, (size_t)size, f) == (size_t)size;
            } catch (const std::bad_alloc&) {
                if (err) *err = "out of memory reading " + path;
                std::fclose(f);
                out.clear();
                return false;
            }
        }
    }
    std::fclose(f);
    if (!ok) {
        out.clear();
        if (err) *err = "read error on " + path;
    }
    return ok;
}

// Replaces the file. The result of fclose is checked too, because buffered data is only
// written out on close and a full disk may show up only there.
bool write_file(const std::string& path, const std::vector<unsigned char>& data, std::string* err) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, widen(path).c_str(), L"wb") != 0 || !f) {
        if (err) *err = "cannot write " + path;
        return false;
    }
    const bool ok = data.empty() || std::fwrite(data.data(), 1, data.size(), f) == data.size();
    const bool closed = std::fclose(f) == 0;
    if (!(ok && closed)) {
        if (err) *err = "write error on " + path + " (disk full?)";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- shell dialogs

namespace {

// Minimal owner of one COM interface pointer: releases it on scope exit. operator& hands
// out the address of the raw pointer for "out" parameters; it is only used on empty ones.
template <class T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr() {
        if (p) p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// The file-system path of a shell item, or "" for items that are not real files or folders.
std::string shell_item_path(IShellItem* item) {
    if (!item) return std::string();
    PWSTR raw = nullptr;
    std::string out;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) && raw) out = narrow(raw);
    if (raw) ::CoTaskMemFree(raw);
    return out;
}

// SetFolder, not SetDefaultFolder: the dialog opens in this folder every time instead of in
// the one Windows remembers from the last use.
void set_initial_folder(IFileDialog* dlg, const std::string& folder) {
    if (folder.empty() || !is_dir(folder)) return;
    ComPtr<IShellItem> item;
    if (SUCCEEDED(::SHCreateItemFromParsingName(widen(normalize(folder)).c_str(), nullptr, IID_PPV_ARGS(&item))))
        dlg->SetFolder(item.p);
}

} // namespace

// The Common Item Dialog (the normal Explorer-style dialog) in folder-picking mode.
// FOS_FORCEFILESYSTEM: only real folders, no virtual shell locations. FOS_NOCHANGEDIR: do
// not change the process's current directory, which normalize() resolves relative paths
// against. Show() runs a modal message loop on the calling thread until the dialog closes;
// it fails when the user cancels.
std::string pick_folder(void* owner, const std::string& title, const std::string& initial) {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return std::string();
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
    if (!title.empty()) dlg->SetTitle(widen(title).c_str());
    // The last-used folder may have been moved or deleted: start in its nearest existing parent.
    std::string start = initial;
    while (!start.empty() && !is_dir(start)) {
        const std::string up = parent_dir(start);
        if (up == start) break;
        start = up;
    }
    set_initial_folder(dlg.p, start);
    if (FAILED(dlg->Show((HWND)owner))) return std::string();
    ComPtr<IShellItem> result;
    if (FAILED(dlg->GetResult(&result))) return std::string();
    return shell_item_path(result.p);
}

// Same dialog in file mode, used by Open dataset: the user picks any tile and App loads its
// folder. The filter lists the tile formats the app accepts; "All files" lets the user pick
// something else, and App then explains why it is not a tile.
std::string pick_image_file(void* owner, const std::string& title, const std::string& initial_dir) {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return std::string();
    const COMDLG_FILTERSPEC types[] = {
        {L"Tile images (*.bmp;*.png;*.tif;*.tiff;*.jpg;*.jpeg)", L"*.bmp;*.png;*.tif;*.tiff;*.jpg;*.jpeg"},
        {L"All files (*.*)", L"*.*"},
    };
    dlg->SetFileTypes(2, types);
    dlg->SetFileTypeIndex(1);   // 1-based: the tile filter is selected first
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST | FOS_NOCHANGEDIR);
    if (!title.empty()) dlg->SetTitle(widen(title).c_str());
    std::string start = initial_dir;
    while (!start.empty() && !is_dir(start)) {
        const std::string up = parent_dir(start);
        if (up == start) break;
        start = up;
    }
    set_initial_folder(dlg.p, start);
    if (FAILED(dlg->Show((HWND)owner))) return std::string();
    ComPtr<IShellItem> result;
    if (FAILED(dlg->GetResult(&result))) return std::string();
    return shell_item_path(result.p);
}

// "open" on a folder shows it in Explorer. ShellExecuteW reports success with a value
// greater than 32 (a convention kept from 16-bit Windows).
bool open_in_explorer(const std::string& path, std::string* err) {
    const HINSTANCE r = ::ShellExecuteW(nullptr, L"open", widen(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)r > 32) return true;
    if (err) *err = error_text(::GetLastError());
    return false;
}

// ---------------------------------------------------------------- time and error text

// Wall-clock stamp for log lines that have no event time of their own.
std::string local_time_hms() {
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// Date and time, for crash.log.
std::string local_timestamp() {
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour,
                  st.wMinute, st.wSecond);
    return buf;
}

// Windows' own message for an error code, in the user's language, without the trailing
// newline and full stop so it reads well inside a sentence. "error N" if Windows has no text.
std::string error_text(unsigned long code) {
    wchar_t* raw = nullptr;
    const DWORD n = ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                         FORMAT_MESSAGE_IGNORE_INSERTS,
                                     nullptr, code, 0, (LPWSTR)&raw, 0, nullptr);
    std::string out;
    if (n && raw) {
        out = narrow(std::wstring(raw, n));
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ' || out.back() == '.'))
            out.pop_back();
    }
    if (raw) ::LocalFree(raw);
    if (out.empty()) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "error %lu", code);
        out = buf;
    }
    return out;
}

} // namespace plat
