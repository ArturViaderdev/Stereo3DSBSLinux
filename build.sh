#!/bin/sh
# Compila la capa stereo3d. Solo necesita g++ (C++17). Las cabeceras Vulkan van incluidas.
set -e
cd "$(dirname "$0")"
mkdir -p build
CXX=${CXX:-g++}
$CXX -std=c++17 -O2 -g0 -fPIC -shared -fvisibility=hidden -Wall -Wno-missing-field-initializers \
  -Ithird_party/include \
  src/layer.cpp src/spirv_patch.cpp src/config.cpp src/output.cpp \
  -o build/libstereo3d_layer.so.new \
  -static-libstdc++ -static-libgcc -Wl,--no-undefined -Wl,-z,defs -ldl -pthread
mv -f build/libstereo3d_layer.so.new build/libstereo3d_layer.so
$CXX -std=c++17 -O2 -Ithird_party/include tools/sbs-desktop.cpp -o build/sbs-desktop.new -ldl
mv -f build/sbs-desktop.new build/sbs-desktop   # works even while it is running
echo "OK -> build/libstereo3d_layer.so build/sbs-desktop"
