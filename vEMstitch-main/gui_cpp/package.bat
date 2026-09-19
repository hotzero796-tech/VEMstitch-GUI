@echo off
rem Builds a ready-to-send copy of the app:
rem   share\vEMstitchWorkbench\      the folder
rem   share\vEMstitchWorkbench.zip   the same folder, zipped
rem
rem Run build.bat first. The package runs on any 64-bit Windows 10/11 PC, with no install:
rem it carries the Visual C++ and OpenMP runtime DLLs next to the exe (Microsoft permits
rem this "app-local" copy), so the other PC does not need Visual Studio or the redistributable.

setlocal
set HERE=%~dp0
set ROOT=%HERE%..
set DIST=%HERE%dist
set OUT=%HERE%share\vEMstitchWorkbench
set ZIP=%HERE%share\vEMstitchWorkbench.zip
set OCV=C:\Users\hotze\tools\opencv-4.14.0\opencv\build
set REDIST=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Redist\MSVC\14.44.35112\x64

if not exist "%DIST%\vEMstitchWorkbench.exe" (echo Run build.bat first. & exit /b 1)
if not exist "%REDIST%\Microsoft.VC143.CRT\vcruntime140.dll" (echo Visual C++ redistributable files not found at %REDIST% & exit /b 1)

if exist "%OUT%" rmdir /s /q "%OUT%"
if exist "%ZIP%" del /q "%ZIP%"
mkdir "%OUT%\licenses"

rem the app and OpenCV
copy /y "%DIST%\vEMstitchWorkbench.exe" "%OUT%\" >nul
copy /y "%DIST%\opencv_world4140.dll"   "%OUT%\" >nul

rem the runtime DLLs the exe and OpenCV import (checked with dumpbin /dependents)
for %%F in (msvcp140.dll vcruntime140.dll vcruntime140_1.dll concrt140.dll) do copy /y "%REDIST%\Microsoft.VC143.CRT\%%F" "%OUT%\" >nul
copy /y "%REDIST%\Microsoft.VC143.OpenMP\vcomp140.dll" "%OUT%\" >nul

rem demo data, so the app opens with section 165 loaded
xcopy /e /i /q /y "%ROOT%\test" "%OUT%\test" >nul

rem licences: vEMstitch (GPL-3.0), OpenCV (Apache-2.0), Dear ImGui (MIT)
copy /y "%ROOT%\LICENSE"                            "%OUT%\licenses\vEMstitch-LICENSE.txt" >nul
copy /y "%OCV%\LICENSE"                             "%OUT%\licenses\OpenCV-LICENSE.txt" >nul
copy /y "%HERE%third_party\imgui\LICENSE.txt"       "%OUT%\licenses\DearImGui-LICENSE.txt" >nul

(
echo vEMstitch Workbench
echo ===================
echo.
echo Double-click vEMstitchWorkbench.exe. Nothing to install.
echo It opens with a 3x3 test section already loaded: press F9 or "Run stitch".
echo A run takes about 2 minutes; the result is written to the "output" folder here.
echo.
echo Your own data: File ^> Open dataset, and pick a folder of tiles named
echo   {section}_{row}_{col}.bmp   ^(also .png .tif .tiff .jpg^), e.g. 165_1_1.bmp
echo.
echo Needs 64-bit Windows 10 or 11. Keep all the files in this folder together.
echo If Windows says "Windows protected your PC", click "More info" then "Run anyway":
echo the program is not code-signed, which is normal for research tools.
echo.
echo The stitching algorithm is vEMstitch by its original authors, released under GPL-3.0
echo ^(see licenses\^). This program uses their C++ code unmodified.
) > "%OUT%\README.txt"

rem zip it with the tar.exe built into Windows 10/11
pushd "%HERE%share"
tar -a -c -f "vEMstitchWorkbench.zip" "vEMstitchWorkbench"
set TARERR=%ERRORLEVEL%
popd
if not "%TARERR%"=="0" (echo zip failed & exit /b 1)

echo.
echo PACKAGE OK
echo   folder: %OUT%
for %%Z in ("%ZIP%") do echo   zip:    %%~fZ  ^(%%~zZ bytes^)
exit /b 0
