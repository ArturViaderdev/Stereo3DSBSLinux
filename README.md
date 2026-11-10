<a id="english"></a>

# stereo3d — real stereoscopic 3D for Linux games on 3D displays

**English** · [Español](#español)

A Vulkan layer that turns Linux games into **real geometric 3D**: each eye is a full render with its own camera (shader patching, like NVIDIA 3D Vision's automatic mode), not a 2D image warped with a depth map. It works on any 3D display:

- **Full SBS 3840x1080** (each eye 1920x1080): AR glasses in 3D mode such as **Rokid Air** (tested), XREAL, VITURE…
- **Half SBS 1920x1080** (the display stretches each half): 3D TVs, 3D monitors and projectors with an SBS input mode.
- **Weave** (experimental): **lenticular 3D monitors** such as the **Acer SpatialLabs View** and the **Samsung Odyssey 3D**, without their Windows software. Each colour subpixel gets the eye its lens sends it to; the lens is switched on through the monitor's USB port. Fixed viewing position (no eye tracking), manual calibration.

Works with Windows games on Proton/Wine (DX9/10/11 through DXVK, DX12 through VKD3D‑Proton, Vulkan) and native Linux Vulkan games, 64 and 32 bits.

Shares its engine with [Nvidia3dVisionLinux](https://github.com/ArturViaderdev/Nvidia3dVisionLinux) (3D Vision shutter glasses): replay, alternate, depth and hybrid modes, Unreal Engine shadow fixes and 3D video recording are the same in both.

Not supported: OpenGL-only games, top-and-bottom / frame-packing / anaglyph output.

## Installation

```sh
./install.sh
```

If `g++` is available it rebuilds for your system (with `g++-multilib` it also builds the layer for 32-bit games). The layer goes to `~/.local/share/vulkan/implicit_layer.d/` (and the Flatpak Steam path if present), plus `~/.local/bin/stereo3d-run` and the tools. The layer is **inactive** unless `ENABLE_STEREO3D=1` is set. Run `./install.sh` again after every update.

## Launch options

### Steam (Windows games on Proton)

Game properties → General → Launch options:

```
/home/YOUR_USER/.local/bin/stereo3d-run %command%
```

The launcher sets `ENABLE_STEREO3D=1`, turns off DXVK's own vsync and frame limiter (`DXVK_CONFIG="dxgi.syncInterval=0;d3d9.presentInterval=0;dxgi.maxFrameRate=0;d3d9.maxFrameRate=0"`, without it there is stutter) and starts the video recorder.

Variants (put the variables in front of the launcher, e.g. `STEREO3D_MODE=hybrid /home/YOUR_USER/.local/bin/stereo3d-run %command%`):

| Case | Add |
|---|---|
| Games that read back from the GPU every frame (EDF5/EDF6/EDF WB2), emulators (Azimuth…) | `STEREO3D_MODE=alternate` |
| Those games when they are capped at 60 fps (alternate would give 30 per eye) | `STEREO3D_MODE=hybrid` (or `depth`) |
| Force replay without automatic fallback | `STEREO3D_MODE=replay STEREO3D_AUTO_SWITCH=0` |
| Force the output | `STEREO3D_OUTPUT=full` / `half` / `weave` |
| Force / forbid the layer's own output window | `STEREO3D_OWN_WINDOW=1` / `0` |
| Vulkan 1.0 apps (older native games) | `STEREO3D_LEGACY=1` |

Instead of the variable you can put `mode = hybrid` in the game profile (`~/.config/stereo3d/<Game.exe>.conf`).

### Native Linux games (Vulkan)

```sh
~/.local/bin/stereo3d-run ./run.sh     # or: ENABLE_STEREO3D=1 ./run.sh
```

### In-game settings

| Setting | Value | Why |
|---|---|---|
| Resolution | glasses: **1920x1080** (one eye) · 3D TV: the TV's · lenticular monitor: **3840x2160** | |
| Display mode | Fullscreen or borderless (best); windowed also works | see "Own output window" |
| Anti-aliasing | **FXAA/SMAA/none, NEVER TAA** | TAA, DLSS and FSR2/3 blend in another frame and you see double |
| Motion blur | Off | same reason |
| Frame generation | Off | breaks 3D |

## Displays

`output = auto` (default) picks **full SBS** when the display is 32:9 (glasses in 3D mode) and **half SBS** otherwise. Switch the glasses / TV to its 3D SBS mode **before** starting the game. With several monitors the layer picks the 32:9 one for its own window; otherwise set `STEREO3D_OUTPUT_RECT=x,y,width,height`.

### Lenticular 3D monitors (weave, experimental)

Acer SpatialLabs View, Samsung Odyssey 3D and other LeiaSR monitors have a lens in front of the panel that sends each colour subpixel to one eye or the other. With `output = weave` the layer builds the display image subpixel by subpixel from both eyes, for a **fixed viewing position** (centred, at your usual distance): there is no eye tracking, so moving your head sideways swaps or mixes the views.

1. **The lens.** Connect the monitor's USB cable (USB-C / USB-B upstream) to the PC. The monitor appears as a serial port (`/dev/ttyACM0`, USB `354b`) that takes `SR+LENS=1` / `SR+LENS=0`. Give your user access once:
   ```sh
   sudo cp tools/70-spatial-lens.rules /etc/udev/rules.d/ && sudo udevadm control --reload && sudo udevadm trigger
   spatial-lens info      # port found? serial numbers
   spatial-lens on        # the picture should look doubled / striped
   spatial-lens off
   ```
   The layer switches the lens on when the game starts, follows Ctrl+Shift+F8 (3D on/off) and switches it off on exit (`lens = 0` to leave it alone).
2. **Output.** `output = weave` in `~/.config/stereo3d/default.conf` (or `STEREO3D_OUTPUT=weave`). The game runs at the monitor's resolution (3840x2160) for each eye, so replay mode means two 4K renders per frame: use `hybrid` if the GPU cannot keep up. The layer presents on its own full-screen window on the monitor (the lens is fixed to the monitor's pixels).
3. **Calibration** (once per monitor; the values of each model are not public yet). Start any game with the layer, sit centred at your usual distance and press **Ctrl+Shift+F10**: test pattern, **left eye red, right eye cyan**. With the pattern on, the keys change the lens parameters (the longer you hold a key, the faster):

   | Key | Parameter | What to look for |
   |---|---|---|
   | F3 / F4 | `weave_pitch` (lens width in pixels) | the coloured bands across the screen get wider and wider until one eye sees one colour everywhere |
   | F5 / F6 | `weave_slant` (lens tilt) | the bands stop being tilted |
   | Insert / Delete | `weave_offset` (lens phase) | close your right eye: **red**; close your left eye: **cyan** (if swapped, keep moving: half a turn) |
   | End | `weave_bgr` | colour fringes: panel with B,G,R subpixel order |
   | Home | `weave_subpixel` | subpixel / whole-pixel weaving |

   **Ctrl+Shift+F9** saves the calibration in `~/.config/stereo3d/weave.conf` (used by every game); F10 again goes back to the game. `weave_soft` (0–0.25) blends the views a little at the lens edges. When the real values of a model are known, they go in `weave.conf` and no calibration is needed.

## Compatibility

See **[COMPATIBILITY.md](COMPATIBILITY.md)** for the list of tested games. Please report your results by opening an issue with the *Game compatibility report* template.

## Modes

- **replay** (default): the game renders each frame once and the layer submits the same commands again with the other eye's camera. A game at 60 fps gives **60 fps per eye**, both eyes at the same instant. Twice the GPU work. If the game needs GPU results in the middle of a frame, those frames are 2D and the layer **switches to alternate** by itself. Replay starts after a **warm-up** of `replay_warmup` seconds (default 8) in alternate: intros, shader compilation and loading screens that would hang in replay get going first.
- **alternate**: one game frame per eye. The game must run at 120 fps for 60 per eye; a game capped at 60 gives 30 per eye.
- **depth**: for games that cannot use replay and are capped at 60 fps. The game renders one normal image per frame; the layer copies the scene's depth buffer right after the main pass and rebuilds **both eyes** from the image and the depth (60 fps per eye). It learns how the game's depth maps to distance from samples stored by the patched shaders, so convergence works in the game's units. Limits: small halos where background is uncovered next to near objects, and the HUD takes the depth of what is behind it. Needs DXVK / VKD3D-Proton (dynamic rendering). Menus and loading screens without a 3D scene are shown flat. Scenes drawn in several stages (EDF, Unity) are combined pixel by pixel; an upside-down depth buffer (Unity) and scenes with black bars are detected.
- **hybrid**: alternate and depth together, for the same games. Each game frame is rendered for one eye with real stereo geometry, and the **other eye is rebuilt from that frame's depth**: 60 fps per eye, half of the images real, and the background uncovered next to near objects comes from the last real image of that eye: far fewer halos than depth.
  - **HUD separation** (depth and hybrid, experimental, `hud_separation = 1`): the layer copies the game's back buffer right after the scene and before the HUD, rebuilds the eyes from the scene alone and keeps the HUD, crosshair and menus on the screen plane. **Ctrl+Shift+Scroll Lock** cycles off → automatic → copy after draw 1 … 6 → off; F9 saves it (`hud_separation`, `hud_split`).
  - `flat_menus = 1` (hybrid): screens without a 3D scene (title, menus, loading) without stereo shift, for games whose 2D menus sit far behind the screen (EDF 6).

Whatever the mode, the display shows the newest complete stereo pair on every refresh; if the game is slower the last pair is shown again.

## Own output window

With full SBS, if the game window does not cover the glasses (a windowed game, or a game that stays at 1920x1080 on a 3840x1080 desktop), the layer opens **its own full-screen window** on the 3D display (input-transparent, does not steal focus). With weave the layer always uses it on the 3D monitor. The log says which one is used. Force with `own_window = 1|0` (`auto` by default).

Launchers and helper processes (Ubisoft Connect, Epic, EA, Steam, explorer.exe…) are ignored automatically. To ignore another one: `STEREO3D_IGNORE=Process.exe`.

## Desktop on the 3D display (no monitor switching)

`tools/sbs-desktop.sh` keeps the desktop usable with full-SBS glasses in 3D mode:

```sh
tools/sbs-desktop.sh            # finds the 3840x1080 output; or: tools/sbs-desktop.sh HDMI-0
```

- Splits the glasses screen into two logical 1920x1080 monitors: windows, panels and maximized windows go to the left half.
- Mirrors the left half into the right half (with the mouse pointer), so each eye sees the whole desktop in 2D.
- While a stereo3d game is on screen the mirror steps aside, and comes back when the game closes.
- Ctrl+C in the terminal undoes everything.

Requires an **X11** session (not Wayland). If the right half stays black or frozen, disable desktop compositing (KDE: Alt+Shift+F12).

## In-game hotkeys (Ctrl + Shift + …)

| Key | Action |
|---|---|
| F3 / F4 | less / more **separation** (overall depth) |
| F5 / F6 | less / more **convergence** (which distance sits on the screen plane) |
| F1 / F2 | bring the **HUD** closer / farther |
| F7 | swap eyes |
| F8 | 3D on/off (weave: also the lens) |
| F9 | save this game's profile (weave: also the calibration) |
| F10 | test pattern on/off (SBS: left eye white, right eye black; weave: calibration, see above) |
| F11 | capture: game image + left + right eye (PPM; the 5 previous ones are kept) |
| F12 | start / stop recording a 3D video (full SBS) |
| Pause | Unreal shadow corrections on/off (`fix_shadows`) |
| Scroll Lock | depth / hybrid: HUD separation off → automatic → copy after draw 1 … 6 |
| PgUp / PgDn / Home | zoom in / out / reset |

It is **Ctrl + Shift** (not Alt + Shift: Alt+Shift+F-key may trigger the game's or the desktop's own functions).

**How to tune:** start with low separation. Raise convergence until the weapon, car or character is near the screen plane. Then raise separation until the background has depth without straining your eyes. If text and markers look "behind" the scene, bring them forward with F1 (`hud_depth`). Save with F9.

## Recording 3D videos

**Ctrl+Shift+F12** starts and stops recording the game as a **full side-by-side** video (left eye | right eye) with the desktop sound, ready for any SBS player (e.g. [3dvision-play](https://github.com/ArturViaderdev/3dvisionvideoplayer)), 3D TV or glasses.

- Needs `ffmpeg` and the game started with `stereo3d-run` (it starts `stereo3d-recorder`). Encoded with **NVENC (HEVC)** on the graphics card; without NVENC it falls back to libx264.
- Files: `~/Videos/stereo3d/<Game>-<date>-sbs.mkv` (or `~/Vídeos/...`), constant 60 fps. `record_dir`, `record_width` / `record_height` (each eye, default 1920x1080), `record_fps`, `record = 1` to record from the start.

## Unreal Engine shadows (`fix_shadows`)

Unreal Engine computes shadows (and other effects) in full-screen passes that rebuild each pixel's 3D position from the depth buffer with the camera's inverse matrix, which knows nothing about the stereo shift: shadows were shifted or doubled in one eye. `fix_shadows` (on by default, `auto`) fixes it while the game runs:

- **Screen-space rebuilds**: the layer finds the rebuild pattern in fragment and compute shaders and undoes the eye shift there, before the driver compiles the shader (no cost while playing). The pattern is very specific, so it is active in every game.
- **Shadow maps**: Unreal draws them in 16-bit depth, so 16-bit depth-only pipelines are left unshifted. Only in Unreal games (`*-Shipping.exe`) or with `fix_shadows = 1`.
- **Ctrl+Shift+Pause** compares with / without. `fix_shadows = 0` disables everything.

Tools to look into other games: `STEREO3D_DUMP_SHADERS=1` saves every shader in `~/.cache/stereo3d/shaders/<Game>/`; `stereo3d-tool shader-fix [folder]` lists the shaders `fix_shadows` corrects; `stereo3d-shader-scan [folder] [-v]` is a wider search (`--dis <hash>` disassembles one, needs `spirv-tools`).

## Configuration

- `~/.config/stereo3d/default.conf`: all games (see `default.conf.example`).
- `~/.config/stereo3d/weave.conf`: lens calibration of the 3D monitor.
- `~/.config/stereo3d/<Game.exe>.conf`: per-game profile (F9 creates it). Edits are reloaded live.
- Environment variables (highest priority): `STEREO3D_<KEY>=value` for any key (`STEREO3D_MODE`, `STEREO3D_OUTPUT`, `STEREO3D_WEAVE_PITCH`…). Short ones:

| Variable | Values |
|---|---|
| `STEREO3D_MODE` | `replay` (default), `alternate`, `depth`, `hybrid` |
| `STEREO3D_OUTPUT` | `auto` (default), `full`, `half`, `weave` (`STEREO3D_SBS` also works) |
| `STEREO3D_SEP` / `STEREO3D_CONV` | separation / convergence |
| `STEREO3D_HUD` | HUD depth (same units as convergence; `0` = screen plane) |
| `STEREO3D_SWAP` | `1` swaps eyes |
| `STEREO3D_OWN_WINDOW` | `auto` (default), `1`, `0` |
| `STEREO3D_OUTPUT_RECT` | `x,y,width,height` of the 3D display |
| `STEREO3D_HALF` | game sees half the width on 32:9 displays: `auto`, `1`, `0` |
| `STEREO3D_FPS_LIMIT` | extra game fps cap (`0` default: the display sets the pace) |
| `STEREO3D_IGNORE` | processes to ignore, e.g. `Launcher.exe` |
| `STEREO3D_AUTO_SWITCH` | `1` (default) replay → alternate when the game does not support it |
| `STEREO3D_REPLAY_WARMUP` | seconds of alternate before replay (`8`; `0` none) |
| `STEREO3D_HIDE_EXT` | `1` (default), `0` allows *graphics pipeline library* (less compile stutter) |
| `STEREO3D_LENS` / `STEREO3D_LENS_PORT` | weave: `auto` / `0`; serial port of the monitor |
| `STEREO3D_LOG` / `STEREO3D_LOG_LEVEL` | extra log file / `1` (default), `2` verbose |

## Logs and diagnostics

Each game writes **`~/.config/stereo3d/logs/<Game>.log`** (overwritten on every start).

- `swapchain (...): game WxH -> display WxH, full SBS / half SBS / WEAVE, each eye WxH`: what the layer decided.
- `display 60.0 Hz | game pairs shown 60.0/s`: every 5 s, the display rate and the stereo pairs per second.
- `game 60.0 fps -> 60.0 stereo pairs/s [replay: N frames 3D, M frames 2D]`: frames that came out 2D in replay.
- `STALL ... Layer state: ...`: the game stopped presenting for more than 2 s; the line says what the layer and every thread were doing. Held signals are released and, if still stuck, the layer switches to alternate.
- `switching to alternate mode`: the game does not support replay. Use `mode = hybrid` (60 fps games) or `alternate` in its profile.
- `lens: ...`: weave lens switching (port not found / no permission → udev rule).
- `shader stereo OFF`: the GPU/driver lacks buffer device address or Int64.

Ctrl+Shift+F11 saves `<Game>_game.ppm`, `<Game>_left.ppm`, `<Game>_right.ppm` and `<Game>_info.txt` in `~/.config/stereo3d/` (or `STEREO3D_DUMP_DIR`).

## How it works

1. **Shader patching**: every vertex/tessellation/geometry shader shifts geometry by depth, `x += eye · separation · (w − convergence)`. Pure 2D (`w = 1`) is untouched; HUD vertices get the HUD depth.
2. **Virtual swapchain**: the game renders into images owned by the layer. On each present they become one eye (alternate), both eyes (replay) or both rebuilt from depth (depth / hybrid) of a stereo pair (triple buffered).
3. **Presenter thread** on its own Vulkan queue owns the real swapchain (FIFO) and shows the newest pair on every refresh: side by side (copy or scaled blit) or woven by a compute shader (`src/shaders/weave.comp`), so the game's GPU work never delays a flip.

## Known limitations

- OpenGL-only games: no. Mesh-shader geometry stays flat (`hide_mesh_shaders = 1` makes some engines fall back).
- Weave: fixed viewing position, manual calibration; no eye tracking yet.
- Replay: effects that advance each time they are drawn may differ between eyes. Emulators only work in alternate / hybrid.

## Layout

```
src/layer.cpp            Vulkan layer: virtual swapchain, replay/alternate/depth/hybrid, presenter (SBS / weave)
src/spirv_patch.cpp      SPIR-V patcher (stereo shift)
src/spirv_screenfix.cpp  screen-space position rebuild correction (fix_shadows)
src/shaders/             depth3d.comp (depth / hybrid), weave.comp (lenticular); gen.sh -> *_spv.h
src/lens.cpp             lens switching of LeiaSR monitors (USB serial)
src/output.cpp           own output window on the 3D display (X11)
src/config.cpp           configuration, profiles, logs, hotkeys
tools/stereo3d-run       launcher;  stereo3d-recorder  video recording (ffmpeg)
tools/stereo3d-tool      shader-fix, lens on/off/status;  stereo3d-shader-scan  shader search
tools/spatial-lens       lens on/off from a shell;  70-spatial-lens.rules  udev rule
tools/sbs-desktop*       desktop mirror for full-SBS glasses
tests/                   patcher tests and a Vulkan 1.3 test app (depth, HUD, DXVK-like back buffer...)
third_party/include      Vulkan (Khronos) and X11 (MIT) headers
```

---

<a id="español"></a>

# stereo3d — 3D estereoscópico real para juegos de Linux en pantallas 3D

[English](#english) · **Español**

Una capa Vulkan que convierte los juegos de Linux en **3D geométrico real**: cada ojo es un renderizado completo con su propia cámara (parcheo de shaders, como el modo automático de NVIDIA 3D Vision), no una imagen 2D deformada con un mapa de profundidad. Funciona en cualquier pantalla 3D:

- **SBS completo 3840x1080** (cada ojo 1920x1080): gafas AR en modo 3D como las **Rokid Air** (probadas), XREAL, VITURE…
- **Half SBS 1920x1080** (la pantalla estira cada mitad): televisores, monitores y proyectores 3D con entrada SBS.
- **Weave** (experimental): **monitores 3D lenticulares** como el **Acer SpatialLabs View** y el **Samsung Odyssey 3D**, sin su software de Windows. Cada subpíxel de color recibe el ojo al que lo manda su lente; la lente se enciende por el puerto USB del monitor. Posición de visión fija (sin seguimiento ocular), calibración manual.

Funciona con juegos de Windows en Proton/Wine (DX9/10/11 con DXVK, DX12 con VKD3D‑Proton, Vulkan) y juegos nativos de Linux con Vulkan, de 64 y 32 bits.

Comparte el motor con [Nvidia3dVisionLinux](https://github.com/ArturViaderdev/Nvidia3dVisionLinux) (gafas de obturación 3D Vision): los modos replay, alternate, depth e hybrid, las correcciones de sombras de Unreal Engine y la grabación de vídeo 3D son los mismos en los dos.

No soportado: juegos solo OpenGL, salida arriba/abajo, frame packing o anaglifo.

## Instalación

```sh
./install.sh
```

Si hay `g++` recompila para tu sistema (con `g++-multilib` también la capa para juegos de 32 bits). La capa se instala en `~/.local/share/vulkan/implicit_layer.d/` (y en la ruta de Steam Flatpak si existe), más `~/.local/bin/stereo3d-run` y las herramientas. La capa está **inactiva** si no se pone `ENABLE_STEREO3D=1`. Vuelve a ejecutar `./install.sh` después de cada actualización.

## Parámetros de lanzamiento

### Steam (juegos de Windows con Proton)

Propiedades del juego → General → Opciones de lanzamiento:

```
/home/TU_USUARIO/.local/bin/stereo3d-run %command%
```

El lanzador pone `ENABLE_STEREO3D=1`, desactiva el vsync y el limitador propios de DXVK (`DXVK_CONFIG="dxgi.syncInterval=0;d3d9.presentInterval=0;dxgi.maxFrameRate=0;d3d9.maxFrameRate=0"`, sin eso hay tirones) y arranca el grabador de vídeo.

Variantes (las variables van delante del lanzador, p. ej. `STEREO3D_MODE=hybrid /home/TU_USUARIO/.local/bin/stereo3d-run %command%`):

| Caso | Añadir |
|---|---|
| Juegos que leen de la GPU en cada fotograma (EDF5/EDF6/EDF WB2), emuladores (Azimuth…) | `STEREO3D_MODE=alternate` |
| Esos juegos cuando están limitados a 60 fps (alternate daría 30 por ojo) | `STEREO3D_MODE=hybrid` (o `depth`) |
| Forzar replay sin cambio automático | `STEREO3D_MODE=replay STEREO3D_AUTO_SWITCH=0` |
| Forzar la salida | `STEREO3D_OUTPUT=full` / `half` / `weave` |
| Forzar / prohibir la ventana propia | `STEREO3D_OWN_WINDOW=1` / `0` |
| Aplicaciones Vulkan 1.0 (juegos nativos antiguos) | `STEREO3D_LEGACY=1` |

En vez de la variable puedes poner `mode = hybrid` en el perfil del juego (`~/.config/stereo3d/<Juego.exe>.conf`).

### Juegos nativos de Linux (Vulkan)

```sh
~/.local/bin/stereo3d-run ./run.sh     # o: ENABLE_STEREO3D=1 ./run.sh
```

### Ajustes dentro del juego

| Ajuste | Valor | Por qué |
|---|---|---|
| Resolución | gafas: **1920x1080** (un ojo) · tele 3D: la de la tele · monitor lenticular: **3840x2160** | |
| Modo de pantalla | Pantalla completa o sin bordes (mejor); en ventana también funciona | ver "Ventana propia" |
| Antialiasing | **FXAA/SMAA/ninguno, NUNCA TAA** | TAA, DLSS y FSR2/3 mezclan otro fotograma y se ve doble |
| Desenfoque de movimiento | Desactivado | mismo motivo |
| Generación de fotogramas | Desactivada | rompe el 3D |

## Pantallas

`output = auto` (por defecto) elige **SBS completo** si la pantalla es 32:9 (gafas en modo 3D) y **half SBS** en el resto. Pon las gafas / la tele en su modo 3D SBS **antes** de arrancar el juego. Con varios monitores la capa usa el de 32:9 para su ventana propia; si no, pon `STEREO3D_OUTPUT_RECT=x,y,ancho,alto`.

### Monitores 3D lenticulares (weave, experimental)

El Acer SpatialLabs View, el Samsung Odyssey 3D y otros monitores LeiaSR tienen una lente delante del panel que manda cada subpíxel de color a un ojo o al otro. Con `output = weave` la capa construye la imagen del monitor subpíxel a subpíxel con los dos ojos, para una **posición de visión fija** (centrado, a tu distancia habitual): no hay seguimiento ocular, así que si mueves la cabeza a un lado las vistas se cruzan o se mezclan.

1. **La lente.** Conecta el cable USB del monitor (USB-C / USB-B de subida) al PC. El monitor aparece como un puerto serie (`/dev/ttyACM0`, USB `354b`) que acepta `SR+LENS=1` / `SR+LENS=0`. Da acceso a tu usuario una vez:
   ```sh
   sudo cp tools/70-spatial-lens.rules /etc/udev/rules.d/ && sudo udevadm control --reload && sudo udevadm trigger
   spatial-lens info      # ¿encuentra el puerto? números de serie
   spatial-lens on        # la imagen debería verse doble / rayada
   spatial-lens off
   ```
   La capa enciende la lente al arrancar el juego, sigue a Ctrl+Shift+F8 (3D sí/no) y la apaga al salir (`lens = 0` para no tocarla).
2. **Salida.** `output = weave` en `~/.config/stereo3d/default.conf` (o `STEREO3D_OUTPUT=weave`). El juego va a la resolución del monitor (3840x2160) en cada ojo, así que replay son dos renderizados 4K por fotograma: usa `hybrid` si la gráfica no llega. La capa presenta en su propia ventana a pantalla completa en el monitor (la lente está fija a los píxeles del monitor).
3. **Calibración** (una vez por monitor; los valores de cada modelo aún no son públicos). Arranca cualquier juego con la capa, siéntate centrado a tu distancia habitual y pulsa **Ctrl+Shift+F10**: patrón de prueba, **ojo izquierdo rojo, ojo derecho cian**. Con el patrón activo las teclas cambian los parámetros de la lente (cuanto más tiempo mantienes la tecla, más rápido):

   | Tecla | Parámetro | Qué buscar |
   |---|---|---|
   | F3 / F4 | `weave_pitch` (ancho de lente en píxeles) | las bandas de colores que cruzan la pantalla se hacen cada vez más anchas hasta que cada ojo ve un solo color en toda la pantalla |
   | F5 / F6 | `weave_slant` (inclinación de la lente) | las bandas dejan de estar inclinadas |
   | Insert / Supr | `weave_offset` (fase de la lente) | cierra el ojo derecho: **rojo**; cierra el izquierdo: **cian** (si sale al revés, sigue moviendo: media vuelta) |
   | Fin | `weave_bgr` | bordes de color: panel con orden de subpíxeles B,G,R |
   | Inicio | `weave_subpixel` | entrelazado por subpíxel / por píxel entero |

   **Ctrl+Shift+F9** guarda la calibración en `~/.config/stereo3d/weave.conf` (la usan todos los juegos); F10 otra vez vuelve al juego. `weave_soft` (0–0,25) mezcla un poco las vistas en los bordes de la lente. Cuando se conozcan los valores reales de un modelo, se ponen en `weave.conf` y no hace falta calibrar.

## Compatibilidad

Lista de juegos probados en **[COMPATIBILITY.md](COMPATIBILITY.md)**. Puedes enviar tus resultados abriendo un issue con la plantilla *Game compatibility report*.

## Modos

- **replay** (por defecto): el juego dibuja cada fotograma una vez y la capa envía los mismos comandos otra vez con la cámara del otro ojo. Un juego a 60 fps da **60 fps por ojo**, los dos ojos en el mismo instante. Doble trabajo de GPU. Si el juego necesita resultados de la GPU a mitad de fotograma, esos fotogramas salen en 2D y la capa **pasa sola a alternate**. Replay empieza tras un **calentamiento** de `replay_warmup` segundos (8 por defecto) en alternate: intros, compilación de shaders y cargas que se colgarían en replay arrancan primero.
- **alternate**: un fotograma del juego para cada ojo. El juego necesita ir a 120 fps para 60 por ojo; limitado a 60 da 30 por ojo.
- **depth**: para juegos que no admiten replay y están limitados a 60 fps. El juego dibuja una imagen normal por fotograma; la capa copia el buffer de profundidad de la escena justo después de la pasada principal y reconstruye **los dos ojos** con la imagen y la profundidad (60 fps por ojo). Aprende cómo la profundidad del juego se convierte en distancia con muestras que guardan los shaders parcheados, así que la convergencia funciona en las unidades del juego. Límites: pequeños halos donde se descubre el fondo junto a objetos cercanos, y el HUD toma la profundidad de lo que hay detrás. Necesita DXVK / VKD3D-Proton (dynamic rendering). Menús y cargas sin escena 3D se ven planos. Escenas dibujadas en varias etapas (EDF, Unity) se combinan píxel a píxel; se detectan profundidades al revés (Unity) y escenas con barras negras.
- **hybrid**: alternate y depth juntos, para los mismos juegos. Cada fotograma del juego se dibuja para un ojo con geometría estéreo real y **el otro ojo se reconstruye con la profundidad de ese fotograma**: 60 fps por ojo, la mitad de las imágenes reales, y el fondo que se descubre junto a objetos cercanos sale de la última imagen real de ese ojo: muchos menos halos que depth.
  - **Separación del HUD** (depth e hybrid, experimental, `hud_separation = 1`): la capa copia el back buffer del juego justo después de la escena y antes del HUD, reconstruye los ojos solo con la escena y deja el HUD, la mira y los menús en el plano de la pantalla. **Ctrl+Shift+Bloq Despl** recorre desactivada → automática → copia tras el dibujo 1 … 6 → desactivada; F9 lo guarda (`hud_separation`, `hud_split`).
  - `flat_menus = 1` (hybrid): pantallas sin escena 3D (título, menús, cargas) sin desplazamiento estéreo, para juegos cuyos menús 2D quedan muy por detrás de la pantalla (EDF 6).

Sea cual sea el modo, la pantalla muestra en cada refresco el par estéreo completo más reciente; si el juego va más lento se repite el último par.

## Ventana propia

En SBS completo, si la ventana del juego no cubre las gafas (un juego en ventana, o que se queda en 1920x1080 en un escritorio de 3840x1080), la capa abre **su propia ventana a pantalla completa** en la pantalla 3D (transparente al ratón y al teclado, no roba el foco). En weave la capa la usa siempre en el monitor 3D. El registro dice cuál se usa. Se fuerza con `own_window = 1|0` (`auto` por defecto).

Los lanzadores y procesos auxiliares (Ubisoft Connect, Epic, EA, Steam, explorer.exe…) se ignoran solos. Para ignorar otro: `STEREO3D_IGNORE=Proceso.exe`.

## Escritorio en la pantalla 3D (sin cambiar de monitor)

`tools/sbs-desktop.sh` deja el escritorio usable con gafas en SBS completo:

```sh
tools/sbs-desktop.sh            # busca la salida de 3840x1080; o: tools/sbs-desktop.sh HDMI-0
```

- Divide la pantalla de las gafas en dos monitores lógicos de 1920x1080: ventanas, paneles y ventanas maximizadas van a la mitad izquierda.
- Copia la mitad izquierda en la derecha (con el puntero), así cada ojo ve todo el escritorio en 2D.
- Mientras hay un juego de stereo3d en pantalla la copia se aparta, y vuelve cuando el juego se cierra.
- Ctrl+C en el terminal lo deshace todo.

Necesita una sesión **X11** (no Wayland). Si la mitad derecha queda negra o congelada, desactiva la composición del escritorio (KDE: Alt+Shift+F12).

## Teclas en juego (Ctrl + Shift + …)

| Tecla | Acción |
|---|---|
| F3 / F4 | menos / más **separación** (profundidad general) |
| F5 / F6 | menos / más **convergencia** (qué distancia queda en el plano de la pantalla) |
| F1 / F2 | acercar / alejar el **HUD** |
| F7 | invertir ojos |
| F8 | 3D sí/no (weave: también la lente) |
| F9 | guardar el perfil del juego (weave: también la calibración) |
| F10 | patrón de prueba sí/no (SBS: ojo izquierdo blanco, derecho negro; weave: calibración, ver arriba) |
| F11 | captura: imagen del juego + ojo izquierdo + derecho (PPM; se guardan las 5 anteriores) |
| F12 | empezar / parar la grabación de un vídeo 3D (SBS completo) |
| Pausa | correcciones de sombras de Unreal sí/no (`fix_shadows`) |
| Bloq Despl | depth / hybrid: separación del HUD desactivada → automática → copia tras el dibujo 1 … 6 |
| RePág / AvPág / Inicio | acercar / alejar / zoom normal |

Es **Ctrl + Shift** (no Alt + Shift: Alt+Shift+tecla F puede activar funciones del juego o del escritorio).

**Cómo ajustar:** empieza con poca separación. Sube la convergencia hasta que el arma, el coche o el personaje queden cerca del plano de la pantalla. Después sube la separación hasta que el fondo tenga profundidad sin forzar la vista. Si los textos y marcadores se ven "detrás" de la escena, acércalos con F1 (`hud_depth`). Guarda con F9.

## Grabar vídeos 3D

**Ctrl+Shift+F12** empieza y para la grabación del juego en un vídeo **SBS completo** (ojo izquierdo | ojo derecho) con el sonido del escritorio, listo para cualquier reproductor SBS (p. ej. [3dvision-play](https://github.com/ArturViaderdev/3dvisionvideoplayer)), tele 3D o gafas.

- Necesita `ffmpeg` y arrancar el juego con `stereo3d-run` (arranca `stereo3d-recorder`). Se codifica con **NVENC (HEVC)** en la gráfica; sin NVENC usa libx264.
- Archivos: `~/Vídeos/stereo3d/<Juego>-<fecha>-sbs.mkv` (o `~/Videos/...`), 60 fps constantes. `record_dir`, `record_width` / `record_height` (cada ojo, 1920x1080 por defecto), `record_fps`, `record = 1` para grabar desde el principio.

## Sombras de Unreal Engine (`fix_shadows`)

Unreal Engine calcula las sombras (y otros efectos) en pasadas a pantalla completa que reconstruyen la posición 3D de cada píxel a partir de la profundidad con la matriz inversa de la cámara, que no sabe nada del desplazamiento estéreo: las sombras salían desplazadas o dobles en un ojo. `fix_shadows` (activo por defecto, `auto`) lo corrige con el juego en marcha:

- **Reconstrucciones en pantalla**: la capa encuentra el patrón de reconstrucción en los shaders de fragmento y compute y deshace allí el desplazamiento del ojo, antes de que el driver compile el shader (sin coste al jugar). El patrón es muy concreto, así que está activo en todos los juegos.
- **Mapas de sombras**: Unreal los dibuja con profundidad de 16 bits, así que los pipelines solo-profundidad de 16 bits se dejan sin desplazar. Solo en juegos de Unreal (`*-Shipping.exe`) o con `fix_shadows = 1`.
- **Ctrl+Shift+Pausa** compara con / sin. `fix_shadows = 0` lo desactiva todo.

Herramientas para estudiar otros juegos: `STEREO3D_DUMP_SHADERS=1` guarda todos los shaders en `~/.cache/stereo3d/shaders/<Juego>/`; `stereo3d-tool shader-fix [carpeta]` lista los shaders que corrige `fix_shadows`; `stereo3d-shader-scan [carpeta] [-v]` es una búsqueda más amplia (`--dis <hash>` desensambla uno, necesita `spirv-tools`).

## Configuración

- `~/.config/stereo3d/default.conf`: todos los juegos (ver `default.conf.example`).
- `~/.config/stereo3d/weave.conf`: calibración de la lente del monitor 3D.
- `~/.config/stereo3d/<Juego.exe>.conf`: perfil de cada juego (F9 lo crea). Los cambios se recargan en directo.
- Variables de entorno (máxima prioridad): `STEREO3D_<CLAVE>=valor` para cualquier clave (`STEREO3D_MODE`, `STEREO3D_OUTPUT`, `STEREO3D_WEAVE_PITCH`…). Las cortas:

| Variable | Valores |
|---|---|
| `STEREO3D_MODE` | `replay` (por defecto), `alternate`, `depth`, `hybrid` |
| `STEREO3D_OUTPUT` | `auto` (por defecto), `full`, `half`, `weave` (también vale `STEREO3D_SBS`) |
| `STEREO3D_SEP` / `STEREO3D_CONV` | separación / convergencia |
| `STEREO3D_HUD` | profundidad del HUD (mismas unidades que la convergencia; `0` = plano de la pantalla) |
| `STEREO3D_SWAP` | `1` invierte los ojos |
| `STEREO3D_OWN_WINDOW` | `auto` (por defecto), `1`, `0` |
| `STEREO3D_OUTPUT_RECT` | `x,y,ancho,alto` de la pantalla 3D |
| `STEREO3D_HALF` | el juego ve la mitad de ancho en pantallas 32:9: `auto`, `1`, `0` |
| `STEREO3D_FPS_LIMIT` | límite extra de fps del juego (`0` por defecto: el ritmo lo marca la pantalla) |
| `STEREO3D_IGNORE` | procesos a ignorar, p. ej. `Launcher.exe` |
| `STEREO3D_AUTO_SWITCH` | `1` (por defecto) replay → alternate si el juego no lo admite |
| `STEREO3D_REPLAY_WARMUP` | segundos de alternate antes de replay (`8`; `0` ninguno) |
| `STEREO3D_HIDE_EXT` | `1` (por defecto), `0` permite *graphics pipeline library* (menos tirones al compilar) |
| `STEREO3D_LENS` / `STEREO3D_LENS_PORT` | weave: `auto` / `0`; puerto serie del monitor |
| `STEREO3D_LOG` / `STEREO3D_LOG_LEVEL` | archivo de registro extra / `1` (por defecto), `2` detallado |

## Registros y diagnóstico

Cada juego escribe **`~/.config/stereo3d/logs/<Juego>.log`** (se sobrescribe en cada arranque).

- `swapchain (...): game WxH -> display WxH, full SBS / half SBS / WEAVE, each eye WxH`: lo que ha decidido la capa.
- `display 60.0 Hz | game pairs shown 60.0/s`: cada 5 s, la frecuencia de la pantalla y los pares estéreo por segundo.
- `game 60.0 fps -> 60.0 stereo pairs/s [replay: N frames 3D, M frames 2D]`: fotogramas que salieron en 2D en replay.
- `STALL ... Layer state: ...`: el juego dejó de presentar más de 2 s; la línea dice qué hacían la capa y cada hilo. Se liberan las señales retenidas y, si sigue parado, la capa pasa a alternate.
- `switching to alternate mode`: el juego no admite replay. Usa `mode = hybrid` (juegos a 60 fps) o `alternate` en su perfil.
- `lens: ...`: encendido de la lente en weave (puerto no encontrado / sin permiso → regla udev).
- `shader stereo OFF`: a la gráfica/driver le falta buffer device address o Int64.

Ctrl+Shift+F11 guarda `<Juego>_game.ppm`, `<Juego>_left.ppm`, `<Juego>_right.ppm` y `<Juego>_info.txt` en `~/.config/stereo3d/` (o `STEREO3D_DUMP_DIR`).

## Cómo funciona

1. **Parcheo de shaders**: cada shader de vértices/teselado/geometría desplaza la geometría según la profundidad, `x += ojo · separación · (w − convergencia)`. Lo 2D puro (`w = 1`) no se toca; los vértices del HUD reciben la profundidad del HUD.
2. **Swapchain virtual**: el juego dibuja en imágenes de la capa. En cada presentación pasan a ser un ojo (alternate), los dos ojos (replay) o los dos reconstruidos con la profundidad (depth / hybrid) de un par estéreo (triple búfer).
3. **Hilo presentador** en su propia cola Vulkan: es dueño del swapchain real (FIFO) y muestra en cada refresco el par más reciente: lado a lado (copia o escalado) o entrelazado por un compute shader (`src/shaders/weave.comp`), así el trabajo del juego nunca retrasa un refresco.

## Limitaciones conocidas

- Juegos solo OpenGL: no. La geometría de mesh shaders queda plana (`hide_mesh_shaders = 1` hace que algunos motores usen otra vía).
- Weave: posición de visión fija y calibración manual; todavía sin seguimiento ocular.
- Replay: efectos que avanzan cada vez que se dibujan pueden diferir entre ojos. Los emuladores solo funcionan en alternate / hybrid.

## Estructura

```
src/layer.cpp            capa Vulkan: swapchain virtual, replay/alternate/depth/hybrid, presentador (SBS / weave)
src/spirv_patch.cpp      parcheador SPIR-V (desplazamiento estéreo)
src/spirv_screenfix.cpp  corrección de reconstrucciones de posición en pantalla (fix_shadows)
src/shaders/             depth3d.comp (depth / hybrid), weave.comp (lenticular); gen.sh -> *_spv.h
src/lens.cpp             encendido de la lente de monitores LeiaSR (serie USB)
src/output.cpp           ventana propia en la pantalla 3D (X11)
src/config.cpp           configuración, perfiles, registros, teclas
tools/stereo3d-run       lanzador;  stereo3d-recorder  grabación de vídeo (ffmpeg)
tools/stereo3d-tool      shader-fix, lens on/off/status;  stereo3d-shader-scan  búsqueda de shaders
tools/spatial-lens       lente on/off desde el terminal;  70-spatial-lens.rules  regla udev
tools/sbs-desktop*       copia del escritorio para gafas SBS completo
tests/                   pruebas del parcheador y app de prueba Vulkan 1.3 (profundidad, HUD, back buffer tipo DXVK...)
third_party/include      cabeceras de Vulkan (Khronos) y X11 (MIT)
```
