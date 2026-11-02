// sbs-desktop: shows the left half of the glasses screen (your 1920x1080 desktop) again in the
// right half, so the whole desktop is visible in both eyes with the glasses in SBS mode.
// The mirror hides itself while a stereo3d game is running.
//
// usage: sbs-desktop [x y width height]   (region of the glasses monitor, default: whole screen)
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <dlfcn.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
struct FixesCursorImage {
  short x, y;
  unsigned short width, height;
  unsigned short xhot, yhot;
  unsigned long cursor_serial;
  unsigned long* pixels;
  Atom atom;
  const char* name;
};

#define XFN(n) decltype(&X##n) f##n = nullptr;
struct Fns {
  XFN(OpenDisplay) XFN(DefaultScreen) XFN(RootWindow) XFN(DisplayWidth) XFN(DisplayHeight) XFN(CreateWindow)
  XFN(MapRaised) XFN(UnmapWindow) XFN(RaiseWindow) XFN(CreateGC) XFN(SetSubwindowMode) XFN(CopyArea) XFN(Flush)
  XFN(Sync) XFN(InternAtom) XFN(GetWindowProperty) XFN(Free) XFN(DefaultDepth) XFN(DefaultVisual) XFN(BlackPixel)
  XFN(StoreName) XFN(CreateImage) XFN(PutImage) XFN(CreateBitmapFromData) XFN(SetClipMask) XFN(SetClipOrigin)
  XFN(FreePixmap) XFN(GetSelectionOwner) XFN(CloseDisplay)
  void (*ShapeCombineRectangles)(Display*, Window, int, int, int, XRectangle*, int, int, int) = nullptr;
  FixesCursorImage* (*GetCursorImage)(Display*) = nullptr;
};
Fns x;
volatile sig_atomic_t g_quit = 0;
void OnSignal(int) { g_quit = 1; }

bool Load() {
  void* lib = dlopen("libX11.so.6", RTLD_NOW);
  if (!lib) return false;
#define L(n) x.f##n = (decltype(x.f##n))dlsym(lib, "X" #n); if (!x.f##n) { fprintf(stderr, "missing X" #n "\n"); return false; }
  L(OpenDisplay) L(DefaultScreen) L(RootWindow) L(DisplayWidth) L(DisplayHeight) L(CreateWindow) L(MapRaised)
  L(UnmapWindow) L(RaiseWindow) L(CreateGC) L(SetSubwindowMode) L(CopyArea) L(Flush) L(Sync) L(InternAtom)
  L(GetWindowProperty) L(Free) L(DefaultDepth) L(DefaultVisual) L(BlackPixel) L(StoreName) L(CreateImage)
  L(PutImage) L(CreateBitmapFromData) L(SetClipMask) L(SetClipOrigin) L(FreePixmap) L(GetSelectionOwner)
  L(CloseDisplay)
#undef L
  if (void* e = dlopen("libXext.so.6", RTLD_NOW))
    x.ShapeCombineRectangles = (decltype(x.ShapeCombineRectangles))dlsym(e, "XShapeCombineRectangles");
  if (void* f = dlopen("libXfixes.so.3", RTLD_NOW))
    x.GetCursorImage = (decltype(x.GetCursorImage))dlsym(f, "XFixesGetCursorImage");
  return true;
}

// stereo3d writes the time into this root property while a game is presenting
bool GameActive(Display* dpy, Window root, Atom atom) {
  Atom type;
  int format;
  unsigned long n, after;
  unsigned char* data = nullptr;
  bool active = false;
  if (x.fGetWindowProperty(dpy, root, atom, 0, 1, False, AnyPropertyType, &type, &format, &n, &after, &data) ==
          Success && data && n == 1 && format == 32) {
    long t = *(long*)data;
    active = labs(long(time(nullptr)) - t) <= 3;
  }
  if (data) x.fFree(data);
  return active;
}
}  // namespace

