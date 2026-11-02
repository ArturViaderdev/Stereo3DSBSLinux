// stereo3d - Vulkan layer: real (geometry) stereoscopic 3D for Proton games, SBS output.
//
// How it works
//  * Every vertex / tess-eval / geometry shader that reaches the driver is patched so that
//    gl_Position.x += k * (gl_Position.w - convergence)   (not applied when w == 1).
//    k (= ±separation) and convergence live in a tiny GPU buffer read through a
//    buffer-device-address constant baked into the shader.
//  * The game renders alternately for the left eye (even frames) and the right eye (odd
//    frames). The game's swapchain is virtual: it gets 1920x1080 images; on every present
//    we copy the image into the left/right half of the real 3840x1080 swapchain and only
//    present the real one after the right eye -> the game runs at 120 fps, the glasses get
//    60 SBS frames per second, each eye with its own 60 fps image.
//  * The eye value for the next frame is written into the buffer from the queue right
//    after the copy, so it is perfectly ordered with the game's GPU work.
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <dirent.h>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "config.h"
#include "output.h"
#include "spirv_patch.h"

#define LAYER_NAME "VK_LAYER_STEREO3D_sbs"
#define EXPORT extern "C" __attribute__((visibility("default")))

using namespace stereo3d;
using Clock = std::chrono::steady_clock;

namespace {

// ------------------------------------------------------------------------------------
// Dispatch tables
// ------------------------------------------------------------------------------------
#define INST_FUNCS(X)                                                                     \
  X(DestroyInstance) X(EnumeratePhysicalDevices) X(EnumeratePhysicalDeviceGroups)         \
  X(EnumeratePhysicalDeviceGroupsKHR) X(EnumerateDeviceExtensionProperties)               \
  X(GetPhysicalDeviceSurfaceCapabilitiesKHR) X(GetPhysicalDeviceSurfaceCapabilities2KHR)  \
  X(GetPhysicalDeviceSurfacePresentModesKHR) X(GetPhysicalDeviceMemoryProperties)         \
  X(GetPhysicalDeviceQueueFamilyProperties) X(GetPhysicalDeviceFeatures2)                 \
  X(GetPhysicalDeviceFeatures2KHR) X(GetPhysicalDeviceProperties)                         \
  X(GetPhysicalDeviceFormatProperties) X(DestroySurfaceKHR) X(GetPhysicalDeviceSurfaceSupportKHR)          \
  X(GetPhysicalDeviceSurfaceFormatsKHR)

#define DEV_FUNCS(X)                                                                        \
  X(DestroyDevice) X(GetDeviceQueue) X(GetDeviceQueue2) X(CreateShaderModule)               \
  X(DestroyShaderModule) X(CreateGraphicsPipelines) X(QueueSubmit) X(QueueSubmit2)          \
  X(QueueSubmit2KHR) X(QueueWaitIdle) X(QueueBindSparse) X(DeviceWaitIdle)                  \
  X(CreateSwapchainKHR) X(DestroySwapchainKHR) X(GetSwapchainImagesKHR)                     \
  X(AcquireNextImageKHR) X(AcquireNextImage2KHR) X(QueuePresentKHR) X(CreateImage)          \
  X(DestroyImage) X(GetImageMemoryRequirements) X(AllocateMemory) X(FreeMemory)             \
  X(BindImageMemory) X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements)        \
  X(BindBufferMemory) X(MapMemory) X(UnmapMemory) X(GetBufferDeviceAddress)                 \
  X(GetBufferDeviceAddressKHR) X(CreateCommandPool) X(DestroyCommandPool)                   \
  X(AllocateCommandBuffers) X(FreeCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) \
  X(ResetCommandBuffer) X(CmdPipelineBarrier) X(CmdCopyImage) X(CmdBlitImage)               \
  X(CmdUpdateBuffer) X(CmdClearColorImage) X(CmdCopyImageToBuffer) X(CreateFence) X(DestroyFence) X(WaitForFences)  \
  X(ResetFences) X(CreateSemaphore) X(DestroySemaphore) X(WaitSemaphores) X(WaitSemaphoresKHR)

struct InstDispatch {
#define DECL(n) PFN_vk##n n = nullptr;
  INST_FUNCS(DECL)
};
struct DevDispatch {
  PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
  DEV_FUNCS(DECL)
#undef DECL
};

inline void* Key(const void* dispatchable) { return *(void* const*)dispatchable; }

struct InstanceData {
  VkInstance instance = VK_NULL_HANDLE;
  PFN_vkGetInstanceProcAddr gipa = nullptr;
  InstDispatch d;
  uint32_t apiVersion = VK_API_VERSION_1_0;
  PFN_vkVoidFunction createXlibSurface = nullptr;  // vkCreateXlibSurfaceKHR (own output window)
};
// VkXlibSurfaceCreateInfoKHR without pulling Xlib into this file
struct XlibSurfaceCI {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  void* dpy;
  unsigned long window;
};
typedef VkResult(VKAPI_PTR* PFN_CreateXlibSurface)(VkInstance, const XlibSurfaceCI*, const VkAllocationCallbacks*,
                                                    VkSurfaceKHR*);

struct QueueInfo {
  uint32_t family = UINT32_MAX;
  std::mutex m;
};

struct VImage {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory mem = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkCommandBuffer cmd2 = VK_NULL_HANDLE;  // replay mode: right-eye copy
  VkSemaphore acqSem = VK_NULL_HANDLE;
  VkSemaphore doneSem = VK_NULL_HANDLE;
};

struct Swapchain {
  VkSwapchainKHR handle = VK_NULL_HANDLE;  // == real swapchain handle
  VkFormat format = VK_FORMAT_UNDEFINED;      // game (virtual) format
  VkFormat realFormat = VK_FORMAT_UNDEFINED;  // real swapchain format
  bool formatDiffers = false;
  bool onOwn = false;                         // real swapchain lives on our own output window
  VkExtent2D vext{}, rext{};
  VkOffset2D srcOff{};      // region of the game image that holds one eye (crops Proton letterbox)
  VkExtent2D srcExt{};
  VkOffset2D dstOff{};      // where that region goes inside one eye (keeps aspect ratio)
  VkExtent2D dstExt{};
  bool detectOn = false;    // detect black bars from the image content
  int cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;     // current game-content rectangle (end exclusive)
  int kx0 = -1, ky0 = -1, kx1 = -1, ky1 = -1;  // candidate rectangle waiting for confirmation
  int candCount = 0;
  uint64_t nextDetect = 0;
  int detections = 0;
  float lastZoom = -1.0f;
  std::vector<VkImage> realImages;
  std::vector<VImage> v;
  VkImage left = VK_NULL_HANDLE;
  VkDeviceMemory leftMem = VK_NULL_HANDLE;
  bool leftValid = false;
  VkCommandPool pool = VK_NULL_HANDLE;
  uint32_t poolFamily = UINT32_MAX;
  bool canBlit = true;
  uint32_t nextAcquire = 0;
  Clock::time_point lastRight{};
  bool haveLastRight = false;
  double periodEma = 0.0;  // seconds between real presents
  uint64_t realPresents = 0;
  Clock::time_point statStart = Clock::now();
  uint64_t statFrames = 0;
};

struct ModuleEntry {
  std::vector<uint32_t> code;
  struct Variant { uint32_t model; std::string entry; VkShaderModule patched; };
  std::vector<Variant> variants;
};

struct DeviceData {
  VkDevice device = VK_NULL_HANDLE;
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  InstanceData* inst = nullptr;
  DevDispatch d;
  PFN_vkSetDeviceLoaderData setLoaderData = nullptr;
  uint32_t gfxFamily = UINT32_MAX;
  uint32_t apiVersion = VK_API_VERSION_1_0;
  VkPhysicalDeviceMemoryProperties memProps{};

  std::mutex queueLock;
  std::unordered_map<VkQueue, std::unique_ptr<QueueInfo>> queues;
  VkQueue gfxQueue = VK_NULL_HANDLE;

  OutputWindow* out = nullptr;     // own window on the glasses monitor
  VkSurfaceKHR outSurface = VK_NULL_HANDLE;
  int outState = 0;                // 0 undecided, 1 using own window, -1 using the game window

  bool stereoShaders = false;
  VkBuffer paramBuf = VK_NULL_HANDLE;
  VkDeviceMemory paramMem = VK_NULL_HANDLE;
  VkDeviceAddress paramAddr = 0;

  std::mutex shaderLock;
  std::unordered_map<VkShaderModule, ModuleEntry> modules;
  std::atomic<uint32_t> patchedCount{0}, skippedCount{0}, pipelineCount{0};

  std::mutex swapLock;
  std::unordered_map<VkSwapchainKHR, Swapchain*> swapchains;
  uint64_t frame = 0;  // presents to virtual swapchains; even = left eye
  Clock::time_point lastPresent{};  // for the frame limiter

  // ---- replay mode: the frame's command buffers are submitted a second time for the right eye
  std::atomic<bool> replay{false};
  std::atomic<const char*> stage{"init"};     // what the layer is doing (for the stall watchdog)
  std::thread watchdog;
  std::atomic<bool> stopWatchdog{false};
  int badReplayWindows = 0;
  std::mutex recLock;                         // held by submits on the present queue and by present
  std::vector<VkCommandBuffer> recCbs;        // this frame's command buffers, in order
  std::unordered_map<VkSemaphore, uint64_t> heldTimeline;  // signals delayed until after the replay
  std::vector<VkFence> heldFences;
  bool recMono = false;                       // replay impossible this frame (app needed a signal early)
  std::mutex semLock;
  std::unordered_set<VkSemaphore> timelineSems;
  std::atomic<uint32_t> monoFrames{0}, replayFrames{0};
  std::atomic<uint64_t> presentCount{0};
  std::atomic<int> swapCount{0};
  std::atomic<uint64_t> lastAbortPresent{0};
};

std::mutex g_lock;
std::unordered_map<void*, InstanceData*> g_instByKey;
std::unordered_map<VkPhysicalDevice, InstanceData*> g_instByPhys;
std::unordered_map<void*, DeviceData*> g_devByKey;

InstanceData* Inst(const void* h) {
  std::lock_guard<std::mutex> l(g_lock);
  auto it = g_instByKey.find(Key(h));
  return it == g_instByKey.end() ? nullptr : it->second;
}
InstanceData* InstFromPhys(VkPhysicalDevice p) {
  std::lock_guard<std::mutex> l(g_lock);
  auto it = g_instByPhys.find(p);
  if (it != g_instByPhys.end()) return it->second;
  auto it2 = g_instByKey.find(Key(p));
  if (it2 != g_instByKey.end()) return it2->second;
  if (g_instByKey.size() == 1) return g_instByKey.begin()->second;
  return nullptr;
}
DeviceData* Dev(const void* h) {
  std::lock_guard<std::mutex> l(g_lock);
  auto it = g_devByKey.find(Key(h));
  return it == g_devByKey.end() ? nullptr : it->second;
}

QueueInfo* GetQueueInfo(DeviceData* dd, VkQueue q) {
  std::lock_guard<std::mutex> l(dd->queueLock);
  auto& p = dd->queues[q];
  if (!p) p.reset(new QueueInfo());
  return p.get();
}

// Presentation extensions can never work with the virtual swapchain (the left-eye frame is
// never presented on its own, present IDs / waits would never complete): always hidden.
const char* const kAlwaysHidden[] = {
    "VK_KHR_present_wait",           "VK_KHR_present_wait2",          "VK_KHR_present_id",
    "VK_KHR_present_id2",            "VK_EXT_swapchain_maintenance1", "VK_KHR_swapchain_maintenance1",
    "VK_NV_low_latency2",            "VK_AMD_anti_lag",               "VK_EXT_full_screen_exclusive",
    "VK_GOOGLE_display_timing",      "VK_EXT_present_timing",
    // pipelines built from cached shader identifiers or shader objects carry no SPIR-V to patch
    "VK_EXT_shader_module_identifier", "VK_EXT_shader_object",
};
// Hidden by default (hide_extensions = 0 shows it): library pipelines are patched too, but less tested
const char* const kShaderPathHidden[] = {
    "VK_EXT_graphics_pipeline_library",
};
bool IsHidden(const char* name) {
  for (const char* h : kAlwaysHidden)
    if (!strcmp(h, name)) return true;
  if (Cfg().hideExtensions)
    for (const char* h : kShaderPathHidden)
      if (!strcmp(h, name)) return true;
  return false;
}

float EyeK(uint64_t frame) {
  const Config& c = Cfg();
  if (!c.stereo) return 0.0f;
  float sign = (frame & 1) ? 1.0f : -1.0f;  // even frame = left eye
  if (c.swapEyes) sign = -sign;
  return sign * c.separation;
}

uint32_t FindMemType(DeviceData* dd, uint32_t bits, VkMemoryPropertyFlags want,
                     VkMemoryPropertyFlags prefer = 0) {
  for (int pass = 0; pass < 2; ++pass) {
    VkMemoryPropertyFlags f = want | (pass == 0 ? prefer : 0);
    for (uint32_t i = 0; i < dd->memProps.memoryTypeCount; ++i)
      if ((bits & (1u << i)) && (dd->memProps.memoryTypes[i].propertyFlags & f) == f) return i;
  }
  for (uint32_t i = 0; i < dd->memProps.memoryTypeCount; ++i)
    if (bits & (1u << i)) return i;
  return UINT32_MAX;
}

