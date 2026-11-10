// stereo3d - lens switching of LeiaSR lenticular monitors (Acer SpatialLabs View, Samsung Odyssey 3D...)
//
// The monitor has a USB serial port (CDC-ACM, USB vendor 354b, /dev/ttyACMx on Linux) that takes
// text commands ending in '\r' (found in a USB capture of the Windows software):
//   SR+LENS=1   lens on (3D)      SR+LENS=0   lens off (2D)      SR+LENS   state, answers 0 / 1
//   SR+SERIAL / SR+SER4           serial numbers
#pragma once
#include <string>

namespace stereo3d {

std::string LensPort();   // "/dev/ttyACMx" of the monitor, "" if none
// Switches the lens; returns true when the monitor answered OK. The lens is switched off again
// when the process exits if it was switched on here.
bool LensSet(bool on);

}  // namespace stereo3d
