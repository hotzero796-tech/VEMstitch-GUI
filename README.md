# vEMstitch Workbench

A Windows desktop interface for **vEMstitch**, the automatic tile stitcher for volume electron
microscopy.

vEMstitch joins the overlapping tiles of one section into a single mosaic, and is normally run from
the command line. This repository adds an application that does it through a window instead: open a
folder of tiles, press one key, and watch the stitch as it runs.

The authors' own code is included unchanged, in `vEMstitch-main\source` (Python) and
`vEMstitch-main\vEMstitch_c++` (C++). The interface is in `vEMstitch-main\gui_cpp`.

## Run it

Download the repository (green **Code** button, **Download ZIP**), unzip it, and run:

```
vEMstitch-main\gui_cpp\share\vEMstitchWorkbench\vEMstitchWorkbench.exe
```

Nothing to install. That folder carries everything the app needs, so it works on any 64-bit
Windows 10 or 11 PC. If Windows says "Windows protected your PC", click **More info**, then
**Run anyway**: the program is not code-signed.

## Use it

1. **File → Open dataset** (`Ctrl+O`) and pick any tile of your set.
2. Choose the grid size, **2 x 2** or **3 x 3**, and the output folder.
3. Press **Run stitch** (`F9`), or **Run all** (`Shift+F9`) for every section in the folder.

Tiles are named `{section}_{row}_{col}`, numbered from 1, for example `165_1_1.bmp`, `165_1_2.bmp`.
`.bmp`, `.png`, `.tif`, `.tiff`, `.jpg` and `.jpeg` all work. The mosaic is saved as
`{section}-res.bmp` in the output folder.

A 3 x 3 section of 2048 x 1768 tiles takes about 80 seconds. Sample tiles come with the app, in its
`test` folder, and more are in `vEMstitch-main\related_data`.

**Refine** is the authors' optional second pass for sections where a seam is still visible. It is
very slow: 78 minutes on the sample section, against 80 seconds without it.

## Build it

Needs Visual Studio 2022 with the C++ workload, OpenCV 4.14.0 and Dear ImGui. No CMake, no Python.

```
git clone --depth 1 --branch v1.92.9b https://github.com/ocornut/imgui.git vEMstitch-main/gui_cpp/third_party/imgui
```

Point `set OCV=` in `vEMstitch-main\gui_cpp\build.bat` at your OpenCV folder, then run that script.
The result appears in `gui_cpp\dist`. Details are in `vEMstitch-main\gui_cpp\README.md`.

## Credit

vEMstitch is by its original authors: <https://github.com/HeracleBT/vEMstitch>, GPL-3.0 code and
CC0 data. This interface links their code, so it is GPL-3.0 too. Dear ImGui is MIT, OpenCV is
Apache-2.0.

Interface by **Abdulrahman Ali**, for the volume electron microscopy pipeline at
[CIMA Lab](https://cima-lab.github.io/research/index.html).
