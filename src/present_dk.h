// present_dk.h - the deko3d backend's entry points for present.c (the Switch's own graphics API, libdeko3d; no deko3d types here).
// The frame is an RGBA texture; a draw is one quad into the swapchain image, and the overlay (Dear ImGui) records into the same
// command buffer before the swap, as the Vulkan backend does. Built only for the Switch (CT_HAVE_DEKO3D).
#ifndef CT_PRESENT_DK_H
#define CT_PRESENT_DK_H
#include "SDL.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct CtDk CtDk;
int         CtDkStart(SDL_Window* win, int vsync, int picW, int picH, CtDk** out);   // 1 = running
void        CtDkStop(CtDk* d);
const char* CtDkName(const CtDk* d);
void        CtDkOutputSize(CtDk* d, int* w, int* h);
void        CtDkSetVsync(CtDk* d, int on);
void        CtDkUpload(CtDk* d, const uint8_t* rgb24);
void        CtDkDraw(CtDk* d, const SDL_Rect* dst, int linear);          // starts the frame: clear + the picture into `dst` (NULL = all)
int         CtDkSetPicSize(CtDk* d, int w, int h);
void        CtDkSwap(CtDk* d);                                           // submit, present
// Dear ImGui over the frame being recorded (between Draw and Swap).
int         CtDkImGuiInit(CtDk* d);                                      // the font texture; 1 = ok
void        CtDkImGuiShutdown(CtDk* d);
void        CtDkImGuiRender(CtDk* d, void* imDrawData);                  // an ImDrawData*
#ifdef __cplusplus
}
#endif
#endif
