// stereo3d-tool - helpers for the stereo3d layer
//
//   stereo3d-tool shader-fix [folder]     which dumped shaders fix_shadows (Unreal shadows) corrects
//   stereo3d-tool lens on|off|status      lens of LeiaSR 3D monitors (Acer SpatialLabs, Samsung Odyssey 3D)
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "../src/config.h"
#include "../src/lens.h"
#include "../src/spirv_patch.h"

using namespace stereo3d;

// shader-fix <folder of a shader dump>: which shaders the layer's fix_shadows would correct
int ShaderFix(int argc, char** argv) {
  std::string dir;
  if (argc > 0) {
    dir = argv[0];
  } else {  // newest dump in ~/.cache/stereo3d/shaders
    std::string base = CacheDir() + "/shaders";
    time_t best = 0;
    if (DIR* d = opendir(base.c_str())) {
      while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        struct stat st;
        std::string p = base + "/" + e->d_name;
        if (stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_mtime >= best) { best = st.st_mtime; dir = p; }
      }
      closedir(d);
    }
  }
  DIR* d = dir.empty() ? nullptr : opendir(dir.c_str());
  if (!d) {
    fprintf(stderr, "no shader dump (run the game with STEREO3D_DUMP_SHADERS=1)\n");
    return 1;
  }
  std::vector<std::string> names;
  while (dirent* e = readdir(d)) {
    std::string n = e->d_name;
    if (n.size() > 7 && (n.compare(n.size() - 7, 7, ".fs.spv") == 0 || n.compare(n.size() - 7, 7, ".cs.spv") == 0))
      names.push_back(n);
  }
  closedir(d);
  std::sort(names.begin(), names.end());
  int fixed = 0;
  for (const std::string& n : names) {
    FILE* f = fopen((dir + "/" + n).c_str(), "rb");
    if (!f) continue;
    std::vector<uint32_t> code;
    uint32_t w;
    while (fread(&w, 4, 1, f) == 1) code.push_back(w);
    fclose(f);
    std::vector<uint32_t> out;
    std::string log;
    if (stereo3d::PatchScreenSpace(code.data(), code.size(), 0x100000ull, out, &log) == stereo3d::PatchResult::Patched) {
      printf("  %s  %s\n", n.c_str(), log.c_str());
      ++fixed;
    }
  }
  printf("%s: %zu fragment/compute shaders, %d would be corrected by fix_shadows\n", dir.c_str(), names.size(),
         fixed);
  return 0;
}

static int Lens(int argc, char** argv) {
  std::string port = LensPort();
  if (argc < 1 || !strcmp(argv[0], "status")) {
    printf("LeiaSR monitor serial port: %s\n", port.empty() ? "not found (USB 354b, /dev/ttyACM*)" : port.c_str());
    return port.empty() ? 1 : 0;
  }
  if (!strcmp(argv[0], "on")) return LensSet(true) ? 0 : 1;
  if (!strcmp(argv[0], "off")) return LensSet(false) ? 0 : 1;
  fprintf(stderr, "usage: stereo3d-tool lens on|off|status\n");
  return 2;
}

int main(int argc, char** argv) {
  Cfg().logLevel = 1;
  if (argc < 2) {
    printf("stereo3d-tool\n\n"
           "  stereo3d-tool shader-fix [shader dump folder]   which shaders fix_shadows corrects\n"
           "  stereo3d-tool lens on|off|status                lens of LeiaSR 3D monitors\n");
    return 2;
  }
  std::string cmd = argv[1];
  if (cmd == "shader-fix") return ShaderFix(argc - 2, argv + 2);
  if (cmd == "lens") {
    int r = Lens(argc - 2, argv + 2);
    if (argc > 2 && !strcmp(argv[2], "on")) _exit(r);  // keep it on: skip the switch-off at exit
    return r;
  }
  fprintf(stderr, "unknown command %s\n", cmd.c_str());
  return 2;
}
