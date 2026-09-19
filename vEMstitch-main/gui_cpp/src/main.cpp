// vEMstitch Workbench - entry point (WinMain) and host process.
//
// This file owns the Win32 window, the Direct3D 11 device and swap chain, the Dear ImGui
// context with its Win32 + DX11 backends, and the message loop. Everything drawn inside the
// window lives in app.cpp (class App); the stitch itself runs in engine.cpp. The skeleton
// comes from Dear ImGui's third_party\imgui\examples\example_win32_directx11\main.cpp.
//
// What was added to that example, and why:
//   - Crash reporting. A windowed app has no console, so a crash would just make the window
//     vanish. Unhandled exceptions and std::terminate now write one line to
//     %LOCALAPPDATA%\vEMstitchWorkbench\crash.log and show a message box.
//   - DPI. The app is per-monitor DPI aware (app.manifest), so Windows does not bitmap-scale
//     it (which looks blurry at 125 %). Sizes here are chosen at 96 dpi and multiplied by the
//     monitor's scale; App scales its fonts and style itself.
//   - Idle waiting. When nothing changes, the loop sleeps until input arrives instead of
//     redrawing at the display refresh rate. A worker thread (the image loader, when a
//     picture is ready) wakes it by posting WM_APP_WAKE.
//   - Occlusion skip. While the window is minimized or completely covered, frames are not
//     drawn and App::frame() does not run. Events from a running stitch wait in their queue
//     meanwhile, which is why the log stamps them with the time they happened, not the time
//     they are drawn.
//   - Closing. The close button asks App first (it confirms while a stitch runs). vEMstitch's
//     calls cannot be interrupted, so a worker still busy at exit is left behind and the
//     process ends with TerminateProcess.
//
// Threading: everything in this file runs on the UI thread. Windows only calls wnd_proc on
// the thread that created the window, so the g_* globals below need no locks.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <objbase.h>

#include <cstdio>
#include <exception>
#include <memory>
#include <string>

#include "imgui.h"
#include "imgui_internal.h"   // ImGui::ErrorRecoveryStoreState / ErrorRecoveryTryToRecoverState
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "app.h"
#include "platform.h"
#include "texture.h"

// Declared in imgui_impl_win32.h only inside "#if 0", so it is declared again here, as the
// Dear ImGui example does.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

constexpr int kIconResource = 101;           // IDI_APPICON in app.rc
constexpr UINT WM_APP_WAKE = WM_APP + 1;     // posted through App::Host::wake by worker threads to wake the loop
constexpr int kIdleWaitMs = 250;             // longest wait for input while nothing is happening

// UI-thread state. wnd_proc only records resize and DPI requests (g_resize_*, g_pending_scale);
// the main loop applies them between frames.
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
bool g_occluded = false;           // last Present said nothing of the window is visible
UINT g_resize_w = 0, g_resize_h = 0;
float g_pending_scale = 0.0f;      // new DPI scale from WM_DPICHANGED, 0 = none pending
App* g_app = nullptr;              // lets wnd_proc forward the close request; null outside App's lifetime

// ---------------------------------------------------------------- crash reporting

// %LOCALAPPDATA%\vEMstitchWorkbench\crash.log, next to settings_cpp.ini. Falls back to the
// exe's folder when the local app-data folder is unknown.
std::string crash_log_path() {
    std::string base = plat::local_appdata();
    if (base.empty()) base = plat::exe_dir();
    return plat::join(plat::join(base, "vEMstitchWorkbench"), "crash.log");
}

// Appends one timestamped line. Opened through the wide-char API so a non-English user
// name in the path still works. Failures are ignored: we are already crashing.
void write_crash_log(const std::string& text) {
    const std::string path = crash_log_path();
    plat::make_dirs(plat::parent_dir(path));
    FILE* f = nullptr;
    if (_wfopen_s(&f, plat::widen(path).c_str(), L"ab") == 0 && f) {
        const std::string line = plat::local_timestamp() + "  " + text + "\r\n";
        std::fwrite(line.data(), 1, line.size(), f);
        std::fclose(f);
    }
}

