// stereo3d - runtime configuration, per-game profiles and hotkeys
#include "config.h"

#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace stereo3d {

static Config g_cfg;
static std::string g_game;
static std::string g_profile;
static time_t g_profileMtime = 0;
static FILE* g_logFile = nullptr;
static std::mutex g_logMutex;

Config& Cfg() { return g_cfg; }
float HudFactor() {
  if (g_cfg.hudDepth <= 0.0f) return 0.0f;
  return 1.0f - g_cfg.convergence / g_cfg.hudDepth;
}
const std::string& GameName() { return g_game; }
std::string ProfilePath() { return g_profile; }

void Log(int level, const char* fmt, ...) {
  if (level > g_cfg.logLevel) return;
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  std::lock_guard<std::mutex> l(g_logMutex);
  fprintf(stderr, "[stereo3d] %s\n", buf);
  if (g_logFile) {
    fprintf(g_logFile, "%s\n", buf);
    fflush(g_logFile);
  }
}

// --- small parsing helpers (avoid strtol: glibc 2.38 C23 symbol versioning) -------
static bool ParseFloat(const char* s, float& out) {
  if (!s || !*s) return false;
  char* end = nullptr;
  float v = strtof(s, &end);
  if (end == s) return false;
  out = v;
  return true;
}
static bool ParseBool(const char* s, bool& out) {
  if (!s) return false;
  if (!strcmp(s, "1") || !strcasecmp(s, "true") || !strcasecmp(s, "on") || !strcasecmp(s, "yes")) {
    out = true; return true;
  }
  if (!strcmp(s, "0") || !strcasecmp(s, "false") || !strcasecmp(s, "off") || !strcasecmp(s, "no")) {
    out = false; return true;
  }
  return false;
}
static int ParsePresentMode(const char* s, int def) {
  if (!s) return def;
  if (!strcasecmp(s, "fifo") || !strcasecmp(s, "vsync")) return 2;
  if (!strcasecmp(s, "mailbox")) return 1;
  if (!strcasecmp(s, "immediate")) return 0;
  if (!strcasecmp(s, "fifo_relaxed")) return 3;
  return def;
}
static const char* PresentModeName(int m) {
  switch (m) { case 0: return "immediate"; case 1: return "mailbox"; case 3: return "fifo_relaxed"; default: return "fifo"; }
}

static void ApplyKey(const char* key, const char* val) {
  float f;
  bool b;
  if (!strcmp(key, "separation") && ParseFloat(val, f)) g_cfg.separation = f;
  else if (!strcmp(key, "convergence") && ParseFloat(val, f)) g_cfg.convergence = f;
  else if (!strcmp(key, "swap_eyes") && ParseBool(val, b)) g_cfg.swapEyes = b;
  else if (!strcmp(key, "stereo") && ParseBool(val, b)) g_cfg.stereo = b;
  else if (!strcmp(key, "present_mode")) g_cfg.presentMode = ParsePresentMode(val, g_cfg.presentMode);
  else if (!strcmp(key, "pace") && ParseBool(val, b)) g_cfg.pace = b;
  else if (!strcmp(key, "zoom") && ParseFloat(val, f)) g_cfg.zoom = f;
  else if (!strcmp(key, "hud_depth") && ParseFloat(val, f)) g_cfg.hudDepth = f;
  else if (!strcmp(key, "auto_switch") && ParseBool(val, b)) g_cfg.autoSwitch = b;
  else if (!strcmp(key, "fps_limit") && ParseFloat(val, f)) g_cfg.fpsLimit = f;
  else if (!strcmp(key, "own_window")) {
    if (!strcasecmp(val, "auto")) g_cfg.ownWindow = -1;
    else if (ParseBool(val, b)) g_cfg.ownWindow = b ? 1 : 0;
  }
  else if (!strcmp(key, "mode")) g_cfg.mode = !strcasecmp(val, "replay") ? 1 : 0;
  else if (!strcmp(key, "half_width")) {
    if (!strcasecmp(val, "auto")) g_cfg.halfWidth = -1;
    else if (ParseBool(val, b)) g_cfg.halfWidth = b ? 1 : 0;
  }
  else if (!strcmp(key, "crop")) {
    if (!strcasecmp(val, "auto")) g_cfg.crop = -1;
    else if (!strcasecmp(val, "center")) g_cfg.crop = 1;
    else if (!strcasecmp(val, "content")) g_cfg.crop = 2;
    else if (ParseBool(val, b)) g_cfg.crop = b ? 1 : 0;
  }
  else if (!strcmp(key, "hide_extensions") && ParseBool(val, b)) g_cfg.hideExtensions = b;
  else if (!strcmp(key, "patch_shaders") && ParseBool(val, b)) g_cfg.patchShaders = b;
  else if (!strcmp(key, "log_level") && ParseFloat(val, f)) g_cfg.logLevel = int(f);
  else if (!strcmp(key, "hotkeys") && ParseBool(val, b)) g_cfg.hotkeys = b;
}