int main(int argc, char** argv) {
  if (!Load()) { fprintf(stderr, "sbs-desktop: libX11 not found\n"); return 1; }
  Display* dpy = x.fOpenDisplay(nullptr);
  if (!dpy) { fprintf(stderr, "sbs-desktop: cannot open the X display (Wayland session?)\n"); return 1; }
  int scr = x.fDefaultScreen(dpy);
  Window root = x.fRootWindow(dpy, scr);
  int gx = 0, gy = 0, gw = x.fDisplayWidth(dpy, scr), gh = x.fDisplayHeight(dpy, scr);
  if (argc >= 5) { gx = atoi(argv[1]); gy = atoi(argv[2]); gw = atoi(argv[3]); gh = atoi(argv[4]); }
  const int hw = gw / 2;
  printf("sbs-desktop: copying %dx%d+%d+%d to %dx%d+%d+%d (Ctrl+C to stop)\n", hw, gh, gx, gy, hw, gh, gx + hw, gy);
  if (x.fGetSelectionOwner(dpy, x.fInternAtom(dpy, "_NET_WM_CM_S0", False)))
    printf("sbs-desktop: a compositor is active. If the right half stays black or frozen, disable desktop\n"
           "             compositing (e.g. KDE: Alt+Shift+F12, XFCE: Settings > Window Manager Tweaks).\n");

  XSetWindowAttributes a;
  memset(&a, 0, sizeof(a));
  a.override_redirect = True;
  a.background_pixel = x.fBlackPixel(dpy, scr);
  Window win = x.fCreateWindow(dpy, root, gx + hw, gy, unsigned(gw - hw), unsigned(gh), 0, x.fDefaultDepth(dpy, scr),
                               InputOutput, x.fDefaultVisual(dpy, scr), CWOverrideRedirect | CWBackPixel, &a);
  x.fStoreName(dpy, win, "sbs-desktop");
  if (x.ShapeCombineRectangles) x.ShapeCombineRectangles(dpy, win, 2 /*ShapeInput*/, 0, 0, nullptr, 0, 0, 0);
  x.fMapRaised(dpy, win);
  GC gc = x.fCreateGC(dpy, root, 0, nullptr);
  x.fSetSubwindowMode(dpy, gc, IncludeInferiors);
  GC cgc = x.fCreateGC(dpy, win, 0, nullptr);
  Atom activeAtom = x.fInternAtom(dpy, "_STEREO3D_ACTIVE", False);

  signal(SIGINT, OnSignal);
  signal(SIGTERM, OnSignal);
  bool shown = true;
  std::vector<unsigned int> cur;
  std::vector<unsigned char> mask;
  for (unsigned frame = 0; !g_quit; ++frame) {
    if (frame % 15 == 0) {
      bool game = GameActive(dpy, root, activeAtom);
      if (game && shown) { x.fUnmapWindow(dpy, win); shown = false; }
      if (!game && !shown) { x.fMapRaised(dpy, win); shown = true; }
      if (shown && frame % 120 == 0) x.fRaiseWindow(dpy, win);
    }
    if (shown) {
      x.fCopyArea(dpy, root, win, gc, gx, gy, unsigned(hw), unsigned(gh), 0, 0);
      // the mouse pointer is not part of the screen image: draw it
      if (x.GetCursorImage) {
        if (FixesCursorImage* ci = x.GetCursorImage(dpy)) {
          int cx = ci->x - ci->xhot - gx, cy = ci->y - ci->yhot - gy;
          int w = ci->width, h = ci->height;
          if (cx > -w && cx < hw && cy > -h && cy < gh && w <= 256 && h <= 256) {
            cur.assign(size_t(w) * h, 0);
            int stride = (w + 7) / 8;
            mask.assign(size_t(stride) * h, 0);
            for (int i = 0; i < w * h; ++i) {
              unsigned long p = ci->pixels[i];
              cur[i] = unsigned(p & 0xFFFFFF);
              if (((p >> 24) & 0xFF) > 100) mask[(i / w) * stride + (i % w) / 8] |= 1 << (i % 8);
            }
            XImage* img = x.fCreateImage(dpy, x.fDefaultVisual(dpy, scr), 24, ZPixmap, 0, (char*)cur.data(),
                                         unsigned(w), unsigned(h), 32, 0);
            Pixmap pm = x.fCreateBitmapFromData(dpy, win, (const char*)mask.data(), unsigned(w), unsigned(h));
            if (img && pm) {
              x.fSetClipMask(dpy, cgc, pm);
              x.fSetClipOrigin(dpy, cgc, cx, cy);
              x.fPutImage(dpy, win, cgc, img, 0, 0, cx, cy, unsigned(w), unsigned(h));
              x.fSetClipMask(dpy, cgc, None);
            }
            if (pm) x.fFreePixmap(dpy, pm);
            if (img) { img->data = nullptr; x.fFree(img); }
          }
          x.fFree(ci);
        }
      }
      x.fSync(dpy, False);
      usleep(14000);  // ~60 fps
    } else {
      usleep(100000);
    }
  }
  x.fCloseDisplay(dpy);
  return 0;
}
