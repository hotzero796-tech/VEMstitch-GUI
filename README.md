# vEMstitch Workbench

A Windows desktop interface for **vEMstitch**, the automatic tile stitcher for volume electron
microscopy.

An electron microscope photographs a section in overlapping tiles. vEMstitch finds where those
tiles overlap and joins them into one seamless image, correcting the rotation and the local
stretching that sectioning introduces. It is normally driven from the command line. This
repository adds an application that does it through a window: open a folder of tiles, press one
key, and watch the stitch as it runs, step by step, with the result on screen when it finishes.

The authors' own code is included unchanged, in `vEMstitch-main\source` (Python) and
`vEMstitch-main\vEMstitch_c++` (C++). The interface, written in C++, is in
`vEMstitch-main\gui_cpp` and runs their C++ implementation directly.

## Using it

Download the repository (green **Code** button, **Download ZIP**), unzip it, and run:

```
vEMstitch-main\gui_cpp\share\vEMstitchWorkbench\vEMstitchWorkbench.exe
```

Nothing to install. That folder carries everything the application needs, so it runs on any 64-bit
Windows 10 or 11 PC. If Windows says "Windows protected your PC", click **More info**, then
**Run anyway**: the program is not code-signed.

Then:

1. **File → Open dataset** (`Ctrl+O`) and pick any tile of your set. Its folder becomes the dataset.
2. Choose the grid size, **2 x 2** or **3 x 3**, and where the result should go.
3. Press **Run stitch** (`F9`) for the section you picked, or **Run all** (`Shift+F9`) for every
   section in the folder.

Tiles must be named `{section}_{row}_{col}`, numbered from 1, for example `165_1_1.bmp` and
`165_1_2.bmp`. `.bmp`, `.png`, `.tif`, `.tiff`, `.jpg` and `.jpeg` are all accepted. The mosaic is
saved as `{section}-res.bmp` in the output folder.

While it runs, the **Steps** table lists each step with its time, and the **Log** shows both the
application's messages and the messages vEMstitch itself prints. **Cancel** (`Esc`) takes effect
between steps; a step already under way cannot be interrupted. A 3 x 3 section of 2048 x 1768 tiles
takes about 80 seconds.

To build the application from source, see `vEMstitch-main\gui_cpp\README.md`.

## Refine

**Refine** is the authors' optional second pass. It re-examines the joins between rows and is meant
for sections where a seam is still visible after a normal run. Their advice, which the application
repeats, is to stitch without it first and re-run only the sections that need it.

It is slow. On the sample section it took **78 minutes**, against about **80 seconds** without it,
and because the pass cannot be interrupted, Cancel will not stop it until the step ends.

## Sample data in this repo

| Folder | What it is |
|---|---|
| `vEMstitch-main\test` | One 3 x 3 section of real mussel tissue, 2048 x 1768 per tile. The quickest thing to try. |
| `vEMstitch-main\related_data\real_data\sample_1` … `sample_3` | Three more real 3 x 3 sections. Each folder also holds the results from Fiji, MIST and TrakEM2 for comparison. |
| `vEMstitch-main\related_data\simulated_data` | 29 simulated 2 x 2 sets built from CREMI images: shifted, rotated, warped and noisy versions. `raw_data` holds the uncut originals, so a stitch can be checked against the true image. |

Do not set the output folder to one of these dataset folders. The application writes
`{section}-res.bmp`, which is also the name of the authors' published results stored there.

## Licences and credits

**This repository is licensed under GPL-3.0**, the full text being in `LICENSE`. It has to be:
vEMstitch is GPL-3.0 and the interface compiles that code into the same program, so the combined
work carries the same licence.

vEMstitch is the work of its original authors and is included here unchanged:
<https://github.com/HeracleBT/vEMstitch>. Their code is **GPL-3.0** and their data **CC0**.

The interface also uses **Dear ImGui** (MIT) and **OpenCV** (Apache-2.0), neither of which
conflicts with GPL-3.0. Their licence texts are in
`vEMstitch-main\gui_cpp\share\vEMstitchWorkbench\licenses`.

Interface written by **Abdulrahman Ali** for the volume electron microscopy pipeline at
[CIMA Lab](https://cima-lab.github.io/research/index.html), supervised by Dr. Haythem
El-Messiry. The copyright notice is in the header of every interface source file, under
`vEMstitch-main\gui_cpp\src`.
