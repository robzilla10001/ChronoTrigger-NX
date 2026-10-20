// slang_chain.h - a loaded slang preset ready to run: every pass compiled, every name the shaders use (the spec's semantics) resolved to what it means, the look-up textures loaded,
// and the render targets sized frame by frame. Backend independent: a CtSlGpuOps (slang_gpu.h) does the GPU work.
//
//   CtSlChain* c = CtSlChainLoad("shaders/crt/crt-lottes.slangp", &ops, dev, NULL, NULL, err, sizeof err);
//   every frame, the game's picture being the GPU texture `pic` of ow x oh pixels (the 1x frame; the chain does the scaling):
//     CtSlFrame f = { pic, ow, oh, frameNumber, {x, y, w, h}, ... };  CtSlChainRun(c, &f);
//
// The user-facing parameters of the shaders (#pragma parameter) are listed by CtSlChainParam*; the menu changes them live and they can be saved next to the preset.
#ifndef CT_SLANG_CHAIN_H
#define CT_SLANG_CHAIN_H
#include "slang_gpu.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct CtSlChain CtSlChain;

typedef struct CtSlChainParam { CtSlParam def; float value; int pass; /* the first pass that declares it */ } CtSlChainParam;

typedef struct CtSlFrame {
  void* original; int ow, oh;          // the game's picture: a texture of the backend, ow x oh (cleared by nothing: the chain only reads it)
  uint64_t frame;                      // frame counter (FrameCount is this, modulo the pass's frame_count_mod)
  CtSlRect dst;                        // where the last pass draws, in backbuffer pixels (FinalViewportSize is its size)
  int defaultLinear;                   // the filter for passes whose preset does not say (filter_linearN absent)
  float fps;                           // OriginalFPS
  float aspect;                        // OriginalAspect: the picture's display aspect ratio (0 = ow / oh)
} CtSlFrame;

CtSlChain* CtSlChainLoad(const char* presetPath, const CtSlGpuOps* ops, void* dev, CtSlReadFn rd, void* rdCtx, char* err, size_t errN);
void       CtSlChainFree(CtSlChain* c);
int        CtSlChainRun(CtSlChain* c, const CtSlFrame* f);        // 0 = ok

int                   CtSlChainPasses(const CtSlChain* c);
// DEBUGGING A PRESET: a capture runs the chain once per pass into a private texture, reads each one back and writes them as PNGs plus a table of numbers (mean brightness, how much is non-black,
// whether a pass is identical to the one before) to <dir>/. The backend calls CtSlChainCapture at the START of a frame when CtSlChainCapturePending says the user asked (the mods menu's toggle).
void                  CtSlChainCaptureRequest(CtSlChain* c);
int                   CtSlChainCapturePending(const CtSlChain* c);
int                   CtSlChainCapture(CtSlChain* c, const char* dir);          // 0 = done
int                   CtSlChainCaptureAuto(CtSlChain* c);                       // into logs/slang_<preset>_<time>/
const char*           CtSlChainCaptureStatus(const CtSlChain* c);
void                  CtSlChainPassInfo(const CtSlChain* c, int i, char* name, size_t n, int* w, int* h);
const char*           CtSlChainPath(const CtSlChain* c);
int                   CtSlChainParamCount(const CtSlChain* c);
const CtSlChainParam* CtSlChainParamAt(const CtSlChain* c, int i);
void                  CtSlChainParamSet(CtSlChain* c, int i, float v);          // clamped to the shader's range
void                  CtSlChainParamsReset(CtSlChain* c);                        // back to the preset's values
// The warnings of the load (a name no semantic covers, a texture format the backend lacks): "" when none. The chain still runs.
const char*           CtSlChainWarnings(const CtSlChain* c);

// An image file (PNG) as top-row-first RGBA8, malloc'd; NULL when it cannot be read or the PNG reader is not built in. (A look-up texture.)
uint8_t* CtSlLoadImage(const char* path, int* w, int* h);
// RGBA8 (top row first) to a PNG file; 1 = written (0 when the PNG writer is not built in).
int      CtSlSavePng(const char* path, const uint8_t* rgba, int w, int h);

#ifdef __cplusplus
}
#endif
#endif
