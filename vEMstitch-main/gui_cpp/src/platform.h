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
// platform.h - the thin layer between the workbench and Windows (platform layer).
//
// Paths are UTF-8 std::string everywhere in the app. They are converted to UTF-16 only here,
// at the Win32 boundary, and every file-system call in platform.cpp uses the wide-char ("W")
// Windows API, so folder and file names in any language work.
// The dialog helpers use COM: the calling thread must have called CoInitializeEx
// (apartment-threaded), which main.cpp does for the UI thread. They are modal and block the
// calling thread until the dialog closes, so only the UI thread calls them.
// The other helpers keep no state of their own; the image loader thread uses read_file().

#include <cstdint>
#include <string>
#include <vector>

namespace plat {

// ---- UTF-8 <-> UTF-16 (Windows' "wide" strings). "" when the conversion fails.
std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& wide);

// ---- well-known folders
std::string exe_dir();          // folder that holds the running .exe
std::string local_appdata();    // %LOCALAPPDATA%, "" when unknown (settings and crash.log live below it)
std::string fonts_dir();        // usually C:\Windows\Fonts

// ---- path helpers. Both / and \ are accepted as separators; join() inserts a backslash
// and normalize() turns every separator into one.
bool is_dir(const std::string& path);
bool is_file(const std::string& path);
bool make_dirs(const std::string& path, std::string* err = nullptr);   // like mkdir -p; true if it exists afterwards
std::string join(const std::string& a, const std::string& b);
std::string file_name(const std::string& path);     // last component
std::string parent_dir(const std::string& path);    // keeps a drive root as "C:\"; "" when there is no parent
std::string normalize(const std::string& path);     // absolute, backslashes, no trailing separator
std::string lower_ext(const std::string& path);     // ".bmp", "" when none
bool same_path(const std::string& a, const std::string& b);   // same place after normalize(), ignoring case

// ---- whole-file I/O. On failure they return false and, if err is given, say why.
bool read_file(const std::string& path, std::vector<unsigned char>& out, std::string* err = nullptr);
bool write_file(const std::string& path, const std::vector<unsigned char>& data, std::string* err = nullptr);

// ---- shell
// Modal shell dialogs. owner is the HWND of the main window. "" means cancelled.
// The initial folder only seeds the dialog; if it no longer exists, its nearest existing
// parent is used.
std::string pick_folder(void* owner, const std::string& title, const std::string& initial);
// Open-file dialog filtered to tile images (bmp/png/tif/tiff/jpg/jpeg); "" when cancelled.
std::string pick_image_file(void* owner, const std::string& title, const std::string& initial_dir);
bool open_in_explorer(const std::string& path, std::string* err = nullptr);   // opens a folder in Explorer

// ---- text for the log and error messages
std::string local_time_hms();                 // "HH:MM:SS"
std::string local_timestamp();                // "YYYY-MM-DD HH:MM:SS"
std::string error_text(unsigned long code);   // FormatMessage text for a Win32 error code

} // namespace plat
