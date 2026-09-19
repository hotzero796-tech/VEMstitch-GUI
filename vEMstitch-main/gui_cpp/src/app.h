#pragma once
// --------------------------------------------------------------------------------------
// app.h - the public face of the workbench user interface.
//
// Where it sits: main.cpp is the entry point. It owns the Win32 window, the Direct3D 11
// device and the Dear ImGui context, runs the message loop and calls App once per frame.
// App owns everything drawn inside the window, plus the stitch runner (engine.h) and the
// background image loader. The code is in app.cpp.
//
// Things to know:
//  - Every method here runs on the UI thread (the thread that runs WinMain).
//  - App is a thin "pimpl" wrapper: all state and UI code live in App::Impl in app.cpp,
//    so this header needs no ImGui, OpenCV or engine types.
//  - main.cpp calls, in order: the constructor, set_dpi_scale, startup, then frame() once
//    per frame, and at the end shutdown. If a stitch is still inside their code by then,
//    main.cpp leaks the App and terminates the process instead of destroying it.
// --------------------------------------------------------------------------------------

#include <functional>
#include <memory>
#include <string>

class App {
public:
    // What App needs from its host (main.cpp).
    struct Host {
        void* hwnd = nullptr;            // HWND of the main window (owner of the shell dialogs)
        // Wakes the message loop when it is asleep waiting for input; callable from any
        // thread. The image loader calls it after each decoded image. The engine does not
        // need it: during a run wants_continuous_frames() keeps the loop drawing.
        std::function<void()> wake;
    };

    explicit App(const Host& host);      // needs a current ImGui context; loads the fonts, starts the loader
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // Rebuilds the style and font sizes for a DPI scale (1.0 = 96 dpi, 1.25 at 125 %).
    // main.cpp calls it at start and after WM_DPICHANGED, always between frames.
    void set_dpi_scale(float scale);
    // Writes the banner to the Log, loads the settings and sets the output folder. Nothing is
    // scanned: the app starts empty until a dataset is opened. Call once, after set_dpi_scale.
    void startup();
    // One UI frame; call between ImGui::NewFrame() and ImGui::Render().
    void frame();
    // Something threw (a frame, startup or a queued action): log it, and show a dialog once
    // per distinct message (at most three dialogs per session).
    void report_internal_error(const std::string& what);
    // True while something changes without user input (a run, image loads, closing).
    // main.cpp then keeps drawing instead of sleeping until the next input.
    bool wants_continuous_frames() const;

    // Close button, Alt+F4, File > Exit. Quits at once when idle; during a run it asks
    // first, then cancels and waits a few seconds for the worker.
    void request_close();
    bool should_quit() const;            // main.cpp leaves its loop once this is true
    // Saves settings, stops the loader and releases every GPU texture. Call before the
    // D3D device and the ImGui context go away.
    void shutdown();
    // True while the stitch worker thread has not finished (it may be inside one of their
    // calls, which cannot be interrupted). main.cpp checks it after shutdown().
    bool worker_alive() const;

    static const char* name();           // "vEMstitch Workbench"
    static const char* version();        // kAppVersion in app.cpp
    // The window background colour; main.cpp clears the back buffer with it.
    static void clear_color(float out_rgba[4]);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
