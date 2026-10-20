// present_vk.c - present.h's Vulkan backend (built when third_party/vulkan has the Khronos headers: CT_HAVE_VULKAN).
// No SDK and nothing linked on the PC: vkGetInstanceProcAddr comes from the driver's vulkan-1 through SDL, everything else from
// it. ON THE SWITCH (mesa-switch's NVK, linked statically and loaderless, switch/get_mesa.sh): vkGetInstanceProcAddr is the linked
// libvulkan's own, and the surface is VK_NN_vi_surface on the console's NWindow (SDL has no Vulkan there). A frame: the 256x224 picture goes through a host-visible staging buffer into an image, the swapchain image is
// cleared black and the picture BLITTED into the placed rectangle (nearest or linear), then a render pass that LOADS
// the result lets the F1 overlay (ImGui's Vulkan backend) draw on top, and the image is presented. One frame in flight.
#ifdef CT_HAVE_VULKAN
#define VK_NO_PROTOTYPES
#ifdef __SWITCH__
#define VK_USE_PLATFORM_VI_NN
#include <switch.h>
#endif
#include <vulkan/vulkan.h>
#include "SDL.h"
#ifndef __SWITCH__
#include "SDL_vulkan.h"
#else
extern VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* pName);   // (libvulkan.a)
#endif
#include "present_vk.h"
#ifdef CT_HAVE_SLANG
#include "slang/slang_vk.h"
#include "slang/slang_chain.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VK_FUNCS_INSTANCE(X) \
  X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
  X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceSurfaceSupportKHR) \
  X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
  X(vkGetPhysicalDeviceSurfacePresentModesKHR) X(vkGetPhysicalDeviceMemoryProperties) \
  X(vkGetPhysicalDeviceFormatProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkDestroySurfaceKHR)
#define VK_FUNCS_DEVICE(X) \
  X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) \
  X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateRenderPass) \
  X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCreateCommandPool) X(vkDestroyCommandPool) \
  X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkQueueSubmit) \
  X(vkDeviceWaitIdle) X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCreateFence) X(vkDestroyFence) \
  X(vkWaitForFences) X(vkResetFences) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) \
  X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkCreateImage) \
  X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCmdPipelineBarrier) \
  X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdBlitImage) X(vkCmdClearColorImage) \
  X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) \
  X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) X(vkGetQueryPoolResults)
#define DECL(f) static PFN_##f f;
static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr_;
static PFN_vkCreateInstance vkCreateInstance;
VK_FUNCS_INSTANCE(DECL)
VK_FUNCS_DEVICE(DECL)
#undef DECL

static double nowMsVk(void) { return (double)SDL_GetPerformanceCounter() * 1000.0 / (double)SDL_GetPerformanceFrequency(); }
// The size of the surface's window in pixels: SDL's Vulkan drawable on the PC, the window on the Switch.
static void winSize(SDL_Window* w, int* ww, int* hh) {
#ifdef __SWITCH__
  *ww = 0; *hh = 0; if (w) SDL_GetWindowSize(w, ww, hh);
  if (*ww <= 0 || *hh <= 0) { *ww = 1920; *hh = 1080; }                 // (no SDL window: 1920x1080 in every mode, like SDL's own EGL window on the console)
#else
  SDL_Vulkan_GetDrawableSize(w, ww, hh);
#endif
}

#ifdef CT_HAVE_SLANG
struct SlState; static void slStop(struct CtVk* v);
#endif
#define MAXIMG 8
struct CtVk {
  SDL_Window* win;
  int picW, picH, vsync;
  VkInstance inst; VkSurfaceKHR surf; VkPhysicalDevice phys; VkDevice dev; VkQueue queue; uint32_t qfam;
  VkSwapchainKHR swap; VkFormat fmt; VkExtent2D ext; uint32_t nimg, minImg; VkImageUsageFlags swapUsage;
  VkImage img[MAXIMG]; VkImageView view[MAXIMG]; VkFramebuffer fb[MAXIMG]; VkSemaphore done[MAXIMG];
  VkRenderPass pass; VkCommandPool pool; VkCommandBuffer cmd; VkSemaphore avail; VkFence fence;
  VkBuffer stage; VkDeviceMemory stageMem; void* stageMap;
  VkImage pic; VkDeviceMemory picMem; VkImageView picView; VkImageLayout picLayout; int upload;
  struct SlState* sl;                                   // the slang shader chain (slang_vk.h), created on the first CtVkShaderSet
  VkBuffer rb; VkDeviceMemory rbMem; void* rbMap; VkDeviceSize rbSize;   // the probe's readback
  VkDescriptorPool dpool;
  uint32_t cur; int acquired, recording, needRecreate;
  VkQueryPool qp; double tsPeriod; int qArmed; double lastGpuMs, waitedMs; long gpuSeq;      // GPU timestamps: top and bottom of the frame's commands
  char name[160];
};

