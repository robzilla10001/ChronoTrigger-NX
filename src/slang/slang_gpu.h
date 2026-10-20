// slang_gpu.h - what a video backend gives the shader chain (slang_chain.h). The chain knows presets, passes, sizes and uniform semantics and nothing about a GPU; a backend (OpenGL, Vulkan, deko3d)
// fills one CtSlGpuOps with eleven small functions, and the chain drives it. A texture is an opaque pointer to the backend; so is a program.
//
// Conventions every backend keeps, so a shader written for the spec's Vulkan means the same everywhere:
//   - A texture's row 0 is the TOP of the picture, and uv (0,0) is the top-left. The chain passes an MVP that maps the quad's Position (0..1) to clip space (-1..1) y-down (Vulkan's), and
//     OpenGL-like backends whose framebuffers count rows upward store their offscreen textures the same way (row 0 = clip y -1); only the backbuffer differs: `flipBackbuffer`.
//   - A texture is cleared to transparent black when created (frames before the first are black, as the spec says).
//   - The final pass renders into the rectangle `vp` of the backbuffer; the backend has already cleared the rest.
#ifndef CT_SLANG_GPU_H
#define CT_SLANG_GPU_H
#include "slang_compile.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct CtSlRect { int x, y, w, h; } CtSlRect;

typedef struct CtSlBind {
  uint32_t binding;                    // the sampler's binding in the program
  void* tex;
  int filter;                          // CT_SL_FILTER_NEAREST / LINEAR
  int wrap;                            // CtSlWrap
  int mip;                             // 1: sample with the texture's mip chain (it has been generated)
} CtSlBind;

typedef struct CtSlGpuOps {
  const char* name;
  int flipBackbuffer;                  // 1: rows of the backbuffer count UP (OpenGL): the chain flips the final pass's MVP
  int flipOffscreen;                   // 1: the same for the offscreen passes (deko3d: clip +y is row 0 of EVERY render target)
  int wantsGlsl;                       // 1: progCreate reads CtSlProgram.glsl[] (the chain calls CtSlTranslate with glslVersion / glslEs); 0: it reads the SPIR-V
  int glslVersion, glslEs;
  int maxPushBytes;                    // a backend without push constants returns the block as a UBO (CtSlProgram.pushBinding); 0 = unlimited (that case)

  void* (*texCreate)(void* dev, int w, int h, CtSlFormat fmt, int mip);                         // sampled + render target; cleared to 0
  void* (*texFromRgba)(void* dev, const uint8_t* rgba, int w, int h, int mip);                  // a look-up texture (R8G8B8A8, top row first); mip: generate the chain
  void  (*texDestroy)(void* dev, void* tex);
  void  (*texCopy)(void* dev, void* dst, void* src, int w, int h);                              // same size and format (the history ring)
  void  (*texGenMips)(void* dev, void* tex);                                                    // after a pass rendered into it, before a pass that samples it with mip = 1

  void* (*progCreate)(void* dev, const CtSlProgram* p, CtSlFormat fmt, char* err, size_t errN); // 'fmt': the render target's format (a Vulkan pipeline needs it)
  void  (*progDestroy)(void* dev, void* prog);

  int   (*passBegin)(void* dev, void* target, const CtSlRect* vp);                              // target NULL = the backbuffer. 0 = ok
  void  (*passDraw)(void* dev, void* prog, const CtSlBind* binds, int nBinds, const void* ubo, size_t uboN, const void* push, size_t pushN);
  void  (*passEnd)(void* dev, void* target);

  // For the capture (slang_chain.h): a PRIVATE frame -- the passes run in a command buffer of the backend's own, submitted and waited for at frameEnd -- and a readback of a texture as
  // top-row-first RGBA8 (malloc'd; the texture is RGBA8). Any of them may be NULL (OpenGL needs no private frame): no texReadback = no capture on that backend.
  void     (*setCompilerOpt)(int level);       // optional: the shader compiler's optimisation level for the programs created next (deko3d / uam); the capture's what-if for a miscompiled pass
  void     (*frameBegin)(void* dev);
  void     (*frameEnd)(void* dev);
  uint8_t* (*texReadback)(void* dev, void* tex, int* w, int* h);
} CtSlGpuOps;

#ifdef __cplusplus
}
#endif
#endif
