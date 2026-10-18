// present.h - how ct_field puts its picture on the screen: the window, a backend that uploads the 256x224 RGB24 frame,
// places and filters it, and swaps. The game composes into its own buffer and never talks to a graphics API; the F1
// overlay (overlay.cpp) draws through the same backend.
//
// Backends (settings.h `backend`, chosen at start):
//   CT_BACKEND_SDL     SDL_Renderer (Direct3D / OpenGL / software, settings `render_driver`) -- also the headless one
//   CT_BACKEND_OPENGL  an OpenGL 3.3 core context: the frame is a texture, blitted to the back buffer
//   CT_BACKEND_VULKAN  Vulkan 1.0 (present_vk.c; built when third_party/vulkan has the Khronos headers; the PC only)
//   CT_BACKEND_DEKO3D  the Switch's own API (present_dk.cpp, libdeko3d; the Switch build only)
// A backend that cannot start falls back to SDL_Renderer, and SDL_Renderer to its software renderer.
#ifndef CT_PRESENT_H
#define CT_PRESENT_H
#include "SDL.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { CT_BACKEND_SDL = 0, CT_BACKEND_OPENGL = 1, CT_BACKEND_VULKAN = 2, CT_BACKEND_DEKO3D = 3 };

typedef struct CtPresenter CtPresenter;

typedef struct CtPresentConfig {
  const char* title;
  int w, h;                  // the window's size
  int fullscreen;            // desktop fullscreen
  int backend;               // CT_BACKEND_*
  const char* sdlDriver;     // SDL_Renderer driver hint ("" = SDL's choice)
  int vsync;
  int software;              // headless: SDL's software renderer
  int logical;               // not interactive: the picture under SDL_RenderSetLogicalSize (CtPresentDraw's dst ignored)
  int hidden;                // no visible window (the overlay probe)
  int picW, picH;            // the frame (256x224)
} CtPresentConfig;

CtPresenter*  CtPresentCreate(const CtPresentConfig* cfg);
void          CtPresentDestroy(CtPresenter* p);
SDL_Window*   CtPresentWindow(CtPresenter* p);
const char*   CtPresentName(const CtPresenter* p);        // e.g. "OpenGL 3.3 (NVIDIA ...)" / "SDL direct3d11"
void          CtPresentUpload(CtPresenter* p, const uint8_t* rgb24);   // a picW x picH RGB24 frame
// Clear to black and draw the frame into `dst` (window pixels; NULL = the whole target -- headless, under the logical
// size), nearest or linear.
// The plain k-times pixel replication of a w x 224 RGB24 frame, straight into the SDL texture's RGBA (one pass, on the host pool, no
// intermediate RGB24 picture): the same bytes as motion.c's upscale() followed by CtPresentUpload. The picture size must already be
// w*k x 224*k (CtPresentSetPicSize). Returns 0 (nothing done) for a backend that is not the SDL_Renderer.
int           CtPresentUploadScaled(CtPresenter* p, const uint8_t* rgb, int w, int k);
void          CtPresentDraw(CtPresenter* p, const SDL_Rect* dst, int linear);
void          CtPresentSetPicSize(CtPresenter* p, int w, int h);   // the uploaded picture's size (the internal scale)
void          CtPresentSwap(CtPresenter* p);
// THE RENDER THREAD (SDL_Renderer backend, accelerated renderer only; the Switch's default, elsewhere CT_ASYNC_PRESENT=1): upload, draw
// and swap run on their own thread, overlapping the next frame's engine work. While it is on, Upload converts into a staging buffer
// and Swap hands the frame over (and waits only if the previous one is not done yet). Anything else that touches the renderer from the
// main thread (the overlay, a resize, a screenshot) must stand on a sync point: SetAsync(0) / Sync.
void          CtPresentSetAsync(CtPresenter* p, int on);              // on: start the thread if the renderer allows; off: wait for it, then back to direct calls
void          CtPresentDrawHud(CtPresenter* p);                       // the OpenGL backend's HUD (drawn by the swap; a probe draws it before a screenshot)
void          CtPresentSync(CtPresenter* p);                          // wait until the render thread has finished the last frame
void          CtPresentStopThread(CtPresenter* p);                    // end and join the render thread (at exit; the presenter itself stays)

void          CtPresentSetVsync(CtPresenter* p, int on);
void          CtPresentPicSize(CtPresenter* p, int* w, int* h);       // the uploaded picture's size: what is RENDERED
void          CtPresentOutputSize(CtPresenter* p, int* w, int* h);    // the drawable, in pixels
int           CtPresentSaveBMP(CtPresenter* p, const char* path);     // what was drawn since the last swap; 1 = saved
SDL_Renderer* CtPresentSdlRenderer(CtPresenter* p);       // CT_BACKEND_SDL: the renderer (the overlay's), else NULL
void*         CtPresentGlContext(CtPresenter* p);         // CT_BACKEND_OPENGL: the context, else NULL
struct CtDk*  CtPresentDk(CtPresenter* p);                // CT_BACKEND_DEKO3D: the backend (present_dk.h), else NULL
struct CtVk*  CtPresentVk(CtPresenter* p);                // CT_BACKEND_VULKAN: the backend (present_vk.h), else NULL
#ifdef __cplusplus
}
#endif
#endif
