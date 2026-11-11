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

# monitor 3D sin gafas: permiso para encender la lente por el USB del monitor
as_root() {   # $1 = comandos; sudo si funciona, si no su (contrasena de root, Debian sin sudo)
  if command -v sudo >/dev/null 2>&1 && sudo -v 2>/dev/null; then
    sudo sh -c "$1"
  else
    echo "  (sudo no disponible para tu usuario: escribe la contrasena de root)"
    su -c "$1" root
  fi
}
if [ "$OUTPUT" = weave ]; then
  LENS="$HOME/.local/bin/spatial-lens"
  echo
  echo "Monitor 3D sin gafas: comprobando la lente..."
  PORT="$("$LENS" port 2>/dev/null || true)"
  if [ -z "$PORT" ]; then
    echo "  No encuentro el monitor por USB (354b). Conecta su cable USB (USB-C o USB-B) al PC y ejecuta"
    echo "  otra vez ./install.sh lente  (sin ese cable la lente no se puede encender)."
  elif "$LENS" status >/dev/null 2>&1; then
    echo "  Lente accesible en $PORT."
  else
    echo "  Hace falta permiso de administrador para usar $PORT (la lente del monitor)."
    U="$(id -un)"
    if as_root "cp '$PWD/tools/70-spatial-lens.rules' /etc/udev/rules.d/ && udevadm control --reload && \
                udevadm trigger --action=add --subsystem-match=tty && udevadm settle; \
                chmod 666 '$PORT'; usermod -aG dialout '$U' 2>/dev/null; true"; then
      echo "  regla udev -> /etc/udev/rules.d/70-spatial-lens.rules"
    else
      echo "  AVISO: no se pudo (contrasena incorrecta o sin permisos de administrador)."
    fi
    sleep 1
    if "$LENS" status; then
      echo "  Lente accesible: se encendera sola al jugar."
    else
      echo "  La lente sigue sin permiso. Desconecta y conecta el cable USB del monitor, o cierra sesion y vuelve"
      echo "  a entrar (grupo dialout), y prueba:  spatial-lens on"
    fi
  fi
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
