// stereo3d - runtime configuration, per-game profiles and hotkeys
#pragma once
#include <cstdint>
#include <string>

namespace stereo3d {

// what the display gets
enum Output { kOutAuto = 0, kOutFull = 1, kOutHalf = 2, kOutWeave = 3 };

struct Config {
  float separation = 0.015f;   // per-eye shift at infinity, in NDC units (1.0 = half screen)
  float convergence = 1.5f;    // view depth (game units) that lands on the screen plane
  bool swapEyes = false;       // flip left/right
  bool stereo = true;          // false = same image to both eyes (2D)
  bool hideExtensions = true;  // hide GPL/shader-object/present-wait etc. so we see every shader
  bool patchShaders = true;    // false = only the SBS / weave output (debug)
  int logLevel = 1;            // 0 errors, 1 info, 2 verbose
  bool hotkeys = true;
  bool passthrough = false;    // launcher/helper process: layer does nothing
  int ownWindow = -1;          // -1 auto, 0 present on the game window, 1 own full-screen window
  int mode = 1;                // 1 replay (both eyes every game frame, default), 0 alternate (L/R frames),
                               // 2 depth (one game frame -> both eyes, rebuilt from the depth buffer)
                               // 3 hybrid (alternate L/R frames; each one also gives the other eye from its depth)
  float replayWarmup = 8.0f;   // replay: seconds of alternate at start (intros, shader compilation)
  float fpsLimit = 0.0f;       // extra game fps cap (pairs per second in replay). 0 = only the display sync
  bool gameSync = true;        // the game waits for the display (no frames rendered just to be dropped)
  float hudDepth = 0.0f;       // apparent depth of the HUD (same units as convergence). 0 = screen plane
  bool autoSwitch = true;      // replay -> alternate automatically when the game reads back every frame
  float zoom = 1.0f;           // 1 = whole game image in each eye; >1 zoom in
  int crop = 0;                // 0 off, -1 auto, 1 center 16:9, 2 always detect black bars
  int presentImages = 3;       // images of the real swapchain (3 = 2 frames queued, robust)
  bool testPattern = false;    // SBS: left eye white, right eye black | weave: left red, right cyan (calibration)
  bool hideMeshShaders = false;  // hide VK_EXT_mesh_shader so engines use vertex shaders (patched)
  bool shiftShadows = true;    // false: depth-only pipelines (shadow maps) get no stereo shift
  bool skipRepeats = false;    // alternate: ignore presents that only repeat the previous image (emulators)
  int recordWidth = 1920;      // recording: size of each eye in the SBS video (0 = eye size)
  int recordHeight = 1080;
  float recordFps = 60.0f;     // constant frame rate of the recording
  bool recordAtStart = false;  // start recording as soon as the game shows (STEREO3D_RECORD=1)
  int inputLayout = 0;         // 0 game (render each eye), 1 SBS input (left/right halves), 2 top-bottom input
  bool hudSeparation = false;  // depth/hybrid: rebuild the eyes from a copy of the scene taken before the HUD (experimental)
  int hudSplit = 0;            // the copy is taken after this draw into the back buffer (0 = automatic)
  bool flatMenus = false;      // hybrid: screens without a 3D scene (menus) without stereo shift
  bool dumpShaders = false;    // save every shader (SPIR-V) the game creates to ~/.cache/stereo3d/shaders/<game>
  int fixShadows = 2;          // screen-space position rebuilds corrected (Unreal shadows): 0 off, 1 on, 2 auto
  bool fixShadowsOn = true;    // runtime switch (Ctrl+Shift+Pause) to compare with / without

  // ---- output
  int output = kOutAuto;       // auto: full SBS on 32:9 displays (glasses), half SBS on the rest (3D TVs)
  int halfWidth = -1;          // -1 auto (32:9 surfaces), 0 never, 1 always: the game sees half the width

  // ---- weave (lenticular 3D monitors: Acer SpatialLabs, Samsung Odyssey 3D...)
  float weavePitch = 6.0f;     // lens width in pixels (horizontal)
  float weaveSlant = 0.3333f;  // horizontal lens shift per pixel row (pixels)
  float weaveOffset = 0.0f;    // lens phase (0..1)
  bool weaveSubpixel = true;   // each colour subpixel takes the view of its own position
  bool weaveBgr = false;       // panel subpixel order B,G,R instead of R,G,B
  float weaveSoft = 0.0f;      // blend width between the views (0 hard .. 0.25)
  int lens = -1;               // lens switching (SR+LENS on the monitor's USB serial port): -1 auto (weave), 0 never
};

Config& Cfg();
float HudFactor();            // 1 - convergence / hud_depth (0 when hud_depth = 0)
void LoadConfig();            // env + config files; call once at startup
const char* ModeName(int mode);
const char* OutputName(int output);
void SaveConfig();            // writes per-game profile (and weave.conf in weave output)
void SaveWeave();             // writes ~/.config/stereo3d/weave.conf (lens calibration of the monitor)
const std::string& GameName();
std::string ProfilePath();
std::string ConfigDir();      // ~/.config/stereo3d
std::string CacheDir();       // ~/.cache/stereo3d

void Log(int level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// Polls hotkeys (X11, Ctrl+Shift+F-keys) and config-file changes.
// Returns true if something changed.
bool PollControls();

// Frame capture (Ctrl+Shift+F11 or STEREO3D_DUMP=<frame>): returns true once per request.
bool TakeDumpRequest(uint64_t frame);
// Ctrl+Shift+F12: start / stop recording. Returns true once per key press.
bool TakeRecordToggle();
std::string DumpDir();

}  // namespace stereo3d
