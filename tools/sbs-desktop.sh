#!/bin/bash
# Escritorio en las gafas en modo 3D (SBS 3840x1080):
#  - divide la pantalla de las gafas en dos "monitores" de 1920x1080 para que las ventanas,
#    paneles y maximizados vayan a la mitad izquierda,
#  - duplica esa mitad en la derecha con sbs-desktop, asi cada ojo ve el escritorio entero.
# Mientras un juego con stereo3d esta en pantalla, el espejo se aparta solo.
# Ctrl+C (o cerrar la terminal) lo deshace todo.
#
# Uso: tools/sbs-desktop.sh [salida]     p. ej. tools/sbs-desktop.sh HDMI-0
set -u
DIR="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$DIR/build/sbs-desktop"
[ -x "$BIN" ] || { echo "Falta $BIN: ejecuta ./build.sh"; exit 1; }
[ -n "${WAYLAND_DISPLAY:-}" ] && echo "Aviso: sesion Wayland. Esto solo funciona en una sesion X11."

OUT="${1:-}"
LINE=""
if [ -z "$OUT" ]; then
  # salida conectada a 3840x1080 (las gafas en modo 3D)
  LINE=$(xrandr --query | awk '/ connected/ && /3840x1080\+/ {print; exit}')
  OUT=$(echo "$LINE" | awk '{print $1}')
else
  LINE=$(xrandr --query | awk -v o="$OUT" '$1==o {print; exit}')
fi
if [ -z "$OUT" ] || [ -z "$LINE" ]; then
  echo "No encuentro ninguna salida a 3840x1080. Pon las gafas en modo 3D o indica la salida:"
  xrandr --query | awk '/ connected/ {print "   " $0}'
  exit 1
fi
GEO=$(echo "$LINE" | grep -oE '[0-9]+x[0-9]+\+[0-9]+\+[0-9]+' | head -1)
W=${GEO%%x*}; REST=${GEO#*x}; H=${REST%%+*}; REST=${REST#*+}; X=${REST%%+*}; Y=${REST#*+}
HW=$((W / 2))
echo "Gafas: $OUT ${W}x${H}+${X}+${Y}  ->  escritorio en ${HW}x${H}, duplicado a la derecha"

cleanup() {
  [ -n "${PID:-}" ] && kill "$PID" 2>/dev/null
  xrandr --delmonitor SBS-IZQ 2>/dev/null
  xrandr --delmonitor SBS-DER 2>/dev/null
  echo "Escritorio normal restaurado."
}
trap cleanup EXIT INT TERM

xrandr --delmonitor SBS-IZQ 2>/dev/null; xrandr --delmonitor SBS-DER 2>/dev/null
xrandr --setmonitor SBS-IZQ "${HW}/300x${H}/170+${X}+${Y}" "$OUT" || echo "Aviso: no se pudo dividir la pantalla (xrandr --setmonitor)."
xrandr --setmonitor SBS-DER "${HW}/300x${H}/170+$((X + HW))+${Y}" none 2>/dev/null

"$BIN" "$X" "$Y" "$W" "$H" &
PID=$!
wait "$PID"
