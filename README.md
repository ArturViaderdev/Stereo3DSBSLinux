<a id="english"></a>

# stereo3d — real stereoscopic 3D for Linux games on SBS displays

**English** · [Español](#español)

A Vulkan layer that turns Linux games into **real geometric 3D**: each eye is a full render with its own camera, not a depth-buffer reprojection. Output is **side-by-side (SBS)** for any 3D display that accepts it:

- **Full SBS 3840x1080** (each eye 1920x1080): AR glasses in 3D mode such as **Rokid Air** (tested), XREAL, VITURE…
- **Half SBS 1920x1080** (the display stretches each half): 3D TVs, 3D monitors and projectors with an SBS input mode.

Works with:

- Windows games on Proton/Wine (DX9/10/11 through DXVK, DX12 through VKD3D‑Proton, Vulkan),
- native Linux games that use Vulkan (e.g. the Burnout Revenge port).

Not supported: OpenGL-only games, top-and-bottom / frame-packing / anaglyph output (SBS only).

## Installation

```sh
./install.sh
```

If `g++` is available it rebuilds for your system. The layer is installed in `~/.local/share/vulkan/implicit_layer.d/` (and in the Flatpak Steam path if present), plus the launcher `~/.local/bin/stereo3d-run`. The layer is **inactive** unless `ENABLE_STEREO3D=1` is set. Run `./install.sh` again after every update.

## Launch options

### Steam (Windows games on Proton)

Game properties → General → Launch options:

```
/home/YOUR_USER/.local/bin/stereo3d-run %command%
```

The launcher is the same as `ENABLE_STEREO3D=1 DXVK_CONFIG="dxgi.syncInterval=0;d3d9.presentInterval=0;dxgi.maxFrameRate=0;d3d9.maxFrameRate=0" %command%` (it disables DXVK's own vsync and frame limiter; without that there is stutter).

Variants (put the variables in front of the launcher, e.g. `STEREO3D_MODE=alternate /home/YOUR_USER/.local/bin/stereo3d-run %command%`):

| Case | Add |
|---|---|
| Games that read back from the GPU every frame (EDF5/EDF6/EDF WB2) | `STEREO3D_MODE=alternate` |
| Emulators (Azimuth…): replay corrupts the emulated state | `STEREO3D_MODE=alternate` |
| Force replay without automatic fallback | `STEREO3D_MODE=replay STEREO3D_AUTO_SWITCH=0` |
| Force / forbid the layer's own output window | `STEREO3D_OWN_WINDOW=1` / `STEREO3D_OWN_WINDOW=0` |
| Force the SBS layout | `STEREO3D_SBS=full` / `STEREO3D_SBS=half` |

Instead of the variable you can put `mode = alternate` in the game profile (`~/.config/stereo3d/<Game.exe>.conf`).

### Native Linux games (Vulkan)

```sh
~/.local/bin/stereo3d-run ./run.sh     # or: ENABLE_STEREO3D=1 ./run.sh
```

### In-game settings

| Setting | Value | Why |
|---|---|---|
| Resolution | **1920x1080** | one eye |
| Display mode | Fullscreen or borderless (best); windowed also works | see "Own output window" |
| Anti-aliasing | **FXAA/SMAA/none, NEVER TAA** | TAA, DLSS and FSR2/3 blend in another frame and you see double |
| Motion blur | Off | same reason |
| Frame generation | Off | breaks 3D |

### Display

Switch the glasses / TV to its 3D SBS mode **before** starting the game. With several monitors the layer picks the 32:9 one; otherwise set `STEREO3D_OUTPUT_RECT=x,y,width,height`.

## Compatibility

See **[COMPATIBILITY.md](COMPATIBILITY.md)** for the list of tested games. Please report your results by opening an issue with the *Game compatibility report* template.

## Modes

- **replay** (default): the game renders each frame once and the layer re-submits the same draw commands to the GPU with the other eye's camera. A game capped at 60 fps gives **60 fps per eye**, both eyes show exactly the same instant. Twice the GPU work.
  - If the game needs GPU results in the middle of a frame, that frame comes out in 2D. If more than half of the frames do, the layer **switches to alternate** by itself.
- **alternate**: one frame per eye (even = left, odd = right). The game must run at 120 fps for 60 per eye; a game capped at 60 gives 30 per eye.

Set with `mode = replay|alternate` in the game profile or `STEREO3D_MODE=`.

## Own output window

If the game window already covers the 3D display, the layer presents there. If it is smaller (a windowed game, or a game that stays at 1920x1080 on a 3840x1080 desktop), the layer opens **its own full-screen window** on the 3D display (input-transparent, does not steal focus) and presents there. The log tells which one is used: `game window WxH -> presenting on ...`. Force with `own_window = 1|0` (`auto` by default) or `STEREO3D_OWN_WINDOW=1|0`.

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

Requires an **X11** session (not Wayland). If the right half stays black or frozen, disable desktop compositing (KDE: Alt+Shift+F12; XFCE: Settings → Window Manager Tweaks → Compositor).

## In-game hotkeys (Ctrl + Shift + …)

| Key | Action |
|---|---|
| F3 / F4 | less / more **separation** (overall depth) |
| F5 / F6 | less / more **convergence** (which distance sits on the screen plane) |
| F1 / F2 | bring the **HUD** closer / farther |
| F7 | swap eyes |
| F8 | 3D on/off |
| F9 | save this game's profile |
| F10 | frame pacing on/off (alternate mode) |
| F11 | capture the game image and the SBS output (diagnostics) |
| PgUp / PgDn | zoom in / out |
| Home | zoom = 1 (whole image) |

It is **Ctrl + Shift** (not Alt + Shift: Alt+Shift+F-key may trigger the game's or the desktop's own functions).

**HUD:** by default text and markers sit on the screen plane (the convergence distance). If the scene comes out in front of them, the text looks "behind": bring it forward with Ctrl+Shift+F1 until it is in front of the car or character. Saved in the profile as `hud_depth`.

**How to tune:** start with low separation. Raise convergence until the weapon, car or character is near the screen plane. Then raise separation until the background has depth without straining your eyes. Save with F9.

## Configuration

- `~/.config/stereo3d/default.conf`: all games (see `default.conf.example`).
- `~/.config/stereo3d/<Game.exe>.conf`: per-game profile (F9 creates it). Edits are reloaded live.
- Environment variables (highest priority):

| Variable | Values |
|---|---|
| `STEREO3D_MODE` | `replay` (default), `alternate` |
| `STEREO3D_SEP` / `STEREO3D_CONV` | separation / convergence |
| `STEREO3D_HUD` | HUD depth (same units as convergence; `0` = screen plane) |
| `STEREO3D_SWAP` | `1` swaps eyes |
| `STEREO3D_SBS` | `auto` (default: full on 32:9 displays, half otherwise), `full`, `half` |
| `STEREO3D_FPS_LIMIT` | `120` (default; in replay the game is limited to half), `0` no limit |
| `STEREO3D_OWN_WINDOW` | `auto` (default), `1` always, `0` never |
| `STEREO3D_OUTPUT_RECT` | `x,y,width,height` of the 3D display |
| `STEREO3D_IGNORE` | processes to ignore, e.g. `Launcher.exe` |
| `STEREO3D_AUTO_SWITCH` | `1` (default) replay → alternate when the game does not support it, `0` never |
| `STEREO3D_ZOOM` | `1.0` |
| `STEREO3D_CROP` | `0` (default), `auto`, `content`, `center` |
| `STEREO3D_HIDE_EXT` | `1` (default), `0` allows *graphics pipeline library* (less compile stutter). Presentation extensions (*present wait/id*, *swapchain maintenance*, *low latency*) and shader identifiers / shader objects are always hidden |
| `STEREO3D_PRESENT_MODE` | `fifo` (default), `mailbox`, `immediate` |
| `STEREO3D_LOG` | path of an extra log file |
| `STEREO3D_LOG_LEVEL` | `1` (default), `2` verbose |

## Logs and diagnostics

Each game writes **`~/.config/stereo3d/logs/<Game>.log`** (overwritten on every start). Every 5 s it logs game fps, fps per eye, 3D/2D frames and patched shaders.

- **`STALL ... Layer state: ...`**: the game stopped presenting for more than 2 s. The line says what the layer was doing and what every thread is waiting on; held signals are released and, if still stuck, the layer switches to alternate.
- **`switching to alternate mode`**: the game does not support replay. Put `mode = alternate` in its profile to start that way.
- **`shader stereo OFF`**: the GPU/driver lacks buffer device address or Int64.
- **Stutter in the first minutes**: the layer hides *graphics pipeline library* to see every shader. Try `STEREO3D_HIDE_EXT=0`.
- **Shadows or lights slightly off in one eye**: classic limitation of automatic stereo. Lower convergence hides it.

Ctrl+Shift+F11 saves `<Game>_juego.ppm` (what the game renders), `<Game>_sbs.ppm` (what goes to the display) and `<Game>_info.txt` in `~/.config/stereo3d/` (or `STEREO3D_DUMP_DIR`).

Quick test without a game: `ENABLE_STEREO3D=1 STEREO3D_LEGACY=1 STEREO3D_SEP=0.04 STEREO3D_CONV=5 vkcube`

## How it works

1. **Shader patching.** Every vertex, tessellation or geometry shader is patched to shift geometry by depth: `x += eye · separation · (w − convergence)` (like NVIDIA 3D Vision automatic mode). Pure 2D (`w = 1`: full-screen passes) is untouched; HUD vertices get the configurable HUD depth.
2. **Virtual swapchain.** The game renders into 1920x1080 images owned by the layer; the layer composes each eye into its half of the real swapchain.
3. **Replay:** at present time the layer copies the left eye, switches the camera to the right eye and re-submits the frame's command buffers. The game's sync signals are held back until the second pass is done.
4. **Alternate:** even frames left eye, odd frames right eye, with the left frame paced between two refreshes.

## Known limitations

- OpenGL games (no Vulkan): no.
- Mesh shaders (some modern DX12 games): that geometry would be flat.
- Replay: GPU effects that advance every time they are drawn (some particles) may run twice as fast or differ between eyes. Emulators only work in alternate.

## Layout

```
src/layer.cpp        Vulkan layer (virtual swapchain, replay, alternate, SBS composition, watchdog)
src/spirv_patch.cpp  SPIR-V patcher (stereo shift injection)
src/output.cpp       own output window on the 3D display (X11)
src/config.cpp       configuration, profiles, logs, hotkeys
tools/stereo3d-run   launcher
tools/sbs-desktop*   desktop mirror for full-SBS glasses
tests/               patcher tests and a Vulkan 1.3 test app
third_party/include  Vulkan (Khronos) and X11 (MIT) headers
```

---

<a id="español"></a>

# stereo3d — 3D estereoscópico real para juegos de Linux en pantallas SBS

[English](#english) · **Español**

Es una capa Vulkan que convierte juegos de Linux en **3D geométrico real**: cada ojo es un render completo con su propia cámara, sin reproyección de profundidad. La salida es **side‑by‑side (SBS)** para cualquier pantalla 3D que lo acepte:

- **SBS completo 3840x1080** (cada ojo 1920x1080): gafas AR en modo 3D como las **Rokid Air** (probadas), XREAL, VITURE…
- **Half SBS 1920x1080** (la pantalla estira cada mitad): televisores, monitores y proyectores 3D con entrada SBS.

Funciona con:

- juegos de Windows con Proton/Wine (DX9/10/11 vía DXVK, DX12 vía VKD3D‑Proton, Vulkan),
- juegos nativos de Linux que usen Vulkan (p. ej. el port de Burnout Revenge).

No compatible: juegos solo OpenGL, salida arriba/abajo, *frame packing* o anaglifo (solo SBS).

## Instalación

```sh
./install.sh
```

Si tienes `g++`, recompila para tu sistema. La capa se instala en `~/.local/share/vulkan/implicit_layer.d/` (y en la ruta de Steam Flatpak si existe), junto con el lanzador `~/.local/bin/stereo3d-run`. Está **inactiva** salvo que pongas `ENABLE_STEREO3D=1`. Después de cada actualización hay que volver a ejecutar `./install.sh`.

## Parámetros de lanzamiento

### Steam (juegos de Windows con Proton)

Propiedades del juego → General → Opciones de lanzamiento:

```
/home/TU_USUARIO/.local/bin/stereo3d-run %command%
```

El lanzador equivale a `ENABLE_STEREO3D=1 DXVK_CONFIG="dxgi.syncInterval=0;d3d9.presentInterval=0;dxgi.maxFrameRate=0;d3d9.maxFrameRate=0" %command%` (desactiva el vsync y el limitador propios de DXVK; sin eso hay tirones).

Variantes (variables delante del lanzador, p. ej. `STEREO3D_MODE=alternate /home/TU_USUARIO/.local/bin/stereo3d-run %command%`):

| Caso | Añadir |
|---|---|
| Juegos que leen de la GPU en cada fotograma (EDF5/EDF6/EDF WB2) | `STEREO3D_MODE=alternate` |
| Emuladores (Azimuth…): replay corrompe el estado emulado | `STEREO3D_MODE=alternate` |
| Forzar replay sin cambio automático | `STEREO3D_MODE=replay STEREO3D_AUTO_SWITCH=0` |
| Forzar / impedir la ventana propia | `STEREO3D_OWN_WINDOW=1` / `STEREO3D_OWN_WINDOW=0` |
| Forzar el formato SBS | `STEREO3D_SBS=full` / `STEREO3D_SBS=half` |

En vez de la variable, puedes poner `mode = alternate` en el perfil del juego (`~/.config/stereo3d/<Juego.exe>.conf`).

### Juegos nativos de Linux (Vulkan)

```sh
~/.local/bin/stereo3d-run ./run.sh     # o: ENABLE_STEREO3D=1 ./run.sh
```

### Ajustes dentro del juego

| Ajuste | Valor | Por qué |
|---|---|---|
| Resolución | **1920x1080** | es la de cada ojo |
| Modo | Pantalla completa o sin bordes (mejor); ventana también vale | ver "Ventana propia" |
| Antialiasing | **FXAA/SMAA/ninguno, NUNCA TAA** | TAA, DLSS y FSR2/3 mezclan imágenes de otro fotograma y se ve doble |
| Desenfoque de movimiento | Desactivado | mismo motivo |
| Generación de fotogramas | Desactivada | rompe el 3D |

### Pantalla

Pon las gafas o la tele en su modo 3D SBS **antes** de abrir el juego. Con varios monitores, la capa elige el de proporción 32:9; si no, indícalo con `STEREO3D_OUTPUT_RECT=x,y,ancho,alto`.

## Compatibilidad

La lista de juegos probados está en **[COMPATIBILITY.md](COMPATIBILITY.md)**. Para añadir resultados, abre una *issue* con la plantilla *Game compatibility report*.

## Modos

- **replay** (por defecto): el juego hace cada fotograma una vez y la capa reenvía a la GPU los mismos comandos de dibujo con la cámara del otro ojo. Un juego limitado a 60 fps da **60 fps por ojo**, y los dos ojos ven exactamente el mismo instante. La GPU trabaja el doble.
  - Si el juego pide resultados a la GPU a mitad de fotograma, ese fotograma sale en 2D. Si pasa en más de la mitad, la capa **cambia sola a alternate**.
- **alternate**: un fotograma para cada ojo (pares izquierdo, impares derecho). El juego tiene que ir a 120 fps para dar 60 por ojo; un juego limitado a 60 da 30 por ojo.

Se fija con `mode = replay|alternate` en el perfil del juego o con `STEREO3D_MODE=`.

## Ventana propia

Si la ventana del juego ya ocupa la pantalla 3D, la capa presenta en ella. Si es más pequeña (un juego en ventana, o que se queda a 1920x1080 en un escritorio de 3840x1080), la capa abre **su propia ventana** a pantalla completa en la pantalla 3D (transparente al ratón, sin quitar el foco) y presenta allí. El registro dice cuál usa: `game window WxH -> presenting on ...`. Se fuerza con `own_window = 1|0` (`auto` por defecto) o `STEREO3D_OWN_WINDOW=1|0`.

Los lanzadores y procesos auxiliares (Ubisoft Connect, Epic, EA, Steam, explorer.exe…) se ignoran automáticamente. Para ignorar otro: `STEREO3D_IGNORE=Proceso.exe`.

## Escritorio en la pantalla 3D (sin cambiar de monitor)

`tools/sbs-desktop.sh` deja el escritorio usable con gafas SBS completo en modo 3D:

```sh
tools/sbs-desktop.sh            # detecta la salida a 3840x1080; o: tools/sbs-desktop.sh HDMI-0
```

- Divide la pantalla de las gafas en dos monitores lógicos de 1920x1080: ventanas, paneles y maximizados van a la mitad izquierda.
- Duplica la mitad izquierda en la derecha (con el puntero), así cada ojo ve el escritorio completo en 2D.
- Cuando un juego con stereo3d está en pantalla, el espejo se aparta solo y vuelve al cerrar el juego.
- Ctrl+C en la terminal lo deshace todo.

Requisitos: sesión **X11** (no Wayland). Si la mitad derecha sale negra o congelada, desactiva la composición del escritorio (KDE: Alt+Shift+F12; XFCE: Ajustes → Retoques del gestor de ventanas → Compositor).

## Teclas en juego (Ctrl + Shift + …)

| Tecla | Acción |
|---|---|
| F3 / F4 | menos / más **separación** (profundidad total) |
| F5 / F6 | menos / más **convergencia** (qué distancia queda en el plano de la pantalla) |
| F1 / F2 | acercar / alejar el **HUD** |
| F7 | invertir ojos |
| F8 | 3D on/off |
| F9 | guardar perfil de este juego |
| F10 | reparto temporal on/off (modo alternate) |
| F11 | capturar la imagen del juego y la SBS (diagnóstico) |
| RePág / AvPág | acercar / alejar la imagen (zoom) |
| Inicio | zoom = 1 (imagen completa) |

Son **Ctrl + Shift** (no Alt + Shift: Alt+Shift+F-tecla puede activar funciones del propio juego o del escritorio).

**HUD:** por defecto los textos y marcadores quedan en el plano de la pantalla (la profundidad de la convergencia). Si la escena sale por delante de ellos, los textos parecen "al fondo": tráelos hacia delante con Ctrl+Shift+F1 hasta que queden por delante del coche o del personaje. Se guarda en el perfil como `hud_depth`.

**Cómo ajustarlo:** empieza con separación baja. Sube la convergencia hasta que el arma, el coche o el personaje queden cerca del plano de la pantalla. Después sube la separación hasta que el fondo tenga profundidad sin forzar la vista. Guarda con F9.

## Configuración

- `~/.config/stereo3d/default.conf`: para todos los juegos (mira `default.conf.example`).
- `~/.config/stereo3d/<Juego.exe>.conf`: perfil de cada juego (F9 lo crea). Si lo editas, se recarga en vivo.
- Variables de entorno (prioridad máxima):

| Variable | Valores |
|---|---|
| `STEREO3D_MODE` | `replay` (defecto), `alternate` |
| `STEREO3D_SEP` / `STEREO3D_CONV` | separación / convergencia |
| `STEREO3D_HUD` | profundidad del HUD (mismas unidades que la convergencia; `0` = plano de la pantalla) |
| `STEREO3D_SWAP` | `1` invierte ojos |
| `STEREO3D_SBS` | `auto` (defecto: completo en pantallas 32:9, half en el resto), `full`, `half` |
| `STEREO3D_FPS_LIMIT` | `120` (defecto; en replay el juego se limita a la mitad), `0` sin límite |
| `STEREO3D_OWN_WINDOW` | `auto` (defecto), `1` siempre, `0` nunca |
| `STEREO3D_OUTPUT_RECT` | `x,y,ancho,alto` de la pantalla 3D |
| `STEREO3D_IGNORE` | procesos a ignorar, p. ej. `Launcher.exe` |
| `STEREO3D_AUTO_SWITCH` | `1` (defecto) pasa de replay a alternate si el juego no lo admite, `0` nunca |
| `STEREO3D_ZOOM` | `1.0` |
| `STEREO3D_CROP` | `0` (defecto), `auto`, `content`, `center` |
| `STEREO3D_HIDE_EXT` | `1` (defecto), `0` deja usar *graphics pipeline library* (menos tirones de compilación). Las extensiones de presentación (*present wait/id*, *swapchain maintenance*, *low latency*) y los identificadores de shader / *shader objects* se ocultan siempre |
| `STEREO3D_PRESENT_MODE` | `fifo` (defecto), `mailbox`, `immediate` |
| `STEREO3D_LOG` | ruta de un registro adicional |
| `STEREO3D_LOG_LEVEL` | `1` (defecto), `2` detallado |

## Registros y diagnóstico

Cada juego escribe **`~/.config/stereo3d/logs/<Juego>.log`** (se sobrescribe en cada arranque). Cada 5 s anota los fps del juego, los fps por ojo, los fotogramas en 3D/2D y los shaders parcheados.

- **`STALL ... Layer state: ...`**: el juego dejó de presentar más de 2 s. La línea dice qué hacía la capa y qué espera cada hilo; la capa suelta lo que tenga retenido y, si sigue bloqueado, cambia a alternate.
- **`switching to alternate mode`**: el juego no admite replay. Pon `mode = alternate` en su perfil para que empiece así.
- **`shader stereo OFF`**: la GPU o el driver no tiene buffer device address o Int64.
- **Tirones los primeros minutos**: la capa oculta *graphics pipeline library* para ver todos los shaders. Prueba `STEREO3D_HIDE_EXT=0`.
- **Sombras o luces algo desplazadas en un ojo**: limitación clásica del estéreo automático. Una convergencia más baja lo disimula.

Ctrl+Shift+F11 guarda `<Juego>_juego.ppm` (lo que entrega el juego), `<Juego>_sbs.ppm` (lo que va a la pantalla) e `<Juego>_info.txt` en `~/.config/stereo3d/` (o en `STEREO3D_DUMP_DIR`).

Prueba rápida sin juego: `ENABLE_STEREO3D=1 STEREO3D_LEGACY=1 STEREO3D_SEP=0.04 STEREO3D_CONV=5 vkcube`

## Cómo funciona

1. **Parcheo de shaders.** Cada vertex, teselado o geometry shader se modifica para desplazar la geometría según su profundidad: `x += ojo · separación · (w − convergencia)` (como NVIDIA 3D Vision automático). El 2D puro (`w = 1`: pasadas a pantalla completa) no se toca; los vértices del HUD reciben la profundidad de HUD configurable.
2. **Swapchain virtual.** El juego dibuja en imágenes 1920x1080 de la capa; la capa compone cada ojo en su mitad del swapchain real.
3. **Replay:** al presentar, la capa copia el ojo izquierdo, cambia la cámara al derecho y reenvía los mismos comandos del fotograma. Las señales de sincronización del juego se retienen hasta terminar la segunda pasada.
4. **Alternate:** fotogramas pares para el ojo izquierdo e impares para el derecho, con el izquierdo colocado entre dos refrescos.

## Limitaciones conocidas

- Juegos OpenGL (sin Vulkan): no.
- Mesh shaders (algunos DX12 modernos): esa geometría saldría plana.
- Replay: efectos calculados en la GPU que avanzan cada vez que se dibuja (algunas partículas) pueden ir el doble de rápido o diferir entre ojos. Los emuladores solo funcionan en alternate.

## Estructura

```
src/layer.cpp        capa Vulkan (swapchain virtual, replay, alternancia, composición SBS, vigilante)
src/spirv_patch.cpp  parcheador SPIR-V (inyección del desplazamiento estéreo)
src/output.cpp       ventana propia en la pantalla 3D (X11)
src/config.cpp       configuración, perfiles, registros, teclas
tools/stereo3d-run   lanzador
tools/sbs-desktop*   espejo del escritorio para gafas SBS completo
tests/               pruebas del parcheador y app Vulkan 1.3 de prueba
third_party/include  cabeceras Vulkan (Khronos) y X11 (MIT)
```
