#!/bin/sh

# builds a preset
build_preset() {
    echo Configuring $1 ...
    cmake --preset $1
    echo Building $1 ...
    cmake --build --preset $1
}

build_preset MinSizeRel

rm -rf out

# --- SWITCH --- #
mkdir -p out/switch/sphaira/
cp -r build/MinSizeRel/*.nro out/switch/sphaira/sphaira.nro
cp -r out/switch/sphaira/sphaira.nro out/hbmenu.nro
cd out
zip -r9 sphaira.zip switch
zip -r9 hbmenu.zip hbmenu.nro
cd ../..