static bool LoadFile(const std::string& path) {
  FILE* f = fopen(path.c_str(), "r");
  if (!f) return false;
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    char* p = line;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '#' || *p == ';' || *p == '\n' || !*p) continue;
    char* eq = strchr(p, '=');
    if (!eq) continue;
    *eq = 0;
    char* k = p;
    char* v = eq + 1;
    auto trim = [](char* s) {
      size_t n = strlen(s);
      while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
      while (*s == ' ' || *s == '\t') ++s;
      return s;
    };
    ApplyKey(trim(k), trim(v));
  }
  fclose(f);
  return true;
}

static std::string ConfigDir() {
  if (const char* x = getenv("XDG_CONFIG_HOME"); x && *x) return std::string(x) + "/stereo3d";
  if (const char* h = getenv("HOME"); h && *h) return std::string(h) + "/.config/stereo3d";
  return "/tmp/stereo3d";
}

static std::string DetectGameName() {
  // Under Proton argv[0] is usually the Windows exe path (Z:\...\Game.exe or C:\...).
  std::string name;
  FILE* f = fopen("/proc/self/cmdline", "rb");
  if (f) {
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    // pick the first argument that ends with .exe, else argv[0]
    size_t off = 0;
    std::string first;
    while (off < n) {
      std::string arg(buf + off);
      if (first.empty()) first = arg;
      size_t len = arg.size();
      if (len > 4 && !strcasecmp(arg.c_str() + len - 4, ".exe")) { name = arg; break; }
      off += len + 1;
    }
    if (name.empty()) name = first;
  }
  size_t slash = name.find_last_of("/\\");
  if (slash != std::string::npos) name = name.substr(slash + 1);
  for (char& c : name)
    if (c == ' ' || c == ':' || c == '"' || c == '\'') c = '_';
  if (name.empty()) name = "unknown";
  return name;
}

static void LoadEnv() {
  float f;
  bool b;
  if (ParseFloat(getenv("STEREO3D_SEP"), f)) g_cfg.separation = f;
  if (ParseFloat(getenv("STEREO3D_CONV"), f)) g_cfg.convergence = f;
  if (ParseFloat(getenv("STEREO3D_ZOOM"), f)) g_cfg.zoom = f;
  if (ParseFloat(getenv("STEREO3D_HUD"), f)) g_cfg.hudDepth = f;
  if (ParseBool(getenv("STEREO3D_AUTO_SWITCH"), b)) g_cfg.autoSwitch = b;
  if (ParseFloat(getenv("STEREO3D_FPS_LIMIT"), f)) g_cfg.fpsLimit = f;
  if (const char* m = getenv("STEREO3D_MODE")) ApplyKey("mode", m);
  if (const char* ow = getenv("STEREO3D_OWN_WINDOW")) ApplyKey("own_window", ow);
  if (ParseBool(getenv("STEREO3D_SWAP"), b)) g_cfg.swapEyes = b;
  if (ParseBool(getenv("STEREO3D_STEREO"), b)) g_cfg.stereo = b;
  if (ParseBool(getenv("STEREO3D_PACE"), b)) g_cfg.pace = b;
  if (ParseBool(getenv("STEREO3D_PATCH"), b)) g_cfg.patchShaders = b;
  if (ParseBool(getenv("STEREO3D_HIDE_EXT"), b)) g_cfg.hideExtensions = b;
  if (ParseBool(getenv("STEREO3D_HOTKEYS"), b)) g_cfg.hotkeys = b;
  if (const char* s = getenv("STEREO3D_PRESENT_MODE")) g_cfg.presentMode = ParsePresentMode(s, g_cfg.presentMode);
  if (const char* s = getenv("STEREO3D_HALF")) ApplyKey("half_width", s);
  if (const char* s = getenv("STEREO3D_CROP")) ApplyKey("crop", s);
  if (ParseFloat(getenv("STEREO3D_LOG_LEVEL"), f)) g_cfg.logLevel = int(f);
}

