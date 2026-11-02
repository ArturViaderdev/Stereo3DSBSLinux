// stereo3d - runtime configuration, per-game profiles and hotkeys
#pragma once
#include <cstdint>
#include <string>

namespace stereo3d {

struct Config {
  float separation = 0.015f;   // per-eye shift at infinity, in NDC units (1.0 = half screen)
  float convergence = 1.5f;    // view depth (game units) that lands on the screen plane
  bool swapEyes = false;       // flip left/right
  bool stereo = true;          // false = same image to both eyes (2D, still SBS)
  int presentMode = 2;         // real swapchain present mode (VkPresentModeKHR, 2 = FIFO/vsync)
  bool pace = true;            // evenly space left/right frames in time
  int halfWidth = -1;          // -1 auto (32:9 surfaces), 0 never, 1 always: report half width to game
  bool hideExtensions = true;  // hide GPL/shader-object/present-wait etc. so we see every shader
  bool patchShaders = true;    // false = only SBS composition (debug)
  int logLevel = 1;            // 0 errors, 1 info, 2 verbose
  bool hotkeys = true;
  bool passthrough = false;    // launcher/helper process: layer does nothing
  int ownWindow = -1;          // -1 auto (only if the game window is not 32:9), 0 never, 1 always
  int mode = 1;                // 1 replay (both eyes every game frame, default), 0 alternate (L/R frames, game at 120)
  float fpsLimit = 120.0f;     // max game fps (2 x 60 Hz). 0 = no limit
  float hudDepth = 0.0f;       // apparent depth of the HUD (same units as convergence). 0 = screen plane
  bool autoSwitch = true;      // replay -> alternate automatically when the game reads back every frame
  float zoom = 1.0f;           // 1 = whole game image in each eye; >1 zoom in (fills the eye)
  int crop = 0;                // 0 off (default), -1 auto (detect black bars if the game image is too wide), 1 center 16:9, 2 always detect
};

Config& Cfg();
float HudFactor();            // 1 - convergence / hud_depth (0 when hud_depth = 0)
void LoadConfig();            // env + config files; call once at startup
void SaveConfig();            // writes per-game profile
const std::string& GameName();
std::string ProfilePath();

void Log(int level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// Polls hotkeys (X11, Ctrl+Shift+F-keys) and config-file changes.
// Returns true if something changed.
bool PollControls();

// Frame capture (Ctrl+Shift+F11 or STEREO3D_DUMP=<frame>): returns true once per request.
bool TakeDumpRequest(uint64_t frame);
std::string DumpDir();

}  // namespace stereo3d