// ------------------------------------------------------------------------------------
// Instance hooks
// ------------------------------------------------------------------------------------
VKAPI_ATTR VkResult VKAPI_CALL hook_CreateInstance(const VkInstanceCreateInfo* ci,
                                                   const VkAllocationCallbacks* alloc,
                                                   VkInstance* pInstance) {
  auto* chain = (VkLayerInstanceCreateInfo*)ci->pNext;
  while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
                    chain->function == VK_LAYER_LINK_INFO))
    chain = (VkLayerInstanceCreateInfo*)chain->pNext;
  if (!chain) return VK_ERROR_INITIALIZATION_FAILED;
  PFN_vkGetInstanceProcAddr gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
  auto create = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
  LoadConfig();
  // enable the Xlib surface extension so we can open our own output window on the glasses
  VkResult r = VK_ERROR_EXTENSION_NOT_PRESENT;
  if (Cfg().ownWindow == 1 && getenv("STEREO3D_ADD_XLIB")) {  // opt-in: games normally already have it
    std::vector<const char*> exts(ci->ppEnabledExtensionNames, ci->ppEnabledExtensionNames + ci->enabledExtensionCount);
    for (const char* want : {"VK_KHR_surface", "VK_KHR_xlib_surface"}) {
      bool have = false;
      for (const char* e : exts) have |= !strcmp(e, want);
      if (!have) exts.push_back(want);
    }
    VkInstanceCreateInfo ci2 = *ci;
    ci2.enabledExtensionCount = uint32_t(exts.size());
    ci2.ppEnabledExtensionNames = exts.data();
    r = create(&ci2, alloc, pInstance);
  }
  if (r != VK_SUCCESS) r = create(ci, alloc, pInstance);
  if (r != VK_SUCCESS) return r;

  auto* id = new InstanceData();
  id->instance = *pInstance;
  id->gipa = gipa;
  if (ci->pApplicationInfo && ci->pApplicationInfo->apiVersion)
    id->apiVersion = ci->pApplicationInfo->apiVersion;
#define LOAD(n) id->d.n = (PFN_vk##n)gipa(*pInstance, "vk" #n);
  INST_FUNCS(LOAD)
#undef LOAD
  id->createXlibSurface = gipa(*pInstance, "vkCreateXlibSurfaceKHR");
  std::lock_guard<std::mutex> l(g_lock);
  g_instByKey[Key(*pInstance)] = id;
  return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL hook_DestroyInstance(VkInstance instance, const VkAllocationCallbacks* a) {
  InstanceData* id = Inst(instance);
  if (!id) return;
  id->d.DestroyInstance(instance, a);
  std::lock_guard<std::mutex> l(g_lock);
  g_instByKey.erase(Key(instance));
  for (auto it = g_instByPhys.begin(); it != g_instByPhys.end();)
    it = (it->second == id) ? g_instByPhys.erase(it) : std::next(it);
  delete id;
}

VKAPI_ATTR VkResult VKAPI_CALL hook_EnumeratePhysicalDevices(VkInstance instance, uint32_t* count,
                                                             VkPhysicalDevice* devs) {
  InstanceData* id = Inst(instance);
  VkResult r = id->d.EnumeratePhysicalDevices(instance, count, devs);
  if (devs && (r == VK_SUCCESS || r == VK_INCOMPLETE)) {
    std::lock_guard<std::mutex> l(g_lock);
    for (uint32_t i = 0; i < *count; ++i) g_instByPhys[devs[i]] = id;
  }
  return r;
}

VkResult EnumGroups(PFN_vkEnumeratePhysicalDeviceGroups fn, InstanceData* id, VkInstance instance,
                    uint32_t* count, VkPhysicalDeviceGroupProperties* props) {
  VkResult r = fn(instance, count, props);
  if (props && (r == VK_SUCCESS || r == VK_INCOMPLETE)) {
    std::lock_guard<std::mutex> l(g_lock);
    for (uint32_t i = 0; i < *count; ++i)
      for (uint32_t j = 0; j < props[i].physicalDeviceCount; ++j)
        g_instByPhys[props[i].physicalDevices[j]] = id;
  }
  return r;
}
VKAPI_ATTR VkResult VKAPI_CALL hook_EnumeratePhysicalDeviceGroups(
    VkInstance instance, uint32_t* count, VkPhysicalDeviceGroupProperties* props) {
  InstanceData* id = Inst(instance);
  return EnumGroups(id->d.EnumeratePhysicalDeviceGroups, id, instance, count, props);
}
VKAPI_ATTR VkResult VKAPI_CALL hook_EnumeratePhysicalDeviceGroupsKHR(
    VkInstance instance, uint32_t* count, VkPhysicalDeviceGroupProperties* props) {
  InstanceData* id = Inst(instance);
  return EnumGroups(id->d.EnumeratePhysicalDeviceGroupsKHR, id, instance, count, props);
}

VKAPI_ATTR VkResult VKAPI_CALL hook_EnumerateDeviceExtensionProperties(
    VkPhysicalDevice phys, const char* layerName, uint32_t* count, VkExtensionProperties* props) {
  if (layerName && !strcmp(layerName, LAYER_NAME)) {
    *count = 0;
    return VK_SUCCESS;
  }
  InstanceData* id = InstFromPhys(phys);
  if (!id) return VK_ERROR_INITIALIZATION_FAILED;
  if (layerName || Cfg().passthrough)
    return id->d.EnumerateDeviceExtensionProperties(phys, layerName, count, props);

  uint32_t n = 0;
  VkResult r = id->d.EnumerateDeviceExtensionProperties(phys, nullptr, &n, nullptr);
  if (r != VK_SUCCESS) return r;
  std::vector<VkExtensionProperties> all(n);
  r = id->d.EnumerateDeviceExtensionProperties(phys, nullptr, &n, all.data());
  if (r != VK_SUCCESS && r != VK_INCOMPLETE) return r;
  all.resize(n);
  std::vector<VkExtensionProperties> kept;
  for (auto& e : all)
    if (!IsHidden(e.extensionName)) kept.push_back(e);
  if (!props) {
    *count = uint32_t(kept.size());
    return VK_SUCCESS;
  }
  uint32_t c = std::min<uint32_t>(*count, uint32_t(kept.size()));
  std::copy(kept.begin(), kept.begin() + c, props);
  *count = c;
  return c < kept.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

bool ShouldHalve(const VkExtent2D& e) {
  if (e.width == 0xFFFFFFFFu || e.width < 2) return false;
  int h = Cfg().halfWidth;
  if (h == 0) return false;
  if (h == 1) return true;
  return uint64_t(e.width) * 10 >= uint64_t(e.height) * 25;  // >= 2.5:1 -> SBS display
}

void AdjustCaps(VkSurfaceCapabilitiesKHR& c) {
  if (Cfg().passthrough || !ShouldHalve(c.currentExtent)) return;
  c.currentExtent.width /= 2;
  c.minImageExtent.width = std::min(c.minImageExtent.width, c.currentExtent.width);
  c.maxImageExtent.width = std::max(c.currentExtent.width, c.maxImageExtent.width / 2);
}

VKAPI_ATTR VkResult VKAPI_CALL hook_GetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice phys, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR* caps) {
  InstanceData* id = InstFromPhys(phys);
  VkResult r = id->d.GetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, caps);
  if (r == VK_SUCCESS) AdjustCaps(*caps);
  return r;
}

VKAPI_ATTR VkResult VKAPI_CALL hook_GetPhysicalDeviceSurfaceCapabilities2KHR(
    VkPhysicalDevice phys, const VkPhysicalDeviceSurfaceInfo2KHR* info,
    VkSurfaceCapabilities2KHR* caps) {
  InstanceData* id = InstFromPhys(phys);
  VkResult r = id->d.GetPhysicalDeviceSurfaceCapabilities2KHR(phys, info, caps);
  if (r == VK_SUCCESS) AdjustCaps(caps->surfaceCapabilities);
  return r;
}

// ------------------------------------------------------------------------------------
// Stall watchdog: if the game stops presenting, say where we are and release held signals
// ------------------------------------------------------------------------------------
void AbortReplayLocked(DeviceData* dd, const char* why);

// What every thread of the process is doing (name, state, kernel wait channel)
void LogThreads() {
  DIR* d = opendir("/proc/self/task");
  if (!d) return;
  int n = 0;
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    std::string base = std::string("/proc/self/task/") + e->d_name;
    auto readFile = [](const std::string& p) {
      std::string s;
      if (FILE* f = fopen(p.c_str(), "r")) {
        char buf[256];
        size_t k = fread(buf, 1, sizeof(buf) - 1, f);
        buf[k] = 0;
        s = buf;
        fclose(f);
      }
      while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
      return s;
    };
    std::string comm = readFile(base + "/comm"), wchan = readFile(base + "/wchan"), stat = readFile(base + "/stat");
    std::string sys = readFile(base + "/syscall");
    char state = '?';
    size_t rp = stat.rfind(')');
    if (rp != std::string::npos && rp + 2 < stat.size()) state = stat[rp + 2];
    if (sys.size() > 12) sys = sys.substr(0, sys.find(' '));
    Log(0, "  thread %s %-16s state %c wchan %-24s syscall %s", e->d_name, comm.c_str(), state, wchan.c_str(),
        sys.c_str());
    if (++n >= 64) break;
  }
  closedir(d);
}

void WatchdogMain(DeviceData* dd) {
  uint64_t last = dd->presentCount.load();
  auto lastChange = Clock::now();
  int level = 0;
  while (!dd->stopWatchdog.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    uint64_t pc = dd->presentCount.load();
    auto now = Clock::now();
    if (pc != last || dd->swapCount.load() == 0) {
      if (level > 0 && pc != last) Log(0, "stall over, presenting again");
      last = pc;
      lastChange = now;
      level = 0;
      continue;
    }
    double st = std::chrono::duration<double>(now - lastChange).count();
    if ((level == 0 && st > 2.0) || (level == 1 && st > 6.0)) {
      ++level;
      Log(0, "STALL %.0f s without presenting (frame %llu). Layer state: %s. mode=%s", st,
          (unsigned long long)pc, dd->stage.load(), dd->replay ? "replay" : "alternate");
      LogThreads();
      if (dd->replay) {
        if (dd->recLock.try_lock()) {
          Log(0, "  held: %zu fences, %zu timeline semaphores, %zu command buffers -> releasing",
              dd->heldFences.size(), dd->heldTimeline.size(), dd->recCbs.size());
          AbortReplayLocked(dd, "watchdog");
          if (level == 2) {
            Log(0, "  still stalled: switching to alternate mode");
            dd->replay = false;
            dd->recMono = false;
          }
          dd->recLock.unlock();
        } else {
          Log(0, "  replay lock busy (the present thread is blocked: %s)", dd->stage.load());
        }
      }
    }
  }
}

// ------------------------------------------------------------------------------------
// Device creation
// ------------------------------------------------------------------------------------
bool CreateParamBuffer(DeviceData* dd) {
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = 256;
  bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (dd->d.CreateBuffer(dd->device, &bci, nullptr, &dd->paramBuf) != VK_SUCCESS) return false;
  VkMemoryRequirements req;
  dd->d.GetBufferMemoryRequirements(dd->device, dd->paramBuf, &req);
  uint32_t type = FindMemType(dd, req.memoryTypeBits,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VkMemoryAllocateFlagsInfo fi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
  fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fi};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = type;
  if (type == UINT32_MAX || dd->d.AllocateMemory(dd->device, &ai, nullptr, &dd->paramMem) != VK_SUCCESS)
    return false;
  dd->d.BindBufferMemory(dd->device, dd->paramBuf, dd->paramMem, 0);
  void* map = nullptr;
  if (dd->d.MapMemory(dd->device, dd->paramMem, 0, VK_WHOLE_SIZE, 0, &map) == VK_SUCCESS) {
    float init[4] = {EyeK(0), Cfg().convergence, EyeK(0) == 0 ? 0.0f : HudFactor(), 0};
    memcpy(map, init, sizeof(init));
    dd->d.UnmapMemory(dd->device, dd->paramMem);
  }
  VkBufferDeviceAddressInfo bai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
  bai.buffer = dd->paramBuf;
  auto gbda = (dd->apiVersion >= VK_API_VERSION_1_2 && dd->d.GetBufferDeviceAddress)
                  ? dd->d.GetBufferDeviceAddress : dd->d.GetBufferDeviceAddressKHR;
  if (!gbda) gbda = dd->d.GetBufferDeviceAddress;
  if (!gbda) return false;
  dd->paramAddr = gbda(dd->device, &bai);
  return dd->paramAddr != 0;
}

