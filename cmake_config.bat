@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=amd64
cd /d C:\projects.local\etherwaver
if exist build rmdir /s /q build
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022" -A x64 ^
  -DBARRIER_BUILD_GUI=ON ^
  -DBARRIER_BUILD_TESTS=OFF ^
  -DQt5_DIR="C:/Qt/5.15.2/lib/cmake/Qt5" ^
  -DQt5Svg_DIR="C:/Qt/5.15.2-src/qtsvg/lib/cmake/Qt5Svg" ^
  -DCMAKE_PREFIX_PATH="C:/Qt/5.15.2;C:/Qt/5.15.2-src/qtsvg"
