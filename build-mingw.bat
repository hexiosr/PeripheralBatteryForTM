@echo off
rem ============================================================
rem PeripheralBattery - MinGW-w64 build script
rem Produces out\PeripheralBattery.dll without Visual Studio.
rem
rem Key flags:
rem   -static      link libstdc++ / libgcc / winpthread statically.
rem                Without it the DLL depends on libwinpthread-1.dll,
rem                which TrafficMonitor will not find -> plugin fails to load.
rem   -s           strip the symbol table after linking (otherwise the
rem                DLL is more than twice as large).
rem   -ffunction-sections + --gc-sections  drop unreferenced functions.
rem
rem Usage: build-mingw.bat              (defaults to F:\Tools\App\mingw64)
rem        set MINGW_ROOT=D:\mingw64 && build-mingw.bat
rem ============================================================
setlocal

if not defined MINGW_ROOT set "MINGW_ROOT=F:\Tools\App\mingw64"
set "GXX=%MINGW_ROOT%\bin\g++.exe"
set "WINDRES=%MINGW_ROOT%\bin\windres.exe"

if not exist "%GXX%" (
    echo [ERROR] g++ not found: %GXX%
    echo         Set MINGW_ROOT to your MinGW-w64 directory.
    exit /b 1
)

set "ROOT=%~dp0"
set "OUT=%ROOT%out"
if not exist "%OUT%" mkdir "%OUT%"

echo.
echo == [1/2] Compile resources ==
"%WINDRES%" --codepage=65001 -I "%ROOT%src\plugin" -I "%ROOT%src\common" ^
    -i "%ROOT%src\plugin\Plugin.rc" -o "%OUT%\Plugin.res.o"
if errorlevel 1 exit /b 1

echo.
echo == [2/2] Build plugin DLL ==
"%GXX%" -std=c++17 -O2 -DNDEBUG -DUNICODE -D_UNICODE -shared -static -s ^
    -ffunction-sections -fdata-sections ^
    -I"%ROOT%src\common" -I"%ROOT%src\plugin" -I"%ROOT%src\keyboard" ^
    -I"%ROOT%src\mouse" -I"%ROOT%src\dualsense" -I"%ROOT%src\g7" ^
    -I"%ROOT%src\logitech" ^
    "%ROOT%src\keyboard\KeyboardDevice.cpp" ^
    "%ROOT%src\keyboard\KeyboardReader.cpp" ^
    "%ROOT%src\mouse\MouseDevice.cpp" ^
    "%ROOT%src\mouse\MouseReader.cpp" ^
    "%ROOT%src\dualsense\DualSenseDevice.cpp" ^
    "%ROOT%src\dualsense\DualSenseReader.cpp" ^
    "%ROOT%src\g7\G7Device.cpp" ^
    "%ROOT%src\g7\G7Reader.cpp" ^
    "%ROOT%src\logitech\LogitechDevice.cpp" ^
    "%ROOT%src\logitech\LogitechReader.cpp" ^
    "%ROOT%src\plugin\PluginConfig.cpp" ^
    "%ROOT%src\plugin\PeripheralItem.cpp" ^
    "%ROOT%src\plugin\Plugin.cpp" ^
    "%ROOT%src\plugin\dllmain.cpp" ^
    "%OUT%\Plugin.res.o" -o "%OUT%\PeripheralBattery.dll" ^
    -Wl,--gc-sections ^
    -lkernel32 -luser32 -lgdi32 -lgdiplus -lsetupapi -lhid -luuid -lcomctl32 -lcomdlg32
if errorlevel 1 exit /b 1

echo.
echo [OK] %OUT%\PeripheralBattery.dll
endlocal