VKAPI_ATTR VkResult VKAPI_CALL hook_CreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo* pci,
                                                 const VkAllocationCallbacks* alloc, VkDevice* pDevice) {
  InstanceData* inst = InstFromPhys(phys);
  auto* chain = (VkLayerDeviceCreateInfo*)pci->pNext;
  while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
                    chain->function == VK_LAYER_LINK_INFO))
    chain = (VkLayerDeviceCreateInfo*)chain->pNext;
  if (!chain || !inst) return VK_ERROR_INITIALIZATION_FAILED;
  PFN_vkSetDeviceLoaderData setLoaderData = nullptr;
  for (auto* c = (VkLayerDeviceCreateInfo*)pci->pNext; c; c = (VkLayerDeviceCreateInfo*)c->pNext)
    if (c->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && c->function == VK_LOADER_DATA_CALLBACK)
      setLoaderData = c->u.pfnSetDeviceLoaderData;

  PFN_vkGetInstanceProcAddr gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  PFN_vkGetDeviceProcAddr gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
  chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
  auto create = (PFN_vkCreateDevice)gipa(inst->instance, "vkCreateDevice");

  // ---- can we do shader stereo? (buffer device address + int64) ----
  VkPhysicalDeviceProperties props{};
  inst->d.GetPhysicalDeviceProperties(phys, &props);
  uint32_t api = std::min(props.apiVersion, inst->apiVersion);
  bool supported = false;
  auto gpf2 = inst->d.GetPhysicalDeviceFeatures2 ? inst->d.GetPhysicalDeviceFeatures2
                                                 : inst->d.GetPhysicalDeviceFeatures2KHR;
  if (gpf2) {
    VkPhysicalDeviceBufferDeviceAddressFeatures bda{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &bda};
    gpf2(phys, &f2);
    supported = bda.bufferDeviceAddress && f2.features.shaderInt64;
  }
  // Proton (DXVK / VKD3D-Proton) always uses Vulkan 1.3. For 1.0 apps BDA would need extra
  // instance extensions, so only Vulkan >= 1.1 is handled (STEREO3D_LEGACY=1 forces it).
  const char* legacy = getenv("STEREO3D_LEGACY");
  bool apiOk = api >= VK_API_VERSION_1_1 || (legacy && legacy[0] == '1');
  bool wantStereo = Cfg().patchShaders && supported && apiOk;

  VkDeviceCreateInfo ci = *pci;
  std::vector<const char*> exts;
  for (uint32_t i = 0; i < pci->enabledExtensionCount; ++i) {
    bool always = false;
    for (const char* h : kAlwaysHidden) always |= !strcmp(h, pci->ppEnabledExtensionNames[i]);
    if (always && !Cfg().passthrough) {
      Log(1, "not enabling %s (incompatible with the stereo swapchain)", pci->ppEnabledExtensionNames[i]);
      continue;
    }
    exts.push_back(pci->ppEnabledExtensionNames[i]);
  }
  ci.enabledExtensionCount = uint32_t(exts.size());
  ci.ppEnabledExtensionNames = exts.data();
  VkPhysicalDeviceFeatures featCopy{};
  VkPhysicalDeviceBufferDeviceAddressFeatures ourBda{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
  if (wantStereo) {
    if (api < VK_API_VERSION_1_2) {
      bool have = false;
      for (const char* e : exts) have |= !strcmp(e, "VK_KHR_buffer_device_address");
      if (!have) exts.push_back("VK_KHR_buffer_device_address");
      ci.enabledExtensionCount = uint32_t(exts.size());
      ci.ppEnabledExtensionNames = exts.data();
    }
    bool haveBda = false, haveI64 = false;
    for (auto* p = (VkBaseOutStructure*)ci.pNext; p; p = p->pNext) {
      switch (p->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
          ((VkPhysicalDeviceVulkan12Features*)p)->bufferDeviceAddress = VK_TRUE;
          haveBda = true;
          break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES:
          ((VkPhysicalDeviceBufferDeviceAddressFeatures*)p)->bufferDeviceAddress = VK_TRUE;
          haveBda = true;
          break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2:
          ((VkPhysicalDeviceFeatures2*)p)->features.shaderInt64 = VK_TRUE;
          haveI64 = true;
          break;
        default: break;
      }
    }
    if (!haveI64) {
      if (ci.pEnabledFeatures) featCopy = *ci.pEnabledFeatures;
      featCopy.shaderInt64 = VK_TRUE;
      ci.pEnabledFeatures = &featCopy;
    }
    if (!haveBda) {
      ourBda.bufferDeviceAddress = VK_TRUE;
      ourBda.pNext = (void*)ci.pNext;
      ci.pNext = &ourBda;
    }
  } else {
    Log(0, "shader stereo NOT available (bda+int64 supported=%d, api>=1.1=%d, patch=%d)",
        supported, apiOk, Cfg().patchShaders);
  }

  VkResult r = create(phys, &ci, alloc, pDevice);
  if (r != VK_SUCCESS) return r;

  auto* dd = new DeviceData();
  dd->device = *pDevice;
  dd->phys = phys;
  dd->inst = inst;
  dd->setLoaderData = setLoaderData;
  dd->apiVersion = api;
  dd->d.GetDeviceProcAddr = gdpa;
#define LOAD(n) dd->d.n = (PFN_vk##n)gdpa(*pDevice, "vk" #n);
  DEV_FUNCS(LOAD)
#undef LOAD
  inst->d.GetPhysicalDeviceMemoryProperties(phys, &dd->memProps);

  uint32_t qfCount = 0;
  inst->d.GetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, nullptr);
  std::vector<VkQueueFamilyProperties> qf(qfCount);
  inst->d.GetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, qf.data());
  for (uint32_t i = 0; i < pci->queueCreateInfoCount; ++i) {
    uint32_t fam = pci->pQueueCreateInfos[i].queueFamilyIndex;
    if (fam < qfCount && (qf[fam].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
      dd->gfxFamily = fam;
      break;
    }
  }

  if (wantStereo) {
    dd->stereoShaders = CreateParamBuffer(dd);
    dd->replay = dd->stereoShaders && Cfg().mode == 1;
    if (!dd->stereoShaders) Log(0, "failed to create stereo parameter buffer");
  }
  Log(1, "device '%s' api %u.%u: shader stereo %s, mode %s, param @0x%llx", props.deviceName,
      VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), dd->stereoShaders ? "ON" : "OFF",
      dd->replay ? "replay (both eyes every frame)" : "alternate (L/R frames)", (unsigned long long)dd->paramAddr);

  if (!Cfg().passthrough) dd->watchdog = std::thread(WatchdogMain, dd);
  std::lock_guard<std::mutex> l(g_lock);
  g_devByKey[Key(*pDevice)] = dd;
  return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL hook_DestroyDevice(VkDevice device, const VkAllocationCallbacks* a) {
  DeviceData* dd = Dev(device);
  if (!dd) return;
  dd->stopWatchdog = true;
  if (dd->watchdog.joinable()) dd->watchdog.join();
  {
    std::lock_guard<std::mutex> l(dd->shaderLock);
    for (auto& m : dd->modules)
      for (auto& v : m.second.variants)
        if (v.patched) dd->d.DestroyShaderModule(device, v.patched, nullptr);
    dd->modules.clear();
  }
  if (dd->outSurface) dd->inst->d.DestroySurfaceKHR(dd->inst->instance, dd->outSurface, nullptr);
  if (dd->out) DestroyOutputWindow(dd->out);
  if (dd->paramBuf) dd->d.DestroyBuffer(device, dd->paramBuf, nullptr);
  if (dd->paramMem) dd->d.FreeMemory(device, dd->paramMem, nullptr);
  dd->d.DestroyDevice(device, a);
  {
    std::lock_guard<std::mutex> l(g_lock);
    g_devByKey.erase(Key(device));
  }
  delete dd;
}

void RegisterQueue(DeviceData* dd, VkQueue q, uint32_t family) {
  QueueInfo* qi = GetQueueInfo(dd, q);
  qi->family = family;
  if (family == dd->gfxFamily && !dd->gfxQueue) dd->gfxQueue = q;
}

VKAPI_ATTR void VKAPI_CALL hook_GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* q) {
  DeviceData* dd = Dev(device);
  dd->d.GetDeviceQueue(device, family, index, q);
  if (*q) RegisterQueue(dd, *q, family);
}
VKAPI_ATTR void VKAPI_CALL hook_GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* q) {
  DeviceData* dd = Dev(device);
  dd->d.GetDeviceQueue2(device, info, q);
  if (*q) RegisterQueue(dd, *q, info->queueFamilyIndex);
}

// ------------------------------------------------------------------------------------
// Queue ops. In replay mode the frame's command buffers are recorded and the timeline
// signals / fences of the present queue are held back until the right-eye replay is done.
// ------------------------------------------------------------------------------------
bool IsTimeline(DeviceData* dd, VkSemaphore sem) {
  std::lock_guard<std::mutex> l(dd->semLock);
  return dd->timelineSems.count(sem) != 0;
}

// caller holds dd->recLock and the queue lock of q
void ReleaseHeldLocked(DeviceData* dd, VkQueue q) {
  if (dd->heldTimeline.empty() && dd->heldFences.empty()) return;
  std::vector<VkSemaphore> sems;
  std::vector<uint64_t> vals;
  for (auto& kv : dd->heldTimeline) { sems.push_back(kv.first); vals.push_back(kv.second); }
  VkTimelineSemaphoreSubmitInfo tl{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
  tl.signalSemaphoreValueCount = uint32_t(vals.size());
  tl.pSignalSemaphoreValues = vals.data();
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, sems.empty() ? nullptr : &tl};
  si.signalSemaphoreCount = uint32_t(sems.size());
  si.pSignalSemaphores = sems.data();
  size_t fi = 0;
  VkFence f0 = dd->heldFences.empty() ? VK_NULL_HANDLE : dd->heldFences[fi++];
  dd->d.QueueSubmit(q, 1, &si, f0);
  for (; fi < dd->heldFences.size(); ++fi) {
    VkSubmitInfo e{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    dd->d.QueueSubmit(q, 1, &e, dd->heldFences[fi]);
  }
  dd->heldTimeline.clear();
  dd->heldFences.clear();
}

// The app needs a held signal before the frame is complete: no replay for this frame.
// caller holds dd->recLock (not the queue lock)
void AbortReplayLocked(DeviceData* dd, const char* why) {
  if (!dd->recMono) {
    dd->recMono = true;
    ++dd->monoFrames;
    dd->lastAbortPresent = dd->presentCount.load();
    Log(2, "replay skipped this frame: %s", why);
  }
  dd->recCbs.clear();
  if (dd->gfxQueue) {
    std::lock_guard<std::mutex> l(GetQueueInfo(dd, dd->gfxQueue)->m);
    ReleaseHeldLocked(dd, dd->gfxQueue);
  }
}

bool HeldLocked(DeviceData* dd, VkSemaphore sem, uint64_t value) {
  auto it = dd->heldTimeline.find(sem);
  return it != dd->heldTimeline.end() && value <= it->second;
}
bool WaitsOnHeldLocked(DeviceData* dd, uint32_t n, const VkSubmitInfo* s) {
  if (dd->heldTimeline.empty()) return false;
  for (uint32_t i = 0; i < n; ++i) {
    const VkTimelineSemaphoreSubmitInfo* tl = nullptr;
    for (auto* p = (const VkBaseInStructure*)s[i].pNext; p; p = p->pNext)
      if (p->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) tl = (const VkTimelineSemaphoreSubmitInfo*)p;
    if (!tl || !tl->pWaitSemaphoreValues) continue;
    for (uint32_t w = 0; w < s[i].waitSemaphoreCount && w < tl->waitSemaphoreValueCount; ++w)
      if (HeldLocked(dd, s[i].pWaitSemaphores[w], tl->pWaitSemaphoreValues[w])) return true;
  }
  return false;
}
bool WaitsOnHeldLocked(DeviceData* dd, uint32_t n, const VkSubmitInfo2* s) {
  if (dd->heldTimeline.empty()) return false;
  for (uint32_t i = 0; i < n; ++i)
    for (uint32_t w = 0; w < s[i].waitSemaphoreInfoCount; ++w)
      if (HeldLocked(dd, s[i].pWaitSemaphoreInfos[w].semaphore, s[i].pWaitSemaphoreInfos[w].value)) return true;
  return false;
}

bool RecordingQueue(DeviceData* dd, VkQueue q) {
  return dd->replay && q == dd->gfxQueue && dd->swapCount.load() > 0;
}

VKAPI_ATTR VkResult VKAPI_CALL hook_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo* s, VkFence f) {
  DeviceData* dd = Dev(q);
  if (!dd->replay) {
    std::lock_guard<std::mutex> l(GetQueueInfo(dd, q)->m);
    return dd->d.QueueSubmit(q, n, s, f);
  }
  std::unique_lock<std::mutex> rl(dd->recLock);
  if (WaitsOnHeldLocked(dd, n, s)) AbortReplayLocked(dd, "submit waits on a held signal");
  if (!RecordingQueue(dd, q) || dd->recMono) {
    rl.unlock();
    std::lock_guard<std::mutex> l(GetQueueInfo(dd, q)->m);
    return dd->d.QueueSubmit(q, n, s, f);
  }
  std::vector<VkSubmitInfo> si(s, s + n);
  std::vector<VkTimelineSemaphoreSubmitInfo> tls(n);
  std::vector<std::vector<VkSemaphore>> sigs(n);
  std::vector<std::vector<uint64_t>> vals(n);
  for (uint32_t i = 0; i < n; ++i) {
    dd->recCbs.insert(dd->recCbs.end(), s[i].pCommandBuffers, s[i].pCommandBuffers + s[i].commandBufferCount);
    const VkTimelineSemaphoreSubmitInfo* tl = nullptr;
    for (auto* p = (const VkBaseInStructure*)s[i].pNext; p; p = p->pNext)
      if (p->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) tl = (const VkTimelineSemaphoreSubmitInfo*)p;
    bool changed = false;
    for (uint32_t k = 0; k < s[i].signalSemaphoreCount; ++k) {
      VkSemaphore sem = s[i].pSignalSemaphores[k];
      uint64_t v = (tl && tl->pSignalSemaphoreValues && k < tl->signalSemaphoreValueCount)
                       ? tl->pSignalSemaphoreValues[k] : 0;
      if (IsTimeline(dd, sem)) {
        uint64_t& h = dd->heldTimeline[sem];
        h = std::max(h, v);
        changed = true;
      } else {
        sigs[i].push_back(sem);
        vals[i].push_back(v);
      }
    }
    if (changed) {
      si[i].signalSemaphoreCount = uint32_t(sigs[i].size());
      si[i].pSignalSemaphores = sigs[i].data();
      if (tl) {
        tls[i] = *tl;
        tls[i].pNext = nullptr;
        tls[i].signalSemaphoreValueCount = uint32_t(vals[i].size());
        tls[i].pSignalSemaphoreValues = vals[i].data();
        si[i].pNext = &tls[i];
      }
    }
  }
  if (f) dd->heldFences.push_back(f);
  std::lock_guard<std::mutex> l(GetQueueInfo(dd, q)->m);
  return dd->d.QueueSubmit(q, n, si.data(), VK_NULL_HANDLE);
}

