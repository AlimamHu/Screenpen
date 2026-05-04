@echo off
echo Building Screenpen...
windres resources.rc -o resources.o
g++ main.cpp resources.o -o Screenpen.exe -lgdi32 -luser32 -ldwmapi -lgdiplus -mwindows -municode -static
if %ERRORLEVEL% EQU 0 (
    echo Build successful!
    echo Running Screenpen...
    start Screenpen.exe
) else (
    echo Build failed.
)