static uint32_t memType(CtVk* v, uint32_t bits, VkMemoryPropertyFlags want) {
  VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(v->phys, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
    if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
  return UINT32_MAX;
}
static int makeBuffer(CtVk* v, VkDeviceSize size, VkBufferUsageFlags use, VkBuffer* b, VkDeviceMemory* m, void** map) {
  VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO }; bi.size = size; bi.usage = use;
  bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(v->dev, &bi, NULL, b) != VK_SUCCESS) return 0;
  VkMemoryRequirements mr; vkGetBufferMemoryRequirements(v->dev, *b, &mr);
  VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO }; ai.allocationSize = mr.size;
  ai.memoryTypeIndex = memType(v, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(v->dev, &ai, NULL, m) != VK_SUCCESS) return 0;
  vkBindBufferMemory(v->dev, *b, *m, 0);
  return vkMapMemory(v->dev, *m, 0, size, 0, map) == VK_SUCCESS;
}
static void barrier(VkCommandBuffer c, VkImage im, VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da,
                    VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
  VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
  b.oldLayout = from; b.newLayout = to; b.srcAccessMask = sa; b.dstAccessMask = da;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = im;
  b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.subresourceRange.levelCount = 1; b.subresourceRange.layerCount = 1;
  vkCmdPipelineBarrier(c, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
}

// the picture: the staging buffer the frame is written to and the image the blit reads
static void picDestroy(CtVk* v) {
  if (v->stage) { vkDestroyBuffer(v->dev, v->stage, NULL); vkFreeMemory(v->dev, v->stageMem, NULL); }
  if (v->picView) vkDestroyImageView(v->dev, v->picView, NULL);
  if (v->pic) { vkDestroyImage(v->dev, v->pic, NULL); vkFreeMemory(v->dev, v->picMem, NULL); }
  v->picView = VK_NULL_HANDLE; v->stage = VK_NULL_HANDLE; v->stageMem = VK_NULL_HANDLE; v->stageMap = NULL; v->pic = VK_NULL_HANDLE; v->picMem = VK_NULL_HANDLE;
}
static int picCreate(CtVk* v, int picW, int picH) {
  if (!makeBuffer(v, (VkDeviceSize)picW * picH * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &v->stage, &v->stageMem, &v->stageMap)) return 0;
  VkImageCreateInfo ic = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
  ic.imageType = VK_IMAGE_TYPE_2D; ic.format = VK_FORMAT_R8G8B8A8_UNORM; ic.extent.width = (uint32_t)picW; ic.extent.height = (uint32_t)picH;
  ic.extent.depth = 1; ic.mipLevels = 1; ic.arrayLayers = 1; ic.samples = VK_SAMPLE_COUNT_1_BIT; ic.tiling = VK_IMAGE_TILING_OPTIMAL;
  ic.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT; ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(v->dev, &ic, NULL, &v->pic) != VK_SUCCESS) return 0;
  VkMemoryRequirements mr; vkGetImageMemoryRequirements(v->dev, v->pic, &mr);
  VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO }; ai.allocationSize = mr.size;
  ai.memoryTypeIndex = memType(v, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(v->dev, &ai, NULL, &v->picMem) != VK_SUCCESS) return 0;
  vkBindImageMemory(v->dev, v->pic, v->picMem, 0);
  { VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO }; vi.image = v->pic; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 1;
    vkCreateImageView(v->dev, &vi, NULL, &v->picView); }                                          // (the slang chain samples it)
  v->picLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  v->picW = picW; v->picH = picH;
  memset(v->stageMap, 0, (size_t)picW * picH * 4); v->upload = 1;
  return 1;
}
int CtVkSetPicSize(CtVk* v, int w, int h) {
  if (!v || v->recording) return 0;
  vkDeviceWaitIdle(v->dev);
  picDestroy(v);
  return picCreate(v, w, h);
}