VkResult Submit2Common(DeviceData* dd, VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f,
                       PFN_vkQueueSubmit2 fn) {
  if (!dd->replay) {
    std::lock_guard<std::mutex> l(GetQueueInfo(dd, q)->m);
    return fn(q, n, s, f);
  }
  std::unique_lock<std::mutex> rl(dd->recLock);
  if (WaitsOnHeldLocked(dd, n, s)) AbortReplayLocked(dd, "submit waits on a held signal");
  if (!RecordingQueue(dd, q) || dd->recMono) {
    rl.unlock();
    std::lock_guard<std::mutex> l(GetQueueInfo(dd, q)->m);
    return fn(q, n, s, f);
  }
  std::vector<VkSubmitInfo2> si(s, s + n);
  std::vector<std::vector<VkSemaphoreSubmitInfo>> sigs(n);
  for (uint32_t i = 0; i < n; ++i) {
    for (uint32_t c = 0; c < s[i].commandBufferInfoCount; ++c)
      dd->recCbs.push_back(s[i].pCommandBufferInfos[c].commandBuffer);
    bool changed = false;
    for (uint32_t k = 0; k < s[i].signalSemaphoreInfoCount; ++k) {
      const VkSemaphoreSubmitInfo& ss = s[i].pSignalSemaphoreInfos[k];
      if (IsTimeline(dd, ss.semaphore)) {
        uint64_t& h = dd->heldTimeline[ss.semaphore];
        h = std::max(h, ss.value);
        changed = true;
      } else {
        sigs[i].push_back(ss);
      }
    }
    if (changed) {
      si[i].signalSemaphoreInfoCount = uint32_t(sigs[i].size());
      si[i].pSignalSemaphoreInfos = sigs[i].data();
    }
  }
  if (f) dd->heldFences.push_back(f);
  std::lock_guard<std::mutex> l(GetQueueInfo(dd, q)->m);
  return fn(q, n, si.data(), VK_NULL_HANDLE);
}
VKAPI_ATTR VkResult VKAPI_CALL hook_QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f) {
  DeviceData* dd = Dev(q);
  return Submit2Common(dd, q, n, s, f, dd->d.QueueSubmit2);
}
VKAPI_ATTR VkResult VKAPI_CALL hook_QueueSubmit2KHR(VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f) {
  DeviceData* dd = Dev(q);
  return Submit2Common(dd, q, n, s, f, dd->d.QueueSubmit2KHR);
}
VKAPI_ATTR VkResult VKAPI_CALL hook_QueueWaitIdle(VkQueue q) {
  DeviceData* dd = Dev(q);
  if (dd->replay && q == dd->gfxQueue) {
    std::lock_guard<std::mutex> rl(dd->recLock);
    if (!dd->heldFences.empty() || !dd->heldTimeline.empty() || !dd->recCbs.empty())
      AbortReplayLocked(dd, "vkQueueWaitIdle");
  }
  std::lock_guard<std::mutex> l(GetQueueInfo(dd, q)->m);
  return dd->d.QueueWaitIdle(q);
}
VKAPI_ATTR VkResult VKAPI_CALL hook_QueueBindSparse(VkQueue q, uint32_t n, const VkBindSparseInfo* b, VkFence f) {
  DeviceData* dd = Dev(q);
  std::lock_guard<std::mutex> l(GetQueueInfo(dd, q)->m);
  return dd->d.QueueBindSparse(q, n, b, f);
}
VKAPI_ATTR VkResult VKAPI_CALL hook_DeviceWaitIdle(VkDevice device) {
  DeviceData* dd = Dev(device);
  if (!dd->gfxQueue) return dd->d.DeviceWaitIdle(device);
  if (dd->replay) {
    std::lock_guard<std::mutex> rl(dd->recLock);
    if (!dd->heldFences.empty() || !dd->heldTimeline.empty() || !dd->recCbs.empty())
      AbortReplayLocked(dd, "vkDeviceWaitIdle");
  }
  std::lock_guard<std::mutex> l(GetQueueInfo(dd, dd->gfxQueue)->m);
  return dd->d.DeviceWaitIdle(device);
}

// CPU waits: if the app blocks on a held signal and no present arrives, it is waiting for
// something mid-frame (e.g. a GPU readback) -> release and skip the replay for this frame.
template <class WaitFn, class HeldFn>
VkResult WaitWithReplay(DeviceData* dd, uint64_t timeout, WaitFn waitFn, HeldFn heldFn) {
  if (!dd->replay || timeout == 0) return waitFn(timeout);
  const auto begin = Clock::now();
  auto stallStart = begin;
  uint64_t seenPresent = dd->presentCount.load();
  for (;;) {
    auto now = Clock::now();
    uint64_t total = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now - begin).count());
    uint64_t slice = 2000000ull;  // 2 ms
    if (timeout != UINT64_MAX) {
      if (total >= timeout) return VK_TIMEOUT;
      slice = std::min(slice, timeout - total);
    }
    VkResult r = waitFn(slice);
    if (r != VK_TIMEOUT) return r;
    uint64_t pc = dd->presentCount.load();
    if (pc != seenPresent) {  // frames are still being presented: a normal wait
      seenPresent = pc;
      stallStart = Clock::now();
      continue;
    }
    uint64_t stalled = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - stallStart).count());
    bool recentAbort = pc - dd->lastAbortPresent.load() < 120;
    if (stalled > (recentAbort ? 3000000ull : 40000000ull)) {
      std::lock_guard<std::mutex> rl(dd->recLock);
      if (heldFn()) AbortReplayLocked(dd, "CPU waits for a held signal mid-frame");
      stallStart = Clock::now();
    }
  }
}

VKAPI_ATTR VkResult VKAPI_CALL hook_WaitForFences(VkDevice device, uint32_t n, const VkFence* fences, VkBool32 all,
                                                  uint64_t timeout) {
  DeviceData* dd = Dev(device);
  return WaitWithReplay(
      dd, timeout, [&](uint64_t t) { return dd->d.WaitForFences(device, n, fences, all, t); },
      [&]() {
        for (uint32_t i = 0; i < n; ++i)
          if (std::find(dd->heldFences.begin(), dd->heldFences.end(), fences[i]) != dd->heldFences.end())
            return true;
        return false;
      });
}
VkResult WaitSemCommon(DeviceData* dd, VkDevice device, const VkSemaphoreWaitInfo* info, uint64_t timeout,
                       PFN_vkWaitSemaphores fn) {
  return WaitWithReplay(
      dd, timeout, [&](uint64_t t) { return fn(device, info, t); },
      [&]() {
        for (uint32_t i = 0; i < info->semaphoreCount; ++i)
          if (HeldLocked(dd, info->pSemaphores[i], info->pValues[i])) return true;
        return false;
      });
}
VKAPI_ATTR VkResult VKAPI_CALL hook_WaitSemaphores(VkDevice device, const VkSemaphoreWaitInfo* info, uint64_t t) {
  DeviceData* dd = Dev(device);
  return WaitSemCommon(dd, device, info, t, dd->d.WaitSemaphores);
}
VKAPI_ATTR VkResult VKAPI_CALL hook_WaitSemaphoresKHR(VkDevice device, const VkSemaphoreWaitInfo* info, uint64_t t) {
  DeviceData* dd = Dev(device);
  return WaitSemCommon(dd, device, info, t, dd->d.WaitSemaphoresKHR);
}

VKAPI_ATTR VkResult VKAPI_CALL hook_CreateSemaphore(VkDevice device, const VkSemaphoreCreateInfo* ci,
                                                    const VkAllocationCallbacks* a, VkSemaphore* sem) {
  DeviceData* dd = Dev(device);
  VkResult r = dd->d.CreateSemaphore(device, ci, a, sem);
  if (r == VK_SUCCESS)
    for (auto* p = (const VkBaseInStructure*)ci->pNext; p; p = p->pNext)
      if (p->sType == VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO &&
          ((const VkSemaphoreTypeCreateInfo*)p)->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE) {
        std::lock_guard<std::mutex> l(dd->semLock);
        dd->timelineSems.insert(*sem);
      }
  return r;
}
VKAPI_ATTR void VKAPI_CALL hook_DestroySemaphore(VkDevice device, VkSemaphore sem, const VkAllocationCallbacks* a) {
  DeviceData* dd = Dev(device);
  {
    std::lock_guard<std::mutex> l(dd->semLock);
    dd->timelineSems.erase(sem);
  }
  dd->d.DestroySemaphore(device, sem, a);
}

VKAPI_ATTR VkResult VKAPI_CALL hook_BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo* bi) {
  DeviceData* dd = Dev(cb);
  if (!dd->replay) return dd->d.BeginCommandBuffer(cb, bi);
  VkCommandBufferBeginInfo b = *bi;  // the frame is submitted twice
  b.flags &= ~VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  b.flags |= VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
  return dd->d.BeginCommandBuffer(cb, &b);
}

// ------------------------------------------------------------------------------------
// Shader patching
// ------------------------------------------------------------------------------------
VKAPI_ATTR VkResult VKAPI_CALL hook_CreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo* ci,
                                                       const VkAllocationCallbacks* a, VkShaderModule* m) {
  DeviceData* dd = Dev(device);
  VkResult r = dd->d.CreateShaderModule(device, ci, a, m);
  if (r == VK_SUCCESS && dd->stereoShaders && ci->codeSize % 4 == 0 &&
      HasPreRasterEntryPoint(ci->pCode, ci->codeSize / 4)) {
    std::lock_guard<std::mutex> l(dd->shaderLock);
    dd->modules[*m].code.assign(ci->pCode, ci->pCode + ci->codeSize / 4);
  }
  return r;
}

VKAPI_ATTR void VKAPI_CALL hook_DestroyShaderModule(VkDevice device, VkShaderModule m, const VkAllocationCallbacks* a) {
  DeviceData* dd = Dev(device);
  {
    std::lock_guard<std::mutex> l(dd->shaderLock);
    auto it = dd->modules.find(m);
    if (it != dd->modules.end()) {
      for (auto& v : it->second.variants)
        if (v.patched) dd->d.DestroyShaderModule(device, v.patched, nullptr);
      dd->modules.erase(it);
    }
  }
  dd->d.DestroyShaderModule(device, m, a);
}

uint32_t StageToModel(VkShaderStageFlagBits s) {
  switch (s) {
    case VK_SHADER_STAGE_VERTEX_BIT: return 0;
    case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: return 2;
    case VK_SHADER_STAGE_GEOMETRY_BIT: return 3;
    default: return UINT32_MAX;
  }
}

bool PatchCode(DeviceData* dd, const uint32_t* code, size_t words, uint32_t model, const char* entry,
               std::vector<uint32_t>& out) {
  std::string log;
  PatchResult pr = PatchStereo(code, words, entry, model, dd->paramAddr, out, &log);
  if (pr == PatchResult::Patched) {
    uint32_t n = ++dd->patchedCount;
    Log(2, "patched shader #%u (model %u, %s)", n, model, log.c_str());
    return true;
  }
  ++dd->skippedCount;
  Log(2, "shader not patched: %s (model %u)", PatchResultName(pr), model);
  return false;
}

VkShaderModule GetPatchedModule(DeviceData* dd, VkShaderModule m, uint32_t model, const char* entry) {
  std::lock_guard<std::mutex> l(dd->shaderLock);
  auto it = dd->modules.find(m);
  if (it == dd->modules.end()) return VK_NULL_HANDLE;
  for (auto& v : it->second.variants)
    if (v.model == model && v.entry == entry) return v.patched;
  std::vector<uint32_t> out;
  VkShaderModule pm = VK_NULL_HANDLE;
  if (PatchCode(dd, it->second.code.data(), it->second.code.size(), model, entry, out)) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = out.size() * 4;
    ci.pCode = out.data();
    if (dd->d.CreateShaderModule(dd->device, &ci, nullptr, &pm) != VK_SUCCESS) pm = VK_NULL_HANDLE;
  }
  it->second.variants.push_back({model, entry, pm});
  return pm;
}

VKAPI_ATTR VkResult VKAPI_CALL hook_CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                                            const VkGraphicsPipelineCreateInfo* infos,
                                                            const VkAllocationCallbacks* a, VkPipeline* out) {
  DeviceData* dd = Dev(device);
  if (!dd->stereoShaders) return dd->d.CreateGraphicsPipelines(device, cache, count, infos, a, out);

  std::vector<VkGraphicsPipelineCreateInfo> ci(infos, infos + count);
  std::vector<std::vector<VkPipelineShaderStageCreateInfo>> stages(count);
  struct Restore { VkShaderModuleCreateInfo* smci; const uint32_t* code; size_t size; };
  std::vector<Restore> restores;
  std::vector<std::unique_ptr<std::vector<uint32_t>>> inlineCode;

  for (uint32_t i = 0; i < count; ++i) {
    if (!ci[i].pStages || !ci[i].stageCount) continue;
    int best = -1, bestRank = -1;
    for (uint32_t s = 0; s < ci[i].stageCount; ++s) {
      int rank = -1;
      switch (ci[i].pStages[s].stage) {
        case VK_SHADER_STAGE_VERTEX_BIT: rank = 0; break;
        case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: rank = 1; break;
        case VK_SHADER_STAGE_GEOMETRY_BIT: rank = 2; break;
        default: break;
      }
      if (rank > bestRank) { bestRank = rank; best = int(s); }
    }
    if (best < 0) continue;
    ++dd->pipelineCount;
    const VkPipelineShaderStageCreateInfo& st = ci[i].pStages[best];
    uint32_t model = StageToModel(st.stage);
    if (st.module != VK_NULL_HANDLE) {
      VkShaderModule pm = GetPatchedModule(dd, st.module, model, st.pName);
      if (pm) {
        stages[i].assign(ci[i].pStages, ci[i].pStages + ci[i].stageCount);
        stages[i][best].module = pm;
        ci[i].pStages = stages[i].data();
      }
    } else {
      // VK_KHR_maintenance5: shader code chained inline
      for (auto* p = (VkBaseOutStructure*)st.pNext; p; p = p->pNext) {
        if (p->sType != VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) continue;
        auto* smci = (VkShaderModuleCreateInfo*)p;
        auto buf = std::make_unique<std::vector<uint32_t>>();
        if (PatchCode(dd, smci->pCode, smci->codeSize / 4, model, st.pName, *buf)) {
          restores.push_back({smci, smci->pCode, smci->codeSize});
          smci->pCode = buf->data();
          smci->codeSize = buf->size() * 4;
          inlineCode.push_back(std::move(buf));
        }
        break;
      }
    }
  }
  VkResult r = dd->d.CreateGraphicsPipelines(device, cache, count, ci.data(), a, out);
  for (auto& rs : restores) {
    rs.smci->pCode = rs.code;
    rs.smci->codeSize = rs.size;
  }
  return r;
}

// ------------------------------------------------------------------------------------
// Virtual swapchain
// ------------------------------------------------------------------------------------
void ComputeRects(Swapchain* sc);
bool HalfSbs(const VkExtent2D& r);
Swapchain* FindSwapchain(DeviceData* dd, VkSwapchainKHR h);

