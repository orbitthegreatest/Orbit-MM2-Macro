@echo off
title OrbitMM2Macro Compiler
echo ================================
echo  OrbitMM2Macro C++ Compiler
echo ================================
echo.

if not exist OrbitMM2Macro.cpp (
    echo [ERROR] OrbitMM2Macro.cpp not found!
    pause
    exit /b 1
)

if not exist mm2_macro_logo.ico (
    echo [WARNING] mm2_macro_logo.ico not found. The .exe will use default icon.
)

where g++ >nul 2>nul
if errorlevel 1 (
    echo [ERROR] g++ not found in PATH.
    echo Install MinGW-w64 and add its bin folder to PATH.
    pause
    exit /b 1
)

echo Compiling resources...
if exist mm2_macro_logo.ico (
    windres resource.rc -O coff -o resource.o
    if errorlevel 1 (
        echo [ERROR] Resource compilation failed.
        pause
        exit /b 1
    )
)

echo Compiling OrbitMM2Macro.cpp ...
if exist resource.o (
    g++ -O2 -std=c++11 -mwindows OrbitMM2Macro.cpp resource.o -o OrbitMM2Macro.exe -lwinmm -lgdi32 -luser32 -lkernel32 -lcomctl32 -lshell32 -lole32 -luxtheme -ldwmapi -liphlpapi -static
) else (
    g++ -O2 -std=c++11 -mwindows OrbitMM2Macro.cpp -o OrbitMM2Macro.exe -lwinmm -lgdi32 -luser32 -lkernel32 -lcomctl32 -lshell32 -lole32 -luxtheme -ldwmapi -liphlpapi -static
)

if errorlevel 1 (
    echo [ERROR] Compilation failed.
    pause
    exit /b 1
)

echo.
echo ================================
echo [SUCCESS] OrbitMM2Macro.exe created!
echo ================================
echo.
echo The icon is embedded, so it will show in the folder.
pause