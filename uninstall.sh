#!/bin/sh
# Desinstala la capa (deja tus perfiles en ~/.config/stereo3d)
D="${XDG_DATA_HOME:-$HOME/.local/share}"
mv -f "$D/vulkan/implicit_layer.d/stereo3d.json" /tmp/ 2>/dev/null
mv -f "$D/vulkan/implicit_layer.d/stereo3d32.json" /tmp/ 2>/dev/null
mv -f "$HOME/.local/lib/stereo3d" /tmp/stereo3d-lib-old 2>/dev/null
FP="$HOME/.var/app/com.valvesoftware.Steam/data"
mv -f "$FP/vulkan/implicit_layer.d/stereo3d.json" /tmp/stereo3d-flatpak.json 2>/dev/null
mv -f "$FP/vulkan/implicit_layer.d/stereo3d32.json" /tmp/stereo3d32-flatpak.json 2>/dev/null
for t in stereo3d-run stereo3d-recorder stereo3d-shader-scan stereo3d-tool spatial-lens; do
  mv -f "$HOME/.local/bin/$t" /tmp/ 2>/dev/null
done
echo "stereo3d desinstalado (archivos movidos a /tmp)."