bool CreateImageMem(DeviceData* dd, VkImageCreateInfo ici, VkImage* img, VkDeviceMemory* mem) {
  VkResult r = dd->d.CreateImage(dd->device, &ici, nullptr, img);
  if (r != VK_SUCCESS && (ici.usage & VK_IMAGE_USAGE_STORAGE_BIT)) {
    ici.usage &= ~VK_IMAGE_USAGE_STORAGE_BIT;
    r = dd->d.CreateImage(dd->device, &ici, nullptr, img);
  }
  if (r != VK_SUCCESS) return false;
  VkMemoryRequirements req;
  dd->d.GetImageMemoryRequirements(dd->device, *img, &req);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = FindMemType(dd, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (dd->d.AllocateMemory(dd->device, &ai, nullptr, mem) != VK_SUCCESS) return false;
  return dd->d.BindImageMemory(dd->device, *img, *mem, 0) == VK_SUCCESS;
}

void DestroySwapchainResources(DeviceData* dd, Swapchain* sc) {
  VkDevice dev = dd->device;
  for (auto& v : sc->v) {
    if (v.fence) dd->d.WaitForFences(dev, 1, &v.fence, VK_TRUE, 2000000000ull);
  }
  for (auto& v : sc->v) {
    if (v.cmd) dd->d.FreeCommandBuffers(dev, sc->pool, 1, &v.cmd);
    if (v.cmd2) dd->d.FreeCommandBuffers(dev, sc->pool, 1, &v.cmd2);
    if (v.fence) dd->d.DestroyFence(dev, v.fence, nullptr);
    if (v.acqSem) dd->d.DestroySemaphore(dev, v.acqSem, nullptr);
    if (v.doneSem) dd->d.DestroySemaphore(dev, v.doneSem, nullptr);
    if (v.image) dd->d.DestroyImage(dev, v.image, nullptr);
    if (v.mem) dd->d.FreeMemory(dev, v.mem, nullptr);
  }
  if (sc->left) dd->d.DestroyImage(dev, sc->left, nullptr);
  if (sc->leftMem) dd->d.FreeMemory(dev, sc->leftMem, nullptr);
  if (sc->pool) dd->d.DestroyCommandPool(dev, sc->pool, nullptr);
}

VKAPI_ATTR VkResult VKAPI_CALL hook_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* ci,
                                                       const VkAllocationCallbacks* a, VkSwapchainKHR* pSwapchain) {
  DeviceData* dd = Dev(device);
  InstanceData* inst = dd->inst;
  if (Cfg().passthrough) return dd->d.CreateSwapchainKHR(device, ci, a, pSwapchain);
  // Present on our own full-screen window on the glasses (independent of the game window size)
  // Use our own window only when the game window does not already cover the glasses (32:9).
  VkSurfaceCapabilitiesKHR gameCaps{};
  inst->d.GetPhysicalDeviceSurfaceCapabilitiesKHR(dd->phys, ci->surface, &gameCaps);
  const VkExtent2D ge = gameCaps.currentExtent;
  const bool gameWide = ge.width != 0xFFFFFFFFu && uint64_t(ge.width) * 10 >= uint64_t(ge.height) * 25;
  const bool wantOwn = Cfg().ownWindow == 1 || (Cfg().ownWindow < 0 && !gameWide);
  if (wantOwn && dd->outState == 0) {
    dd->outState = -1;
    if (inst->createXlibSurface && dd->gfxFamily != UINT32_MAX) {
      dd->out = CreateOutputWindow();
      if (dd->out) {
        XlibSurfaceCI xci{VkStructureType(1000004000) /*XLIB_SURFACE_CREATE_INFO_KHR*/, nullptr, 0,
                          OutputDisplay(dd->out), OutputXid(dd->out)};
        VkBool32 sup = VK_FALSE;
        if (((PFN_CreateXlibSurface)inst->createXlibSurface)(inst->instance, &xci, nullptr, &dd->outSurface) ==
                VK_SUCCESS &&
            inst->d.GetPhysicalDeviceSurfaceSupportKHR(dd->phys, dd->gfxFamily, dd->outSurface, &sup) == VK_SUCCESS &&
            sup) {
          dd->outState = 1;
        } else {
          Log(0, "own output window: cannot present to it, using the game window");
          if (dd->outSurface) inst->d.DestroySurfaceKHR(inst->instance, dd->outSurface, nullptr);
          dd->outSurface = VK_NULL_HANDLE;
          DestroyOutputWindow(dd->out);
          dd->out = nullptr;
        }
      }
    }
  }
  const bool useOwn = wantOwn && dd->outState == 1;
  if (dd->out) SetOutputWindowVisible(dd->out, useOwn);
  const VkSurfaceKHR realSurface = useOwn ? dd->outSurface : ci->surface;
  VkSurfaceCapabilitiesKHR caps{};
  inst->d.GetPhysicalDeviceSurfaceCapabilitiesKHR(dd->phys, realSurface, &caps);  // real caps
  if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) || dd->gfxFamily == UINT32_MAX) {
    Log(0, "surface does not support transfer dst / no graphics queue: stereo disabled for this swapchain");
    return dd->d.CreateSwapchainKHR(device, ci, a, pSwapchain);
  }

  VkExtent2D rext = caps.currentExtent;
  if (rext.width == 0xFFFFFFFFu) {
    rext = {ci->imageExtent.width * 2, ci->imageExtent.height};
    rext.width = std::clamp(rext.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    rext.height = std::clamp(rext.height, caps.minImageExtent.height, caps.maxImageExtent.height);
  }

  VkSwapchainCreateInfoKHR rci = *ci;
  rci.surface = realSurface;
  rci.imageExtent = rext;
  // the format the game asked for must exist on our surface; otherwise pick one and blit
  VkFormat realFormat = ci->imageFormat;
  VkColorSpaceKHR realSpace = ci->imageColorSpace;
  {
    uint32_t nf = 0;
    inst->d.GetPhysicalDeviceSurfaceFormatsKHR(dd->phys, realSurface, &nf, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(nf);
    inst->d.GetPhysicalDeviceSurfaceFormatsKHR(dd->phys, realSurface, &nf, fmts.data());
    bool found = false;
    for (auto& f : fmts) found |= f.format == realFormat && f.colorSpace == realSpace;
    if (!found && nf) {
      realFormat = fmts[0].format;
      realSpace = fmts[0].colorSpace;
      for (auto& f : fmts)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM) { realFormat = f.format; realSpace = f.colorSpace; }
    }
  }
  rci.imageFormat = realFormat;
  rci.imageColorSpace = realSpace;
  if (realFormat != ci->imageFormat) {  // the game's format list does not apply to ours
    rci.flags &= ~VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;
    rci.pNext = nullptr;
  }
  // oldSwapchain must belong to the same surface
  if (ci->oldSwapchain) {
    Swapchain* old = FindSwapchain(dd, ci->oldSwapchain);
    if (old && old->onOwn != useOwn) rci.oldSwapchain = VK_NULL_HANDLE;
  }
  if (useOwn) {  // our surface: the game's queue family list / sharing still apply
    rci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                             ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : ci->compositeAlpha;
    rci.preTransform = caps.currentTransform;
  }
  rci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) rci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  // present mode
  uint32_t nm = 0;
  inst->d.GetPhysicalDeviceSurfacePresentModesKHR(dd->phys, realSurface, &nm, nullptr);
  std::vector<VkPresentModeKHR> modes(nm);
  inst->d.GetPhysicalDeviceSurfacePresentModesKHR(dd->phys, realSurface, &nm, modes.data());
  VkPresentModeKHR want = VkPresentModeKHR(Cfg().presentMode);
  rci.presentMode = std::find(modes.begin(), modes.begin() + nm, want) != modes.begin() + nm
                        ? want : VK_PRESENT_MODE_FIFO_KHR;
  rci.minImageCount = std::max(ci->minImageCount, caps.minImageCount);
  if (caps.maxImageCount) rci.minImageCount = std::min(rci.minImageCount, caps.maxImageCount);

  VkResult r = dd->d.CreateSwapchainKHR(device, &rci, a, pSwapchain);
  if (r != VK_SUCCESS) return r;

  auto* sc = new Swapchain();
  sc->handle = *pSwapchain;
  sc->format = ci->imageFormat;
  sc->realFormat = realFormat;
  sc->formatDiffers = realFormat != ci->imageFormat;
  sc->onOwn = useOwn;
  Log(1, "game window %ux%u -> presenting on %s", ge.width, ge.height, useOwn ? "own full-screen window" : "game window");
  sc->vext = ci->imageExtent;
  sc->rext = rext;
  // Crop: if the game image itself is much wider than one eye (e.g. Proton scaled a
  // 1920x1080 fullscreen game into the 3840x1080 window with black bars), take the
  // centred region that has the per-eye aspect ratio.
  sc->srcOff = {0, 0};
  sc->srcExt = sc->vext;
  sc->dstOff = {0, 0};
  sc->dstExt = {rext.width / 2, rext.height};
  {
    double eyeAspect = double(rext.width / 2) / double(rext.height) * (HalfSbs(rext) ? 2.0 : 1.0);
    double gameAspect = double(sc->vext.width) / double(sc->vext.height);
    int crop = Cfg().crop;
    bool wide = gameAspect > eyeAspect * 1.25;
    sc->detectOn = crop == 2 || (crop < 0 && wide);
    bool doCrop = crop == 1 || (crop < 0 && wide);
    sc->cx0 = 0; sc->cy0 = 0; sc->cx1 = int(sc->vext.width); sc->cy1 = int(sc->vext.height);
    if (doCrop) {
      uint32_t w = std::min<uint32_t>(sc->vext.width, uint32_t(sc->vext.height * eyeAspect + 0.5));
      sc->cx0 = int((sc->vext.width - w) / 2);
      sc->cx1 = sc->cx0 + int(w);
      Log(1, "game image %ux%u is wider than one eye: using centred %ux%u region (crop=%s)%s",
          sc->vext.width, sc->vext.height, w, sc->vext.height, crop == 1 ? "center" : "auto",
          sc->detectOn ? ", detecting black bars..." : "");
    }
    ComputeRects(sc);
  }
  uint32_t n = 0;
  dd->d.GetSwapchainImagesKHR(device, sc->handle, &n, nullptr);
  sc->realImages.resize(n);
  dd->d.GetSwapchainImagesKHR(device, sc->handle, &n, sc->realImages.data());

  VkFormatProperties fp{};
  inst->d.GetPhysicalDeviceFormatProperties(dd->phys, sc->format, &fp);
  VkFormatProperties fpr{};
  inst->d.GetPhysicalDeviceFormatProperties(dd->phys, realFormat, &fpr);
  sc->canBlit = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) &&
                (fpr.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT);

  bool ok = true;
  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = dd->gfxFamily;
  sc->poolFamily = dd->gfxFamily;
  ok &= dd->d.CreateCommandPool(device, &pci, nullptr, &sc->pool) == VK_SUCCESS;

  // virtual images handed to the game
  VkImageFormatListCreateInfo fmtList{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
  bool haveFmtList = false;
  for (auto* p = (const VkBaseInStructure*)ci->pNext; p; p = p->pNext)
    if (p->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) {
      fmtList = *(const VkImageFormatListCreateInfo*)p;
      fmtList.pNext = nullptr;
      haveFmtList = true;
    }
  VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  if (ci->flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR) {
    ici.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    if (haveFmtList) ici.pNext = &fmtList;
  }
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = ci->imageFormat;
  ici.extent = {ci->imageExtent.width, ci->imageExtent.height, 1};
  ici.mipLevels = 1;
  ici.arrayLayers = std::max(1u, ci->imageArrayLayers);
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = ci->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  ici.sharingMode = ci->imageSharingMode;
  ici.queueFamilyIndexCount = ci->queueFamilyIndexCount;
  ici.pQueueFamilyIndices = ci->pQueueFamilyIndices;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  uint32_t count = std::max(ci->minImageCount, 3u);
  sc->v.resize(count);
  for (auto& v : sc->v) {
    ok &= CreateImageMem(dd, ici, &v.image, &v.mem);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    ok &= dd->d.CreateFence(device, &fci, nullptr, &v.fence) == VK_SUCCESS;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    ok &= dd->d.CreateSemaphore(device, &sci, nullptr, &v.acqSem) == VK_SUCCESS;
    ok &= dd->d.CreateSemaphore(device, &sci, nullptr, &v.doneSem) == VK_SUCCESS;
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = sc->pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    ok &= sc->pool && dd->d.AllocateCommandBuffers(device, &cai, &v.cmd) == VK_SUCCESS;
    ok &= sc->pool && dd->d.AllocateCommandBuffers(device, &cai, &v.cmd2) == VK_SUCCESS;
    for (VkCommandBuffer c : {v.cmd, v.cmd2}) {
      if (c && dd->setLoaderData) dd->setLoaderData(device, c);
      else if (c) *(void**)c = *(void**)device;
    }
  }
  // left-eye store
  VkImageCreateInfo lci = ici;
  lci.format = realFormat;
  lci.pNext = nullptr;
  lci.flags = 0;
  lci.extent = {rext.width / 2, rext.height, 1};
  lci.arrayLayers = 1;
  lci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  lci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  lci.queueFamilyIndexCount = 0;
  lci.pQueueFamilyIndices = nullptr;
  ok &= CreateImageMem(dd, lci, &sc->left, &sc->leftMem);

  if (!ok) {
    Log(0, "failed to create virtual swapchain resources");
    DestroySwapchainResources(dd, sc);
    delete sc;
    dd->d.DestroySwapchainKHR(device, *pSwapchain, a);
    *pSwapchain = VK_NULL_HANDLE;
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  }
  {
    std::lock_guard<std::mutex> l(dd->swapLock);
    dd->swapchains[sc->handle] = sc;
  }
  ++dd->swapCount;
  Log(1, "swapchain (%s): game %ux%u -> glasses %ux%u SBS (%ux%u per eye), %u virtual / %u real images, "
      "format %d->%d, present mode %d, blit %d",
      useOwn ? "own window" : "game window", sc->vext.width, sc->vext.height, rext.width, rext.height,
      rext.width / 2, rext.height, count, n, sc->format, realFormat, rci.presentMode, sc->canBlit);
  Log(1, "output layout: %s", HalfSbs(rext) ? "half SBS (display stretches each half: 3D TV/monitor)"
                                             : "full SBS (each eye 1:1: Rokid/XREAL-style glasses)");
  return VK_SUCCESS;
}

Swapchain* FindSwapchain(DeviceData* dd, VkSwapchainKHR h) {
  std::lock_guard<std::mutex> l(dd->swapLock);
  auto it = dd->swapchains.find(h);
  return it == dd->swapchains.end() ? nullptr : it->second;
}

