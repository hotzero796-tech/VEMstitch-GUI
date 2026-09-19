#pragma once
// engine.h - public interface of the stitching engine (engine layer; engine.cpp has no UI code).
//
// vEMstitch Workbench is a native Windows GUI for vEMstitch, which stitches the overlapping
// tiles of one volume-EM section into a single mosaic. This header is all the UI (app.cpp)
// needs from the engine:
//   * scan_dataset() finds the tiles in a folder and groups them into sections;
//   * Runner stitches sections on a background thread and reports progress as Events.
// The UI never calls vEMstitch's code itself. The two sides meet only in Runner's event queue:
// the worker pushes Events, the UI thread pops them with poll() once per frame.
//
// Paths are UTF-8 std::string everywhere, so folder names in any language survive; the engine
// turns them into wide-char Windows paths when it opens files.

#include <string>
#include <vector>
#include <memory>
#include <cstdint>

namespace vem {

// One image file of the grid. row/col are 1-based, taken from the file name. file is the bare
// file name, path the full UTF-8 path. w/h are the pixel size (0 when it could not be read) and
// bytes the file size; the tile map shows them in a tile's tooltip.
struct Tile { int row = 0, col = 0; std::string file, path; int w = 0, h = 0; std::uint64_t bytes = 0; };

// All tiles of one section (one slice of the volume). rows/cols are the largest row and column
// index seen, so a hole anywhere inside that rectangle is listed in missing and makes complete
// false.
struct Section {
    std::string id; int rows = 0, cols = 0; bool complete = false;
    std::vector<Tile> tiles;                       // sorted by (row, col)
    std::vector<std::pair<int,int>> missing;       // (row, col), 1-indexed
    const Tile* find(int row, int col) const;      // nullptr when missing
};

// What scan_dataset() found. path is the scanned folder, made absolute when possible. ok means
// at least one section was found. error explains a failure, but can also be set while ok is
// true (the folder listing stopped early). pattern_guess is the grid size (2 or 3) the UI
// preselects: the largest row/col index seen, clamped to the grids vEMstitch supports.
struct ScanResult { std::string path; bool ok = false; std::string error; int pattern_guess = 3;
                    std::vector<Section> sections; };

// Lists the tiles in one folder (not its subfolders). Names are "{section}_{row}_{col}.{ext}":
// row and col are the last two '_'-separated numbers and the section is everything before
// them, so a section name may itself contain '_'. Extensions .bmp .png .tif .tiff .jpg .jpeg,
// case-insensitive. Image sizes are read from the file header (BMP, PNG, TIFF/BigTIFF, JPEG) so
// a big folder scans quickly; only when that fails is the whole image decoded. Section ids are
// sorted in natural order ("sec2" before "sec10"). Never throws: problems go in error.
ScanResult scan_dataset(const std::string& folder);

// Kinds of message the worker sends to the UI. For each section a run sends SectionStart, a
// StepStart/StepEnd pair per step, then SectionEnd; the run ends with exactly one RunEnd.
// Steps of different rows interleave because rows run at the same time. Log events are plain
// lines for the Log panel and can arrive at any point.
enum class EventType { Log, StepStart, StepEnd, SectionStart, SectionEnd, RunEnd };

// Severity of a message; the Log colours its lines by it. Ok marks a success.
enum class Level { Info, Ok, Warn, Error };

// Measurements of one finished step, shown in the Steps table and in the Log line.
struct StepMetrics {
    double elapsed = 0; int out_w = 0, out_h = 0;   // seconds; output size (0 x 0 if the step failed)
    // How the step produced its image:
    //   "stitch"              their stitching_pair / stitching_rows ran;
    //   "preprocess-shortcut" their preprocess() found a nearly blank overlap and placed the
    //                         two images itself, so stitching_pair was skipped (as in their code);
    //   "passthrough"         a tile was missing, so the step passed on the image it had.
    std::string path;          // "stitch" | "preprocess-shortcut" | "passthrough"
};

// One message from the worker to the UI. It is one flat struct for every kind so that a single
// queue carries them all in order; each EventType fills only the fields it needs.
struct Event {
    EventType type = EventType::Log;
    // Seconds since the run started, stamped when the event is queued. The UI adds it to the
    // wall-clock start of the run, so a Log line shows when it happened, not when it was drawn
    // (frames are skipped while the window is covered or minimized).
    double t = 0;                       // seconds since run start
    Level level = Level::Info; std::string msg;   // all kinds; msg is the text for the Log
    std::string section;                // section id; empty for RunEnd and for run-wide Log lines
    // StepStart/StepEnd: step_id is the step's key within the section (e.g. "r2c1+2", "merge1"),
    // label its readable name, index its 0-based place in the section's step list. total is the
    // number of steps in the section (also sent with SectionStart/SectionEnd).
    std::string step_id, label; int index = 0, total = 0;   // index = step order within section
    // StepEnd: "ok" | "shortcut" | "failed" (a cancelled step is never started, so it sends no
    // StepEnd). SectionEnd and RunEnd: "ok" | "failed" | "cancelled".
    std::string status;
    StepMetrics metrics;                // StepEnd
    std::string error;                  // StepEnd / SectionEnd when failed
    // output_path/width/height (SectionEnd): the mosaic file written, empty/0 unless status "ok".
    // elapsed: seconds taken by the step (StepEnd), section (SectionEnd) or run (RunEnd).
    std::string output_path; int width = 0, height = 0; double elapsed = 0;
    // SectionStart: sections finished before this one. SectionEnd: this section's 1-based
    // number. RunEnd: sections that were started and ended, including a cancelled one.
    int sections_done = 0, sections_total = 0;                               // Section*/RunEnd
};

// What to run. input/output are UTF-8 folder paths; each section is written to the output
// folder as {section}-res.bmp, the same name their own tool uses.
struct RunConfig {
    std::string input, output;
    std::vector<std::string> sections;  // one id for "Run stitch", every id for "Run all"
    int pattern = 3;                    // 2 or 3: the grids vEMstitch supports
    bool refine = false;                // passed to their stitching_rows (row merges); very slow
};

// Runs one stitch job (its sections one after another) on a background worker thread. The UI
// thread owns it and calls its members; the worker and its row threads only push Events into
// a mutex-guarded queue. One run at a time: start() refuses while a run is in progress.
class Runner {
public:
    // The destructor cancels and joins, which can block until their current step ends (a
    // Refine merge can take over an hour), so main.cpp never destroys it while a run is still
    // going: it terminates the process instead.
    Runner(); ~Runner();                // destructor cancels and joins
    // Checks the config and starts the worker; false, with the reason in *why_not, if it
    // cannot. When scan is a scan of cfg.input, every requested section must be in it and it
    // supplies the tile paths; otherwise the tiles are looked up by name in the input folder.
    bool start(const RunConfig& cfg, const ScanResult& scan, std::string* why_not = nullptr);
    // Asks the run to stop. The flag is checked before each step and each section: a step
    // already inside their code runs to its end, because their calls cannot be interrupted.
    void cancel();                      // checked between steps; returns immediately
    bool running() const;               // true from start() until the worker is done
    bool poll(Event& out);              // non-blocking pop from a mutex-guarded deque
    bool join_for(int milliseconds);    // true if the worker finished within the time
private:
    // pimpl: keeps <thread>, <mutex> and the engine internals out of this header.
    struct Impl; std::unique_ptr<Impl> d;
};

std::string opencv_version();           // OpenCV version at run time, e.g. "4.14.0"
int hardware_threads();                 // logical CPU count, at least 1 (Log, About box)

} // namespace vem
