// present_vk.h - the Vulkan backend's entry points for present.c (no Vulkan types: the handles go out as void*).
#ifndef CT_PRESENT_VK_H
#define CT_PRESENT_VK_H
#include "SDL.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct CtVk CtVk;
typedef struct CtVkInfo {                 // what ImGui's Vulkan backend needs (overlay.cpp casts them back)
  void* instance; void* physicalDevice; void* device; uint32_t queueFamily; void* queue;
  void* descriptorPool; void* renderPass; uint32_t minImageCount, imageCount;
} CtVkInfo;
int         CtVkStart(SDL_Window* win, int vsync, int picW, int picH, CtVk** out);
void        CtVkStop(CtVk* v);
const char* CtVkName(const CtVk* v);
void        CtVkOutputSize(CtVk* v, int* w, int* h);
double      CtVkGpuMs(CtVk* v, long* seq);             // the GPU time (ms) of the last finished frame, from timestamp queries; *seq counts new ones
double      CtVkWaitedMs(CtVk* v);                     // ms the CPU has spent blocked on the GPU (fence, acquire) so far
void        CtVkSetVsync(CtVk* v, int on);
void        CtVkUpload(CtVk* v, const uint8_t* rgb24);
void        CtVkDraw(CtVk* v, const SDL_Rect* dst, int linear);
int         CtVkSetPicSize(CtVk* v, int w, int h);
void        CtVkSwap(CtVk* v);
int         CtVkSaveBMP(CtVk* v, const char* path);
// SLANG SHADERS (src/slang/slang_vk.c): the preset is (re)loaded at the start of the next frame. "" / NULL = off. The status is one line for the menu; the chain is a CtSlChain* (its parameters).
int         CtVkShaderSet(CtVk* v, const char* path);
const char* CtVkShaderStatus(CtVk* v);
void*       CtVkShaderChain(CtVk* v);
int         CtVkShaderWanted(CtVk* v);                 // a preset is chosen and has not failed: the 1x picture goes up, the chain does the scaling
int         CtVkInfoGet(CtVk* v, CtVkInfo* out);
void*       CtVkCommandBuffer(CtVk* v);   // the frame being recorded (between Draw and Swap), else NULL
void*       CtVkProc(CtVk* v, const char* name);   // vkGetInstanceProcAddr(instance, name)
#ifdef __cplusplus
}
#endif
#endif
