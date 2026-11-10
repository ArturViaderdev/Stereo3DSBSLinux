// stereo3d - lens switching of LeiaSR lenticular monitors (see lens.h)
#include "lens.h"

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "config.h"

namespace stereo3d {
namespace {

std::mutex g_m;
bool g_onByUs = false;
bool g_atexit = false;
bool g_warned = false;

std::string ReadLine(const std::string& path) {
  std::string s;
  if (FILE* f = fopen(path.c_str(), "r")) {
    char buf[64] = {};
    if (fgets(buf, sizeof(buf), f)) s = buf;
    fclose(f);
  }
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
  return s;
}

// sends "cmd\r" and returns the answer (up to "OK" / "ERROR" or ~300 ms)
std::string Command(const std::string& port, const char* cmd) {
  int fd = open(port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) return "!open";
  termios t{};
  if (tcgetattr(fd, &t) == 0) {
    cfmakeraw(&t);
    cfsetispeed(&t, B115200);
    cfsetospeed(&t, B115200);
    t.c_cflag |= CLOCAL | CREAD;
    tcsetattr(fd, TCSANOW, &t);
  }
  tcflush(fd, TCIFLUSH);
  std::string out = std::string(cmd) + "\r";
  bool ok = write(fd, out.data(), out.size()) == ssize_t(out.size());
  std::string reply;
  for (int i = 0; ok && i < 15; ++i) {
    pollfd p{fd, POLLIN, 0};
    if (poll(&p, 1, 20) <= 0) continue;
    char buf[128];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0) reply.append(buf, size_t(n));
    if (reply.find("OK") != std::string::npos || reply.find("ERROR") != std::string::npos) break;
  }
  close(fd);
  if (!ok) return "!write";
  std::string clean;
  for (char c : reply)
    if (c == '\r' || c == '\n') {
      if (!clean.empty() && clean.back() != ' ') clean += ' ';
    } else {
      clean += c;
    }
  while (!clean.empty() && clean.back() == ' ') clean.pop_back();
  return clean;
}

void OffAtExit() {
  std::lock_guard<std::mutex> l(g_m);
  if (!g_onByUs) return;
  std::string port = LensPort();
  if (!port.empty()) Command(port, "SR+LENS=0");
  g_onByUs = false;
}

}  // namespace

std::string LensPort() {
  if (const char* p = getenv("STEREO3D_LENS_PORT"); p && *p) return p;
  DIR* d = opendir("/sys/class/tty");
  if (!d) return "";
  std::string found;
  while (dirent* e = readdir(d)) {
    if (strncmp(e->d_name, "ttyACM", 6) != 0) continue;
    // /sys/class/tty/ttyACMx/device is the USB interface; its parent is the USB device
    std::string dev = std::string("/sys/class/tty/") + e->d_name + "/device/../idVendor";
    if (ReadLine(dev) == "354b") {
      found = std::string("/dev/") + e->d_name;
      break;
    }
  }
  closedir(d);
  return found;
}

bool LensSet(bool on) {
  std::lock_guard<std::mutex> l(g_m);
  std::string port = LensPort();
  if (port.empty()) {
    if (!g_warned)
      Log(0, "lens: no LeiaSR monitor serial port found (USB 354b, /dev/ttyACM*): switch the lens on yourself "
             "(tools/spatial-lens on) or connect the monitor's USB cable to the PC");
    g_warned = true;
    return false;
  }
  std::string r = Command(port, on ? "SR+LENS=1" : "SR+LENS=0");
  bool ok = r.find("OK") != std::string::npos;
  if (!ok) {
    Log(0, "lens: %s on %s failed (%s)%s", on ? "SR+LENS=1" : "SR+LENS=0", port.c_str(), r.c_str(),
        r == "!open" ? ": no permission? install tools/70-spatial-lens.rules" : "");
    return false;
  }
  Log(1, "lens %s (%s)", on ? "ON (3D)" : "off (2D)", port.c_str());
  g_onByUs = on;
  if (on && !g_atexit) {
    g_atexit = true;
    atexit(OffAtExit);
  }
  return true;
}

}  // namespace stereo3d