// Error message box with no owner window, so it also works before the window exists.
void fatal_box(const std::string& text) {
    ::MessageBoxW(nullptr, plat::widen(text).c_str(), L"vEMstitch Workbench", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

// Process-wide handler for crashes that nothing caught (access violations and other SEH
// exceptions), on any thread, including the stitch workers. Windows does not call it while a
// debugger is attached. It logs the exception code and the module + offset of the faulting
// address, which is enough to find the function in a debugger, then tells the user.
LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep) {
    // Only the first crash is logged and shown. A second one (another thread, or a crash
    // inside this handler) returns at once, which ends the process.
    static volatile LONG entered = 0;
    if (::InterlockedExchange(&entered, 1) != 0) return EXCEPTION_EXECUTE_HANDLER;
    const DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    void* addr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
    HMODULE mod = nullptr;
    wchar_t mod_name[MAX_PATH] = L"?";
    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             (LPCWSTR)addr, &mod) &&
        mod)
        ::GetModuleFileNameW(mod, mod_name, MAX_PATH);
    char buf[512];
    std::snprintf(buf, sizeof(buf), "unhandled exception 0x%08lX at %s+0x%llX (thread %lu)", (unsigned long)code,
                  plat::file_name(plat::narrow(mod_name)).c_str(),
                  (unsigned long long)((const char*)addr - (const char*)mod), (unsigned long)::GetCurrentThreadId());
    write_crash_log(buf);
    fatal_box(std::string("vEMstitch Workbench stopped because of an unexpected error:\n\n") + buf +
              "\n\nA note was written to\n" + crash_log_path());
    return EXCEPTION_EXECUTE_HANDLER;   // the process ends now
}

// std::terminate handler: runs when a C++ exception escapes a thread function or a noexcept
// function. Rethrowing the current exception (if any) recovers its message for the log.
void on_terminate() {
    std::string what = "std::terminate was called";
    try {
        if (std::exception_ptr p = std::current_exception()) std::rethrow_exception(p);
    } catch (const std::exception& e) {
        what += std::string(" (uncaught exception: ") + e.what() + ")";
    } catch (...) {
        what += " (uncaught non-standard exception)";
    }
    write_crash_log(what);
    fatal_box("vEMstitch Workbench stopped because of an unexpected error:\n\n" + what + "\n\nA note was written to\n" +
              crash_log_path());
    // A terminate handler must not return, so end the process here (exit code 3, like abort()).
    ::TerminateProcess(::GetCurrentProcess(), 3);
}

// ---------------------------------------------------------------- Direct3D 11

// The render target view wraps the swap chain's back buffer. It is released and recreated
// around every ResizeBuffers call, which fails while such a view still exists.
void create_render_target() {
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(g_swap->GetBuffer(0, IID_PPV_ARGS(&back))) && back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

void cleanup_render_target() {
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
}

// Creates the device, the immediate context and a double-buffered swap chain for the window,
// as the Dear ImGui example does. Width/height 0 means "use the window's client size".
// Returns false when not even the software renderer works.
bool create_device(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    // Feature level 10.0 is enough for the ImGui renderer. The level actually obtained also
    // decides the largest texture, and so the largest mosaic shown at full size (texture.cpp).
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 3,
                                               D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &got, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED || FAILED(hr))   // no usable GPU: the WARP software rasteriser
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 3, D3D11_SDK_VERSION,
                                           &sd, &g_swap, &g_device, &got, &g_context);
    if (FAILED(hr)) return false;

    // Alt+Enter would switch to exclusive full screen; a lab tool has no use for that.
    IDXGIFactory* factory = nullptr;
    if (SUCCEEDED(g_swap->GetParent(IID_PPV_ARGS(&factory))) && factory) {
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        factory->Release();
    }
    create_render_target();
    return g_rtv != nullptr;
}

void cleanup_device() {
    cleanup_render_target();
    if (g_swap) {
        g_swap->Release();
        g_swap = nullptr;
    }
    if (g_context) {
        g_context->Release();
        g_context = nullptr;
    }
    if (g_device) {
        g_device->Release();
        g_device = nullptr;
    }
}

// ---------------------------------------------------------------- window

// The usable part of a monitor (without the taskbar), in physical pixels. Falls back to the
// primary monitor's work area, or 1280 x 800 if even that query fails.
RECT work_area_for(HMONITOR mon) {
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (mon && ::GetMonitorInfoW(mon, &mi)) return mi.rcWork;
    RECT r = {0, 0, 1280, 800};
    ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &r, 0);
    return r;
}