static void swapDestroy(CtVk* v) {
  for (uint32_t i = 0; i < v->nimg; i++) {
    if (v->fb[i]) vkDestroyFramebuffer(v->dev, v->fb[i], NULL);
    if (v->view[i]) vkDestroyImageView(v->dev, v->view[i], NULL);
    if (v->done[i]) vkDestroySemaphore(v->dev, v->done[i], NULL);
    v->fb[i] = VK_NULL_HANDLE; v->view[i] = VK_NULL_HANDLE; v->done[i] = VK_NULL_HANDLE;
  }
}
static int swapCreate(CtVk* v) {
  VkSurfaceCapabilitiesKHR caps; vkGetPhysicalDeviceSurfaceCapabilitiesKHR(v->phys, v->surf, &caps);
  int w = 0, h = 0; winSize(v->win, &w, &h);
  if (caps.currentExtent.width != UINT32_MAX) v->ext = caps.currentExtent;
  else { v->ext.width = (uint32_t)w; v->ext.height = (uint32_t)h; }
  if (v->ext.width == 0 || v->ext.height == 0) return 0;          // minimised: no swapchain until it comes back
  uint32_t n = 0; VkPresentModeKHR pm[16];
  vkGetPhysicalDeviceSurfacePresentModesKHR(v->phys, v->surf, &n, NULL); if (n > 16) n = 16;
  vkGetPhysicalDeviceSurfacePresentModesKHR(v->phys, v->surf, &n, pm);
  VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;                 // vsync: FIFO, always there
  if (!v->vsync) for (uint32_t i = 0; i < n; i++) {
    if (pm[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) { mode = pm[i]; break; }
    if (pm[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = pm[i]; }
  uint32_t count = caps.minImageCount + 1;
  if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
  if (count > MAXIMG) count = MAXIMG;
  v->minImg = caps.minImageCount < 2 ? 2 : caps.minImageCount;
  v->swapUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) v->swapUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  VkSwapchainCreateInfoKHR si = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
  si.surface = v->surf; si.minImageCount = count; si.imageFormat = v->fmt; si.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  si.imageExtent = v->ext; si.imageArrayLayers = 1; si.imageUsage = v->swapUsage;
  si.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; si.preTransform = caps.currentTransform;
  si.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR; si.presentMode = mode; si.clipped = VK_TRUE;
  VkSwapchainKHR old = v->swap; si.oldSwapchain = old;
  if (vkCreateSwapchainKHR(v->dev, &si, NULL, &v->swap) != VK_SUCCESS) { v->swap = VK_NULL_HANDLE; return 0; }
  if (old) vkDestroySwapchainKHR(v->dev, old, NULL);
  v->nimg = MAXIMG; vkGetSwapchainImagesKHR(v->dev, v->swap, &v->nimg, NULL); if (v->nimg > MAXIMG) v->nimg = MAXIMG;
  vkGetSwapchainImagesKHR(v->dev, v->swap, &v->nimg, v->img);
  for (uint32_t i = 0; i < v->nimg; i++) {
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = v->img[i]; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = v->fmt;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(v->dev, &vi, NULL, &v->view[i]) != VK_SUCCESS) return 0;
    VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fi.renderPass = v->pass; fi.attachmentCount = 1; fi.pAttachments = &v->view[i];
    fi.width = v->ext.width; fi.height = v->ext.height; fi.layers = 1;
    if (vkCreateFramebuffer(v->dev, &fi, NULL, &v->fb[i]) != VK_SUCCESS) return 0;
    VkSemaphoreCreateInfo sc = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    if (vkCreateSemaphore(v->dev, &sc, NULL, &v->done[i]) != VK_SUCCESS) return 0;
  }
  v->needRecreate = 0;
  return 1;
}
static int swapRecreate(CtVk* v) {
  vkDeviceWaitIdle(v->dev);
  swapDestroy(v);
  return swapCreate(v);
}

int CtVkStart(SDL_Window* win, int vsync, int picW, int picH, CtVk** out) {
  *out = NULL;
#ifdef __SWITCH__
  vkGetInstanceProcAddr_ = vkGetInstanceProcAddr;
#else
  vkGetInstanceProcAddr_ = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
#endif
  if (!vkGetInstanceProcAddr_) { fprintf(stderr, "[video] Vulkan: no loader (%s)\n", SDL_GetError()); return 0; }
  vkCreateInstance = (PFN_vkCreateInstance)vkGetInstanceProcAddr_(NULL, "vkCreateInstance");
  if (!vkCreateInstance) return 0;
  CtVk* v = (CtVk*)calloc(1, sizeof *v);
  if (!v) return 0;
  v->win = win; v->picW = picW; v->picH = picH; v->vsync = vsync;
  unsigned ne = 0; const char* ext[16];
#ifdef __SWITCH__
  ext[ne++] = VK_KHR_SURFACE_EXTENSION_NAME; ext[ne++] = VK_NN_VI_SURFACE_EXTENSION_NAME;
#else
  if (!SDL_Vulkan_GetInstanceExtensions(win, &ne, NULL) || ne > 16 || !SDL_Vulkan_GetInstanceExtensions(win, &ne, ext)) goto fail;
#endif
  VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
  app.pApplicationName = "Chrono Trigger (native)"; app.apiVersion = VK_API_VERSION_1_0;
  VkInstanceCreateInfo ii = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
  ii.pApplicationInfo = &app; ii.enabledExtensionCount = ne; ii.ppEnabledExtensionNames = ext;
  if (vkCreateInstance(&ii, NULL, &v->inst) != VK_SUCCESS) { fprintf(stderr, "[video] Vulkan: no instance\n"); goto fail; }
#define LOADI(f) if (!(f = (PFN_##f)vkGetInstanceProcAddr_(v->inst, #f))) { fprintf(stderr, "[video] Vulkan: no %s\n", #f); goto fail; }
  VK_FUNCS_INSTANCE(LOADI)
#undef LOADI
#ifdef __SWITCH__
  { PFN_vkCreateViSurfaceNN createVi = (PFN_vkCreateViSurfaceNN)vkGetInstanceProcAddr_(v->inst, "vkCreateViSurfaceNN");
    VkViSurfaceCreateInfoNN si = { VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN }; si.window = nwindowGetDefault();
    { int sw, sh; winSize(NULL, &sw, &sh); nwindowSetDimensions((NWindow*)si.window, (u32)sw, (u32)sh); }          // (1920x1080 in every mode, as deko3d and SDL's EGL window; the surface's extent follows)
    if (!createVi || createVi(v->inst, &si, NULL, &v->surf) != VK_SUCCESS) { fprintf(stderr, "[video] Vulkan: no surface (VK_NN_vi_surface)\n"); goto fail; } }
#else
  if (!SDL_Vulkan_CreateSurface(win, v->inst, &v->surf)) { fprintf(stderr, "[video] Vulkan: no surface (%s)\n", SDL_GetError()); goto fail; }
#endif
  { uint32_t nd = 0; VkPhysicalDevice pd[16];             // a device whose queue draws and presents; a discrete GPU first
    vkEnumeratePhysicalDevices(v->inst, &nd, NULL); if (nd > 16) nd = 16;
    vkEnumeratePhysicalDevices(v->inst, &nd, pd);
    int best = -1;
    for (int pass = 0; pass < 2 && best < 0; pass++)
      for (uint32_t i = 0; i < nd && best < 0; i++) {
        VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pd[i], &pp);
        if (pass == 0 && pp.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) continue;
        uint32_t nq = 0; VkQueueFamilyProperties qp[16];
        vkGetPhysicalDeviceQueueFamilyProperties(pd[i], &nq, NULL); if (nq > 16) nq = 16;
        vkGetPhysicalDeviceQueueFamilyProperties(pd[i], &nq, qp);
        for (uint32_t q = 0; q < nq; q++) {
          VkBool32 pres = VK_FALSE; vkGetPhysicalDeviceSurfaceSupportKHR(pd[i], q, v->surf, &pres);
          if ((qp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && pres) {
            best = (int)i; v->qfam = q;
            snprintf(v->name, sizeof v->name, "Vulkan %u.%u (%s)", VK_VERSION_MAJOR(pp.apiVersion), VK_VERSION_MINOR(pp.apiVersion), pp.deviceName);
            break; } } }
    if (best < 0) { fprintf(stderr, "[video] Vulkan: no device presents to the window\n"); goto fail; }
    v->phys = pd[best]; }
  { float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qi.queueFamilyIndex = v->qfam; qi.queueCount = 1; qi.pQueuePriorities = &prio;
    const char* dext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo di = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi; di.enabledExtensionCount = 1; di.ppEnabledExtensionNames = dext;
    if (vkCreateDevice(v->phys, &di, NULL, &v->dev) != VK_SUCCESS) { fprintf(stderr, "[video] Vulkan: no device\n"); goto fail; } }
#define LOADD(f) if (!(f = (PFN_##f)vkGetDeviceProcAddr(v->dev, #f))) { fprintf(stderr, "[video] Vulkan: no %s\n", #f); goto fail; }
  VK_FUNCS_DEVICE(LOADD)
#undef LOADD
  vkGetDeviceQueue(v->dev, v->qfam, 0, &v->queue);
  { uint32_t nf = 0; VkSurfaceFormatKHR sf[32];           // an 8-bit UNORM format the blit can write
    vkGetPhysicalDeviceSurfaceFormatsKHR(v->phys, v->surf, &nf, NULL); if (nf > 32) nf = 32;
    vkGetPhysicalDeviceSurfaceFormatsKHR(v->phys, v->surf, &nf, sf);
    v->fmt = VK_FORMAT_UNDEFINED;
    for (uint32_t i = 0; i < nf; i++) {
      VkFormat f = sf[i].format == VK_FORMAT_UNDEFINED ? VK_FORMAT_B8G8R8A8_UNORM : sf[i].format;
      if (f != VK_FORMAT_B8G8R8A8_UNORM && f != VK_FORMAT_R8G8B8A8_UNORM) continue;
      VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(v->phys, f, &fp);
      if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) { v->fmt = f; break; } }
    if (v->fmt == VK_FORMAT_UNDEFINED) { fprintf(stderr, "[video] Vulkan: no blittable swapchain format\n"); goto fail; } }
  { VkAttachmentDescription a = { 0 };                    // the overlay's pass: it LOADS the picture the blit put there
    a.format = v->fmt; a.samples = VK_SAMPLE_COUNT_1_BIT; a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; a.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference r = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp = { 0 }; sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sp.colorAttachmentCount = 1; sp.pColorAttachments = &r;
    VkSubpassDependency dep = { 0 }; dep.srcSubpass = VK_SUBPASS_EXTERNAL; dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    VkRenderPassCreateInfo ri = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    ri.attachmentCount = 1; ri.pAttachments = &a; ri.subpassCount = 1; ri.pSubpasses = &sp; ri.dependencyCount = 1; ri.pDependencies = &dep;
    if (vkCreateRenderPass(v->dev, &ri, NULL, &v->pass) != VK_SUCCESS) goto fail; }
  { VkCommandPoolCreateInfo ci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; ci.queueFamilyIndex = v->qfam;
    if (vkCreateCommandPool(v->dev, &ci, NULL, &v->pool) != VK_SUCCESS) goto fail;
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = v->pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(v->dev, &ai, &v->cmd) != VK_SUCCESS) goto fail;
    VkSemaphoreCreateInfo sc = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    if (vkCreateSemaphore(v->dev, &sc, NULL, &v->avail) != VK_SUCCESS) goto fail;
    VkFenceCreateInfo fc = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }; fc.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateFence(v->dev, &fc, NULL, &v->fence) != VK_SUCCESS) goto fail; }
  if (!picCreate(v, picW, picH)) goto fail;
  { VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16 };   // the overlay's (its font)
    VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT; pi.maxSets = 16; pi.poolSizeCount = 1; pi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(v->dev, &pi, NULL, &v->dpool) != VK_SUCCESS) goto fail; }
  { VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(v->phys, &pp);   // GPU timestamps, when the queue has them
    if (pp.limits.timestampComputeAndGraphics && pp.limits.timestampPeriod > 0) {
      VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO }; qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = 2;
      if (vkCreateQueryPool(v->dev, &qi, NULL, &v->qp) == VK_SUCCESS) v->tsPeriod = pp.limits.timestampPeriod; else v->qp = VK_NULL_HANDLE; } }
  if (!swapCreate(v)) { fprintf(stderr, "[video] Vulkan: no swapchain\n"); goto fail; }
  *out = v;
  return 1;
