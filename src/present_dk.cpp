// present_dk.cpp - the deko3d backend (see present_dk.h). One graphics queue; three frame slots, each with its own command memory,
// a dynamic buffer (uniforms, the overlay's vertices) and a staging buffer for the picture, guarded by a fence so a slot is never
// rewritten while the GPU still reads it. The swapchain has two images at 1920x1080 in every mode (the compositor scales it to the handheld panel, as it does SDL's EGL window).
//
// Coordinates: clip space +y is up; the scissor and the viewport count rows from the TOP of the image (deko3d's default window
// origin is the upper left). If the overlay's clipping ever shows mirrored, CT_DK_SCISSOR_FLIP below is the one place to look.
#include "present_dk.h"
#include <switch.h>
#include <deko3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdalign.h>
#ifdef CT_HAVE_IMGUI
#include "imgui.h"
#endif

#define CT_DK_SCISSOR_FLIP 0

#include "dk/dk_shaders.inc"

enum { FRAMES = 3, FB_NUM = 2, CMD_BYTES = 256 * 1024, DYN_BYTES = 4 * 1024 * 1024, DYN_UNI = 4096, MAX_IMG = 4, MAX_SMP = 4 };
enum { IMG_PIC = 0, IMG_FONT = 1, SMP_NEAREST = 0, SMP_LINEAR = 1 };

// deko3d's own report of what it refused, in the log (the run is usually about to end on it).
static void dkDebug(void* user, const char* context, DkResult result, const char* message) {
  (void)user; fprintf(stderr, "[deko3d] %s: result %d: %s\n", context ? context : "?", (int)result, message ? message : ""); fflush(stderr);
}
// The surface is 1920x1080 in every mode, like SDL's own EGL window on the console (the compositor scales it to the 1280x720 panel when handheld), so the menus and the
// picture lay out the same as on SDL_Renderer.
static void screenSize(int* w, int* h) { *w = 1920; *h = 1080; }
static uint32_t up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

struct Frame {
  DkMemBlock cmdMem, dynMem, stageMem; uint32_t stageBytes;
  DkCmdBuf cb; DkFence fence; int fenced;
};
struct CtDk {
  SDL_Window* win;
  DkDevice dev; DkQueue q;
  DkSwapchain sc; DkMemBlock fbMem; DkImage fb[FB_NUM]; int fbW, fbH; int vsync;
  DkMemBlock codeMem; DkShader vsQuad, fsQuad, vsGui, fsGui;
  DkMemBlock descMem;                                     // MAX_IMG image descriptors, then MAX_SMP sampler descriptors
  DkMemBlock picMem; DkImage pic; int picW, picH;
  DkMemBlock fontMem; DkImage font; int haveFont;
  Frame fr[FRAMES]; int cur;
  int recording, slot, uploaded;                          // a frame is open in fr[cur].cb on swapchain image `slot`
  char name[96];
};

static void waitFrame(CtDk* d, Frame* f) { if (f->fenced) { dkFenceWait(&f->fence, UINT64_MAX); f->fenced = 0; } }

static DkMemBlock makeBlock(DkDevice dev, uint32_t bytes, uint32_t flags) {
  DkMemBlockMaker mm; dkMemBlockMakerDefaults(&mm, dev, up(bytes, DK_MEMBLOCK_ALIGNMENT)); mm.flags = flags;
  return dkMemBlockCreate(&mm);
}
static uint32_t descOff(int sampler, int i) { return sampler ? MAX_IMG * sizeof(DkImageDescriptor) + (uint32_t)i * sizeof(DkSamplerDescriptor) : (uint32_t)i * sizeof(DkImageDescriptor); }

static void loadShader(CtDk* d, DkShader* s, uint32_t* off, const unsigned char* code, uint32_t n) {
  uint32_t o = *off; *off += up(n, DK_SHADER_CODE_ALIGNMENT);
  memcpy((uint8_t*)dkMemBlockGetCpuAddr(d->codeMem) + o, code, n);
  DkShaderMaker sm; dkShaderMakerDefaults(&sm, d->codeMem, o); dkShaderInitialize(s, &sm);
}

