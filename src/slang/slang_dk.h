// slang_dk.h - the deko3d implementation of the shader chain's GPU interface (slang_gpu.h). The shaders reach it as GLSL (SPIRV-Cross) and are compiled to DKSH ON THE CONSOLE by uam
// (switch/get_uam.sh builds it as a library); everything else is deko3d: render targets, a descriptor table, the passes recorded into the host's command buffer.
//
// The host (present_dk.cpp) owns the frame. Around the chain:
//     CtSlDkFrameStart(dev);                 // top of the frame, before any command is recorded: what died is freed (waiting for the GPU only if something did)
//     ... acquire, bind the swap image as the render target, upload the picture, clear ...
//     CtSlDkBeginFrame(dev, cb, slot);       // the command buffer being recorded and the frame slot 0..2 (the dynamic memory's partition)
//     CtSlChainRun(chain, &frame);           // offscreen passes bind their own render targets; the last pass draws into the host's current target, viewport and scissor on the rectangle
//     ... the host re-binds ITS state (render target, viewport, scissor, descriptor sets) and goes on with the overlay.
#ifndef CT_SLANG_DK_H
#define CT_SLANG_DK_H
#include <deko3d.h>
#include "slang_gpu.h"
#ifdef __cplusplus
extern "C" {
#endif

int  CtSlDkCreate(DkDevice dev, DkQueue q, CtSlGpuOps* ops, void** out, char* err, size_t errN);
void CtSlDkDestroy(void* dev);
void CtSlDkFrameStart(void* dev);
void CtSlDkBeginFrame(void* dev, DkCmdBuf cb, int slot);
// The swap-chain image of this frame: the last pass binds it as its render target (the pass before it left its own texture bound). Call it before CtSlChainRun each frame.
void CtSlDkSetBackbuffer(void* dev, DkImage* image);
// The game's picture as a chain texture: a DkImage the host keeps ready for sampling. Not freed by the chain. Update on a size change.
void* CtSlDkWrap(void* dev, DkImage* image, int w, int h);
void  CtSlDkWrapSet(void* dev, void* wrapped, DkImage* image, int w, int h);
void  CtSlDkWrapFree(void* dev, void* wrapped);

#ifdef __cplusplus
}
#endif
#endif