// Window procedure. ImGui's backend sees every message first (mouse, keyboard, focus).
// Anything that must touch the swap chain or App's fonts is only recorded here: wnd_proc can
// also run while a frame is in progress, for example while a file dialog opened from
// App::frame() runs its own message loop.
LRESULT WINAPI wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return true;

    switch (msg) {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) return 0;   // keep the buffers as they are while minimized
        g_resize_w = (UINT)LOWORD(lParam);   // applied by the loop, not here
        g_resize_h = (UINT)HIWORD(lParam);
        return 0;
    case WM_GETMINMAXINFO: {
        // Minimum size: 1100 x 700 at 96 dpi, scaled to this monitor's DPI, but never larger
        // than the monitor's work area, so the window still fits on a small screen.
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        const float s = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
        const RECT wa = work_area_for(::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST));
        const LONG min_w = (LONG)(1100 * s), min_h = (LONG)(700 * s);
        mmi->ptMinTrackSize.x = min_w < wa.right - wa.left ? min_w : wa.right - wa.left;
        mmi->ptMinTrackSize.y = min_h < wa.bottom - wa.top ? min_h : wa.bottom - wa.top;
        return 0;
    }
    case WM_DPICHANGED: {
        // Moved to a monitor with another scale, or the scale setting changed. HIWORD(wParam)
        // is the new DPI (96 = 100 %). The loop below hands it to App between frames; the
        // window takes the size and position Windows suggests for the new DPI.
        g_pending_scale = HIWORD(wParam) / 96.0f;
        const RECT* r = reinterpret_cast<const RECT*>(lParam);
        ::SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;   // a lone Alt must not open the system menu
        break;
    case WM_CLOSE:
        // Close button or Alt+F4. The window is not destroyed here; the loop ends once
        // App::should_quit() is true. Without an App, DefWindowProc destroys the window.
        if (g_app) {
            g_app->request_close();   // the app decides: quit now, or confirm while a stitch runs
            return 0;
        }
        break;
    case WM_APP_WAKE:
        return 0;   // nothing to do: its arrival alone ends the idle wait in the loop
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

// ---------------------------------------------------------------- WinMain

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show_cmd) {
    // Crash handlers first, so even a failure during start-up is reported.
    ::SetUnhandledExceptionFilter(crash_filter);
    std::set_terminate(on_terminate);
    // COM for the shell calls in platform.cpp (the Open dialogs and ShellExecuteW). These are
    // the flags Microsoft documents for a thread that calls ShellExecute.
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    // app.manifest declares per-monitor-v2 awareness; this call covers a build without it.
    ImGui_ImplWin32_EnableDpiAwareness();
    // Initial window: 1400 x 880 at 96 dpi, scaled to the primary monitor's DPI and shrunk to
    // fit its work area with a small margin. Centred horizontally, a third of the way down.
    const HMONITOR primary = ::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    const float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(primary);
    const RECT wa = work_area_for(primary);
    const int wa_w = wa.right - wa.left, wa_h = wa.bottom - wa.top;
    int win_w = (int)(1400 * scale), win_h = (int)(880 * scale);
    if (win_w > wa_w - (int)(20 * scale)) win_w = wa_w - (int)(20 * scale);
    if (win_h > wa_h - (int)(20 * scale)) win_h = wa_h - (int)(20 * scale);
    const int win_x = wa.left + (wa_w - win_w) / 2;
    const int win_y = wa.top + ((wa_h - win_h) / 3 > 0 ? (wa_h - win_h) / 3 : 0);

    // Window class. Both icons come from app.rc; the small one (title bar) is loaded at the
    // system's small-icon size.
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = instance;
    wc.hIcon = ::LoadIconW(instance, MAKEINTRESOURCEW(kIconResource));
    wc.hIconSm = (HICON)::LoadImageW(instance, MAKEINTRESOURCEW(kIconResource), IMAGE_ICON,
                                     ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON), 0);
    // IDC_ARROW expands to the narrow MAKEINTRESOURCEA form because UNICODE is not defined,
    // so its value (32512) is spelled out for the wide call.
    wc.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = L"vEMstitchWorkbenchWindow";
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"vEMstitch Workbench", WS_OVERLAPPEDWINDOW, win_x, win_y,
                                  win_w, win_h, nullptr, nullptr, instance, nullptr);
    if (!hwnd) {
        fatal_box("The main window could not be created:\n\n" + plat::error_text(::GetLastError()));
        return 1;
    }

    if (!create_device(hwnd)) {
        cleanup_device();
        ::DestroyWindow(hwnd);
        ::UnregisterClassW(wc.lpszClassName, instance);
        fatal_box("Direct3D 11 could not be initialised, not even with the WARP software renderer.\n\n"
                  "Update the graphics driver, or run the program on a machine with Direct3D 11.");
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);
    ui::textures_init(g_device, g_context);

    // App's constructor loads the fonts and may throw. If it does, everything created so far
    // is torn down in reverse order and the reason is shown.
    std::unique_ptr<App> app;
    try {
        App::Host host;
        host.hwnd = hwnd;
        // PostMessageW may be called from any thread, so workers can use this to wake the loop.
        host.wake = [hwnd]() { ::PostMessageW(hwnd, WM_APP_WAKE, 0, 0); };
        app.reset(new App(host));
        app->set_dpi_scale(ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd));
        g_app = app.get();
    } catch (const std::exception& e) {
        g_app = nullptr;
        app.reset();
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        ui::textures_shutdown();
        cleanup_device();
        ::DestroyWindow(hwnd);
        ::UnregisterClassW(wc.lpszClassName, instance);
        fatal_box(std::string("The application could not start:\n\n") + e.what());
        return 1;
    }

    // A failure in startup (settings, first scan) is logged and shown inside the app, which
    // then keeps running.
    try {
        app->startup();
    } catch (const std::exception& e) {
        app->report_internal_error(std::string("startup: ") + e.what());
    }
    // Honour a "start minimized" request (e.g. from a shortcut); any other request opens the
    // window normally, at the size and position computed above.
    ::ShowWindow(hwnd, show_cmd == SW_SHOWMINIMIZED || show_cmd == SW_MINIMIZE ? show_cmd : SW_SHOWNORMAL);
    ::UpdateWindow(hwnd);

    // ------------------------------------------------------------ main loop

    float clear[4];
    App::clear_color(clear);
    int idle_frames = 0;   // frames in a row drawn without any window message
    bool done = false;
    while (!done) {
        // Sleep until input arrives when nothing animates; a run, a load or closing keeps
        // the loop spinning so progress stays live. Three frames are drawn after the last
        // message first, so what that input changed settles on screen (some ImGui layout
        // catches up a frame later). The wait ends after kIdleWaitMs at the latest.
        if (idle_frames >= 3 && !app->wants_continuous_frames())
            ::MsgWaitForMultipleObjectsEx(0, nullptr, kIdleWaitMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

        MSG msg;
        bool had_messages = false;
        while (::PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            had_messages = true;
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done || app->should_quit()) break;
        idle_frames = had_messages ? 0 : idle_frames + 1;

        // Occlusion skip: the last Present reported that nothing of the window is visible
        // (minimized or fully covered). Poll with a test present, which draws nothing, and
        // skip the frame until the window shows again. Messages are still pumped above, so
        // closing keeps working.
        if (g_occluded && g_swap->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            ::Sleep(10);
            continue;
        }
        g_occluded = false;

        // Apply a resize recorded by wnd_proc. Buffer count 0 and DXGI_FORMAT_UNKNOWN keep
        // the current count and format.
        if (g_resize_w != 0 && g_resize_h != 0) {
            cleanup_render_target();
            g_swap->ResizeBuffers(0, g_resize_w, g_resize_h, DXGI_FORMAT_UNKNOWN, 0);
            g_resize_w = g_resize_h = 0;
            create_render_target();
            idle_frames = 0;
        }
        // Apply a DPI change recorded by wnd_proc. set_dpi_scale replaces ImGui's whole style
        // and the font sizes, which must happen between frames, never inside one.
        if (g_pending_scale > 0.0f) {
            app->set_dpi_scale(g_pending_scale);
            g_pending_scale = 0.0f;
            idle_frames = 0;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        // If App::frame() throws half-way, ImGui's Begin/End and Push/Pop stacks are left
        // unbalanced. The state saved here lets ImGui unwind them, so Render() still works
        // and the app reports the error and keeps running instead of crashing.
        ImGuiErrorRecoveryState recovery;
        ImGui::ErrorRecoveryStoreState(&recovery);
        try {
            app->frame();
        } catch (const std::exception& e) {
            ImGui::ErrorRecoveryTryToRecoverState(&recovery);
            app->report_internal_error(e.what());
        } catch (...) {
            ImGui::ErrorRecoveryTryToRecoverState(&recovery);
            app->report_internal_error("unknown exception in the UI frame");
        }
        ImGui::Render();

        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        const HRESULT hr = g_swap->Present(1, 0);   // wait for vsync
        g_occluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    // ------------------------------------------------------------ shutdown

    // Normal case: everything is released in reverse order of creation.
    // Closing mid-run: App has cancelled and waited up to 5 s. Their long OpenCV calls
    // cannot be interrupted, so a worker that is still busy is left behind and the process
    // is terminated once everything of ours is saved and released.
    app->shutdown();   // saves settings, stops the image loader, releases App's textures
    const bool worker_alive = app->worker_alive();
    g_app = nullptr;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    if (worker_alive) {
        App* left_behind = app.release();   // its destructor would join a thread that may run for minutes
        (void)left_behind;
    } else {
        app.reset();
    }
    ImGui::DestroyContext();
    ui::textures_shutdown();
    cleanup_device();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, instance);
    if (SUCCEEDED(com)) ::CoUninitialize();
    if (worker_alive) ::TerminateProcess(::GetCurrentProcess(), 0);
    return 0;
}
