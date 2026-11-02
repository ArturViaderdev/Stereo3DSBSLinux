# Game compatibility / Compatibilidad de juegos

**English:** results reported by users. To add a game, open an issue with the *Game compatibility report* template (or a pull request editing this table). Please include the line `game X fps -> SBS ...` from `~/.config/stereo3d/logs/<Game>.log`.

**Español:** resultados enviados por usuarios. Para añadir un juego, abre una *issue* con la plantilla *Game compatibility report* (o un pull request editando esta tabla). Incluye la línea `game X fps -> SBS ...` de `~/.config/stereo3d/logs/<Juego>.log`.

Status / Estado: ✅ works / funciona · ⚠️ works with issues / funciona con problemas · ❌ does not work / no funciona

| Game / Juego | API | Mode / Modo | Status | FPS per eye / por ojo | Display / Pantalla | Notes / Notas |
|---|---|---|---|---|---|---|
| Burnout Revenge (native port / port nativo) | Vulkan | replay | ✅ | 60 | Rokid Air | Bring HUD forward with Ctrl+Shift+F1 / Acercar el HUD con Ctrl+Shift+F1 |
| Trials Rising | DX11 (DXVK) | replay | ✅ | | Rokid Air | 1920x1080, own output window / ventana propia |
| Earth Defense Force 6 | DX11 (DXVK) | alternate | ✅ | 30 | Rokid Air | Game capped at 60 and reads back from the GPU every frame: replay gives 2D / Limitado a 60 y lee de la GPU en cada fotograma: replay sale en 2D |
| Earth Defense Force 5 | DX11 (DXVK) | alternate | ⚠️ | 30 | Rokid Air | Not fully tested / Sin probar a fondo |
| Earth Defense Force: World Brothers 2 | | alternate | ❌ | | Rokid Air | Hangs at start in replay; alternate pending / Se cuelga al arrancar en replay; alternate pendiente |
| Forza Horizon 3 (Azimuth, Xbox One emulator) | DX12 (VKD3D) | alternate | ⚠️ | 30 | Rokid Air | Emulator: replay hangs (emulated GPU writes). Do not use `STEREO3D_HIDE_EXT=0` with older versions / Emulador: replay se cuelga |