static void destroyFramebuffers(CtDk* d) {
  if (d->sc) { dkQueueWaitIdle(d->q); dkSwapchainDestroy(d->sc); d->sc = NULL; }
  if (d->fbMem) { dkMemBlockDestroy(d->fbMem); d->fbMem = NULL; }
}
static int makeFramebuffers(CtDk* d, int w, int h) {
  DkImageLayoutMaker lm; dkImageLayoutMakerDefaults(&lm, d->dev);
  lm.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression;
  lm.format = DkImageFormat_RGBA8_Unorm; lm.dimensions[0] = (uint32_t)w; lm.dimensions[1] = (uint32_t)h;
  DkImageLayout layout; dkImageLayoutInitialize(&layout, &lm);
  uint32_t one = up((uint32_t)dkImageLayoutGetSize(&layout), (uint32_t)dkImageLayoutGetAlignment(&layout));
  d->fbMem = makeBlock(d->dev, one * FB_NUM, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
  if (!d->fbMem) return 0;
  DkImage const* imgs[FB_NUM];
  for (int i = 0; i < FB_NUM; i++) { dkImageInitialize(&d->fb[i], &layout, d->fbMem, one * (uint32_t)i); imgs[i] = &d->fb[i]; }
  DkSwapchainMaker sm; dkSwapchainMakerDefaults(&sm, d->dev, nwindowGetDefault(), imgs, FB_NUM);
  d->sc = dkSwapchainCreate(&sm);
  if (!d->sc) return 0;
  dkSwapchainSetSwapInterval(d->sc, d->vsync ? 1 : 0);
  d->fbW = w; d->fbH = h;
  return 1;
}

static void setDescriptor(CtDk* d, int idx, DkImage* img) {
  DkImageView v; dkImageViewDefaults(&v, img);
  dkImageDescriptorInitialize((DkImageDescriptor*)((uint8_t*)dkMemBlockGetCpuAddr(d->descMem) + descOff(0, idx)), &v, false, false);
}

// The picture's texture and every frame's staging buffer for it. The GPU must be idle (a resize).
static int makePicture(CtDk* d, int w, int h) {
  if (d->picMem) { dkMemBlockDestroy(d->picMem); d->picMem = NULL; }
  DkImageLayoutMaker lm; dkImageLayoutMakerDefaults(&lm, d->dev);
  lm.format = DkImageFormat_RGBA8_Unorm; lm.dimensions[0] = (uint32_t)w; lm.dimensions[1] = (uint32_t)h;
  DkImageLayout layout; dkImageLayoutInitialize(&layout, &lm);
  d->picMem = makeBlock(d->dev, (uint32_t)dkImageLayoutGetSize(&layout), DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
  if (!d->picMem) return 0;
  dkImageInitialize(&d->pic, &layout, d->picMem, 0);
  setDescriptor(d, IMG_PIC, &d->pic);
  for (int i = 0; i < FRAMES; i++) {
    Frame* f = &d->fr[i];
    if (f->stageMem) { dkMemBlockDestroy(f->stageMem); f->stageMem = NULL; }
    f->stageBytes = up((uint32_t)w * (uint32_t)h * 4, DK_MEMBLOCK_ALIGNMENT);
    f->stageMem = makeBlock(d->dev, f->stageBytes, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    if (!f->stageMem) return 0;
    memset(dkMemBlockGetCpuAddr(f->stageMem), 0, f->stageBytes);
  }
  d->picW = w; d->picH = h; d->uploaded = 1;     // (the staging is black: copy it, so the first frame is not noise)
  return 1;
}

extern "C" int CtDkStart(SDL_Window* win, int vsync, int picW, int picH, CtDk** out) {
  CtDk* d = (CtDk*)calloc(1, sizeof *d);
  if (!d) return 0;
  d->win = win; d->vsync = vsync;
  // (No SDL window: the screen is ours, see switch_platform.h.)
  int w, h; screenSize(&w, &h);
  DkDeviceMaker dm; dkDeviceMakerDefaults(&dm); dm.cbDebug = dkDebug; d->dev = dkDeviceCreate(&dm);
  fprintf(stderr, "[deko3d] device %s\n", d->dev ? "created" : "FAILED"); fflush(stderr);
  if (!d->dev) { free(d); return 0; }
  DkQueueMaker qm; dkQueueMakerDefaults(&qm, d->dev); qm.flags = DkQueueFlags_Graphics; d->q = dkQueueCreate(&qm);
  fprintf(stderr, "[deko3d] queue %s; creating the %dx%d swapchain\n", d->q ? "created" : "FAILED", w, h); fflush(stderr);
  if (!d->q || !makeFramebuffers(d, w, h)) { CtDkStop(d); return 0; }
  fprintf(stderr, "[deko3d] swapchain up\n"); fflush(stderr);
  d->codeMem = makeBlock(d->dev, 64 * 1024, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code);
  d->descMem = makeBlock(d->dev, DK_MEMBLOCK_ALIGNMENT, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
  if (!d->codeMem || !d->descMem) { CtDkStop(d); return 0; }
  uint32_t off = 0;
  loadShader(d, &d->vsQuad, &off, dkshader_quad_vsh, sizeof dkshader_quad_vsh);
  loadShader(d, &d->fsQuad, &off, dkshader_quad_fsh, sizeof dkshader_quad_fsh);
  loadShader(d, &d->vsGui, &off, dkshader_gui_vsh, sizeof dkshader_gui_vsh);
  loadShader(d, &d->fsGui, &off, dkshader_gui_fsh, sizeof dkshader_gui_fsh);
  for (int s = 0; s < 2; s++) {                                    // the two samplers, the same for ever
    DkSampler sp; dkSamplerDefaults(&sp);
    sp.minFilter = sp.magFilter = s == SMP_LINEAR ? DkFilter_Linear : DkFilter_Nearest;
    sp.wrapMode[0] = sp.wrapMode[1] = sp.wrapMode[2] = DkWrapMode_ClampToEdge;
    dkSamplerDescriptorInitialize((DkSamplerDescriptor*)((uint8_t*)dkMemBlockGetCpuAddr(d->descMem) + descOff(1, s)), &sp);
  }
  for (int i = 0; i < FRAMES; i++) {
    Frame* f = &d->fr[i];
    f->cmdMem = makeBlock(d->dev, CMD_BYTES, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    f->dynMem = makeBlock(d->dev, DYN_BYTES, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    DkCmdBufMaker cm; dkCmdBufMakerDefaults(&cm, d->dev); f->cb = dkCmdBufCreate(&cm);
    if (!f->cmdMem || !f->dynMem || !f->cb) { CtDkStop(d); return 0; }
    dkCmdBufAddMemory(f->cb, f->cmdMem, 0, CMD_BYTES);
  }
  if (!makePicture(d, picW, picH)) { CtDkStop(d); return 0; }
  snprintf(d->name, sizeof d->name, "deko3d (%dx%d, Tegra X1)", w, h);
  *out = d;
  return 1;
}

extern "C" void CtDkStop(CtDk* d) {
  if (!d) return;
  if (d->q) dkQueueWaitIdle(d->q);
  for (int i = 0; i < FRAMES; i++) {
    Frame* f = &d->fr[i];
    if (f->cb) dkCmdBufDestroy(f->cb);
    if (f->cmdMem) dkMemBlockDestroy(f->cmdMem);
    if (f->dynMem) dkMemBlockDestroy(f->dynMem);
    if (f->stageMem) dkMemBlockDestroy(f->stageMem);
  }
  if (d->picMem) dkMemBlockDestroy(d->picMem);
  if (d->fontMem) dkMemBlockDestroy(d->fontMem);
  if (d->descMem) dkMemBlockDestroy(d->descMem);
  if (d->codeMem) dkMemBlockDestroy(d->codeMem);
  destroyFramebuffers(d);
  if (d->q) dkQueueDestroy(d->q);
  if (d->dev) dkDeviceDestroy(d->dev);
  free(d);
}

extern "C" const char* CtDkName(const CtDk* d) { return d->name; }
extern "C" void CtDkOutputSize(CtDk* d, int* w, int* h) { *w = d->fbW; *h = d->fbH; }
extern "C" void CtDkSetVsync(CtDk* d, int on) { d->vsync = on; if (d->sc) dkSwapchainSetSwapInterval(d->sc, on ? 1 : 0); }

extern "C" int CtDkSetPicSize(CtDk* d, int w, int h) {
  if (w == d->picW && h == d->picH) return 1;
  if (d->recording) return 0;
  dkQueueWaitIdle(d->q);
  for (int i = 0; i < FRAMES; i++) d->fr[i].fenced = 0;
  return makePicture(d, w, h);
}

extern "C" void CtDkUpload(CtDk* d, const uint8_t* rgb) {
  Frame* f = &d->fr[d->cur];
  waitFrame(d, f);
  uint8_t* o = (uint8_t*)dkMemBlockGetCpuAddr(f->stageMem);
  size_t n = (size_t)d->picW * (size_t)d->picH;
  for (size_t i = 0; i < n; i++) { o[4 * i] = rgb[3 * i]; o[4 * i + 1] = rgb[3 * i + 1]; o[4 * i + 2] = rgb[3 * i + 2]; o[4 * i + 3] = 255; }
  d->uploaded = 1;
}

static void bindBase(CtDk* d, Frame* f) {
  DkDepthStencilState ds; dkDepthStencilStateDefaults(&ds); ds.depthTestEnable = false; ds.depthWriteEnable = false;
  DkRasterizerState rs; dkRasterizerStateDefaults(&rs); rs.cullMode = DkFace_None;
  DkColorWriteState cw; dkColorWriteStateDefaults(&cw);
  dkCmdBufBindDepthStencilState(f->cb, &ds);
  dkCmdBufBindRasterizerState(f->cb, &rs);
  dkCmdBufBindColorWriteState(f->cb, &cw);
  dkCmdBufBindImageDescriptorSet(f->cb, dkMemBlockGetGpuAddr(d->descMem) + descOff(0, 0), MAX_IMG);
  dkCmdBufBindSamplerDescriptorSet(f->cb, dkMemBlockGetGpuAddr(d->descMem) + descOff(1, 0), MAX_SMP);
}

extern "C" void CtDkDraw(CtDk* d, const SDL_Rect* dst, int linear) {
  if (d->recording) return;
  { int w, h; screenSize(&w, &h);                                       // (a swapchain of another size: rebuilt)
    if (w != d->fbW || h != d->fbH) {
      destroyFramebuffers(d);
      if (!makeFramebuffers(d, w, h)) { fprintf(stderr, "[deko3d] no swapchain at %dx%d\n", w, h); fflush(stderr); makeFramebuffers(d, d->fbW ? d->fbW : 1280, d->fbH ? d->fbH : 720); }
    } }
  Frame* f = &d->fr[d->cur];
  waitFrame(d, f);
  dkCmdBufClear(f->cb);
  d->slot = dkQueueAcquireImage(d->q, d->sc);
  d->recording = 1;
  DkImageView target; dkImageViewDefaults(&target, &d->fb[d->slot]);
  dkCmdBufBindRenderTarget(f->cb, &target, NULL);
  dkCmdBufBarrier(f->cb, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors | DkInvalidateFlags_Shader);
  if (d->uploaded) {                                                  // the staging buffer into the picture's texture
    DkImageView pv; dkImageViewDefaults(&pv, &d->pic);
    DkCopyBuf src = { dkMemBlockGetGpuAddr(f->stageMem), 0, 0 };
    DkImageRect rc = { 0, 0, 0, (uint32_t)d->picW, (uint32_t)d->picH, 1 };
    dkCmdBufCopyBufferToImage(f->cb, &src, &pv, &rc, 0);
    dkCmdBufBarrier(f->cb, DkBarrier_Full, DkInvalidateFlags_Image);
    d->uploaded = 0;
  }
  bindBase(d, f);
  DkViewport vp = { 0.0f, 0.0f, (float)d->fbW, (float)d->fbH, 0.0f, 1.0f };
  DkScissor sc = { 0, 0, (uint32_t)d->fbW, (uint32_t)d->fbH };
  dkCmdBufSetViewports(f->cb, 0, &vp, 1);
  dkCmdBufSetScissors(f->cb, 0, &sc, 1);
  dkCmdBufClearColorFloat(f->cb, 0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);
  // the quad: the destination in clip space (+y up)
  float x0 = 0, y0 = 0, x1 = (float)d->fbW, y1 = (float)d->fbH;
  if (dst) { x0 = (float)dst->x; y0 = (float)dst->y; x1 = (float)(dst->x + dst->w); y1 = (float)(dst->y + dst->h); }
  float rect[4] = { 2.0f * x0 / d->fbW - 1.0f, 1.0f - 2.0f * y0 / d->fbH, 2.0f * x1 / d->fbW - 1.0f, 1.0f - 2.0f * y1 / d->fbH };
  DkGpuAddr uni = dkMemBlockGetGpuAddr(f->dynMem);
  dkCmdBufPushConstants(f->cb, uni, DK_UNIFORM_BUF_ALIGNMENT, 0, sizeof rect, rect);
  dkCmdBufBindUniformBuffer(f->cb, DkStage_Vertex, 0, uni, DK_UNIFORM_BUF_ALIGNMENT);
  DkShader const* shaders[] = { &d->vsQuad, &d->fsQuad };
  dkCmdBufBindShaders(f->cb, DkStageFlag_GraphicsMask, shaders, 2);
  dkCmdBufBindTexture(f->cb, DkStage_Fragment, 0, dkMakeTextureHandle(IMG_PIC, linear ? SMP_LINEAR : SMP_NEAREST));
  DkColorState cs; dkColorStateDefaults(&cs);
  dkCmdBufBindColorState(f->cb, &cs);
  dkCmdBufDraw(f->cb, DkPrimitive_TriangleStrip, 4, 1, 0, 0);
}

extern "C" void CtDkSwap(CtDk* d) {
  if (!d->recording) return;
  Frame* f = &d->fr[d->cur];
  DkCmdList list = dkCmdBufFinishList(f->cb);
  dkQueueSubmitCommands(d->q, list);
  dkQueueSignalFence(d->q, &f->fence, true); f->fenced = 1;
  dkQueuePresentImage(d->q, d->sc, d->slot);
  d->recording = 0;
  d->cur = (d->cur + 1) % FRAMES;
}

// ---- Dear ImGui ----------------------------------------------------------------------------------------------------------------
#ifdef CT_HAVE_IMGUI
extern "C" int CtDkImGuiInit(CtDk* d) {
  ImGuiIO& io = ImGui::GetIO();
  unsigned char* px; int w, h; io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
  DkImageLayoutMaker lm; dkImageLayoutMakerDefaults(&lm, d->dev);
  lm.format = DkImageFormat_RGBA8_Unorm; lm.dimensions[0] = (uint32_t)w; lm.dimensions[1] = (uint32_t)h;
  DkImageLayout layout; dkImageLayoutInitialize(&layout, &lm);
  d->fontMem = makeBlock(d->dev, (uint32_t)dkImageLayoutGetSize(&layout), DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
  uint32_t sb = up((uint32_t)w * (uint32_t)h * 4, DK_MEMBLOCK_ALIGNMENT);
  DkMemBlock stage = makeBlock(d->dev, sb, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
  DkMemBlock cmem = makeBlock(d->dev, DK_MEMBLOCK_ALIGNMENT, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
  DkCmdBufMaker cm; dkCmdBufMakerDefaults(&cm, d->dev); DkCmdBuf cb = dkCmdBufCreate(&cm);
  if (!d->fontMem || !stage || !cmem || !cb) return 0;
  dkImageInitialize(&d->font, &layout, d->fontMem, 0);
  memcpy(dkMemBlockGetCpuAddr(stage), px, (size_t)w * (size_t)h * 4);
  dkCmdBufAddMemory(cb, cmem, 0, DK_MEMBLOCK_ALIGNMENT);
  DkImageView fv; dkImageViewDefaults(&fv, &d->font);
  DkCopyBuf src = { dkMemBlockGetGpuAddr(stage), 0, 0 };
  DkImageRect rc = { 0, 0, 0, (uint32_t)w, (uint32_t)h, 1 };
  dkCmdBufCopyBufferToImage(cb, &src, &fv, &rc, 0);
  dkQueueSubmitCommands(d->q, dkCmdBufFinishList(cb));
  dkQueueWaitIdle(d->q);
  dkCmdBufDestroy(cb); dkMemBlockDestroy(cmem); dkMemBlockDestroy(stage);
  setDescriptor(d, IMG_FONT, &d->font);
  io.Fonts->SetTexID((ImTextureID)(uintptr_t)dkMakeTextureHandle(IMG_FONT, SMP_LINEAR));
  io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
  d->haveFont = 1;
  return 1;
}
extern "C" void CtDkImGuiShutdown(CtDk* d) {
  if (!d->haveFont) return;
  dkQueueWaitIdle(d->q);
  ImGui::GetIO().Fonts->SetTexID(0);
  d->haveFont = 0;
}
extern "C" void CtDkImGuiRender(CtDk* d, void* drawDataPtr) {
  ImDrawData* dd = (ImDrawData*)drawDataPtr;
  if (!d->recording || !dd || dd->CmdListsCount == 0 || dd->TotalVtxCount == 0) return;
  Frame* f = &d->fr[d->cur];
  uint8_t* dyn = (uint8_t*)dkMemBlockGetCpuAddr(f->dynMem);
  DkGpuAddr base = dkMemBlockGetGpuAddr(f->dynMem);
  const uint32_t vtxOff = DYN_UNI, vtxBytes = (uint32_t)dd->TotalVtxCount * sizeof(ImDrawVert);
  const uint32_t idxOff = up(vtxOff + vtxBytes, 16), idxBytes = (uint32_t)dd->TotalIdxCount * sizeof(ImDrawIdx);
  if (idxOff + idxBytes > DYN_BYTES) { fprintf(stderr, "[video] deko3d: the overlay's geometry does not fit (%u bytes)\n", idxOff + idxBytes); return; }
  { uint8_t* vp = dyn + vtxOff; uint8_t* ip = dyn + idxOff;
    for (int n = 0; n < dd->CmdListsCount; n++) {
      const ImDrawList* cl = dd->CmdLists[n];
      memcpy(vp, cl->VtxBuffer.Data, (size_t)cl->VtxBuffer.Size * sizeof(ImDrawVert)); vp += (size_t)cl->VtxBuffer.Size * sizeof(ImDrawVert);
      memcpy(ip, cl->IdxBuffer.Data, (size_t)cl->IdxBuffer.Size * sizeof(ImDrawIdx)); ip += (size_t)cl->IdxBuffer.Size * sizeof(ImDrawIdx);
    } }
  const float L = dd->DisplayPos.x, T = dd->DisplayPos.y, W = dd->DisplaySize.x, H = dd->DisplaySize.y;
  if (W <= 0 || H <= 0) return;
  const float sx = (float)d->fbW / W, sy = (float)d->fbH / H;          // (display units -> swapchain pixels)
  float st[4] = { 2.0f / W, -2.0f / H, -1.0f - L * 2.0f / W, 1.0f + T * 2.0f / H };
  dkCmdBufPushConstants(f->cb, base, DK_UNIFORM_BUF_ALIGNMENT, 0, sizeof st, st);
  dkCmdBufBindUniformBuffer(f->cb, DkStage_Vertex, 0, base, DK_UNIFORM_BUF_ALIGNMENT);
  DkShader const* shaders[] = { &d->vsGui, &d->fsGui };
  dkCmdBufBindShaders(f->cb, DkStageFlag_GraphicsMask, shaders, 2);
  DkVtxAttribState attribs[3] = {
    { 0, 0, (uint32_t)offsetof(ImDrawVert, pos), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0 },
    { 0, 0, (uint32_t)offsetof(ImDrawVert, uv),  DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0 },
    { 0, 0, (uint32_t)offsetof(ImDrawVert, col), DkVtxAttribSize_4x8,  DkVtxAttribType_Unorm, 0 } };
  DkVtxBufferState vb = { (uint32_t)sizeof(ImDrawVert), 0 };
  dkCmdBufBindVtxAttribState(f->cb, attribs, 3);
  dkCmdBufBindVtxBufferState(f->cb, &vb, 1);
  dkCmdBufBindVtxBuffer(f->cb, 0, base + vtxOff, vtxBytes);
  dkCmdBufBindIdxBuffer(f->cb, sizeof(ImDrawIdx) == 2 ? DkIdxFormat_Uint16 : DkIdxFormat_Uint32, base + idxOff);
  DkColorState cs; dkColorStateDefaults(&cs); dkColorStateSetBlendEnable(&cs, 0, true);
  DkBlendState bs; dkBlendStateDefaults(&bs);
  bs.srcAlphaBlendFactor = DkBlendFactor_One; bs.dstAlphaBlendFactor = DkBlendFactor_InvSrcAlpha;
  dkCmdBufBindColorState(f->cb, &cs);
  dkCmdBufBindBlendStates(f->cb, 0, &bs, 1);
  uint32_t gv = 0, gi = 0;
  for (int n = 0; n < dd->CmdListsCount; n++) {
    const ImDrawList* cl = dd->CmdLists[n];
    for (int c = 0; c < cl->CmdBuffer.Size; c++) {
      const ImDrawCmd* pc = &cl->CmdBuffer[c];
      if (pc->UserCallback) continue;
      float x0 = (pc->ClipRect.x - L) * sx, y0 = (pc->ClipRect.y - T) * sy, x1 = (pc->ClipRect.z - L) * sx, y1 = (pc->ClipRect.w - T) * sy;
      if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > d->fbW) x1 = (float)d->fbW; if (y1 > d->fbH) y1 = (float)d->fbH;
      if (x1 <= x0 || y1 <= y0) continue;
#if CT_DK_SCISSOR_FLIP
      DkScissor sc = { (uint32_t)x0, (uint32_t)((float)d->fbH - y1), (uint32_t)(x1 - x0), (uint32_t)(y1 - y0) };
#else
      DkScissor sc = { (uint32_t)x0, (uint32_t)y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0) };
#endif
      dkCmdBufSetScissors(f->cb, 0, &sc, 1);
      dkCmdBufBindTexture(f->cb, DkStage_Fragment, 0, (DkResHandle)(uintptr_t)pc->GetTexID());
      dkCmdBufDrawIndexed(f->cb, DkPrimitive_Triangles, pc->ElemCount, 1, gi + pc->IdxOffset, (int32_t)(gv + pc->VtxOffset), 0);
    }
    gv += (uint32_t)cl->VtxBuffer.Size; gi += (uint32_t)cl->IdxBuffer.Size;
  }
  DkScissor all = { 0, 0, (uint32_t)d->fbW, (uint32_t)d->fbH };
  dkCmdBufSetScissors(f->cb, 0, &all, 1);
}
#else
extern "C" int CtDkImGuiInit(CtDk*) { return 0; }
extern "C" void CtDkImGuiShutdown(CtDk*) {}
extern "C" void CtDkImGuiRender(CtDk*, void*) {}
#endif