VKAPI_ATTR void VKAPI_CALL hook_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR h, const VkAllocationCallbacks* a) {
  DeviceData* dd = Dev(device);
  Swapchain* sc = nullptr;
  {
    std::lock_guard<std::mutex> l(dd->swapLock);
    auto it = dd->swapchains.find(h);
    if (it != dd->swapchains.end()) {
      sc = it->second;
      dd->swapchains.erase(it);
    }
  }
  if (sc) --dd->swapCount;
  if (sc && dd->replay) {
    std::lock_guard<std::mutex> rl(dd->recLock);
    AbortReplayLocked(dd, "swapchain destroyed");
  }
  if (sc && dd->gfxQueue) {
    std::lock_guard<std::mutex> l(GetQueueInfo(dd, dd->gfxQueue)->m);
    dd->d.QueueWaitIdle(dd->gfxQueue);
  }
  dd->d.DestroySwapchainKHR(device, h, a);
  if (sc) {
    DestroySwapchainResources(dd, sc);
    delete sc;
  }
}

VKAPI_ATTR VkResult VKAPI_CALL hook_GetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR h, uint32_t* count,
                                                          VkImage* images) {
  DeviceData* dd = Dev(device);
  Swapchain* sc = FindSwapchain(dd, h);
  if (!sc) return dd->d.GetSwapchainImagesKHR(device, h, count, images);
  uint32_t n = uint32_t(sc->v.size());
  if (!images) {
    *count = n;
    return VK_SUCCESS;
  }
  uint32_t c = std::min(*count, n);
  for (uint32_t i = 0; i < c; ++i) images[i] = sc->v[i].image;
  *count = c;
  return c < n ? VK_INCOMPLETE : VK_SUCCESS;
}

VkResult AcquireVirtual(DeviceData* dd, Swapchain* sc, uint64_t timeout, VkSemaphore sem, VkFence fence,
                        uint32_t* idx) {
  uint32_t i = sc->nextAcquire;
  dd->stage = "game acquire: waiting for the previous copy of this image";
  VkResult w = dd->d.WaitForFences(dd->device, 1, &sc->v[i].fence, VK_TRUE, timeout);
  dd->stage = "game acquire: done";
  if (w == VK_TIMEOUT) return timeout == 0 ? VK_NOT_READY : VK_TIMEOUT;
  if (w != VK_SUCCESS) return w;
  sc->nextAcquire = (i + 1) % uint32_t(sc->v.size());
  if (sem != VK_NULL_HANDLE || fence != VK_NULL_HANDLE) {
    VkQueue q = dd->gfxQueue;
    if (!q) return VK_ERROR_DEVICE_LOST;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.signalSemaphoreCount = sem ? 1 : 0;
    si.pSignalSemaphores = &sem;
    dd->stage = "game acquire: signalling (waiting for queue lock)";
    std::lock_guard<std::mutex> l(GetQueueInfo(dd, q)->m);
    VkResult r = dd->d.QueueSubmit(q, 1, &si, fence);
    dd->stage = "game is rendering";
    if (r != VK_SUCCESS) return r;
  }
  *idx = i;
  return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL hook_AcquireNextImageKHR(VkDevice device, VkSwapchainKHR h, uint64_t timeout,
                                                        VkSemaphore sem, VkFence fence, uint32_t* idx) {
  DeviceData* dd = Dev(device);
  Swapchain* sc = FindSwapchain(dd, h);
  if (!sc) return dd->d.AcquireNextImageKHR(device, h, timeout, sem, fence, idx);
  return AcquireVirtual(dd, sc, timeout, sem, fence, idx);
}
VKAPI_ATTR VkResult VKAPI_CALL hook_AcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR* info,
                                                         uint32_t* idx) {
  DeviceData* dd = Dev(device);
  Swapchain* sc = FindSwapchain(dd, info->swapchain);
  if (!sc) return dd->d.AcquireNextImage2KHR(device, info, idx);
  return AcquireVirtual(dd, sc, info->timeout, info->semaphore, info->fence, idx);
}

void Barrier(DeviceData* dd, VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to,
             VkPipelineStageFlags srcStage, VkAccessFlags srcAccess, VkPipelineStageFlags dstStage,
             VkAccessFlags dstAccess) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = srcAccess;
  b.dstAccessMask = dstAccess;
  b.oldLayout = from;
  b.newLayout = to;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = img;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  dd->d.CmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// copy src rect into dst rectangle at (dx,dy) with size dext; scale if needed
void CopyOrBlit(DeviceData* dd, Swapchain* sc, VkCommandBuffer cb, VkImage src, VkOffset2D so, VkExtent2D sext,
                VkImage dst, int32_t dx, int32_t dy, VkExtent2D dext) {
  if (((sext.width == dext.width && sext.height == dext.height) && !sc->formatDiffers) || !sc->canBlit) {
    VkImageCopy c{};
    c.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.srcOffset = {so.x, so.y, 0};
    c.dstOffset = {dx, dy, 0};
    c.extent = {std::min(sext.width, dext.width), std::min(sext.height, dext.height), 1};
    dd->d.CmdCopyImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &c);
  } else {
    VkImageBlit b{};
    b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.srcOffsets[0] = {so.x, so.y, 0};
    b.srcOffsets[1] = {so.x + int32_t(sext.width), so.y + int32_t(sext.height), 1};
    b.dstOffsets[0] = {dx, dy, 0};
    b.dstOffsets[1] = {dx + int32_t(dext.width), dy + int32_t(dext.height), 1};
    dd->d.CmdBlitImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &b, VK_FILTER_LINEAR);
  }
}

VkResult Worse(VkResult a, VkResult b) {
  if (a < 0) return a;
  if (b < 0) return b;
  if (a == VK_SUBOPTIMAL_KHR || b == VK_SUBOPTIMAL_KHR) return VK_SUBOPTIMAL_KHR;
  return VK_SUCCESS;
}

// ---- frame capture (diagnostics) ----------------------------------------------------
struct HostBuf {
  VkBuffer buf = VK_NULL_HANDLE;
  VkDeviceMemory mem = VK_NULL_HANDLE;
  VkDeviceSize size = 0;
};
bool CreateHostBuf(DeviceData* dd, VkDeviceSize size, HostBuf& hb) {
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = size;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  if (dd->d.CreateBuffer(dd->device, &bci, nullptr, &hb.buf) != VK_SUCCESS) return false;
  VkMemoryRequirements req;
  dd->d.GetBufferMemoryRequirements(dd->device, hb.buf, &req);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = FindMemType(dd, req.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                   VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
  if (dd->d.AllocateMemory(dd->device, &ai, nullptr, &hb.mem) != VK_SUCCESS) return false;
  dd->d.BindBufferMemory(dd->device, hb.buf, hb.mem, 0);
  hb.size = size;
  return true;
}
void FreeHostBuf(DeviceData* dd, HostBuf& hb) {
  if (hb.buf) dd->d.DestroyBuffer(dd->device, hb.buf, nullptr);
  if (hb.mem) dd->d.FreeMemory(dd->device, hb.mem, nullptr);
  hb = HostBuf{};
}
uint32_t BytesPerPixel(VkFormat f) {
  switch (f) {
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
      return 4;
    case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM:
      return 8;
    default:
      return 0;
  }
}
bool Is32bpp(VkFormat f) { return BytesPerPixel(f) != 0; }
void CopyToBuf(DeviceData* dd, VkCommandBuffer cb, VkImage img, VkExtent2D e, HostBuf& hb) {
  VkBufferImageCopy r{};
  r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  r.imageExtent = {e.width, e.height, 1};
  dd->d.CmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hb.buf, 1, &r);
}
inline unsigned char HalfTo8(uint16_t h) {
  uint32_t e = (h >> 10) & 0x1F, m = h & 0x3FF;
  if (h & 0x8000) return 0;
  float f = e == 0 ? m / 16777216.0f : e == 31 ? 1.0f : (1.0f + m / 1024.0f) * float(1u << e) / 32768.0f;
  if (f >= 1.0f) return 255;
  return (unsigned char)(f * 255.0f + 0.5f);
}
inline void Decode(const unsigned char* px, VkFormat fmt, unsigned char& r, unsigned char& g, unsigned char& b) {
  uint32_t v;
  memcpy(&v, px, 4);
  switch (fmt) {
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
      b = v & 0xFF; g = (v >> 8) & 0xFF; r = (v >> 16) & 0xFF; break;
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
      b = (v & 0x3FF) >> 2; g = ((v >> 10) & 0x3FF) >> 2; r = ((v >> 20) & 0x3FF) >> 2; break;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
      r = (v & 0x3FF) >> 2; g = ((v >> 10) & 0x3FF) >> 2; b = ((v >> 20) & 0x3FF) >> 2; break;
    case VK_FORMAT_R16G16B16A16_SFLOAT: {
      uint16_t c[3]; memcpy(c, px, 6);
      r = HalfTo8(c[0]); g = HalfTo8(c[1]); b = HalfTo8(c[2]); break;
    }
    case VK_FORMAT_R16G16B16A16_UNORM: {
      uint16_t c[3]; memcpy(c, px, 6);
      r = c[0] >> 8; g = c[1] >> 8; b = c[2] >> 8; break;
    }
    default:
      r = v & 0xFF; g = (v >> 8) & 0xFF; b = (v >> 16) & 0xFF; break;
  }
}

// Maps the current content rectangle + zoom to the source/destination rectangles of one
// eye. zoom = 1 shows the whole content (aspect kept, may leave bars); larger values
// zoom in around the centre until the eye is filled.
// Full SBS (32:9, e.g. Rokid/XREAL 3840x1080): each half is shown 1:1.
// Half SBS (16:9 3D TVs/monitors at 1920x1080): the display stretches each half to full width,
// so each eye is stored horizontally squeezed.
bool HalfSbs(const VkExtent2D& r) {
  int l = Cfg().sbsLayout;
  if (l == 1) return false;
  if (l == 2) return true;
  return uint64_t(r.width) * 10 < uint64_t(r.height) * 25;
}

void ComputeRects(Swapchain* sc) {
  const bool half = HalfSbs(sc->rext);
  const double ew = sc->rext.width / 2, eh = sc->rext.height, A = ew / eh * (half ? 2.0 : 1.0);
  double zoom = std::clamp(double(Cfg().zoom), 0.5, 4.0);
  sc->lastZoom = Cfg().zoom;
  double cx0 = sc->cx0, cy0 = sc->cy0, cx1 = sc->cx1, cy1 = sc->cy1;
  double cw = cx1 - cx0, ch = cy1 - cy0;
  if (cw < 16 || ch < 16) return;
  // eye-aspect window that contains the content, shrunk by zoom
  double ww = std::max(cw, ch * A) / zoom, wh = ww / A;
  double mx = (cx0 + cx1) / 2, my = (cy0 + cy1) / 2;
  double wx0 = mx - ww / 2, wy0 = my - wh / 2;
  double sx0 = std::max(wx0, cx0), sy0 = std::max(wy0, cy0);
  double sx1 = std::min(wx0 + ww, cx1), sy1 = std::min(wy0 + wh, cy1);
  double scx = ew / ww, scy = eh / wh;
  int ix0 = int(sx0 + 0.5), iy0 = int(sy0 + 0.5), ix1 = int(sx1 + 0.5), iy1 = int(sy1 + 0.5);
  int dx0 = int((sx0 - wx0) * scx + 0.5), dy0 = int((sy0 - wy0) * scy + 0.5);
  int dx1 = int((sx1 - wx0) * scx + 0.5), dy1 = int((sy1 - wy0) * scy + 0.5);
  if (dx0 <= 3) dx0 = 0;
  if (dy0 <= 3) dy0 = 0;
  if (dx1 >= int(ew) - 3) dx1 = int(ew);
  if (dy1 >= int(eh) - 3) dy1 = int(eh);
  if (ix1 <= ix0 || iy1 <= iy0 || dx1 <= dx0 || dy1 <= dy0) return;
  sc->srcOff = {ix0, iy0};
  sc->srcExt = {uint32_t(ix1 - ix0), uint32_t(iy1 - iy0)};
  sc->dstOff = {dx0, dy0};
  sc->dstExt = {uint32_t(dx1 - dx0), uint32_t(dy1 - dy0)};
}