void LoadConfig() {
  static bool loaded = false;
  if (loaded) return;
  loaded = true;
  if (const char* lf = getenv("STEREO3D_LOG"); lf && *lf) g_logFile = fopen(lf, "a");
  g_game = DetectGameName();
  std::string dir = ConfigDir();
  LoadFile(dir + "/default.conf");
  if (const char* p = getenv("STEREO3D_CONFIG"); p && *p) g_profile = p;
  else g_profile = dir + "/" + g_game + ".conf";
  bool had = LoadFile(g_profile);
  struct stat st;
  if (stat(g_profile.c_str(), &st) == 0) g_profileMtime = st.st_mtime;
  LoadEnv();
  // launchers, overlays and Wine helpers: never touch them
  static const char* kIgnore[] = {"explorer.exe", "xalia.exe", "upc.exe", "UbisoftConnect.exe", "UplayWebCore.exe",
                                  "UbisoftGameLauncher.exe", "UbisoftGameLauncher64.exe", "EpicGamesLauncher.exe",
                                  "EpicWebHelper.exe", "EOSOverlayRenderer-Win64-Shipping.exe",
                                  "EOSOverlayRenderer-Win32-Shipping.exe", "steamwebhelper.exe", "steam.exe",
                                  "EADesktop.exe", "EABackgroundService.exe", "Link2EA.exe", "RockstarService.exe",
                                  "LauncherPatcher.exe", "Launcher.exe", "steamwebhelper", "steam", "services.exe",
                                  "winedevice.exe", "plugplay.exe", "rpcss.exe", "tabtip.exe", "conhost.exe"};
  for (const char* n : kIgnore)
    if (!strcasecmp(g_game.c_str(), n)) g_cfg.passthrough = true;
  if (const char* ig = getenv("STEREO3D_IGNORE"); ig && strcasestr(ig, g_game.c_str())) g_cfg.passthrough = true;
  if (!g_cfg.passthrough && !g_logFile) {  // always keep a per-game log
    std::string ldir = dir + "/logs";
    mkdir(dir.c_str(), 0755);
    mkdir(ldir.c_str(), 0755);
    g_logFile = fopen((ldir + "/" + g_game + ".log").c_str(), "w");
  }
  if (g_cfg.passthrough) {
    g_cfg.patchShaders = false;
    g_cfg.ownWindow = 0;
    g_cfg.mode = 0;
    g_cfg.hideExtensions = false;
    g_cfg.fpsLimit = 0;
    Log(1, "%s: launcher/helper, stereo3d inactive", g_game.c_str());
    return;
  }
  Log(1, "game=%s profile=%s%s sep=%.4f conv=%.3f swap=%d present=%s pace=%d", g_game.c_str(),
      g_profile.c_str(), had ? "" : " (new)", g_cfg.separation, g_cfg.convergence, g_cfg.swapEyes,
      PresentModeName(g_cfg.presentMode), g_cfg.pace);
}

