vEMstitch Workbench
===================

Double-click vEMstitchWorkbench.exe. Nothing to install.
It opens with a 3x3 test section already loaded: press F9 or "Run stitch".
A run takes about 2 minutes; the result is written to the "output" folder here.

Your own data: File > Open dataset, and pick a folder of tiles named
  {section}_{row}_{col}.bmp   (also .png .tif .tiff .jpg), e.g. 165_1_1.bmp

Needs 64-bit Windows 10 or 11. Keep all the files in this folder together.
If Windows says "Windows protected your PC", click "More info" then "Run anyway":
the program is not code-signed, which is normal for research tools.

The stitching algorithm is vEMstitch by its original authors, released under GPL-3.0
(see licenses\). This program uses their C++ code unmodified.