// Finds the non-black region of the game image. A new region is adopted only after it
// has been seen twice in a row (so splash screens / fades don't stick).
void DetectBars(DeviceData* dd, Swapchain* sc, HostBuf& hb) {
  void* p = nullptr;
  if (dd->d.MapMemory(dd->device, hb.mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) return;
  const unsigned char* px = (const unsigned char*)p;
  const uint32_t bpp = BytesPerPixel(sc->format);
  const int W = int(sc->vext.width), H = int(sc->vext.height);
  int x0 = W, x1 = -1, y0 = H, y1 = -1;
  for (int y = 0; y < H; y += 2) {
    const unsigned char* row = px + size_t(y) * W * bpp;
    for (int x = 0; x < W; ++x) {
      unsigned char r, g, b;
      Decode(row + size_t(x) * bpp, sc->format, r, g, b);
      if (r > 12 || g > 12 || b > 12) {
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
      }
    }
  }
  dd->d.UnmapMemory(dd->device, hb.mem);
  ++sc->detections;
  if (x1 < 0 || (x1 - x0) < W / 4 || (y1 - y0) < H / 4) return;  // black / loading screen
  // end-exclusive, snap near-edges
  int nx0 = x0 <= 4 ? 0 : x0, nx1 = x1 >= W - 5 ? W : x1 + 1;
  int ny0 = y0 <= 4 ? 0 : y0, ny1 = y1 >= H - 6 ? H : y1 + 2;
  auto near = [](int a, int b) { return std::abs(a - b) <= 8; };
  if (near(nx0, sc->kx0) && near(nx1, sc->kx1) && near(ny0, sc->ky0) && near(ny1, sc->ky1)) {
    ++sc->candCount;
  } else {
    sc->kx0 = nx0; sc->kx1 = nx1; sc->ky0 = ny0; sc->ky1 = ny1;
    sc->candCount = 1;
  }
  bool same = near(nx0, sc->cx0) && near(nx1, sc->cx1) && near(ny0, sc->cy0) && near(ny1, sc->cy1);
  if (sc->candCount >= 2 && !same) {
    sc->cx0 = nx0; sc->cx1 = nx1; sc->cy0 = ny0; sc->cy1 = ny1;
    ComputeRects(sc);
    Log(1, "game content %dx%d at (%d,%d) of %dx%d -> shown %ux%u in each eye (zoom %.2f, Ctrl+Shift+PgUp/PgDn)",
        nx1 - nx0, ny1 - ny0, nx0, ny0, W, H, sc->dstExt.width, sc->dstExt.height, Cfg().zoom);
  }
}

bool WritePPM(DeviceData* dd, HostBuf& hb, VkFormat fmt, VkExtent2D e, const std::string& path) {
  void* p = nullptr;
  if (dd->d.MapMemory(dd->device, hb.mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) return false;
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) { dd->d.UnmapMemory(dd->device, hb.mem); return false; }
  fprintf(f, "P6\n%u %u\n255\n", e.width, e.height);
  const unsigned char* px = (const unsigned char*)p;
  const uint32_t bpp = BytesPerPixel(fmt);
  std::vector<unsigned char> row(e.width * 3);
  for (uint32_t y = 0; y < e.height; ++y) {
    for (uint32_t x = 0; x < e.width; ++x) {
      const unsigned char* v = px + (size_t(y) * e.width + x) * bpp;
      unsigned char r, g, b;
      Decode(v, fmt, r, g, b);
      row[x * 3] = r; row[x * 3 + 1] = g; row[x * 3 + 2] = b;
    }
    fwrite(row.data(), 1, row.size(), f);
  }
  fclose(f);
  dd->d.UnmapMemory(dd->device, hb.mem);
  return true;
}

VkResult PresentOne(DeviceData* dd, VkQueue queue, Swapchain* sc, uint32_t idx, const VkSemaphore* waits,
                    uint32_t waitCount, Clock::time_point* sleepUntil) {
  if (idx >= sc->v.size()) return VK_ERROR_OUT_OF_DATE_KHR;
  VImage& vi = sc->v[idx];
  const uint64_t frame = dd->frame;
  const bool right = (frame & 1) != 0;

  // 1) acquire the real image (may block on vsync) - outside of any queue lock
  uint32_t realIdx = UINT32_MAX;
  VkResult acq = VK_SUCCESS;
  if (right) {
    dd->stage = "present(alternate): acquiring glasses image (vsync)";
    acq = dd->d.AcquireNextImageKHR(dd->device, sc->handle, UINT64_MAX, vi.acqSem, VK_NULL_HANDLE, &realIdx);
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) realIdx = UINT32_MAX;
  }
  const bool doReal = right && realIdx != UINT32_MAX;
  HostBuf dumpGame, dumpSbs;
  bool dump = false;
  if (doReal && TakeDumpRequest(frame)) {
    if (!Is32bpp(sc->format)) {
      Log(0, "capture: unsupported format %d", sc->format);
    } else {
      uint32_t bpp = BytesPerPixel(sc->format);
      dump = Is32bpp(sc->realFormat) && CreateHostBuf(dd, VkDeviceSize(sc->vext.width) * sc->vext.height * bpp, dumpGame) &&
             CreateHostBuf(dd, VkDeviceSize(sc->rext.width) * sc->rext.height * BytesPerPixel(sc->realFormat), dumpSbs);
      if (!dump) { FreeHostBuf(dd, dumpGame); FreeHostBuf(dd, dumpSbs); }
    }
  }
  if (sc->detectOn && !Is32bpp(sc->format)) {
    Log(0, "black-bar detection unavailable for swapchain format %d", sc->format);
    sc->detectOn = false;
  }
  bool detect = doReal && sc->detectOn && frame >= sc->nextDetect;
  if (detect) {
    sc->nextDetect = frame + (sc->detections < 20 ? 30 : 240);
    if (!dumpGame.buf &&
        !CreateHostBuf(dd, VkDeviceSize(sc->vext.width) * sc->vext.height * BytesPerPixel(sc->format), dumpGame)) {
      FreeHostBuf(dd, dumpGame);
      detect = false;
    }
  }
  if (Cfg().zoom != sc->lastZoom) ComputeRects(sc);
  const bool fullDst = sc->dstOff.x == 0 && sc->dstOff.y == 0 && sc->dstExt.width == sc->rext.width / 2 &&
                       sc->dstExt.height == sc->rext.height;
  VkClearColorValue black{};
  VkImageSubresourceRange whole{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

  dd->stage = "present(alternate): waiting for previous copy";
  dd->d.WaitForFences(dd->device, 1, &vi.fence, VK_TRUE, UINT64_MAX);
  dd->d.ResetFences(dd->device, 1, &vi.fence);
  dd->stage = "present(alternate): recording";

  // 2) record
  VkCommandBuffer cb = vi.cmd;
  dd->d.ResetCommandBuffer(cb, 0);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  dd->d.BeginCommandBuffer(cb, &bi);
  const VkPipelineStageFlags ALL = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, XFER = VK_PIPELINE_STAGE_TRANSFER_BIT;
  const VkExtent2D half{sc->rext.width / 2, sc->rext.height};

  Barrier(dd, cb, vi.image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ALL,
          VK_ACCESS_MEMORY_WRITE_BIT, XFER, VK_ACCESS_TRANSFER_READ_BIT);
  if (!right) {
    Barrier(dd, cb, sc->left, sc->leftValid ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, XFER, 0, XFER, VK_ACCESS_TRANSFER_WRITE_BIT);
    if (!fullDst) dd->d.CmdClearColorImage(cb, sc->left, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &whole);
    CopyOrBlit(dd, sc, cb, vi.image, sc->srcOff, sc->srcExt, sc->left, sc->dstOff.x, sc->dstOff.y, sc->dstExt);
    Barrier(dd, cb, sc->left, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, XFER,
            VK_ACCESS_TRANSFER_WRITE_BIT, XFER, VK_ACCESS_TRANSFER_READ_BIT);
    sc->leftValid = true;
  } else if (doReal) {
    VkImage real = sc->realImages[realIdx];
    Barrier(dd, cb, real, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, ALL, 0, XFER,
            VK_ACCESS_TRANSFER_WRITE_BIT);
    if (!fullDst || !sc->leftValid)
      dd->d.CmdClearColorImage(cb, real, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &whole);
    if (sc->leftValid) {
      VkImageCopy c{};
      c.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      c.extent = {half.width, half.height, 1};
      dd->d.CmdCopyImage(cb, sc->left, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, real,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    }
    CopyOrBlit(dd, sc, cb, vi.image, sc->srcOff, sc->srcExt, real, int32_t(half.width) + sc->dstOff.x,
               sc->dstOff.y, sc->dstExt);
    if (detect && !dump) CopyToBuf(dd, cb, vi.image, sc->vext, dumpGame);
    if (dump) {
      Barrier(dd, cb, real, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, XFER,
              VK_ACCESS_TRANSFER_WRITE_BIT, XFER, VK_ACCESS_TRANSFER_READ_BIT);
      CopyToBuf(dd, cb, real, sc->rext, dumpSbs);
      CopyToBuf(dd, cb, vi.image, sc->vext, dumpGame);
      Barrier(dd, cb, real, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, XFER, 0,
              VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    } else {
      Barrier(dd, cb, real, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, XFER,
              VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    }
  }
  Barrier(dd, cb, vi.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, XFER, 0,
          VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);

  // 3) stereo parameters for the NEXT frame, ordered on the queue
  if (dd->stereoShaders) {
    float data[3] = {EyeK(frame + 1), Cfg().convergence, HudFactor()};
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    dd->d.CmdPipelineBarrier(cb, ALL, XFER, 0, 0, nullptr, 0, nullptr, 0, nullptr);
    dd->d.CmdUpdateBuffer(cb, dd->paramBuf, 0, sizeof(data), data);
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_MEMORY_READ_BIT;
    dd->d.CmdPipelineBarrier(cb, XFER, ALL, 0, 1, &mb, 0, nullptr, 0, nullptr);
  }
  dd->d.EndCommandBuffer(cb);

  // 4) submit + present
  std::vector<VkSemaphore> ws(waits, waits + waitCount);
  if (doReal) ws.push_back(vi.acqSem);
  std::vector<VkPipelineStageFlags> wstages(ws.size(), ALL);
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.waitSemaphoreCount = uint32_t(ws.size());
  si.pWaitSemaphores = ws.data();
  si.pWaitDstStageMask = wstages.data();
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cb;
  si.signalSemaphoreCount = doReal ? 1 : 0;
  si.pSignalSemaphores = &vi.doneSem;

  QueueInfo* qi = GetQueueInfo(dd, queue);
  VkQueue subQueue = (qi->family == sc->poolFamily || !dd->gfxQueue) ? queue : dd->gfxQueue;
  VkResult result = VK_SUCCESS;
  {
    dd->stage = "present(alternate): submitting copy";
    std::lock_guard<std::mutex> l(GetQueueInfo(dd, subQueue)->m);
    VkResult sr = dd->d.QueueSubmit(subQueue, 1, &si, vi.fence);
    if (sr != VK_SUCCESS) return sr;
  }
  if (doReal) {
    dd->stage = "present(alternate): presenting to glasses";
    std::lock_guard<std::mutex> l(qi->m);
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &vi.doneSem;
    pi.swapchainCount = 1;
    pi.pSwapchains = &sc->handle;
    pi.pImageIndices = &realIdx;
    result = dd->d.QueuePresentKHR(queue, &pi);
  }
  if (dump) {
    dd->d.WaitForFences(dd->device, 1, &vi.fence, VK_TRUE, UINT64_MAX);
    std::string base = DumpDir() + "/" + GameName();
    std::string g = base + "_juego.ppm", b = base + "_sbs.ppm", t = base + "_info.txt";
    bool ok = WritePPM(dd, dumpGame, sc->format, sc->vext, g) && WritePPM(dd, dumpSbs, sc->realFormat, sc->rext, b);
    if (FILE* f = fopen(t.c_str(), "w")) {
      fprintf(f, "game_image=%ux%u\nglasses=%ux%u\ncrop_offset=%d,%d\ncrop_size=%ux%u\neye_offset=%d,%d\n"
                 "eye_size=%ux%u\nformat=%d\nframe=%llu\n",
              sc->vext.width, sc->vext.height, sc->rext.width, sc->rext.height, sc->srcOff.x, sc->srcOff.y,
              sc->srcExt.width, sc->srcExt.height, sc->dstOff.x, sc->dstOff.y, sc->dstExt.width,
              sc->dstExt.height, sc->format, (unsigned long long)frame);
      fclose(f);
    }
    Log(0, ok ? "capture saved: %s , %s" : "capture FAILED: %s , %s", g.c_str(), b.c_str());
    FreeHostBuf(dd, dumpSbs);
  }
  if (detect) {
    if (!dump) dd->d.WaitForFences(dd->device, 1, &vi.fence, VK_TRUE, UINT64_MAX);
    DetectBars(dd, sc, dumpGame);
  }
  FreeHostBuf(dd, dumpGame);
  if (right && acq < 0) result = acq;
  else if (acq == VK_SUBOPTIMAL_KHR) result = Worse(result, acq);
  if (sc->onOwn && result == VK_SUBOPTIMAL_KHR) result = VK_SUCCESS;
  dd->frame = frame + 1;

  // 5) pacing: put the left frame in the middle of two real presents
  auto now = Clock::now();
  if (right) {
    if (sc->haveLastRight) {
      double dt = std::chrono::duration<double>(now - sc->lastRight).count();
      if (dt > 0.002 && dt < 0.1) sc->periodEma = sc->periodEma > 0 ? sc->periodEma * 0.9 + dt * 0.1 : dt;
    }
    sc->lastRight = now;
    sc->haveLastRight = true;
    ++sc->realPresents;
  } else if (Cfg().pace && sc->haveLastRight && sc->periodEma > 0) {
    auto target = sc->lastRight + std::chrono::duration_cast<Clock::duration>(
                                       std::chrono::duration<double>(sc->periodEma * 0.5));
    if (target > now && target - now < std::chrono::milliseconds(50)) *sleepUntil = target;
  }

  // stats
  ++sc->statFrames;
  double el = std::chrono::duration<double>(now - sc->statStart).count();
  if (el >= 5.0) {
    Log(1, "game %.1f fps -> SBS %.1f fps (%.1f per eye) | shaders patched %u, skipped %u | sep %.4f conv %.3f%s",
        sc->statFrames / el, sc->statFrames / el / 2, sc->statFrames / el / 2, dd->patchedCount.load(),
        dd->skippedCount.load(), Cfg().separation, Cfg().convergence, Cfg().stereo ? "" : " [2D]");
    sc->statFrames = 0;
    sc->statStart = now;
  }
  return result;
}

void UpdateParams(DeviceData* dd, VkCommandBuffer cb, float k) {
  const VkPipelineStageFlags ALL = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, XFER = VK_PIPELINE_STAGE_TRANSFER_BIT;
  float data[3] = {k, Cfg().convergence, HudFactor()};
  dd->d.CmdPipelineBarrier(cb, ALL, XFER, 0, 0, nullptr, 0, nullptr, 0, nullptr);
  dd->d.CmdUpdateBuffer(cb, dd->paramBuf, 0, sizeof(data), data);
  VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_MEMORY_READ_BIT;
  dd->d.CmdPipelineBarrier(cb, XFER, ALL, 0, 1, &mb, 0, nullptr, 0, nullptr);
}
void FullBarrier(DeviceData* dd, VkCommandBuffer cb) {
  VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  dd->d.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                           nullptr, 0, nullptr);
}

// Replay mode: the game rendered this frame for the left eye. Copy it to the left half,
// switch the shaders to the right eye, submit the same command buffers again, copy the
// result to the right half and present. Caller holds dd->recLock.
VkResult PresentReplay(DeviceData* dd, VkQueue queue, Swapchain* sc, uint32_t idx, const VkSemaphore* waits,
                       uint32_t waitCount) {
  QueueInfo* qi = GetQueueInfo(dd, queue);
  std::vector<VkCommandBuffer> cbs;
  cbs.swap(dd->recCbs);
  bool doReplay = !dd->recMono && !cbs.empty() && Cfg().stereo && queue == dd->gfxQueue;
  dd->recMono = false;
  if (idx >= sc->v.size()) {
    std::lock_guard<std::mutex> l(qi->m);
    ReleaseHeldLocked(dd, queue);
    return VK_ERROR_OUT_OF_DATE_KHR;
  }
  VImage& vi = sc->v[idx];
  uint32_t realIdx = UINT32_MAX;
  dd->stage = "present(replay): acquiring glasses image (vsync)";
  VkResult acq = dd->d.AcquireNextImageKHR(dd->device, sc->handle, UINT64_MAX, vi.acqSem, VK_NULL_HANDLE, &realIdx);
  const VkPipelineStageFlags ALL = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, XFER = VK_PIPELINE_STAGE_TRANSFER_BIT;
  std::vector<VkPipelineStageFlags> allStages(waitCount + 1, ALL);
  if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
    // consume the game's semaphores, release held signals and let the game recreate the swapchain
    std::lock_guard<std::mutex> l(qi->m);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = waitCount;
    si.pWaitSemaphores = waits;
    si.pWaitDstStageMask = allStages.data();
    dd->d.QueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    ReleaseHeldLocked(dd, queue);
    return acq;
  }
  dd->stage = "present(replay): waiting for previous copy";
  dd->d.WaitForFences(dd->device, 1, &vi.fence, VK_TRUE, UINT64_MAX);
  dd->d.ResetFences(dd->device, 1, &vi.fence);
  dd->stage = "present(replay): recording";
  if (Cfg().zoom != sc->lastZoom) ComputeRects(sc);
  const bool fullDst = sc->dstOff.x == 0 && sc->dstOff.y == 0 && sc->dstExt.width == sc->rext.width / 2 &&
                       sc->dstExt.height == sc->rext.height;
  VkClearColorValue black{};
  VkImageSubresourceRange whole{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkImage real = sc->realImages[realIdx];
  const int32_t halfW = int32_t(sc->rext.width / 2);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

  // A: left eye -> left half (+ right half too if there is no replay this frame)
  VkCommandBuffer a = vi.cmd;
  dd->d.ResetCommandBuffer(a, 0);
  dd->d.BeginCommandBuffer(a, &bi);
  Barrier(dd, a, vi.image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ALL,
          VK_ACCESS_MEMORY_WRITE_BIT, XFER, VK_ACCESS_TRANSFER_READ_BIT);
  Barrier(dd, a, real, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, ALL, 0, XFER,
          VK_ACCESS_TRANSFER_WRITE_BIT);
  if (!fullDst) dd->d.CmdClearColorImage(a, real, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &whole);
  CopyOrBlit(dd, sc, a, vi.image, sc->srcOff, sc->srcExt, real, sc->dstOff.x, sc->dstOff.y, sc->dstExt);
  if (!doReplay)
    CopyOrBlit(dd, sc, a, vi.image, sc->srcOff, sc->srcExt, real, halfW + sc->dstOff.x, sc->dstOff.y, sc->dstExt);
  Barrier(dd, a, vi.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, XFER, 0, ALL, 0);
  if (doReplay) {
    UpdateParams(dd, a, EyeK(1));  // right eye
    FullBarrier(dd, a);
  } else {
    Barrier(dd, a, real, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, XFER,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
  }
  dd->d.EndCommandBuffer(a);

  // B: right eye -> right half, shaders back to the left eye
  VkCommandBuffer b = vi.cmd2;
  if (doReplay) {
    dd->d.ResetCommandBuffer(b, 0);
    dd->d.BeginCommandBuffer(b, &bi);
    FullBarrier(dd, b);
    Barrier(dd, b, vi.image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ALL,
            VK_ACCESS_MEMORY_WRITE_BIT, XFER, VK_ACCESS_TRANSFER_READ_BIT);
    CopyOrBlit(dd, sc, b, vi.image, sc->srcOff, sc->srcExt, real, halfW + sc->dstOff.x, sc->dstOff.y, sc->dstExt);
    Barrier(dd, b, real, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, XFER,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    Barrier(dd, b, vi.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, XFER, 0, ALL, 0);
    UpdateParams(dd, b, EyeK(0));  // left eye for the next frame
    dd->d.EndCommandBuffer(b);
  }

  std::vector<VkSemaphore> ws(waits, waits + waitCount);
  ws.push_back(vi.acqSem);
  VkSubmitInfo si[3] = {{VK_STRUCTURE_TYPE_SUBMIT_INFO}, {VK_STRUCTURE_TYPE_SUBMIT_INFO}, {VK_STRUCTURE_TYPE_SUBMIT_INFO}};
  si[0].waitSemaphoreCount = uint32_t(ws.size());
  si[0].pWaitSemaphores = ws.data();
  si[0].pWaitDstStageMask = allStages.data();
  si[0].commandBufferCount = 1;
  si[0].pCommandBuffers = &a;
  uint32_t nsi = 1;
  if (doReplay) {
    si[1].commandBufferCount = uint32_t(cbs.size());
    si[1].pCommandBuffers = cbs.data();
    si[2].commandBufferCount = 1;
    si[2].pCommandBuffers = &b;
    nsi = 3;
  }
  si[nsi - 1].signalSemaphoreCount = 1;
  si[nsi - 1].pSignalSemaphores = &vi.doneSem;

  VkResult result;
  {
    dd->stage = "present(replay): submitting replay";
    std::lock_guard<std::mutex> l(qi->m);
    VkResult sr = dd->d.QueueSubmit(queue, nsi, si, vi.fence);
    dd->stage = "present(replay): presenting to glasses";
    ReleaseHeldLocked(dd, queue);
    if (sr != VK_SUCCESS) return sr;
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &vi.doneSem;
    pi.swapchainCount = 1;
    pi.pSwapchains = &sc->handle;
    pi.pImageIndices = &realIdx;
    result = dd->d.QueuePresentKHR(queue, &pi);
  }
  if (acq == VK_SUBOPTIMAL_KHR) result = Worse(result, acq);
  if (sc->onOwn && result == VK_SUBOPTIMAL_KHR) result = VK_SUCCESS;
  if (doReplay) ++dd->replayFrames;
  ++dd->frame;

  auto now = Clock::now();
  ++sc->statFrames;
  double el = std::chrono::duration<double>(now - sc->statStart).count();
  if (el >= 5.0) {
    uint32_t rf = dd->replayFrames.exchange(0), mf = dd->monoFrames.exchange(0);
    // the game needs GPU results in the middle of its frames: replay cannot work, use alternate
    if (Cfg().autoSwitch && mf > rf && rf + mf >= 30) {
      if (++dd->badReplayWindows >= 2) {
        Log(0, "this game reads back from the GPU every frame (%u of %u frames 2D): switching to alternate "
            "mode (3D at half the game fps). Use mode = alternate in its profile to skip this.", mf, rf + mf);
        AbortReplayLocked(dd, "switching to alternate");
        dd->replay = false;
        dd->recMono = false;
        dd->frame = 0;
      }
    } else {
      dd->badReplayWindows = 0;
    }
    Log(1, "game %.1f fps -> SBS %.1f fps (%.1f per eye) [replay: %u frames 3D, %u frames 2D] | shaders %u | "
        "sep %.4f conv %.3f%s",
        sc->statFrames / el, sc->statFrames / el, sc->statFrames / el, rf, mf, dd->patchedCount.load(),
        Cfg().separation, Cfg().convergence, Cfg().stereo ? "" : " [2D]");
    sc->statFrames = 0;
    sc->statStart = now;
  }
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL hook_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pi) {
  DeviceData* dd = Dev(queue);
  std::vector<Swapchain*> scs(pi->swapchainCount, nullptr);
  bool any = false;
  for (uint32_t i = 0; i < pi->swapchainCount; ++i) any |= (scs[i] = FindSwapchain(dd, pi->pSwapchains[i])) != nullptr;
  if (!any) {
    std::lock_guard<std::mutex> l(GetQueueInfo(dd, queue)->m);
    return dd->d.QueuePresentKHR(queue, pi);
  }
  PollControls();

  VkResult overall = VK_SUCCESS;
  Clock::time_point sleepUntil{};
  for (uint32_t i = 0; i < pi->swapchainCount; ++i) {
    const VkSemaphore* waits = i == 0 ? pi->pWaitSemaphores : nullptr;
    uint32_t wc = i == 0 ? pi->waitSemaphoreCount : 0;
    VkResult r;
    if (scs[i] && dd->replay) {
      std::lock_guard<std::mutex> rl(dd->recLock);
      r = PresentReplay(dd, queue, scs[i], pi->pImageIndices[i], waits, wc);
    } else if (scs[i]) {
      r = PresentOne(dd, queue, scs[i], pi->pImageIndices[i], waits, wc, &sleepUntil);
    } else {
      VkPresentInfoKHR one{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
      one.waitSemaphoreCount = wc;
      one.pWaitSemaphores = waits;
      one.swapchainCount = 1;
      one.pSwapchains = &pi->pSwapchains[i];
      one.pImageIndices = &pi->pImageIndices[i];
      std::lock_guard<std::mutex> l(GetQueueInfo(dd, queue)->m);
      r = dd->d.QueuePresentKHR(queue, &one);
    }
    if (pi->pResults) pi->pResults[i] = r;
    overall = Worse(overall, r);
  }
  ++dd->presentCount;
  dd->stage = "game is rendering (between presents)";
  if (dd->out && dd->presentCount % 120 == 1) RaiseOutputWindow(dd->out);
  if (dd->presentCount % 60 == 1) MarkStereoActive();
  // frame limiter (default 120 fps = 60 per eye) combined with left/right pacing
  Clock::time_point target = sleepUntil;
  float lim = dd->replay ? Cfg().fpsLimit / 2 : Cfg().fpsLimit;
  if (lim > 1.0f && dd->lastPresent != Clock::time_point{}) {
    auto t = dd->lastPresent + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / lim));
    if (t > target) target = t;
  }
  auto now = Clock::now();
  if (target > now && target - now < std::chrono::milliseconds(100)) {
    if (target - now > std::chrono::microseconds(1500))
      std::this_thread::sleep_until(target - std::chrono::microseconds(1000));
    while (Clock::now() < target) std::this_thread::yield();
    now = target;
  }
  // keep a steady cadence: advance from the previous target unless we fell behind
  if (lim > 1.0f && dd->lastPresent != Clock::time_point{} &&
      now - dd->lastPresent < std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(2.0 / lim)))
    dd->lastPresent = std::max(target, dd->lastPresent);
  else
    dd->lastPresent = now;
  if (dd->lastPresent < now - std::chrono::milliseconds(50)) dd->lastPresent = now;
  return overall;
}

// ------------------------------------------------------------------------------------
// Proc addr
// ------------------------------------------------------------------------------------
#define DEVICE_HOOKS(X)                                                                          \
  X(DestroyDevice) X(GetDeviceQueue) X(GetDeviceQueue2) X(QueueSubmit) X(QueueSubmit2)          \
  X(QueueSubmit2KHR) X(QueueWaitIdle) X(QueueBindSparse) X(DeviceWaitIdle) X(CreateShaderModule) \
  X(DestroyShaderModule) X(CreateGraphicsPipelines) X(CreateSwapchainKHR) X(DestroySwapchainKHR) \
  X(GetSwapchainImagesKHR) X(AcquireNextImageKHR) X(AcquireNextImage2KHR) X(QueuePresentKHR)
#define REPLAY_HOOKS(X) \
  X(WaitForFences) X(WaitSemaphores) X(WaitSemaphoresKHR) X(CreateSemaphore) X(DestroySemaphore) X(BeginCommandBuffer)

PFN_vkVoidFunction DeviceHook(const char* name) {
#define CHECK(n) if (!strcmp(name, "vk" #n)) return (PFN_vkVoidFunction)&hook_##n;
  if (Cfg().passthrough) return nullptr;  // launchers / helpers: no hooks at all
  if (Cfg().mode == 1) {                 // replay mode needs to see waits and command buffer setup
    REPLAY_HOOKS(CHECK)
  }
  DEVICE_HOOKS(CHECK)
#undef CHECK
  return nullptr;
}

}  // namespace

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL stereo3d_GetDeviceProcAddr(VkDevice device, const char* name);

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL stereo3d_GetInstanceProcAddr(VkInstance instance,
                                                                             const char* name) {
  if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)&stereo3d_GetInstanceProcAddr;
  if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)&stereo3d_GetDeviceProcAddr;
  if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)&hook_CreateInstance;
  if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)&hook_CreateDevice;
  if (!instance) return nullptr;
  InstanceData* id = Inst(instance);
  if (!id) return nullptr;
  PFN_vkVoidFunction next = id->gipa(instance, name);
  if (!next) return nullptr;