void SaveConfig() {
  std::string dir = ConfigDir();
  std::string cmd = dir;
  // mkdir -p (two levels is enough for ~/.config/stereo3d)
  size_t pos = 0;
  while ((pos = cmd.find('/', pos + 1)) != std::string::npos) mkdir(cmd.substr(0, pos).c_str(), 0755);
  mkdir(dir.c_str(), 0755);
  FILE* f = fopen(g_profile.c_str(), "w");
  if (!f) { Log(0, "cannot write %s", g_profile.c_str()); return; }
  fprintf(f, "# stereo3d profile for %s\n", g_game.c_str());
  fprintf(f, "separation = %.5f\nconvergence = %.5f\nswap_eyes = %d\nstereo = %d\n",
          g_cfg.separation, g_cfg.convergence, g_cfg.swapEyes, g_cfg.stereo);
  fprintf(f, "present_mode = %s\npace = %d\n", PresentModeName(g_cfg.presentMode), g_cfg.pace);
  fprintf(f, "half_width = %s\n", g_cfg.halfWidth < 0 ? "auto" : (g_cfg.halfWidth ? "1" : "0"));
  fprintf(f, "mode = %s\nzoom = %.4f\nfps_limit = %.1f\nhud_depth = %.4f\nauto_switch = %d\n", g_cfg.mode ? "replay" : "alternate", g_cfg.zoom, g_cfg.fpsLimit, g_cfg.hudDepth, g_cfg.autoSwitch);
  fprintf(f, "crop = %s\n", g_cfg.crop < 0 ? "auto" : g_cfg.crop == 1 ? "center" : g_cfg.crop == 2 ? "content" : "0");
  fclose(f);
  struct stat st;
  if (stat(g_profile.c_str(), &st) == 0) g_profileMtime = st.st_mtime;
  Log(1, "saved %s", g_profile.c_str());
}

// --- X11 hotkeys via dlopen (no link-time dependency) ---------------------------------
namespace {
struct X11 {
  void* lib = nullptr;
  void* dpy = nullptr;
  void* (*OpenDisplay)(const char*) = nullptr;
  int (*QueryKeymap)(void*, char[32]) = nullptr;
  unsigned char (*KeysymToKeycode)(void*, unsigned long) = nullptr;
  bool tried = false;
  bool ok = false;
  unsigned char kc[16] = {};
};
X11 g_x;

enum Key { kCtrlL, kCtrlR, kShiftL, kShiftR, kF3, kF4, kF5, kF6, kF7, kF8, kF9, kF10, kF11,
           kPgUp, kPgDn, kHome, kF1, kF2, kCount };
const unsigned long kSyms[kCount] = {0xFFE3, 0xFFE4, 0xFFE1, 0xFFE2, 0xFFC0, 0xFFC1, 0xFFC2, 0xFFC3,
                                     0xFFC4, 0xFFC5, 0xFFC6, 0xFFC7, 0xFFC8, 0xFF55, 0xFF56, 0xFF50,
                                     0xFFBE, 0xFFBF};
bool g_dumpRequested = false;

bool InitX11() {
  if (g_x.tried) return g_x.ok;
  g_x.tried = true;
  g_x.lib = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
  if (!g_x.lib) { Log(1, "hotkeys disabled (libX11 not found)"); return false; }
  g_x.OpenDisplay = (void* (*)(const char*))dlsym(g_x.lib, "XOpenDisplay");
  g_x.QueryKeymap = (int (*)(void*, char[32]))dlsym(g_x.lib, "XQueryKeymap");
  g_x.KeysymToKeycode = (unsigned char (*)(void*, unsigned long))dlsym(g_x.lib, "XKeysymToKeycode");
  if (!g_x.OpenDisplay || !g_x.QueryKeymap || !g_x.KeysymToKeycode) return false;
  g_x.dpy = g_x.OpenDisplay(nullptr);
  if (!g_x.dpy) { Log(1, "hotkeys disabled (no X display)"); return false; }
  for (int i = 0; i < kCount; ++i) g_x.kc[i] = g_x.KeysymToKeycode(g_x.dpy, kSyms[i]);
  g_x.ok = true;
  Log(1, "hotkeys: Ctrl+Shift + F3/F4 sep -/+  F5/F6 conv -/+  F7 swap  F8 3D on/off  F9 save  F10 pacing  F11 capture  PgUp/PgDn zoom  Home zoom=1  F1/F2 HUD depth");
  return true;
}

bool Down(const char km[32], int k) {
  unsigned char c = g_x.kc[k];
  return c && (km[c >> 3] & (1 << (c & 7)));
}
}  // namespace

bool TakeDumpRequest(uint64_t frame) {
  static float autoFrame = -2.0f;
  if (autoFrame == -2.0f) {
    autoFrame = -1.0f;
    if (const char* s = getenv("STEREO3D_DUMP"); s && *s) ParseFloat(s, autoFrame);
  }
  if (autoFrame >= 0 && frame >= uint64_t(autoFrame)) {
    autoFrame = -1.0f;
    return true;
  }
  if (g_dumpRequested) {
    g_dumpRequested = false;
    return true;
  }
  return false;
}