fail:
  CtVkStop(v);
  return 0;
}

void CtVkStop(CtVk* v) {
  if (!v) return;
  if (v->dev) {
    vkDeviceWaitIdle(v->dev);
#ifdef CT_HAVE_SLANG
    slStop(v);
#endif
    swapDestroy(v);
    if (v->swap) vkDestroySwapchainKHR(v->dev, v->swap, NULL);
    if (v->dpool) vkDestroyDescriptorPool(v->dev, v->dpool, NULL);
    if (v->qp) vkDestroyQueryPool(v->dev, v->qp, NULL);
    if (v->rb) { vkDestroyBuffer(v->dev, v->rb, NULL); vkFreeMemory(v->dev, v->rbMem, NULL); }
    picDestroy(v);
    if (v->fence) vkDestroyFence(v->dev, v->fence, NULL);
    if (v->avail) vkDestroySemaphore(v->dev, v->avail, NULL);
    if (v->pool) vkDestroyCommandPool(v->dev, v->pool, NULL);
    if (v->pass) vkDestroyRenderPass(v->dev, v->pass, NULL);
    vkDestroyDevice(v->dev, NULL);
  }
  if (v->surf && vkDestroySurfaceKHR) vkDestroySurfaceKHR(v->inst, v->surf, NULL);
  if (v->inst && vkDestroyInstance) vkDestroyInstance(v->inst, NULL);
  free(v);
}

