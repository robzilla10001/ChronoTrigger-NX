// slang_dk.cpp - see slang_dk.h.
#include "slang_dk.h"
#include "uam_lib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

namespace {
enum { MAX_IMG = 1024, MAX_SMP = 32, FRAME_SLOTS = 4, CAPTURE_SLOT = 3, RING_BYTES = 2 * 1024 * 1024, MAX_DEAD = 512, CCMD_BYTES = 1024 * 1024 };      // (the host's three frame slots, and a fourth partition of the ring for a capture)

struct Tex { DkMemBlock mem; DkImage img; int w, h, fmt, levels, desc, owned; };
struct Prog { DkMemBlock code; DkShader vs, fs; int hasUbo, hasPush; uint32_t uboBinding, pushBinding; uint32_t uboSize, pushSize; };
struct Dead { Tex* tex; Prog* prog; };

struct Dev {
  DkDevice dev; DkQueue q;
  DkMemBlock descMem; uint8_t imgUsed[MAX_IMG]; int nextImg;
  DkSamplerDescriptor* smp; int smpMade[MAX_SMP];
  DkMemBlock vbo;
  DkMemBlock ring; uint32_t ringOff;
  uint8_t pushTmp[16 * 1024];                                      // a uniform block padded to its whole (256-byte-multiple) size, to push in one piece
  DkMemBlock tcmdMem; DkCmdBuf tcmd;                              // our own command buffer for the work a texture's creation needs: recorded, submitted, waited for
  DkImage bb; int haveBb;                                          // the swap-chain image of this frame, which the LAST pass renders into (CtSlDkSetBackbuffer)
  DkCmdBuf cb; int slot;                                           // the one the passes record into: the host's this frame, or ccmd during a capture
  DkMemBlock ccmdMem; DkCmdBuf ccmd, hostCb; int hostSlot;         // the capture's private command buffer; the host's, kept while it is in use
  Dead dead[MAX_DEAD]; int nDead;
};

uint32_t up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }
uint64_t up64(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

struct FmtMap { DkImageFormat f; };
DkImageFormat dkFmt(int fmt) {
  switch (fmt) {
    case CT_SL_FMT_RGBA8: return DkImageFormat_RGBA8_Unorm;
    case CT_SL_FMT_RGBA8_SRGB: return DkImageFormat_RGBA8_Unorm_sRGB;
    case CT_SL_FMT_RGBA16F: return DkImageFormat_RGBA16_Float;
    case CT_SL_FMT_RGBA32F: return DkImageFormat_RGBA32_Float;
    case CT_SL_FMT_R8: return DkImageFormat_R8_Unorm;
    case CT_SL_FMT_RG8: return DkImageFormat_RG8_Unorm;
    case CT_SL_FMT_R16F: return DkImageFormat_R16_Float;
    case CT_SL_FMT_RG16F: return DkImageFormat_RG16_Float;
    case CT_SL_FMT_R32F: return DkImageFormat_R32_Float;
    case CT_SL_FMT_RG32F: return DkImageFormat_RG32_Float;
    case CT_SL_FMT_A2B10G10R10: return DkImageFormat_RGB10A2_Unorm;
    case CT_SL_FMT_RGBA8_UINT: return DkImageFormat_RGBA8_Uint;
    case CT_SL_FMT_RGBA8_SINT: return DkImageFormat_RGBA8_Sint;
    case CT_SL_FMT_R8_UINT: return DkImageFormat_R8_Uint;
    case CT_SL_FMT_RG8_UINT: return DkImageFormat_RG8_Uint;
    case CT_SL_FMT_R16_UINT: return DkImageFormat_R16_Uint;
    case CT_SL_FMT_RG16_UINT: return DkImageFormat_RG16_Uint;
    case CT_SL_FMT_RGBA16_UINT: return DkImageFormat_RGBA16_Uint;
    case CT_SL_FMT_R32_UINT: return DkImageFormat_R32_Uint;
    case CT_SL_FMT_RG32_UINT: return DkImageFormat_RG32_Uint;
    case CT_SL_FMT_RGBA32_UINT: return DkImageFormat_RGBA32_Uint;
    default: return DkImageFormat_RGBA8_Unorm;
  }
}
uint32_t levelsFor(int w, int h) { uint32_t n = 1; while (w > 1 || h > 1) { w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; n++; } return n; }

DkMemBlock block(DkDevice dev, uint64_t bytes, uint32_t flags) {
  DkMemBlockMaker mm; dkMemBlockMakerDefaults(&mm, dev, (uint32_t)up64(bytes, DK_MEMBLOCK_ALIGNMENT)); mm.flags = flags;
  return dkMemBlockCreate(&mm);
}
int allocImg(Dev* d) {
  for (int k = 0; k < MAX_IMG; k++) { int i = (d->nextImg + k) % MAX_IMG; if (!d->imgUsed[i]) { d->imgUsed[i] = 1; d->nextImg = (i + 1) % MAX_IMG; return i; } }
  return -1;
}
void setDesc(Dev* d, int idx, DkImage* img, int levels) {
  DkImageView v; dkImageViewDefaults(&v, img); v.mipLevelOffset = 0; v.mipLevelCount = (uint8_t)levels;
  DkImageDescriptor* dp = (DkImageDescriptor*)dkMemBlockGetCpuAddr(d->descMem) + idx;
  dkImageDescriptorInitialize(dp, &v, false, false);
}

// one-time work of our own: record into tcmd, submit, wait
DkCmdBuf tBegin(Dev* d) { dkCmdBufClear(d->tcmd); return d->tcmd; }
void tEnd(Dev* d) { dkQueueSubmitCommands(d->q, dkCmdBufFinishList(d->tcmd)); dkQueueWaitIdle(d->q); }

Tex* newTex(Dev* d, int w, int h, int fmt, int levels, uint32_t flags) {
  Tex* t = (Tex*)calloc(1, sizeof *t); if (!t) return NULL;
  t->w = w; t->h = h; t->fmt = fmt; t->levels = levels; t->owned = 1; t->desc = -1;
  DkImageLayoutMaker lm; dkImageLayoutMakerDefaults(&lm, d->dev);
  lm.flags = flags; lm.format = dkFmt(fmt); lm.dimensions[0] = (uint32_t)w; lm.dimensions[1] = (uint32_t)h; lm.mipLevels = (uint32_t)levels;
  DkImageLayout layout; dkImageLayoutInitialize(&layout, &lm);
  uint64_t sz = dkImageLayoutGetSize(&layout); uint32_t al = dkImageLayoutGetAlignment(&layout);
  t->mem = block(d->dev, up64(sz, al < DK_MEMBLOCK_ALIGNMENT ? DK_MEMBLOCK_ALIGNMENT : al), DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
  if (!t->mem) { free(t); return NULL; }
  dkImageInitialize(&t->img, &layout, t->mem, 0);
  t->desc = allocImg(d);
  if (t->desc < 0) { dkMemBlockDestroy(t->mem); free(t); return NULL; }
  setDesc(d, t->desc, &t->img, levels);
  return t;
}
void freeTex(Dev* d, Tex* t) {
  if (!t) return;
  if (t->owned) { if (t->desc >= 0) d->imgUsed[t->desc] = 0; if (t->mem) dkMemBlockDestroy(t->mem); }
  free(t);
}
void freeProg(Prog* p) { if (!p) return; if (p->code) dkMemBlockDestroy(p->code); free(p); }
void flushDead(Dev* d) {
  if (!d->nDead) return;
  dkQueueWaitIdle(d->q);                                           // something died: the GPU may still be reading it
  for (int i = 0; i < d->nDead; i++) { freeTex(d, d->dead[i].tex); freeProg(d->dead[i].prog); }
  d->nDead = 0;
}

void* texCreate(void* dv, int w, int h, CtSlFormat fmt, int mip) {
  Dev* d = (Dev*)dv;
  Tex* t = newTex(d, w, h, fmt, mip ? (int)levelsFor(w, h) : 1, DkImageFlags_UsageRender);
  if (!t) return NULL;
  DkCmdBuf c = tBegin(d);                                          // transparent black to begin with (the spec: frames before the first are 0)
  DkImageView v; dkImageViewDefaults(&v, &t->img); v.mipLevelCount = 1;
  dkCmdBufBindRenderTarget(c, &v, NULL);
  const uint32_t zero[4] = { 0, 0, 0, 0 };
  dkCmdBufClearColor(c, 0, DkColorMask_RGBA, zero);
  dkCmdBufBarrier(c, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);
  tEnd(d);
  return t;
}

void genMips(Dev* d, DkCmdBuf c, Tex* t) {
  int lw = t->w, lh = t->h;
  for (int l = 1; l < t->levels; l++) {
    int nw = lw > 1 ? lw / 2 : 1, nh = lh > 1 ? lh / 2 : 1;
    DkImageView sv, dv; dkImageViewDefaults(&sv, &t->img); dkImageViewDefaults(&dv, &t->img);
    sv.mipLevelOffset = (uint8_t)(l - 1); sv.mipLevelCount = 1; dv.mipLevelOffset = (uint8_t)l; dv.mipLevelCount = 1;
    DkImageRect sr = { 0, 0, 0, (uint32_t)lw, (uint32_t)lh, 1 }, dr = { 0, 0, 0, (uint32_t)nw, (uint32_t)nh, 1 };
    dkCmdBufBlitImage(c, &sv, &sr, &dv, &dr, DkBlitFlag_FilterLinear, 0);
    dkCmdBufBarrier(c, DkBarrier_Full, DkInvalidateFlags_Image);
    lw = nw; lh = nh;
  }
}

void* texFromRgba(void* dv, const uint8_t* rgba, int w, int h, int mip) {
  Dev* d = (Dev*)dv;
  Tex* t = newTex(d, w, h, CT_SL_FMT_RGBA8, mip ? (int)levelsFor(w, h) : 1, DkImageFlags_UsageRender);        // (render usage: the mip chain is built by blits into it)
  if (!t) return NULL;
  size_t n = (size_t)w * h * 4;
  DkMemBlock st = block(d->dev, n, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
  if (!st) { freeTex(d, t); return NULL; }
  memcpy(dkMemBlockGetCpuAddr(st), rgba, n);
  DkCmdBuf c = tBegin(d);
  DkImageView v; dkImageViewDefaults(&v, &t->img); v.mipLevelCount = 1;
  DkCopyBuf src = { dkMemBlockGetGpuAddr(st), 0, 0 }; DkImageRect rc = { 0, 0, 0, (uint32_t)w, (uint32_t)h, 1 };
  dkCmdBufCopyBufferToImage(c, &src, &v, &rc, 0);
  dkCmdBufBarrier(c, DkBarrier_Full, DkInvalidateFlags_Image);
  genMips(d, c, t);
  dkCmdBufBarrier(c, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);
  tEnd(d);
  dkMemBlockDestroy(st);
  return t;
}
void texDestroy(void* dv, void* tex) {                             // not now: the frame being recorded may still name it (CtSlDkFrameStart frees it)
  Dev* d = (Dev*)dv; Tex* t = (Tex*)tex; if (!t) return;
  if (!t->owned) { free(t); return; }
  if (d->nDead < MAX_DEAD) { d->dead[d->nDead].tex = t; d->dead[d->nDead].prog = NULL; d->nDead++; } else { dkQueueWaitIdle(d->q); freeTex(d, t); }
}
void texCopy(void* dv, void* dst, void* src, int w, int h) {
  Dev* d = (Dev*)dv; Tex* a = (Tex*)dst; Tex* b = (Tex*)src;
  DkImageView sv, dvw; dkImageViewDefaults(&sv, &b->img); dkImageViewDefaults(&dvw, &a->img); sv.mipLevelCount = dvw.mipLevelCount = 1;
  DkImageRect r = { 0, 0, 0, (uint32_t)w, (uint32_t)h, 1 };
  dkCmdBufBarrier(d->cb, DkBarrier_Full, DkInvalidateFlags_Image);
  dkCmdBufCopyImage(d->cb, &sv, &r, &dvw, &r, 0);
  dkCmdBufBarrier(d->cb, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);
}
void texGenMips(void* dv, void* tex) { Dev* d = (Dev*)dv; Tex* t = (Tex*)tex; if (t && t->levels > 1) genMips(d, d->cb, t); }

void* progCreate(void* dv, const CtSlProgram* sp, CtSlFormat fmt, char* err, size_t errN) {
  Dev* d = (Dev*)dv; (void)fmt;
  if (!sp->glsl[0] || !sp->glsl[1]) { snprintf(err, errN, "no GLSL for the pass"); return NULL; }
  void* vsd = NULL; void* fsd = NULL; size_t vsn = 0, fsn = 0; char lg[256];
  if (CtUamCompile(CT_UAM_VERTEX, sp->glsl[0], &vsd, &vsn, lg, sizeof lg)) { snprintf(err, errN, "%s", lg); return NULL; }
  if (CtUamCompile(CT_UAM_FRAGMENT, sp->glsl[1], &fsd, &fsn, lg, sizeof lg)) { free(vsd); snprintf(err, errN, "%s", lg); return NULL; }
  Prog* p = (Prog*)calloc(1, sizeof *p);
  uint32_t vo = 0, fo = up((uint32_t)vsn, DK_SHADER_CODE_ALIGNMENT);
  p->code = block(d->dev, fo + up((uint32_t)fsn, DK_SHADER_CODE_ALIGNMENT), DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code);
  if (!p->code) { free(vsd); free(fsd); free(p); snprintf(err, errN, "no memory for the shader code"); return NULL; }
  memcpy((uint8_t*)dkMemBlockGetCpuAddr(p->code) + vo, vsd, vsn); memcpy((uint8_t*)dkMemBlockGetCpuAddr(p->code) + fo, fsd, fsn);
  free(vsd); free(fsd);
  DkShaderMaker sm; dkShaderMakerDefaults(&sm, p->code, vo); dkShaderInitialize(&p->vs, &sm);
  dkShaderMakerDefaults(&sm, p->code, fo); dkShaderInitialize(&p->fs, &sm);
  p->hasUbo = sp->ubo.present; p->uboBinding = sp->ubo.binding; p->uboSize = up(sp->ubo.size ? sp->ubo.size : 16, DK_UNIFORM_BUF_ALIGNMENT);          // (the buffers are pushed and bound in whole 256-byte units)
  p->hasPush = sp->push.present && sp->push.size; p->pushBinding = sp->pushBinding; p->pushSize = up(sp->push.size ? sp->push.size : 16, DK_UNIFORM_BUF_ALIGNMENT);
  return p;
}
void progDestroy(void* dv, void* prog) {
  Dev* d = (Dev*)dv; Prog* p = (Prog*)prog; if (!p) return;
  if (d->nDead < MAX_DEAD) { d->dead[d->nDead].prog = p; d->dead[d->nDead].tex = NULL; d->nDead++; } else { dkQueueWaitIdle(d->q); freeProg(p); }
}

int passBegin(void* dv, void* target, const CtSlRect* vp) {
  Dev* d = (Dev*)dv; Tex* t = (Tex*)target; DkCmdBuf c = d->cb;
  if (t) {
    DkImageView v; dkImageViewDefaults(&v, &t->img); v.mipLevelCount = 1;
    dkCmdBufBindRenderTarget(c, &v, NULL);
  } else if (d->haveBb) {                                                       // the final pass: the SCREEN. The pass before it left its own texture bound; without this the picture was drawn into that texture
    DkImageView v; dkImageViewDefaults(&v, &d->bb);
    dkCmdBufBindRenderTarget(c, &v, NULL);
  }
  DkViewport vw = { (float)vp->x, (float)vp->y, (float)vp->w, (float)vp->h, 0.0f, 1.0f };                // (deko3d counts rows from the TOP)
  DkScissor sc = { (uint32_t)vp->x, (uint32_t)vp->y, (uint32_t)vp->w, (uint32_t)vp->h };
  dkCmdBufSetViewports(c, 0, &vw, 1); dkCmdBufSetScissors(c, 0, &sc, 1);
  DkDepthStencilState ds; dkDepthStencilStateDefaults(&ds); ds.depthTestEnable = false; ds.depthWriteEnable = false;
  DkRasterizerState rs; dkRasterizerStateDefaults(&rs); rs.cullMode = DkFace_None;
  DkColorWriteState cw; dkColorWriteStateDefaults(&cw);
  DkColorState cs; dkColorStateDefaults(&cs);
  dkCmdBufBindDepthStencilState(c, &ds); dkCmdBufBindRasterizerState(c, &rs); dkCmdBufBindColorWriteState(c, &cw); dkCmdBufBindColorState(c, &cs);
  return 0;
}
DkSamplerDescriptor* smpSlot(Dev* d, int filter, int wrap, int mip, int* idx) {
  int k = (filter ? 1 : 0) | (wrap & 3) << 1 | (mip ? 1 : 0) << 3;
  if (!d->smpMade[k]) {
    DkSampler sp; dkSamplerDefaults(&sp);
    sp.minFilter = sp.magFilter = filter ? DkFilter_Linear : DkFilter_Nearest;
    sp.mipFilter = mip ? (filter ? DkMipFilter_Linear : DkMipFilter_Nearest) : DkMipFilter_None;
    DkWrapMode wm = wrap == CT_SL_WRAP_EDGE ? DkWrapMode_ClampToEdge : wrap == CT_SL_WRAP_REPEAT ? DkWrapMode_Repeat : wrap == CT_SL_WRAP_MIRROR ? DkWrapMode_MirroredRepeat : DkWrapMode_ClampToBorder;
    sp.wrapMode[0] = sp.wrapMode[1] = sp.wrapMode[2] = wm;
    for (int i = 0; i < 4; i++) sp.borderColor[i].value_f = 0.0f;
    dkSamplerDescriptorInitialize(d->smp + k, &sp);
    d->smpMade[k] = 1;
  }
  *idx = k; return d->smp + k;
}
void passDraw(void* dv, void* prog, const CtSlBind* binds, int nBinds, const void* ubo, size_t uboN, const void* push, size_t pushN) {
  Dev* d = (Dev*)dv; Prog* p = (Prog*)prog; DkCmdBuf c = d->cb;
  DkShader const* sh[] = { &p->vs, &p->fs };
  dkCmdBufBindShaders(c, DkStageFlag_GraphicsMask, sh, 2);
  const uint32_t part = (RING_BYTES / FRAME_SLOTS) & ~(uint32_t)(DK_UNIFORM_BUF_ALIGNMENT - 1);
  DkGpuAddr ringBase = dkMemBlockGetGpuAddr(d->ring) + (uint64_t)d->slot * part;
  struct { int has; uint32_t binding, size; const void* data; size_t n; } blocks[2] = { { p->hasUbo, p->uboBinding, p->uboSize, ubo, uboN }, { p->hasPush, p->pushBinding, p->pushSize, push, pushN } };
  for (int b = 0; b < 2; b++) {
    if (!blocks[b].has) continue;
    uint32_t at = up(d->ringOff, DK_UNIFORM_BUF_ALIGNMENT);
    if (at + blocks[b].size > part) continue;                                   // (out of ring: 340 KB a frame is far more than 64 passes need)
    // the block's contents go through the command stream: the GPU writes the buffer, so there is no cache to invalidate
    // the WHOLE padded block, zero-filled past the shader's last member: pushing only the member bytes (a 52-byte push block) left its last word (FrameCount) unwritten -- the block probe read it back as 0
    { size_t whole = blocks[b].size; if (whole > sizeof d->pushTmp) whole = sizeof d->pushTmp;
      memset(d->pushTmp, 0, whole); memcpy(d->pushTmp, blocks[b].data, blocks[b].n < whole ? blocks[b].n : whole);
      dkCmdBufPushConstants(c, ringBase + at, blocks[b].size, 0, (uint32_t)whole, d->pushTmp); }
    dkCmdBufBindUniformBuffer(c, DkStage_Vertex, blocks[b].binding, ringBase + at, blocks[b].size);
    dkCmdBufBindUniformBuffer(c, DkStage_Fragment, blocks[b].binding, ringBase + at, blocks[b].size);
    d->ringOff = at + blocks[b].size;
  }
  for (int i = 0; i < nBinds; i++) {
    const Tex* t = (const Tex*)binds[i].tex; if (!t || t->desc < 0) continue;
    int si; smpSlot(d, binds[i].filter, binds[i].wrap, binds[i].mip && t->levels > 1, &si);
    dkCmdBufBindTexture(c, DkStage_Fragment, binds[i].binding, dkMakeTextureHandle((uint32_t)t->desc, (uint32_t)si));
  }
  dkCmdBufBindVtxBuffer(c, 0, dkMemBlockGetGpuAddr(d->vbo), 4 * 6 * sizeof(float));
  dkCmdBufDraw(c, DkPrimitive_TriangleStrip, 4, 1, 0, 0);
}
void passEnd(void* dv, void* target) {
  Dev* d = (Dev*)dv; (void)target;
  dkCmdBufBarrier(d->cb, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);            // the next pass samples what this one drew
}
}  // namespace

namespace { void frameBegin(void*); void frameEnd(void*); uint8_t* texReadback(void*, void*, int*, int*); }
extern "C" int CtSlDkCreate(DkDevice dev, DkQueue q, CtSlGpuOps* ops, void** out, char* err, size_t errN) {
  Dev* d = (Dev*)calloc(1, sizeof *d); if (!d) { snprintf(err, errN, "out of memory"); return 1; }
  d->dev = dev; d->q = q;
  d->descMem = block(dev, (uint64_t)MAX_IMG * sizeof(DkImageDescriptor) + (uint64_t)MAX_SMP * sizeof(DkSamplerDescriptor), DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
  d->vbo = block(dev, DK_MEMBLOCK_ALIGNMENT, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
  d->ring = block(dev, RING_BYTES, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
  d->tcmdMem = block(dev, 64 * 1024, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
  d->ccmdMem = block(dev, CCMD_BYTES, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
  DkCmdBufMaker cm; dkCmdBufMakerDefaults(&cm, dev); d->tcmd = dkCmdBufCreate(&cm); d->ccmd = dkCmdBufCreate(&cm);
  if (!d->descMem || !d->vbo || !d->ring || !d->tcmdMem || !d->tcmd || !d->ccmdMem || !d->ccmd) { snprintf(err, errN, "deko3d: no memory for the shader chain"); CtSlDkDestroy(d); return 1; }
  dkCmdBufAddMemory(d->tcmd, d->tcmdMem, 0, 64 * 1024); dkCmdBufAddMemory(d->ccmd, d->ccmdMem, 0, CCMD_BYTES);
  d->smp = (DkSamplerDescriptor*)((uint8_t*)dkMemBlockGetCpuAddr(d->descMem) + (size_t)MAX_IMG * sizeof(DkImageDescriptor));
  static const float quad[4][6] = { { 0, 0, 0, 1, 0, 0 }, { 1, 0, 0, 1, 1, 0 }, { 0, 1, 0, 1, 0, 1 }, { 1, 1, 0, 1, 1, 1 } };   // Position (x, y, 0, 1), TexCoord (x, y); (0, 0) is the top left
  memcpy(dkMemBlockGetCpuAddr(d->vbo), quad, sizeof quad);
  memset(ops, 0, sizeof *ops);
  ops->name = "deko3d"; ops->flipBackbuffer = 1; ops->flipOffscreen = 1; ops->wantsGlsl = 1; ops->glslVersion = 430; ops->glslEs = 0; ops->maxPushBytes = 0;
  ops->texCreate = texCreate; ops->texFromRgba = texFromRgba; ops->texDestroy = texDestroy; ops->texCopy = texCopy; ops->texGenMips = texGenMips;
  ops->progCreate = progCreate; ops->progDestroy = progDestroy; ops->passBegin = passBegin; ops->passDraw = passDraw; ops->passEnd = passEnd;
  ops->frameBegin = frameBegin; ops->frameEnd = frameEnd; ops->texReadback = texReadback; ops->setCompilerOpt = CtUamSetOptLevel;
  *out = d; return 0;
}
extern "C" void CtSlDkDestroy(void* dv) {
  Dev* d = (Dev*)dv; if (!d) return;
  CtUamShutdown();                                                 // (the compile thread must be gone before the program ends: the loader panics on a live thread)
  dkQueueWaitIdle(d->q);
  flushDead(d);
  if (d->ccmd) dkCmdBufDestroy(d->ccmd);
  if (d->ccmdMem) dkMemBlockDestroy(d->ccmdMem);
  if (d->tcmd) dkCmdBufDestroy(d->tcmd);
  if (d->tcmdMem) dkMemBlockDestroy(d->tcmdMem);
  if (d->ring) dkMemBlockDestroy(d->ring);
  if (d->vbo) dkMemBlockDestroy(d->vbo);
  if (d->descMem) dkMemBlockDestroy(d->descMem);
  free(d);
}
extern "C" void CtSlDkFrameStart(void* dv) { flushDead((Dev*)dv); }
extern "C" void CtSlDkSetBackbuffer(void* dv, DkImage* image) { Dev* d = (Dev*)dv; d->bb = *image; d->haveBb = 1; }
namespace {
void bindState(Dev* d, DkCmdBuf cb) {
  DkGpuAddr base = dkMemBlockGetGpuAddr(d->descMem);
  dkCmdBufBindImageDescriptorSet(cb, base, MAX_IMG);
  dkCmdBufBindSamplerDescriptorSet(cb, base + (uint64_t)MAX_IMG * sizeof(DkImageDescriptor), MAX_SMP);
  dkCmdBufBarrier(cb, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors | DkInvalidateFlags_Shader);
  DkVtxAttribState attribs[2] = { { 0, 0, 0, DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0 }, { 0, 0, 16, DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0 } };
  DkVtxBufferState vb = { 6 * (uint32_t)sizeof(float), 0 };
  dkCmdBufBindVtxAttribState(cb, attribs, 2); dkCmdBufBindVtxBufferState(cb, &vb, 1);
}
// A private frame for the capture: the passes record into ccmd, submitted and waited for at frameEnd; its own partition of the uniform ring.
void frameBegin(void* dv) {
  Dev* d = (Dev*)dv; d->hostCb = d->cb; d->hostSlot = d->slot; d->cb = d->ccmd; d->slot = CAPTURE_SLOT; d->ringOff = 0;
  dkCmdBufClear(d->ccmd);
  bindState(d, d->ccmd);
}
void frameEnd(void* dv) {
  Dev* d = (Dev*)dv;
  dkQueueSubmitCommands(d->q, dkCmdBufFinishList(d->ccmd));
  dkQueueWaitIdle(d->q);
  d->cb = d->hostCb; d->slot = d->hostSlot; d->ringOff = 0;
}
uint8_t* texReadback(void* dv, void* tex, int* w, int* h) {
  Dev* d = (Dev*)dv; Tex* t = (Tex*)tex; if (!t) return NULL;
  size_t n = (size_t)t->w * t->h * 4;
  // CPU-UNCACHED: the GPU writes this buffer behind the CPU's cache, and a CPU-cached block returned stale 64-byte lines (one cache line of pixels each) -- the capture's first versions did
  DkMemBlock st = block(d->dev, n, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
  if (!st) return NULL;
  DkCmdBuf c = tBegin(d);
  DkImageView v; dkImageViewDefaults(&v, &t->img); v.mipLevelCount = 1;
  DkCopyBuf dst = { dkMemBlockGetGpuAddr(st), 0, 0 }; DkImageRect rc = { 0, 0, 0, (uint32_t)t->w, (uint32_t)t->h, 1 };
  dkCmdBufBarrier(c, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_L2Cache);
  dkCmdBufCopyImageToBuffer(c, &v, &rc, &dst, 0);
  tEnd(d);
  uint8_t* out = (uint8_t*)malloc(n);
  if (out) { memcpy(out, dkMemBlockGetCpuAddr(st), n); *w = t->w; *h = t->h; }
  dkMemBlockDestroy(st);
  return out;
}
}  // namespace
extern "C" void CtSlDkBeginFrame(void* dv, DkCmdBuf cb, int slot) {
  Dev* d = (Dev*)dv; d->cb = cb; d->slot = slot % CAPTURE_SLOT; d->ringOff = 0;
  bindState(d, cb);
}
extern "C" void* CtSlDkWrap(void* dv, DkImage* image, int w, int h) {
  Dev* d = (Dev*)dv; Tex* t = (Tex*)calloc(1, sizeof *t); if (!t) return NULL;
  t->w = w; t->h = h; t->levels = 1; t->fmt = CT_SL_FMT_RGBA8; t->owned = 0; t->desc = allocImg(d); t->img = *image;      // (the image kept as a copy: the readback of the picture reads it)
  if (t->desc >= 0) setDesc(d, t->desc, image, 1);
  return t;
}
extern "C" void CtSlDkWrapSet(void* dv, void* wr, DkImage* image, int w, int h) { Dev* d = (Dev*)dv; Tex* t = (Tex*)wr; if (!t) return; t->w = w; t->h = h; t->img = *image; if (t->desc >= 0) setDesc(d, t->desc, image, 1); }
extern "C" void CtSlDkWrapFree(void* dv, void* wr) { Dev* d = (Dev*)dv; Tex* t = (Tex*)wr; if (!t) return; if (t->desc >= 0) d->imgUsed[t->desc] = 0; free(t); }
