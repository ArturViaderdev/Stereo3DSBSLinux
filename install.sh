#!/bin/sh
# Instala la capa Vulkan stereo3d para tu usuario (no necesita root).
set -e
cd "$(dirname "$0")"

# Recompila para tu sistema si hay compilador; si no, usa el binario incluido.
if command -v g++ >/dev/null 2>&1; then
  ./build.sh || echo "Aviso: no se pudo recompilar, uso el binario incluido."
fi
[ -f build/libstereo3d_layer.so ] || { echo "Falta build/libstereo3d_layer.so (instala g++ y ejecuta ./build.sh)"; exit 1; }

install_to() {   # $1 = data dir (contiene vulkan/implicit_layer.d), $2 = lib dir
  mkdir -p "$1/vulkan/implicit_layer.d" "$2"
  cp build/libstereo3d_layer.so "$2/"
  sed "s#@LIB@#$2/libstereo3d_layer.so#" stereo3d_layer.json.in > "$1/vulkan/implicit_layer.d/stereo3d.json"
  echo "  capa -> $1/vulkan/implicit_layer.d/stereo3d.json"
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
cp tools/stereo3d-run "$HOME/.local/bin/stereo3d-run"
chmod +x "$HOME/.local/bin/stereo3d-run"
echo "  lanzador -> $HOME/.local/bin/stereo3d-run"

CFG="${XDG_CONFIG_HOME:-$HOME/.config}/stereo3d"
mkdir -p "$CFG"
[ -f "$CFG/default.conf" ] || cp default.conf.example "$CFG/default.conf"
[ -d "$FP" ] && mkdir -p "$FP/config/stereo3d" && { [ -f "$FP/config/stereo3d/default.conf" ] || cp default.conf.example "$FP/config/stereo3d/default.conf"; }

cat <<EOF

Listo. Opciones de lanzamiento en Steam (Propiedades del juego > General):

  $HOME/.local/bin/stereo3d-run %command%

Juegos que no admiten el modo replay (EDF, emuladores):

  STEREO3D_MODE=alternate $HOME/.local/bin/stereo3d-run %command%

(o pon "mode = alternate" en ~/.config/stereo3d/<Juego.exe>.conf)
Teclas: Ctrl+Shift + F3/F4 separacion | F5/F6 convergencia | F1/F2 HUD | F9 guardar perfil
Lee README.md para todo lo demas.
EOF