const char* CtVkName(const CtVk* v) { return v ? v->name : ""; }
void CtVkOutputSize(CtVk* v, int* w, int* h) { if (v->ext.width && v->ext.height) { *w = (int)v->ext.width; *h = (int)v->ext.height; } else winSize(v->win, w, h); }
double CtVkGpuMs(CtVk* v, long* seq) { if (seq) *seq = v->gpuSeq; return v->lastGpuMs; }
double CtVkWaitedMs(CtVk* v) { return v->waitedMs; }
void CtVkSetVsync(CtVk* v, int on) { if (v->vsync != (on != 0)) { v->vsync = on != 0; v->needRecreate = 1; } }

void CtVkUpload(CtVk* v, const uint8_t* rgb) {
  uint8_t* d = (uint8_t*)v->stageMap;
  for (int i = 0, n = v->picW * v->picH; i < n; i++) { d[0] = rgb[0]; d[1] = rgb[1]; d[2] = rgb[2]; d[3] = 255; d += 4; rgb += 3; }
  v->upload = 1;
}


// ---- the slang shader chain (src/slang/slang_vk.c): a preset's passes between the picture's upload and the overlay -----------------------------------------------------------------------------
#ifdef CT_HAVE_SLANG
struct SlState {
  CtSlChain* chain; CtSlGpuOps ops; void* dev; void* orig;
  char status[600]; char path[256]; int pending, failed; unsigned long long frame;
};
static int slBeginFinal(void* user, const CtSlRect* vp) {         // the last pass: the backbuffer for drawing, the host's pass begun on it, the viewport on the picture's rectangle
  CtVk* v = (CtVk*)user; VkCommandBuffer c = v->cmd; VkImage im = v->img[v->cur];
  barrier(c, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
  VkRenderPassBeginInfo rp = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO }; rp.renderPass = v->pass; rp.framebuffer = v->fb[v->cur]; rp.renderArea.extent = v->ext;
  vkCmdBeginRenderPass(c, &rp, VK_SUBPASS_CONTENTS_INLINE);
  VkViewport vw = { (float)vp->x, (float)vp->y, (float)vp->w, (float)vp->h, 0.0f, 1.0f }; VkRect2D sc = { { vp->x, vp->y }, { (uint32_t)vp->w, (uint32_t)vp->h } };
  vkCmdSetViewport(c, 0, 1, &vw); vkCmdSetScissor(c, 0, 1, &sc);
  v->recording = 1;
  return 0;
}
static void slStop(CtVk* v) {                                      // (the device is idle or the frame is not in flight)
  if (!v->sl) return;
  if (v->sl->chain) { CtSlChainFree(v->sl->chain); v->sl->chain = NULL; }
  if (v->sl->orig) { CtSlVkWrapFree(v->sl->orig); v->sl->orig = NULL; }
  if (v->sl->dev) { CtSlVkDestroy(v->sl->dev); v->sl->dev = NULL; }
  free(v->sl); v->sl = NULL;
}
// A requested preset is (re)loaded at the start of a frame, after the fence wait (nothing of ours is in flight) and before the commands are recorded.
static void slApply(CtVk* v) {
  struct SlState* s = v->sl; s->pending = 0; s->failed = 0; s->status[0] = 0;
  if (s->chain) { vkDeviceWaitIdle(v->dev); CtSlChainFree(s->chain); s->chain = NULL; }
  if (!s->path[0]) return;
  char err[2048];
  if (!s->dev) {
    CtSlVkEnv env = { (PFN_vkGetInstanceProcAddr)vkGetInstanceProcAddr_, v->inst, v->phys, v->dev, v->queue, v->qfam, v->pass, slBeginFinal, v };
    if (CtSlVkCreate(&env, &s->ops, &s->dev, err, sizeof err)) { snprintf(s->status, sizeof s->status, "%s", err); return; }
    s->orig = CtSlVkWrap(s->dev, v->pic, v->picView, v->picW, v->picH);
  }
  double t0 = nowMsVk();
  CtSlChain* c = CtSlChainLoad(s->path, &s->ops, s->dev, NULL, NULL, err, sizeof err);
  if (!c) { snprintf(s->status, sizeof s->status, "%s", err); fprintf(stderr, "[slang] %s: %s\n", s->path, err); fflush(stderr); return; }
  s->chain = c; s->frame = 0; v->upload = 1;
  snprintf(s->status, sizeof s->status, "%s: %d passes, %d parameters, loaded in %.1f s%s%s", s->path, CtSlChainPasses(c), CtSlChainParamCount(c), (nowMsVk() - t0) / 1000.0,
           CtSlChainWarnings(c)[0] ? "; warnings: " : "", CtSlChainWarnings(c));
  fprintf(stderr, "[slang] %s\n", s->status); fflush(stderr);
}
// Run the chain into the rectangle `d`. 1 = it drew (the host's pass is open); 0 = it did not start (draw the plain picture).
static int slRun(CtVk* v, SDL_Rect d, int linear) {
  struct SlState* s = v->sl;
  CtSlVkWrapSet(s->orig, v->pic, v->picView, v->picW, v->picH);
  CtSlFrame fr = { s->orig, v->picW, v->picH, s->frame++, { d.x, d.y, d.w, d.h }, linear, 60.0f, d.h > 0 ? (float)d.w / (float)d.h : 0.0f };
  CtSlVkBeginFrame(s->dev, v->cmd);
  if (CtSlChainRun(s->chain, &fr)) {
    s->failed = 1; snprintf(s->status, sizeof s->status, "the shader chain failed to run; drawing the plain picture");
    return v->recording;                                           // (if its last pass already opened the host's pass, the frame goes on in it)
  }
  return 1;
}
int CtVkShaderSet(CtVk* v, const char* path) {
  if (!v) return 0;
  if (!v->sl) { if (!path || !*path) return 0; v->sl = (struct SlState*)calloc(1, sizeof *v->sl); if (!v->sl) return 0; }
  snprintf(v->sl->path, sizeof v->sl->path, "%s", path ? path : ""); v->sl->pending = 1;
  snprintf(v->sl->status, sizeof v->sl->status, "%s", path && *path ? "loading..." : "");
  return 1;
}
const char* CtVkShaderStatus(CtVk* v) { return v && v->sl ? v->sl->status : ""; }
void* CtVkShaderChain(CtVk* v) { return v && v->sl ? (void*)v->sl->chain : NULL; }
int CtVkShaderWanted(CtVk* v) { return v && v->sl && v->sl->path[0] && !v->sl->failed; }
#else
int CtVkShaderSet(CtVk* v, const char* path) { (void)v; (void)path; return 0; }
const char* CtVkShaderStatus(CtVk* v) { (void)v; return "this build has no slang shader support"; }
void* CtVkShaderChain(CtVk* v) { (void)v; return NULL; }
int CtVkShaderWanted(CtVk* v) { (void)v; return 0; }
#endif

