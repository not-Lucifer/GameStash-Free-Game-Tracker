@echo off
set BUILD_LOG=build_debug.log
echo Starting Build > %BUILD_LOG%
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
echo CMake Configuration >> %BUILD_LOG%
"vcpkg\downloads\tools\cmake-3.31.10-windows\cmake-3.31.10-windows-x86_64\bin\cmake.exe" -B build -S . -G Ninja -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake >> %BUILD_LOG% 2>&1
echo CMake Build >> %BUILD_LOG%
"vcpkg\downloads\tools\cmake-3.31.10-windows\cmake-3.31.10-windows-x86_64\bin\cmake.exe" --build build --config Debug >> %BUILD_LOG% 2>&1
echo Done >> %BUILD_LOG%
