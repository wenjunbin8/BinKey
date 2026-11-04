@echo off
rem ============================================================
rem  Quick build of BinKey.exe + test tools (no installer packing).
rem  Use build-installer.bat to produce the single-file BinKeySetup.exe.
rem  Icons: icons\*.ico are generated from 1.jpg / 2.jpg / 3.jpg (see README "Icons").
rem ============================================================
cd /d %~dp0

set CXX=g++
where g++ >nul 2>&1
if errorlevel 1 (
    if exist D:\msys2\mingw64\bin\g++.exe set CXX=D:\msys2\mingw64\bin\g++.exe
)

set WINDRES=windres
where windres >nul 2>&1
if errorlevel 1 (
    if exist D:\msys2\mingw64\bin\windres.exe set WINDRES=D:\msys2\mingw64\bin\windres.exe
)

if not exist build mkdir build

rem 图标资源（icons\*.ico 由 1.jpg / 2.jpg / 3.jpg 生成，见 README 的"图标"一节）
set RES=
if exist icons\BinKey.ico (
    %WINDRES% -i src\resources.rc -O coff -o build\binkey_res.o
    set RES=build\binkey_res.o
) else (
    echo [warn] icons\*.ico missing - falls back to the built-in drawn icons
)

%CXX% -O2 -std=c++17 -Wall -Wextra -static -mwindows src\BinKey.cpp %RES% -o build\BinKey.exe -lwinmm -lgdi32 -luser32 -ladvapi32 -lshell32
if errorlevel 1 (echo Build failed & exit /b 1)

%CXX% -O2 -std=c++17 -static src\simkey.cpp -o build\simkey.exe -luser32
%CXX% -O2 -std=c++17 -static src\keyprobe.cpp -o build\keyprobe.exe -luser32

if exist config.cfg copy /Y config.cfg build\config.cfg >nul
echo Built build\BinKey.exe (plus simkey.exe, keyprobe.exe)
