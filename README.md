# stereo3d: 3D estereoscópico real en las Rokid Air (SBS 3840x1080)

Es una capa Vulkan que convierte juegos de Linux en **3D geométrico real**: cada ojo es un render completo con su propia cámara, sin reproyección de profundidad. Funciona con:

- juegos de Windows con Proton/Wine (DX9/10/11 vía DXVK, DX12 vía VKD3D‑Proton, Vulkan),
- juegos nativos de Linux que usen Vulkan (p. ej. el port de Burnout Revenge).

La salida es **Side‑by‑Side 3840x1080 a 60 Hz**, con una imagen distinta para cada ojo.

## Instalación

```sh
./install.sh
```

Si tienes `g++`, recompila para tu sistema. La capa se instala en `~/.local/share/vulkan/implicit_layer.d/` (y en la ruta de Steam Flatpak si existe). Está **inactiva** salvo que pongas `ENABLE_STEREO3D=1`. Después de cada actualización hay que volver a ejecutar `./install.sh`.

## Parámetros de lanzamiento

### Steam (juegos de Windows con Proton)

`install.sh` instala un lanzador en `~/.local/bin/stereo3d-run` que activa la capa y desactiva el vsync y el limitador propios de DXVK (sin eso hay tirones). Propiedades del juego → General → Opciones de lanzamiento:

```
/home/TU_USUARIO/.local/bin/stereo3d-run %command%
```

Es lo mismo que escribir `ENABLE_STEREO3D=1 DXVK_CONFIG="dxgi.syncInterval=0;d3d9.presentInterval=0;dxgi.maxFrameRate=0;d3d9.maxFrameRate=0" %command%`.

Variantes (variables delante del lanzador, p. ej. `STEREO3D_MODE=alternate /home/TU_USUARIO/.local/bin/stereo3d-run %command%`):

| Caso | Añadir |
|---|---|
| Juegos que leen de la GPU en cada fotograma (EDF5/EDF6/EDF WB2) | `STEREO3D_MODE=alternate` |
| Emuladores (Azimuth…): replay corrompe el estado emulado | `STEREO3D_MODE=alternate` |
| Forzar replay sin cambio automático a alternate | `STEREO3D_MODE=replay STEREO3D_AUTO_SWITCH=0` |
| Forzar / impedir la ventana propia | `STEREO3D_OWN_WINDOW=1` / `STEREO3D_OWN_WINDOW=0` |

En vez de la variable, puedes poner `mode = alternate` en el perfil del juego (`~/.config/stereo3d/<Juego.exe>.conf`) y usar el lanzador sin nada más.

### Compatibilidad probada

| Juego | Modo | Resultado |
|---|---|---|
| Burnout Revenge (port nativo, Vulkan) | replay | 3D, 60 fps por ojo |
| Trials Rising | replay | 3D |
| EDF6 | alternate | 3D, 30 fps por ojo (limitado a 60 y lee de la GPU en cada fotograma) |
| Azimuth (emulador Xbox One) | alternate | replay se cuelga (límite del método con emuladores) |

### Juegos nativos de Linux (Vulkan)

Desde una terminal, en la carpeta del juego:

```sh
~/.local/bin/stereo3d-run ./run.sh   # o: ENABLE_STEREO3D=1 ./run.sh
```

### Ajustes dentro del juego

| Ajuste | Valor | Por qué |
|---|---|---|
| Resolución | **1920x1080** | es la de cada ojo |
| Modo | Pantalla completa o sin bordes (mejor); ventana también vale | ver "Ventana propia" |
| Antialiasing | **FXAA/SMAA/ninguno, NUNCA TAA** | TAA, DLSS y FSR2/3 mezclan imágenes de otro fotograma y se ve doble |
| Desenfoque de movimiento | Desactivado | mismo motivo |
| Generación de fotogramas | Desactivada | rompe el 3D |

### Gafas

Pon las Rokid en modo 3D (3840x1080) **antes** de abrir el juego. Si tienes varios monitores, la capa busca el de proporción 32:9; si no lo encuentra, indícalo con `STEREO3D_OUTPUT_RECT=x,y,ancho,alto`.
Probado en las gafas rockid air. Para full 3d pulsar el botón de las gafas un segundo y después ejecutar el 3840x1080hdmioff.sh.

## Modos

- **replay** (por defecto): el juego hace cada fotograma una vez y la capa reenvía a la GPU los mismos comandos de dibujo con la cámara del otro ojo. Un juego limitado a 60 fps da **60 fps por ojo**, y los dos ojos ven exactamente el mismo instante. La GPU trabaja el doble.
  - Si el juego pide resultados a la GPU a mitad de fotograma, ese fotograma sale en 2D. Si pasa en casi todos durante 10 s, la capa **cambia sola a alternate**.
- **alternate**: un fotograma para cada ojo (pares izquierdo, impares derecho). El juego tiene que ir a 120 fps para dar 60 por ojo; un juego limitado a 60 da 30 por ojo.

Se fija con `mode = replay|alternate` en el perfil del juego o con `STEREO3D_MODE=`.

## Ventana propia en las gafas

Si la ventana del juego ya ocupa las gafas (3840x1080), la capa presenta en ella. Si es más pequeña (un juego en ventana, o que se queda a 1920x1080 como Trials Rising), la capa abre **su propia ventana** a pantalla completa en el monitor 32:9, transparente al ratón y sin quitar el foco al juego, y presenta allí. El registro dice cuál usa: `game window WxH -> presenting on ...`. Se fuerza con `own_window = 1|0` (`auto` por defecto) o `STEREO3D_OWN_WINDOW=1|0`.

