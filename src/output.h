// stereo3d - own output window on the SBS (32:9) monitor
#pragma once

namespace stereo3d {

struct OutputWindow;

// Creates a borderless, always-on-top, input-transparent X11 window covering the
// glasses monitor (the first monitor at least 2.5:1 wide), or with lenticular = true the
// 3D monitor (the 3840x2160 one, else the biggest), or STEREO3D_OUTPUT_RECT=x,y,w,h.
// Returns nullptr if X11 is not available.
OutputWindow* CreateOutputWindow(bool lenticular = false);
// a 32:9 (glasses in 3D mode) monitor is connected
bool WideMonitorPresent();
void* OutputDisplay(OutputWindow* o);       // Display*
unsigned long OutputXid(OutputWindow* o);   // Window
void OutputRect(OutputWindow* o, int* x, int* y, int* w, int* h);
void RaiseOutputWindow(OutputWindow* o);
void SetOutputWindowVisible(OutputWindow* o, bool visible);
void DestroyOutputWindow(OutputWindow* o);

// Tells sbs-desktop (the desktop mirror) that a 3D game is on screen, so it hides.
void MarkStereoActive();

}  // namespace stereo3d
