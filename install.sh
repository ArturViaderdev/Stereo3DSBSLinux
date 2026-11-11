#!/bin/sh
# Instala la capa Vulkan stereo3d para tu usuario.
#   ./install.sh           pregunta que pantalla 3D usas
#   ./install.sh gafas     gafas SBS (Rokid, XREAL, VITURE...)        | glasses
#   ./install.sh tele      tele / monitor / proyector 3D en modo SBS  | tv
#   ./install.sh lente     monitor 3D sin gafas (Acer SpatialLabs, Samsung Odyssey 3D) | lenticular
set -e
cd "$(dirname "$0")"

# ---- que pantalla 3D
KIND="$1"
if [ -z "$KIND" ] && [ -t 0 ]; then
  cat <<EOT
Que pantalla 3D vas a usar? / Which 3D display?
  1) Gafas SBS: Rokid Air, XREAL, VITURE... (3840x1080)          / SBS glasses
  2) Tele, monitor o proyector 3D con modo SBS (lado a lado)      / 3D TV or monitor (SBS mode)
  3) Monitor 3D sin gafas: Acer SpatialLabs View, Samsung Odyssey 3D / glasses-free 3D monitor
EOT
  printf "Numero / number [1]: "
  read -r ans || ans=""
  case "$ans" in 2) KIND=tele ;; 3) KIND=lente ;; *) KIND=gafas ;; esac
fi
case "$KIND" in
  2|tele|tv|half) OUTPUT=half ;;
  3|lente|lenticular|weave|monitor) OUTPUT=weave ;;
  *) OUTPUT=auto ;;
esac

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

# default.conf: se crea si no existe y se le pone la salida elegida (output = ...)
setup_conf() {   # $1 = config dir
  mkdir -p "$1"
  [ -f "$1/default.conf" ] || cp default.conf.example "$1/default.conf"
  if grep -q '^[[:space:]]*output[[:space:]]*=' "$1/default.conf"; then
    sed -i "s/^[[:space:]]*output[[:space:]]*=.*/output = $OUTPUT/" "$1/default.conf"
  else
    printf '\noutput = %s\n' "$OUTPUT" >> "$1/default.conf"
  fi
  sed -i '/^[[:space:]]*sbs[[:space:]]*=/d' "$1/default.conf"  # (stereo3d 1.x: the old name of output)
  echo "  configuracion -> $1/default.conf (output = $OUTPUT)"
}

echo "Instalando stereo3d..."
install_to "${XDG_DATA_HOME:-$HOME/.local/share}" "$HOME/.local/lib/stereo3d"
CFG="${XDG_CONFIG_HOME:-$HOME/.config}/stereo3d"
setup_conf "$CFG"

# Steam en Flatpak tiene su propio $XDG_DATA_HOME
FP="$HOME/.var/app/com.valvesoftware.Steam"
if [ -d "$FP" ]; then
  install_to "$FP/data" "$FP/data/stereo3d-lib"
  setup_conf "$FP/config/stereo3d"
  echo "  (detectado Steam Flatpak)"
fi

mkdir -p "$HOME/.local/bin"
for t in stereo3d-run stereo3d-recorder stereo3d-shader-scan spatial-lens; do
  cp "tools/$t" "$HOME/.local/bin/$t"
  chmod +x "$HOME/.local/bin/$t"
done
[ -f build/stereo3d-tool ] && cp build/stereo3d-tool "$HOME/.local/bin/stereo3d-tool"
echo "  lanzador -> $HOME/.local/bin/stereo3d-run"

# monitor 3D sin gafas: permiso para encender la lente por el USB del monitor (regla udev, pide la contrasena)
if [ "$OUTPUT" = weave ]; then
  if [ ! -f /etc/udev/rules.d/70-spatial-lens.rules ]; then
    echo
    echo "Monitor 3D sin gafas: hace falta permiso para encender su lente por USB (sudo)."
    if sudo cp tools/70-spatial-lens.rules /etc/udev/rules.d/ && sudo udevadm control --reload && sudo udevadm trigger; then
      echo "  regla udev -> /etc/udev/rules.d/70-spatial-lens.rules"
      sleep 1
    else
      echo "  AVISO: no se pudo instalar la regla udev; la lente no se encendera sola. Hazlo a mano:"
      echo "    sudo cp tools/70-spatial-lens.rules /etc/udev/rules.d/ && sudo udevadm control --reload && sudo udevadm trigger"
    fi
  fi
  echo
  echo "Comprobando la lente del monitor:"
  "$HOME/.local/bin/spatial-lens" info || echo "  -> Conecta el cable USB del monitor (USB-C o USB-B) al PC y vuelve a probar: spatial-lens info"
fi

cat <<EOT

Listo. En Steam: clic derecho en el juego > Propiedades > General > Opciones de lanzamiento:

  $HOME/.local/bin/stereo3d-run %command%

Luego juega a pantalla completa. Teclas: Ctrl+Shift + F3/F4 profundidad, F5/F6 convergencia, F9 guardar.
EOT
if [ "$OUTPUT" = weave ]; then
  cat <<EOT
Monitor 3D sin gafas: la primera vez calibra la lente (README, "Guia rapida", paso 4):
  en el juego Ctrl+Shift+F10, ajusta con F3/F4, F5/F6, Insert/Supr y guarda con Ctrl+Shift+F9.
EOT
fi
echo "Registro de cada juego: $CFG/logs/<Juego>.log"