// The frame up to the overlay: acquire, upload, clear, blit, open the pass. Drawing again before the swap re-records
// the same acquired image (the overlay probe draws three times).
void CtVkDraw(CtVk* v, const SDL_Rect* dst, int linear) {
  int w = 0, h = 0; winSize(v->win, &w, &h);
  if (w <= 0 || h <= 0) { v->recording = 0; return; }             // minimised
  if (!v->acquired) {
    if (v->needRecreate || v->ext.width != (uint32_t)w || v->ext.height != (uint32_t)h || !v->swap)
      if (!swapRecreate(v)) { v->recording = 0; return; }
    double w0 = nowMsVk();
    vkWaitForFences(v->dev, 1, &v->fence, VK_TRUE, UINT64_MAX);
    if (v->qArmed && v->qp) {                                      // the last frame is done: the GPU's own clock says how long it took
      uint64_t ts[2];
      if (vkGetQueryPoolResults(v->dev, v->qp, 0, 2, sizeof ts, ts, sizeof ts[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[1] > ts[0]) {
        v->lastGpuMs = (double)(ts[1] - ts[0]) * v->tsPeriod / 1e6; v->gpuSeq++; }
      v->qArmed = 0; }
    VkResult r = vkAcquireNextImageKHR(v->dev, v->swap, UINT64_MAX, v->avail, VK_NULL_HANDLE, &v->cur);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
      if (!swapRecreate(v)) { v->recording = 0; return; }
      r = vkAcquireNextImageKHR(v->dev, v->swap, UINT64_MAX, v->avail, VK_NULL_HANDLE, &v->cur);
    }
    v->waitedMs += nowMsVk() - w0;
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { v->recording = 0; return; }
    v->acquired = 1;
  }
#ifdef CT_HAVE_SLANG
  if (v->sl && v->sl->pending) slApply(v);
  int chainOn = v->sl && v->sl->chain && !v->sl->failed;
  if (chainOn && CtSlChainCapturePending(v->sl->chain)) {                    // a capture of every pass was asked for: now, before this frame is recorded (the GPU is idle, the picture is the last frame's)
    CtSlVkWrapSet(v->sl->orig, v->pic, v->picView, v->picW, v->picH);
    CtSlChainCaptureAuto(v->sl->chain);
  }
#else
  const int chainOn = 0;
#endif
  VkCommandBuffer c = v->cmd;
  vkResetCommandBuffer(c, 0);
  VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(c, &bi);
  if (v->qp) { vkCmdResetQueryPool(c, v->qp, 0, 2); vkCmdWriteTimestamp(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, v->qp, 0); }
  if (v->upload || v->picLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
    barrier(c, v->pic, v->picLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy bc = { 0 }; bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; bc.imageSubresource.layerCount = 1;
    bc.imageExtent.width = (uint32_t)v->picW; bc.imageExtent.height = (uint32_t)v->picH; bc.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(c, v->stage, v->pic, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
    barrier(c, v->pic, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    v->picLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; v->upload = 0;
  }
  { VkImageLayout want = chainOn ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;   // the blit reads it as a transfer source, the shader chain samples it
    if (v->picLayout != want) { barrier(c, v->pic, v->picLayout, want, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT); v->picLayout = want; } }
  VkImage im = v->img[v->cur];
  barrier(c, im, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkClearColorValue black = { { 0, 0, 0, 1 } };
  VkImageSubresourceRange rr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
  vkCmdClearColorImage(c, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rr);
  SDL_Rect d = dst ? *dst : (SDL_Rect){ 0, 0, (int)v->ext.width, (int)v->ext.height };
  if (d.x < 0) { d.w += d.x; d.x = 0; } if (d.y < 0) { d.h += d.y; d.y = 0; }
  if (d.x + d.w > (int)v->ext.width) d.w = (int)v->ext.width - d.x;
  if (d.y + d.h > (int)v->ext.height) d.h = (int)v->ext.height - d.y;
  int chained = 0;
#ifdef CT_HAVE_SLANG
  if (chainOn && d.w > 0 && d.h > 0) chained = slRun(v, d, linear);
#endif
  if (!chained && d.w > 0 && d.h > 0) {
    VkImageBlit b = { 0 };
    b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.srcSubresource.layerCount = 1;
    b.srcOffsets[1].x = v->picW; b.srcOffsets[1].y = v->picH; b.srcOffsets[1].z = 1;
    b.dstSubresource = b.srcSubresource;
    b.dstOffsets[0].x = d.x; b.dstOffsets[0].y = d.y; b.dstOffsets[1].x = d.x + d.w; b.dstOffsets[1].y = d.y + d.h; b.dstOffsets[1].z = 1;
    barrier(c, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);   // clear -> blit
    vkCmdBlitImage(c, v->pic, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                   linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
  }
  if (!chained) {
    barrier(c, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkRenderPassBeginInfo rp = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rp.renderPass = v->pass; rp.framebuffer = v->fb[v->cur]; rp.renderArea.extent = v->ext;
    vkCmdBeginRenderPass(c, &rp, VK_SUBPASS_CONTENTS_INLINE);
  }
  v->recording = 1;
}

// Close the pass and the commands; `readback`: copy the image to the probe's buffer first. Submits and waits for the
// fence only when reading back.
static int finish(CtVk* v, int readback) {
  VkCommandBuffer c = v->cmd;
  vkCmdEndRenderPass(c);                                           // -> PRESENT_SRC
  if (v->qp) { vkCmdWriteTimestamp(c, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, v->qp, 1); v->qArmed = 1; }
  int rbOk = 0;
  if (readback && (v->swapUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
    VkDeviceSize need = (VkDeviceSize)v->ext.width * v->ext.height * 4;
    if (v->rbSize < need) {
      if (v->rb) { vkDestroyBuffer(v->dev, v->rb, NULL); vkFreeMemory(v->dev, v->rbMem, NULL); v->rb = VK_NULL_HANDLE; }
      if (makeBuffer(v, need, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &v->rb, &v->rbMem, &v->rbMap)) v->rbSize = need;
    }
    if (v->rb) {
      VkImage im = v->img[v->cur];
      barrier(c, im, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
              VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
      VkBufferImageCopy bc = { 0 }; bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; bc.imageSubresource.layerCount = 1;
      bc.imageExtent.width = v->ext.width; bc.imageExtent.height = v->ext.height; bc.imageExtent.depth = 1;
      vkCmdCopyImageToBuffer(c, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, v->rb, 1, &bc);
      barrier(c, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_READ_BIT, 0,
              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
      rbOk = 1;
    }
  }
  vkEndCommandBuffer(c);
  VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
  VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
  si.waitSemaphoreCount = 1; si.pWaitSemaphores = &v->avail; si.pWaitDstStageMask = &ws;
  si.commandBufferCount = 1; si.pCommandBuffers = &c; si.signalSemaphoreCount = 1; si.pSignalSemaphores = &v->done[v->cur];
  vkResetFences(v->dev, 1, &v->fence);
  vkQueueSubmit(v->queue, 1, &si, v->fence);
  if (readback) vkWaitForFences(v->dev, 1, &v->fence, VK_TRUE, UINT64_MAX);
  VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
  pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &v->done[v->cur]; pi.swapchainCount = 1; pi.pSwapchains = &v->swap; pi.pImageIndices = &v->cur;
  VkResult r = vkQueuePresentKHR(v->queue, &pi);
  if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) v->needRecreate = 1;
  v->acquired = 0; v->recording = 0;
  return rbOk;
}
void CtVkSwap(CtVk* v) { if (v->recording) finish(v, 0); }

int CtVkSaveBMP(CtVk* v, const char* path) {
  if (!v->recording) return 0;
  if (!finish(v, 1)) return 0;
  int w = (int)v->ext.width, h = (int)v->ext.height;
  SDL_Surface* sf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
  if (!sf) return 0;
  const uint8_t* s = (const uint8_t*)v->rbMap;
  for (int y = 0; y < h; y++) {
    uint8_t* d = (uint8_t*)sf->pixels + (size_t)y * sf->pitch;
    for (int x = 0; x < w; x++, s += 4, d += 4) {                // the image's BGRA or RGBA -> ARGB8888 (memory B G R A)
      if (v->fmt == VK_FORMAT_B8G8R8A8_UNORM) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }
      else { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; }
      d[3] = 255; } }
  int ok = SDL_SaveBMP(sf, path) == 0;
  SDL_FreeSurface(sf);
  return ok;
}

int CtVkInfoGet(CtVk* v, CtVkInfo* o) {
  if (!v) return 0;
  o->instance = v->inst; o->physicalDevice = v->phys; o->device = v->dev; o->queueFamily = v->qfam; o->queue = v->queue;
  o->descriptorPool = v->dpool; o->renderPass = v->pass; o->minImageCount = v->minImg; o->imageCount = v->nimg;
  return 1;
}
void* CtVkCommandBuffer(CtVk* v) { return v && v->recording ? (void*)v->cmd : NULL; }
void* CtVkProc(CtVk* v, const char* name) {
  return v && vkGetInstanceProcAddr_ ? (void*)vkGetInstanceProcAddr_(v->inst, name) : NULL;
}
#endif