Los lanzadores y procesos auxiliares (Ubisoft Connect, Epic, EA, Steam, explorer.exe…) se ignoran automáticamente. Para ignorar otro: `STEREO3D_IGNORE=Proceso.exe`.

## Escritorio en las gafas (sin cambiar de monitor)

`tools/sbs-desktop.sh` deja el escritorio usable con las gafas en modo 3D:

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
| F7 | invertir ojos |
| F8 | 3D on/off |
| F9 | guardar perfil de este juego |
| F10 | reparto temporal on/off (modo alternate) |
| F11 | capturar la imagen del juego y la SBS (diagnóstico) |
| F1 / F2 | acercar / alejar el **HUD** (textos, marcadores) |
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
| `STEREO3D_SWAP` | `1` invierte ojos |
| `STEREO3D_FPS_LIMIT` | `120` (defecto; en replay el juego se limita a la mitad), `0` sin límite |
| `STEREO3D_OWN_WINDOW` | `auto` (defecto: solo si la ventana del juego no ocupa las gafas), `1` siempre, `0` nunca |
| `STEREO3D_OUTPUT_RECT` | `x,y,ancho,alto` del monitor de las gafas |
| `STEREO3D_IGNORE` | procesos a ignorar, p. ej. `Launcher.exe` |
| `STEREO3D_ZOOM` | `1.0` |
| `STEREO3D_HUD` | profundidad del HUD (mismas unidades que la convergencia; `0` = plano de la pantalla) |
| `STEREO3D_AUTO_SWITCH` | `1` (defecto) pasa de replay a alternate si el juego no lo admite, `0` nunca |
| `STEREO3D_CROP` | `0` (defecto), `auto`, `content`, `center` |
| `STEREO3D_HIDE_EXT` | `1` (defecto), `0` deja usar *graphics pipeline library* (menos tirones de compilación). Las extensiones de presentación (*present wait*, *present id*, *swapchain maintenance*, *low latency*) se ocultan siempre: con el swapchain estéreo nunca terminarían |
| `STEREO3D_PRESENT_MODE` | `fifo` (defecto), `mailbox`, `immediate` |
| `STEREO3D_LOG` | ruta de un registro adicional |
| `STEREO3D_LOG_LEVEL` | `1` (defecto), `2` detallado |

## Registros y diagnóstico

Cada juego escribe su registro en **`~/.config/stereo3d/logs/<Juego>.log`** (se sobrescribe en cada arranque). Cada 5 s anota los fps del juego, los fps por ojo, cuántos fotogramas salen en 3D/2D y los shaders parcheados.

- **`STALL ... Layer state: ...`**: el juego dejó de presentar más de 2 s. La línea dice en qué paso estaba la capa; la capa suelta lo que tenga retenido y, si sigue bloqueado, cambia a alternate.
- **`switching to alternate mode`**: el juego no permite replay; ahora va a la mitad de fps por ojo. Pon `mode = alternate` en su perfil para que empiece así.
- **`shader stereo OFF`**: la GPU o el driver no tiene buffer device address o Int64.
- **Tirones los primeros minutos**: la capa desactiva *graphics pipeline library* para ver todos los shaders. Prueba `STEREO3D_HIDE_EXT=0`.
- **Sombras o luces algo desplazadas en un ojo**: limitación clásica del estéreo automático. Una convergencia más baja lo disimula.

### Capturas

Ctrl+Shift+F11 guarda `<Juego>_juego.ppm` (lo que entrega el juego), `<Juego>_sbs.ppm` (lo que va a las gafas) e `<Juego>_info.txt` en `~/.config/stereo3d/` (o en `STEREO3D_DUMP_DIR`). `STEREO3D_DUMP=600` captura sola en el fotograma 600.

### Comprobar sin juego

```sh
ENABLE_STEREO3D=1 STEREO3D_LEGACY=1 STEREO3D_SEP=0.04 STEREO3D_CONV=5 vkcube
```

## Cómo funciona

1. **Parcheo de shaders.** Cada vertex, teselado o geometry shader se modifica para desplazar la geometría según su profundidad: `x += ojo · separación · (w − convergencia)` (como NVIDIA 3D Vision automático). Lo que se dibuja en 2D puro (HUD, pasadas a pantalla completa, `w = 1`) no se toca.
2. **Swapchain virtual.** El juego dibuja en imágenes 1920x1080 de la capa; la capa compone cada ojo en su mitad de la imagen 3840x1080 de su propia ventana.
3. **Replay:** al presentar, la capa copia el ojo izquierdo, cambia la cámara al derecho y reenvía los mismos comandos del fotograma. Las señales de sincronización del juego se retienen hasta terminar la segunda pasada.
4. **Alternate:** fotogramas pares para el ojo izquierdo e impares para el derecho, con el izquierdo colocado entre dos refrescos.

## Limitaciones conocidas

- Juegos OpenGL (sin Vulkan): no.
- Mesh shaders (algunos DX12 modernos): esa geometría saldría plana.
- El HUD queda en el plano de la pantalla.
- Replay: efectos calculados en la GPU que avanzan cada vez que se dibuja (algunas partículas) pueden ir el doble de rápido o diferir entre ojos.
- Emuladores (Azimuth, etc.): experimental.

## Estructura

```
src/layer.cpp        capa Vulkan (swapchain virtual, replay, alternancia, composición SBS, vigilante)
src/spirv_patch.cpp  parcheador SPIR-V (inyección del desplazamiento estéreo)
src/output.cpp       ventana propia en las gafas (X11)
src/config.cpp       configuración, perfiles, registros, teclas
tools/sbs-desktop*   espejo del escritorio para las gafas en modo 3D
tests/               pruebas del parcheador y app Vulkan 1.3 de prueba
third_party/include  cabeceras Vulkan (Khronos) y X11 (MIT)
```
