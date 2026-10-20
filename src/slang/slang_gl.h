// slang_gl.h - the OpenGL (desktop 4.3 core) implementation of the shader chain's GPU interface (slang_gpu.h). It loads the GL functions it needs through the callback it is given
// (SDL_GL_GetProcAddress in the program, OSMesaGetProcAddress in tools/slangtool), so the same code runs on the console and on a PC with a software GL.
#ifndef CT_SLANG_GL_H
#define CT_SLANG_GL_H
#include "slang_gpu.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef void* (*CtSlGlLoader)(const char* name);
// 0 = ok (`ops` is filled and `*dev` is the device to pass with it); else the missing function / the GL version in `err`.
int  CtSlGlCreate(CtSlGlLoader load, CtSlGpuOps* ops, void** dev, char* err, size_t errN);
void CtSlGlDestroy(void* dev);
// The backbuffer the final pass draws into: the framebuffer object (0 = the window's) and its height in pixels (GL's rows count from the bottom).
void CtSlGlSetBackbuffer(void* dev, unsigned fbo, int height);
// A texture/framebuffer pair the program owns, as a chain texture (the game's picture). Not freed by the chain. Update it with CtSlGlWrapSet when its size changes.
void* CtSlGlWrap(void* dev, unsigned tex, unsigned fbo, int w, int h);
void  CtSlGlWrapSet(void* wrapped, unsigned tex, unsigned fbo, int w, int h);
void  CtSlGlWrapFree(void* wrapped);
// Reads a chain texture back as top-row-first RGBA8 (tests, screenshots). malloc'd; NULL on failure.
uint8_t* CtSlGlReadback(void* dev, void* tex, int* w, int* h);
// The picture's bytes as a chain texture: RGBA8, top row first.
void* CtSlGlUpload(void* dev, const uint8_t* rgba, int w, int h);

#ifdef __cplusplus
}
#endif
#endif
