@echo off
rem ============================================================
rem  BinKey one-click build -> single-file installer BinKeySetup.exe
rem  Requires MinGW-w64 (g++ + windres). If not on PATH, edit CXX/WINDRES.
rem  Icons: icons\*.ico are the real .ico files you provide (1.ico / 2.ico / 3.ico),
rem         copied verbatim into icons\  -- no conversion needed.
rem         (only if you have .jpg: python src\mkico.py 1.jpg icons\BinKey.ico 16,32,48,64,128,256)
rem ============================================================
cd /d %~dp0

set CXX=g++
where g++ >nul 2>&1
if errorlevel 1 (
    if exist D:\msys2\mingw64\bin\g++.exe set CXX=D:\msys2\mingw64\bin\g++.exe
)
%CXX% --version >nul 2>&1
if errorlevel 1 (
    echo [error] g++ not found. Install MinGW-w64, or set CXX to your compiler path.
    echo   example:  set CXX=D:\msys2\mingw64\bin\g++.exe
    exit /b 1
)

set WINDRES=windres
where windres >nul 2>&1
if errorlevel 1 (
    if exist D:\msys2\mingw64\bin\windres.exe set WINDRES=D:\msys2\mingw64\bin\windres.exe
)

if not exist build mkdir build

rem ---- embedded icon resources ----
set RES_APP=
set RES_SET=
if exist icons\BinKey.ico (
    %WINDRES% -i src\resources.rc -O coff -o build\binkey_res.o
    %WINDRES% -i src\icon.rc     -O coff -o build\icon_res.o
    set RES_APP=build\binkey_res.o
    set RES_SET=build\icon_res.o
) else (
    echo [warn] icons\*.ico missing - the program falls back to its built-in drawn icons
)

echo [1/4] building BinKey.exe ...
%CXX% -O2 -std=c++17 -Wall -Wextra -static -mwindows src\BinKey.cpp %RES_APP% -o build\BinKey.exe -luser32 -ladvapi32 -lshell32 -lwinmm -lgdi32
if errorlevel 1 (echo [error] BinKey build failed & exit /b 1)

echo [2/4] building helper tools (not packed into the installer) ...
%CXX% -O2 -std=c++17 -static src\simkey.cpp -o build\simkey.exe -luser32
%CXX% -O2 -std=c++17 -static src\keyprobe.cpp -o build\keyprobe.exe -luser32

echo [3/4] building the setup wizard ...
%CXX% -O2 -std=c++17 -Wall -Wextra -static -mwindows src\BinKeySetup.cpp %RES_SET% -o build\BinKeySetup_stub.exe -luser32 -ladvapi32 -lshell32 -lole32
if errorlevel 1 (echo [error] setup build failed & exit /b 1)
%CXX% -O2 -std=c++17 -static src\packer.cpp -o build\packer.exe

echo [4/4] packing the single-file installer ...
build\packer.exe build\BinKeySetup_stub.exe BinKeySetup.exe ^
  "BinKey.exe=build\BinKey.exe" ^
  "config.cfg=config.cfg" ^
  "README.md=README.md" ^
  "CHANGELOG.md=CHANGELOG.md" ^
  "fix-icon-cache.bat=fix-icon-cache.bat" ^
  "profiles/snap-tap.cfg=profiles\snap-tap.cfg" ^
  "profiles/valorant-stop.cfg=profiles\valorant-stop.cfg" ^
  "profiles/dkssocd-mix.cfg=profiles\dkssocd-mix.cfg" ^
  "profiles/typing.cfg=profiles\typing.cfg"

copy /Y config.cfg build\config.cfg >nul

echo.
echo Done: BinKeySetup.exe   (double-click it for the wizard)
echo Silent install:  BinKeySetup.exe --silent --dir D:\BinKey
