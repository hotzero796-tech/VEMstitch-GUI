@echo off
rem vEMstitch Workbench (C++ / Dear ImGui) - full build.
rem
rem   build.bat          build everything (their algorithm, ImGui, the app) into dist\
rem   build.bat app      rebuild only the app sources, reuse the rest
rem
rem Needs only Visual Studio 2022 (MSVC) and the OpenCV 4.14.0 prebuilt below.
rem No CMake, no Python. Works from any current directory.
rem
rem Four steps: 1 compiles the authors' algorithm, 2 Dear ImGui, 3 the app sources and the
rem resources in app.rc, 4 links everything into one exe. Object files go to the build
rem folder; the exe goes to the dist folder together with the OpenCV DLL it needs.

rem setlocal: the variables set below disappear again when the script ends.
setlocal
rem vcvars64 puts the 64-bit compiler, linker and resource compiler on the PATH.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (echo vcvars64.bat not found & goto fail)

rem HERE is this script's own folder, so every path below works from any current directory.
rem ALGO is the authors' code next to gui_cpp. OCV is the OpenCV prebuilt: change it if
rem OpenCV is installed somewhere else.
set HERE=%~dp0
set ROOT=%HERE%..
set ALGO=%ROOT%\vEMstitch_c++
set SRC=%HERE%src
set IMGUI=%HERE%third_party\imgui
set OCV=C:\Users\hotze\tools\opencv-4.14.0\opencv\build
set OBJ=%HERE%build
set DIST=%HERE%dist

if not exist "%OCV%\include\opencv2\opencv.hpp" (echo OpenCV not found at %OCV% & goto fail)

rem /DNDEBUG is required, not cosmetic: without it OpenCV's CV_DbgAssert trips on an
rem at^<int^>() read of a CV_8U mask inside their rigid_transform.cpp and aborts the run.
rem /openmp: their stitching.h includes omp.h.  /D_USE_MATH_DEFINES: they use M_PI.
rem /DNOMINMAX: stops windows.h from defining min and max macros, which break std::min.
rem /std:c++17: the app uses std::filesystem.  /utf-8: sources and string literals are UTF-8.
rem /MD: the C runtime as a DLL, the same as the prebuilt OpenCV.  /MP: compile in parallel.
set FLAGS=/nologo /std:c++17 /EHsc /O2 /MD /openmp /DNDEBUG /D_USE_MATH_DEFINES /DNOMINMAX /utf-8 /MP

rem Object folders build\algo, build\imgui and build\ours, and the output folder dist.
for %%D in (algo imgui ours) do if not exist "%OBJ%\%%D" mkdir "%OBJ%\%%D"
if not exist "%DIST%" mkdir "%DIST%"
if not exist "%HERE%assets\icon.ico" (echo assets\icon.ico is missing & goto fail)

rem build.bat app: skip steps 1 and 2 and reuse the objects already in the build folder.
if /i "%1"=="app" goto ours

rem Step 1. Their five algorithm files, compiled as they are. Their main.cpp is left out: it
rem is their command-line program and needs the Linux-only dirent.h; the app calls their
rem functions itself instead. /W1: only serious warnings, since we do not change this code.
rem The doubled backslash at the end of each /Fo folder keeps the closing quote from being
rem read as an escaped quote.
echo === 1/4  their algorithm (vEMstitch_c++, compiled unmodified, main.cpp excluded)
cl %FLAGS% /W1 /c /I"%ALGO%\include" /I"%OCV%\include" /Fo"%OBJ%\algo\\" ^
   "%ALGO%\src\Utils.cpp" "%ALGO%\src\elastic_transform.cpp" "%ALGO%\src\refinement.cpp" ^
   "%ALGO%\src\rigid_transform.cpp" "%ALGO%\src\stitching.cpp"
if errorlevel 1 goto fail

rem Step 2. The Dear ImGui core and its Win32 and Direct3D 11 backends, from third_party.
echo === 2/4  Dear ImGui
cl %FLAGS% /W3 /c /I"%IMGUI%" /I"%IMGUI%\backends" /Fo"%OBJ%\imgui\\" ^
   "%IMGUI%\imgui.cpp" "%IMGUI%\imgui_draw.cpp" "%IMGUI%\imgui_tables.cpp" "%IMGUI%\imgui_widgets.cpp" ^
   "%IMGUI%\backends\imgui_impl_win32.cpp" "%IMGUI%\backends\imgui_impl_dx11.cpp"
if errorlevel 1 goto fail

rem Step 3. The app sources. engine.cpp includes their headers, hence the ALGO include path.
:ours
echo === 3/4  the app
cl %FLAGS% /W3 /c /I"%SRC%" /I"%ALGO%\include" /I"%OCV%\include" /I"%IMGUI%" /I"%IMGUI%\backends" ^
   /Fo"%OBJ%\ours\\" "%SRC%\engine.cpp" "%SRC%\app.cpp" "%SRC%\main.cpp" "%SRC%\platform.cpp" "%SRC%\texture.cpp"
if errorlevel 1 goto fail
rem Resources: app.rc names assets\icon.ico and app.manifest relative to this folder, so rc
rem runs from here. Its exit code is saved before popd, so the check below tests rc.
pushd "%HERE%"
rc /nologo /fo "%OBJ%\app.res" app.rc
set RCERR=%ERRORLEVEL%
popd
if not "%RCERR%"=="0" goto fail

rem Step 4. Link one exe. /SUBSYSTEM:WINDOWS makes a windowed program with no console, which
rem is why the engine captures what their code prints and shows it in the Log.
rem /MANIFEST:NO: app.rc already embeds app.manifest, so the linker must not add another.
rem Besides OpenCV: Direct3D 11, DXGI and the shader compiler for the ImGui renderer, and
rem the usual Windows libraries for windows, the shell and COM.
echo === 4/4  link
link /nologo /SUBSYSTEM:WINDOWS /MANIFEST:NO /OUT:"%DIST%\vEMstitchWorkbench.exe" ^
   "%OBJ%\algo\*.obj" "%OBJ%\imgui\*.obj" "%OBJ%\ours\*.obj" "%OBJ%\app.res" ^
   /LIBPATH:"%OCV%\x64\vc16\lib" opencv_world4140.lib ^
   d3d11.lib dxgi.lib d3dcompiler.lib user32.lib gdi32.lib shell32.lib ole32.lib uuid.lib ^
   comdlg32.lib dwmapi.lib advapi32.lib
if errorlevel 1 goto fail

rem The exe loads the OpenCV DLL from its own folder. The repository's sample tiles, its
rem test folder, are copied once, so there is something to open: until another folder has
rem been used, the Open dialogs start in the exe's folder.
copy /y "%OCV%\x64\vc16\bin\opencv_world4140.dll" "%DIST%\" >nul
if not exist "%DIST%\test" if exist "%ROOT%\test" xcopy /e /i /q /y "%ROOT%\test" "%DIST%\test" >nul

echo.
echo BUILD OK: %DIST%\vEMstitchWorkbench.exe
rem Report the total size of the dist folder.
for /f "tokens=3" %%S in ('dir /s /-c "%DIST%" ^| findstr /c:"File(s)"') do set SIZE=%%S
echo dist folder: %SIZE% bytes
exit /b 0

:fail
echo.
echo BUILD FAILED
exit /b 1
