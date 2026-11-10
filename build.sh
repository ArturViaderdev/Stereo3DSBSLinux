#!/bin/sh
# Compila la capa stereo3d (64 y 32 bits) y sbs-desktop. Solo necesita g++ (C++17).
# Las cabeceras de Vulkan van incluidas.
set -e
cd "$(dirname "$0")"
mkdir -p build
CXX=${CXX:-g++}
SRC="src/layer.cpp src/spirv_patch.cpp src/spirv_screenfix.cpp src/config.cpp src/output.cpp src/lens.cpp"
FLAGS="-std=c++17 -O2 -g0 -fPIC -shared -fvisibility=hidden -Wall -Wno-missing-field-initializers -Ithird_party/include"
LINK="-static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL -Wl,--no-undefined -Wl,-z,defs -ldl -pthread"
$CXX $FLAGS $SRC -o build/libstereo3d_layer.so.new $LINK
mv -f build/libstereo3d_layer.so.new build/libstereo3d_layer.so
# 32-bit layer for 32-bit games (old DX9/DX11 games under Proton). Needs g++-multilib;
# without it the existing build/libstereo3d_layer32.so (if any) is used.
if echo 'int main(){}' | $CXX -m32 -x c++ - -o /dev/null 2>/dev/null; then
  $CXX -m32 $FLAGS $SRC -o build/libstereo3d_layer32.so.new $LINK
  mv -f build/libstereo3d_layer32.so.new build/libstereo3d_layer32.so
else
  [ -f build/libstereo3d_layer32.so ] && echo "(no 32-bit compiler: using the existing build/libstereo3d_layer32.so)" \
    || echo "(no 32-bit compiler: 32-bit games will not get 3D; sudo apt install g++-multilib and run it again)"
fi
$CXX -std=c++17 -O2 -Wall -Ithird_party/include tools/stereo3d-tool.cpp src/config.cpp src/lens.cpp src/spirv_patch.cpp \
  src/spirv_screenfix.cpp -o build/stereo3d-tool.new -ldl -pthread
mv -f build/stereo3d-tool.new build/stereo3d-tool
$CXX -std=c++17 -O2 -Ithird_party/include tools/sbs-desktop.cpp -o build/sbs-desktop.new -ldl
mv -f build/sbs-desktop.new build/sbs-desktop   # works even while it is running
echo "OK -> build/libstereo3d_layer.so build/libstereo3d_layer32.so build/stereo3d-tool build/sbs-desktop"
