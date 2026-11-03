#!/bin/sh
# Builds the desktop configuration harness used by test_desktop_compat.py.
# usage: build.sh <output-dir>
set -e
here=$(cd "$(dirname "$0")" && pwd)
gui=$(cd "$here/../../../gui/src" && pwd)
out=${1:-$here/build}
mkdir -p "$out/src"
for f in AppConfig ServerConfig Screen Hotkey Action KeySequence BaseConfig; do
    cp "$gui/$f.cpp" "$out/src/"
done
cp "$here/MainWindow.h" "$here/AddClientDialog.h" "$here/harness.cpp" "$out/src/"
cd "$out/src"
moc_flags=$(pkg-config --cflags Qt5Widgets Qt5Network)
moc -o moc_AppConfig.cpp $moc_flags -I"$gui" "$gui/AppConfig.h"
c++ -std=c++14 -fPIC -I. -I"$gui" $moc_flags \
    harness.cpp AppConfig.cpp ServerConfig.cpp Screen.cpp Hotkey.cpp Action.cpp KeySequence.cpp \
    BaseConfig.cpp moc_AppConfig.cpp -o "$out/desktop_config" $(pkg-config --libs Qt5Widgets Qt5Network)
echo "$out/desktop_config"