#define CHECK(n) if (!strcmp(name, "vk" #n)) return (PFN_vkVoidFunction)&hook_##n;
  CHECK(DestroyInstance)
  CHECK(EnumeratePhysicalDevices)
  CHECK(EnumeratePhysicalDeviceGroups)
  CHECK(EnumeratePhysicalDeviceGroupsKHR)
  CHECK(EnumerateDeviceExtensionProperties)
  CHECK(GetPhysicalDeviceSurfaceCapabilitiesKHR)
  CHECK(GetPhysicalDeviceSurfaceCapabilities2KHR)
#undef CHECK
  if (PFN_vkVoidFunction h = DeviceHook(name)) return h;
  return next;
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL stereo3d_GetDeviceProcAddr(VkDevice device, const char* name) {
  if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)&stereo3d_GetDeviceProcAddr;
  DeviceData* dd = Dev(device);
  if (!dd) return nullptr;
  PFN_vkVoidFunction next = dd->d.GetDeviceProcAddr(device, name);
  if (!next) return nullptr;
  if (PFN_vkVoidFunction h = DeviceHook(name)) return h;
  return next;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL stereo3d_NegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* p) {
  if (!p || p->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) return VK_ERROR_INITIALIZATION_FAILED;
  if (p->loaderLayerInterfaceVersion >= 2) {
    p->pfnGetInstanceProcAddr = stereo3d_GetInstanceProcAddr;
    p->pfnGetDeviceProcAddr = stereo3d_GetDeviceProcAddr;
    p->pfnGetPhysicalDeviceProcAddr = nullptr;
    p->loaderLayerInterfaceVersion = 2;
  }
  return VK_SUCCESS;
}