std::string DumpDir() {
  if (const char* d = getenv("STEREO3D_DUMP_DIR"); d && *d) return d;
  return ConfigDir();
}

bool PollControls() {
  bool changed = false;
  using clk = std::chrono::steady_clock;
  static clk::time_point lastFileCheck = clk::now();
  static clk::time_point lastKeyPoll = clk::now();
  auto now = clk::now();

  // config file hot reload (once per second)
  if (now - lastFileCheck > std::chrono::seconds(1)) {
    lastFileCheck = now;
    struct stat st;
    if (stat(g_profile.c_str(), &st) == 0 && st.st_mtime != g_profileMtime) {
      g_profileMtime = st.st_mtime;
      LoadFile(g_profile);
      Log(1, "reloaded %s sep=%.4f conv=%.3f", g_profile.c_str(), g_cfg.separation, g_cfg.convergence);
      changed = true;
    }
  }

  if (!g_cfg.hotkeys || !InitX11()) return changed;
  float dt = std::chrono::duration<float>(now - lastKeyPoll).count();
  lastKeyPoll = now;
  if (dt > 0.1f) dt = 0.1f;

  char km[32];
  g_x.QueryKeymap(g_x.dpy, km);
  static bool prev[kCount] = {};
  bool cur[kCount];
  for (int i = 0; i < kCount; ++i) cur[i] = Down(km, i);
  bool mod = (cur[kCtrlL] || cur[kCtrlR]) && (cur[kShiftL] || cur[kShiftR]);
  auto pressed = [&](int k) { return mod && cur[k] && !prev[k]; };
  float rate = 1.0f + 1.2f * dt;  // ~ x2.2 per second while held
  if (mod) {
    if (cur[kF3]) { g_cfg.separation /= rate; changed = true; }
    if (cur[kF4]) { g_cfg.separation *= rate; changed = true; }
    if (cur[kF5]) { g_cfg.convergence /= rate; changed = true; }
    if (cur[kF6]) { g_cfg.convergence *= rate; changed = true; }
    float zr = 1.0f + 0.5f * dt;
    if (cur[kPgUp]) { g_cfg.zoom = std::min(4.0f, g_cfg.zoom * zr); changed = true; }
    if ((cur[kF1] || cur[kF2]) && g_cfg.hudDepth <= 0.0f) g_cfg.hudDepth = g_cfg.convergence;
    if (cur[kF1]) { g_cfg.hudDepth /= rate; changed = true; }   // HUD closer
    if (cur[kF2]) { g_cfg.hudDepth *= rate; changed = true; }   // HUD farther
    if (cur[kPgDn]) { g_cfg.zoom = std::max(0.5f, g_cfg.zoom / zr); changed = true; }
  }
  if (pressed(kF7)) { g_cfg.swapEyes = !g_cfg.swapEyes; changed = true; Log(1, "swap_eyes=%d", g_cfg.swapEyes); }
  if (pressed(kF8)) { g_cfg.stereo = !g_cfg.stereo; changed = true; Log(1, "stereo=%d", g_cfg.stereo); }
  if (pressed(kF9)) SaveConfig();
  if (pressed(kF10)) { g_cfg.pace = !g_cfg.pace; Log(1, "pace=%d", g_cfg.pace); }
  if (pressed(kF11)) { g_dumpRequested = true; Log(1, "capture requested"); }
  if (pressed(kHome)) { g_cfg.zoom = 1.0f; changed = true; Log(1, "zoom=1"); }
  static bool wasAdjusting = false;
  bool adjusting = mod && (cur[kF3] || cur[kF4] || cur[kF5] || cur[kF6] || cur[kPgUp] || cur[kPgDn] || cur[kF1] || cur[kF2]);
  if (wasAdjusting && !adjusting)
    Log(1, "separation=%.4f convergence=%.3f hud_depth=%.3f zoom=%.3f", g_cfg.separation, g_cfg.convergence,
        g_cfg.hudDepth, g_cfg.zoom);
  wasAdjusting = adjusting;
  for (int i = 0; i < kCount; ++i) prev[i] = cur[i];
  return changed;
}

}  // namespace stereo3d
