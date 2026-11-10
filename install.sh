#!/bin/sh
# Instala la capa Vulkan stereo3d para tu usuario (no necesita root).
set -e
cd "$(dirname "$0")"

# Recompila para tu sistema si hay compilador; si no, usa los binarios incluidos.
if command -v g++ >/dev/null 2>&1; then
  ./build.sh || echo "Aviso: no se pudo recompilar, uso los binarios incluidos."
fi
[ -f build/libstereo3d_layer.so ] || { echo "Falta build/libstereo3d_layer.so (instala g++ y ejecuta ./build.sh)"; exit 1; }

install_to() {   # $1 = data dir (contiene vulkan/implicit_layer.d), $2 = lib dir
  mkdir -p "$1/vulkan/implicit_layer.d" "$2"
  cp build/libstereo3d_layer.so "$2/"
  sed "s#@LIB@#$2/libstereo3d_layer.so#" stereo3d_layer.json.in > "$1/vulkan/implicit_layer.d/stereo3d.json"
  echo "  capa -> $1/vulkan/implicit_layer.d/stereo3d.json"
  if [ -f build/libstereo3d_layer32.so ]; then  # juegos de 32 bits
    cp build/libstereo3d_layer32.so "$2/"
    sed "s#@LIB@#$2/libstereo3d_layer32.so#; s#VK_LAYER_STEREO3D_sbs#VK_LAYER_STEREO3D_sbs_32#" stereo3d_layer.json.in \
      > "$1/vulkan/implicit_layer.d/stereo3d32.json"
    echo "  capa 32 bits -> $1/vulkan/implicit_layer.d/stereo3d32.json"
  fi
}

echo "Instalando stereo3d..."
install_to "${XDG_DATA_HOME:-$HOME/.local/share}" "$HOME/.local/lib/stereo3d"

# Steam en Flatpak tiene su propio $XDG_DATA_HOME
FP="$HOME/.var/app/com.valvesoftware.Steam"
if [ -d "$FP" ]; then
  install_to "$FP/data" "$FP/data/stereo3d-lib"
  echo "  (detectado Steam Flatpak)"
fi

mkdir -p "$HOME/.local/bin"
for t in stereo3d-run stereo3d-recorder stereo3d-shader-scan spatial-lens; do
  cp "tools/$t" "$HOME/.local/bin/$t"
  chmod +x "$HOME/.local/bin/$t"
done
[ -f build/stereo3d-tool ] && cp build/stereo3d-tool "$HOME/.local/bin/stereo3d-tool"
echo "  lanzador -> $HOME/.local/bin/stereo3d-run"

CFG="${XDG_CONFIG_HOME:-$HOME/.config}/stereo3d"
mkdir -p "$CFG"
[ -f "$CFG/default.conf" ] || cp default.conf.example "$CFG/default.conf"
if [ -d "$FP" ]; then
  mkdir -p "$FP/config/stereo3d"
  [ -f "$FP/config/stereo3d/default.conf" ] || cp default.conf.example "$FP/config/stereo3d/default.conf"
fi

cat <<EOT

Listo. Opciones de lanzamiento en Steam (Propiedades del juego > General):

  $HOME/.local/bin/stereo3d-run %command%

Juegos que no admiten replay (EDF, emuladores) a 60 fps:   STEREO3D_MODE=hybrid (o alternate) delante
Monitor 3D lenticular (Acer SpatialLabs, Samsung Odyssey 3D):
  sudo cp tools/70-spatial-lens.rules /etc/udev/rules.d/ && sudo udevadm control --reload && sudo udevadm trigger
  y "output = weave" en $CFG/default.conf (calibracion: README)
Teclas: Ctrl+Shift + F3/F4 separacion | F5/F6 convergencia | F1/F2 HUD | F9 guardar perfil
Lee README.md para todo lo demas.
EOT
