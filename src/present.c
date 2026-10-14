// present.c - see present.h. The SDL_Renderer backend and the OpenGL 3.3 backend; the OpenGL functions are loaded
// through SDL_GL_GetProcAddress (no GL headers, no loader library, nothing linked).
#include "present.h"
#include "present_vk.h"
#include "hoststats.h"
#include "hostpool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- the OpenGL subset this needs ----------------------------------------------------------------------------------
#ifdef _WIN32
#define CTGL_API __stdcall
#else
#define CTGL_API
#endif
typedef unsigned int GLenum_; typedef unsigned int GLuint_; typedef int GLint_; typedef int GLsizei_;
typedef unsigned int GLbitfield_; typedef float GLfloat_;
#define CTGL_TEXTURE_2D 0x0DE1
#define CTGL_RGB 0x1907
#define CTGL_RGB8 0x8051
#define CTGL_UNSIGNED_BYTE 0x1401
#define CTGL_TEXTURE_MAG_FILTER 0x2800
#define CTGL_TEXTURE_MIN_FILTER 0x2801
#define CTGL_TEXTURE_WRAP_S 0x2802
#define CTGL_TEXTURE_WRAP_T 0x2803
#define CTGL_NEAREST 0x2600
#define CTGL_LINEAR 0x2601
#define CTGL_CLAMP_TO_EDGE 0x812F
#define CTGL_UNPACK_ALIGNMENT 0x0CF5
#define CTGL_PACK_ALIGNMENT 0x0D05
#define CTGL_READ_FRAMEBUFFER 0x8CA8
#define CTGL_DRAW_FRAMEBUFFER 0x8CA9
#define CTGL_COLOR_ATTACHMENT0 0x8CE0
#define CTGL_FRAMEBUFFER_COMPLETE 0x8CD5
#define CTGL_COLOR_BUFFER_BIT 0x4000
#define CTGL_SCISSOR_TEST 0x0C11
#define CTGL_BGRA 0x80E1
#define CTGL_BACK 0x0405
#define CTGL_VERSION 0x1F02
#define CTGL_RENDERER 0x1F01
static struct {
  void (CTGL_API* GenTextures)(GLsizei_, GLuint_*);
  void (CTGL_API* DeleteTextures)(GLsizei_, const GLuint_*);
  void (CTGL_API* BindTexture)(GLenum_, GLuint_);
  void (CTGL_API* TexParameteri)(GLenum_, GLenum_, GLint_);
  void (CTGL_API* TexImage2D)(GLenum_, GLint_, GLint_, GLsizei_, GLsizei_, GLint_, GLenum_, GLenum_, const void*);
  void (CTGL_API* TexSubImage2D)(GLenum_, GLint_, GLint_, GLint_, GLsizei_, GLsizei_, GLenum_, GLenum_, const void*);
  void (CTGL_API* PixelStorei)(GLenum_, GLint_);
  void (CTGL_API* GenFramebuffers)(GLsizei_, GLuint_*);
  void (CTGL_API* DeleteFramebuffers)(GLsizei_, const GLuint_*);
  void (CTGL_API* BindFramebuffer)(GLenum_, GLuint_);
  void (CTGL_API* FramebufferTexture2D)(GLenum_, GLenum_, GLenum_, GLuint_, GLint_);
  GLenum_ (CTGL_API* CheckFramebufferStatus)(GLenum_);
  void (CTGL_API* BlitFramebuffer)(GLint_, GLint_, GLint_, GLint_, GLint_, GLint_, GLint_, GLint_, GLbitfield_, GLenum_);
  void (CTGL_API* ClearColor)(GLfloat_, GLfloat_, GLfloat_, GLfloat_);
  void (CTGL_API* Clear)(GLbitfield_);
  void (CTGL_API* Viewport)(GLint_, GLint_, GLsizei_, GLsizei_);
  void (CTGL_API* Disable)(GLenum_);
  void (CTGL_API* ReadBuffer)(GLenum_);
  void (CTGL_API* ReadPixels)(GLint_, GLint_, GLsizei_, GLsizei_, GLenum_, GLenum_, void*);
  const unsigned char* (CTGL_API* GetString)(GLenum_);
} gl;
static int glLoad(void) {
#define L(name) if (!(*(void**)&gl.name = SDL_GL_GetProcAddress("gl" #name))) { fprintf(stderr, "[video] OpenGL: no gl%s\n", #name); return 0; }
  L(GenTextures) L(DeleteTextures) L(BindTexture) L(TexParameteri) L(TexImage2D) L(TexSubImage2D) L(PixelStorei)
  L(GenFramebuffers) L(DeleteFramebuffers) L(BindFramebuffer) L(FramebufferTexture2D) L(CheckFramebufferStatus)
  L(BlitFramebuffer) L(ClearColor) L(Clear) L(Viewport) L(Disable) L(ReadBuffer) L(ReadPixels) L(GetString)
#undef L
  return 1;
}

static void presentStop(struct CtPresenter* p);
typedef struct { int w, h, hasDst, linear, nHud; SDL_Rect dst; char hud[9][96]; } PJob;   // one frame's draw, handed to the render thread
struct CtPresenter {
  int backend;
  SDL_Window* win;
  int picW, picH;
  char name[160];
  // SDL_Renderer
  SDL_Renderer* ren;
  SDL_Texture* tex;
  int logical;               // headless: the logical size does the placing
  int sdlLinear;             // the texture's current scale mode
  // Vulkan
  CtVk* vk;
  // OpenGL
  SDL_GLContext ctx;
  GLuint_ glTex, glFbo;
  int glLinear;
  // THE RENDER THREAD (SDL_Renderer): see present.h
  int async;                                   // 1: Upload converts into a staging buffer, Swap hands it to the thread
  SDL_Thread* rt; SDL_mutex* mu; SDL_cond* cv;  // THE MAILBOX: three staging buffers -- the one Upload writes, at most one waiting (the NEWEST frame), at most one being shown
  int pending, inflight;                       //   the buffer waiting / being shown (-1 = none); both guarded by mu
  SDL_atomic_t rtShown;                        //   frames the render thread has presented (the stats' "shown")
  volatile int rtQuit;
  uint32_t* stg[3]; size_t stgN[3]; int stgW[3], stgH[3]; int wbuf;   // the staging buffers; the one Upload writes next
  SDL_Rect pdst; int phasDst, plinear;         // the draw, recorded for the frame
  PJob jobs[3];                                // (per staging buffer)
  SDL_Texture* tex2; int linear2, flip;        // the second texture: uploads alternate, so one is never written while the GPU still reads the other
  volatile double rtWork, rtWait, rtUp; volatile int rtNew;     // the thread's own timings, read by Swap (stats are the main thread's)
};

static int sdlStart(CtPresenter* p, const CtPresentConfig* c) {
  if (c->sdlDriver && *c->sdlDriver) SDL_SetHint(SDL_HINT_RENDER_DRIVER, c->sdlDriver);
  Uint32 rf = c->software ? SDL_RENDERER_SOFTWARE : (SDL_RENDERER_ACCELERATED | (c->vsync ? SDL_RENDERER_PRESENTVSYNC : 0));
  p->ren = SDL_CreateRenderer(p->win, -1, rf);
  if (!p->ren) p->ren = SDL_CreateRenderer(p->win, -1, SDL_RENDERER_SOFTWARE);
  if (!p->ren) return 0;
  p->tex = SDL_CreateTexture(p->ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, p->picW, p->picH);
  if (!p->tex) return 0;
  p->logical = c->logical;
  if (p->logical) SDL_RenderSetLogicalSize(p->ren, p->picW, p->picH);
  SDL_SetTextureScaleMode(p->tex, SDL_ScaleModeNearest);
  SDL_RendererInfo ri; memset(&ri, 0, sizeof ri); SDL_GetRendererInfo(p->ren, &ri);
  snprintf(p->name, sizeof p->name, "SDL_Renderer (%s)", ri.name ? ri.name : "?");
  p->backend = CT_BACKEND_SDL;
  return 1;
}

static int glStart(CtPresenter* p, const CtPresentConfig* c) {
  p->ctx = SDL_GL_CreateContext(p->win);
  if (!p->ctx) { fprintf(stderr, "[video] OpenGL: no context (%s)\n", SDL_GetError()); return 0; }
  SDL_GL_MakeCurrent(p->win, p->ctx);
  if (!glLoad()) return 0;
  SDL_GL_SetSwapInterval(c->vsync ? 1 : 0);
  gl.GenTextures(1, &p->glTex);
  gl.BindTexture(CTGL_TEXTURE_2D, p->glTex);
  gl.TexParameteri(CTGL_TEXTURE_2D, CTGL_TEXTURE_WRAP_S, CTGL_CLAMP_TO_EDGE);
  gl.TexParameteri(CTGL_TEXTURE_2D, CTGL_TEXTURE_WRAP_T, CTGL_CLAMP_TO_EDGE);
  gl.TexParameteri(CTGL_TEXTURE_2D, CTGL_TEXTURE_MIN_FILTER, CTGL_NEAREST);
  gl.TexParameteri(CTGL_TEXTURE_2D, CTGL_TEXTURE_MAG_FILTER, CTGL_NEAREST);
  gl.PixelStorei(CTGL_UNPACK_ALIGNMENT, 1);
  gl.TexImage2D(CTGL_TEXTURE_2D, 0, CTGL_RGB8, p->picW, p->picH, 0, CTGL_RGB, CTGL_UNSIGNED_BYTE, NULL);
  gl.GenFramebuffers(1, &p->glFbo);                     // the frame as a READ framebuffer: one blit places it
  gl.BindFramebuffer(CTGL_READ_FRAMEBUFFER, p->glFbo);
  gl.FramebufferTexture2D(CTGL_READ_FRAMEBUFFER, CTGL_COLOR_ATTACHMENT0, CTGL_TEXTURE_2D, p->glTex, 0);
  if (gl.CheckFramebufferStatus(CTGL_READ_FRAMEBUFFER) != CTGL_FRAMEBUFFER_COMPLETE) {
    fprintf(stderr, "[video] OpenGL: the frame's framebuffer is incomplete\n"); return 0; }
  gl.BindFramebuffer(CTGL_READ_FRAMEBUFFER, 0);
  const char* ver = (const char*)gl.GetString(CTGL_VERSION), *rn = (const char*)gl.GetString(CTGL_RENDERER);
  snprintf(p->name, sizeof p->name, "OpenGL %s (%s)", ver ? ver : "?", rn ? rn : "?");
  p->backend = CT_BACKEND_OPENGL;
  return 1;
}
static void glStop(CtPresenter* p) {
  if (p->ctx) {
    if (p->glFbo) gl.DeleteFramebuffers(1, &p->glFbo);
    if (p->glTex) gl.DeleteTextures(1, &p->glTex);
    SDL_GL_DeleteContext(p->ctx);
  }
  p->ctx = NULL; p->glFbo = p->glTex = 0;
}

static SDL_Window* makeWindow(const CtPresentConfig* c, Uint32 api) {
  Uint32 f = SDL_WINDOW_RESIZABLE | api;
  if (c->fullscreen) f |= SDL_WINDOW_FULLSCREEN_DESKTOP;
  if (c->hidden) f |= SDL_WINDOW_HIDDEN;
  return SDL_CreateWindow(c->title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, c->w, c->h, f);
}

CtPresenter* CtPresentCreate(const CtPresentConfig* c) {
  CtPresenter* p = (CtPresenter*)calloc(1, sizeof *p);
  if (!p) return NULL;
  p->picW = c->picW; p->picH = c->picH;
  if (c->backend == CT_BACKEND_OPENGL && !c->software) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    p->win = makeWindow(c, SDL_WINDOW_OPENGL);
    if (p->win && glStart(p, c)) return p;
    fprintf(stderr, "[video] OpenGL unavailable -- SDL_Renderer instead\n");
    glStop(p);
    if (p->win) SDL_DestroyWindow(p->win);
    p->win = NULL;
  } else if (c->backend == CT_BACKEND_VULKAN && !c->software) {
#ifdef CT_HAVE_VULKAN
    p->win = makeWindow(c, SDL_WINDOW_VULKAN);
    if (p->win && CtVkStart(p->win, c->vsync, p->picW, p->picH, &p->vk)) {
      snprintf(p->name, sizeof p->name, "%s", CtVkName(p->vk));
      p->backend = CT_BACKEND_VULKAN;
      return p;
    }
    fprintf(stderr, "[video] Vulkan unavailable -- SDL_Renderer instead\n");
    if (p->win) SDL_DestroyWindow(p->win);
    p->win = NULL;
#else
    fprintf(stderr, "[video] built without Vulkan (tools/get_vulkan_headers.ps1) -- SDL_Renderer instead\n");
#endif
  }
  p->win = makeWindow(c, 0);
  if (!p->win || !sdlStart(p, c)) { CtPresentDestroy(p); return NULL; }
  return p;
}

void CtPresentDestroy(CtPresenter* p) {
  if (!p) return;
  presentStop(p);
  free(p->stg[0]); free(p->stg[1]);
#ifdef CT_HAVE_VULKAN
  if (p->vk) CtVkStop(p->vk);
#endif
  glStop(p);
  if (p->tex) SDL_DestroyTexture(p->tex);
  if (p->tex2) SDL_DestroyTexture(p->tex2);
  if (p->ren) SDL_DestroyRenderer(p->ren);
  if (p->win) SDL_DestroyWindow(p->win);
  free(p);
}

SDL_Window*   CtPresentWindow(CtPresenter* p) { return p ? p->win : NULL; }
const char*   CtPresentName(const CtPresenter* p) { return p ? p->name : ""; }
SDL_Renderer* CtPresentSdlRenderer(CtPresenter* p) { return p && p->backend == CT_BACKEND_SDL ? p->ren : NULL; }
void*         CtPresentGlContext(CtPresenter* p) { return p && p->backend == CT_BACKEND_OPENGL ? p->ctx : NULL; }
struct CtVk*  CtPresentVk(CtPresenter* p) { return p && p->backend == CT_BACKEND_VULKAN ? p->vk : NULL; }


// THE STATS HUD (hoststats.h): a 5x7 bitmap font drawn with filled rectangles over the SDL_Renderer's picture, just
// before the swap. No texture, no font file; the other backends do not draw it (the log line carries the numbers).
static const struct { char c; uint8_t r[7]; } kFont[] = {
  {'0',{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},{'1',{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},{'2',{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}},
  {'3',{0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}},{'4',{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},{'5',{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
  {'6',{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}},{'7',{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},{'8',{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
  {'9',{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},{'A',{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},{'B',{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
  {'C',{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}},{'D',{0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}},{'E',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},
  {'F',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},{'G',{0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}},{'H',{0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
  {'I',{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}},{'J',{0x07,0x02,0x02,0x02,0x02,0x12,0x0C}},{'K',{0x11,0x12,0x14,0x18,0x14,0x12,0x11}},
  {'L',{0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},{'M',{0x11,0x1B,0x15,0x15,0x11,0x11,0x11}},{'N',{0x11,0x11,0x19,0x15,0x13,0x11,0x11}},
  {'O',{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},{'P',{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},{'Q',{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}},
  {'R',{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},{'S',{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}},{'T',{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
  {'U',{0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},{'V',{0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},{'W',{0x11,0x11,0x11,0x15,0x15,0x15,0x0A}},
  {'X',{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}},{'Y',{0x11,0x11,0x0A,0x04,0x04,0x04,0x04}},{'Z',{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}},
  {'.',{0,0,0,0,0,0x0C,0x0C}},{'-',{0,0,0,0x1F,0,0,0}},{'/',{0x01,0x01,0x02,0x04,0x08,0x10,0x10}},
};
static void drawHudLines(SDL_Renderer* r, const char* const* ln, int n) {
  if (!ln || n <= 0) return;
  int ow, oh; SDL_GetRendererOutputSize(r, &ow, &oh);
  int sc = oh >= 1000 ? 3 : oh >= 540 ? 2 : 1, cw = 6 * sc, lh = 9 * sc, maxc = 0;
  for (int i = 0; i < n; i++) { int l = (int)strlen(ln[i]); if (l > maxc) maxc = l; }
  SDL_Rect bg = { 4, 4, maxc * cw + 6 * sc, n * lh + 4 * sc };
  SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(r, 0, 0, 0, 190); SDL_RenderFillRect(r, &bg);
  SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
  for (int i = 0; i < n; i++)
    for (int k = 0; ln[i][k]; k++) {
      const uint8_t* g = NULL;
      for (size_t f = 0; f < sizeof kFont / sizeof kFont[0]; f++) if (kFont[f].c == ln[i][k]) { g = kFont[f].r; break; }
      if (!g) continue;
      for (int y = 0; y < 7; y++) for (int x = 0; x < 5; x++) if (g[y] & (0x10 >> x)) {
        SDL_Rect px = { 4 + 3 * sc + k * cw + x * sc, 4 + 2 * sc + i * lh + y * sc, sc, sc };
        SDL_RenderFillRect(r, &px);
      }
    }
  SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
}
static void drawHud(SDL_Renderer* r) { int n; const char* const* ln = CtStatsHud(&n); drawHudLines(r, ln, n); }
static double statsNow(void) { return (double)SDL_GetPerformanceCounter() * 1000.0 / (double)SDL_GetPerformanceFrequency(); }

// THE SDL TEXTURE IS 32-BIT (ABGR8888 = bytes R,G,B,A in memory, GL's RGBA): SDL's renderers have no native 24-bit format, so
// a 24-bit texture is converted on EVERY update by SDL's generic single-core converter (measured on the Switch: 5.3 ms for a
// 3x widescreen frame, 2.4 MB = 0.45 GB/s). The host converts instead, on the worker pool, and SDL hands the bytes straight on.
typedef struct { const uint8_t* src; uint32_t* dst; int w; } CvJob;
static void cvRows(void* ctx, int y0, int y1) {
  const CvJob* j = (const CvJob*)ctx;
  for (int y = y0; y < y1; y++) {
    const uint8_t* s = j->src + (size_t)y * j->w * 3; uint32_t* d = j->dst + (size_t)y * j->w;
    for (int x = 0; x < j->w; x++, s += 3) d[x] = 0xFF000000u | ((uint32_t)s[2] << 16) | ((uint32_t)s[1] << 8) | s[0];
  }
}
static uint32_t* g_cvBuf; static size_t g_cvN;

// ---- THE RENDER THREAD --------------------------------------------------------------------------------------------------
static uint32_t fnvBuf(const uint32_t* b, size_t n) { uint32_t h = 2166136261u; for (size_t i = 0; i < n; i += 7) h = (h ^ b[i]) * 16777619u; return h; }
static int renderThread(void* arg) {
  CtPresenter* p = (CtPresenter*)arg;
  static int chk = -1; if (chk < 0) chk = getenv("CT_PRESENT_CHECK") != NULL;       // CT_PRESENT_CHECK=1: report a staging buffer that changes under the thread
  long frames = 0, races = 0;
  for (;;) {
    SDL_LockMutex(p->mu);
    while (p->pending < 0 && !p->rtQuit) SDL_CondWait(p->cv, p->mu);
    if (p->pending < 0) { SDL_UnlockMutex(p->mu); break; }                    // (quit, nothing left to show)
    const int b = p->pending; p->pending = -1; p->inflight = b;              // the NEWEST frame; an older one that was waiting has been dropped
    PJob job = p->jobs[b];
    SDL_UnlockMutex(p->mu);
    uint32_t h0 = chk ? fnvBuf(p->stg[b], (size_t)job.w * job.h) : 0;
    double t0 = statsNow();
    SDL_Texture* tx = (p->flip && p->tex2) ? p->tex2 : p->tex; int* lin = (tx == p->tex2) ? &p->linear2 : &p->sdlLinear;
    if (p->tex2) p->flip ^= 1;
    SDL_UpdateTexture(tx, NULL, p->stg[b], job.w * 4);
    double tu = statsNow();
    if (job.linear != *lin) { SDL_SetTextureScaleMode(tx, job.linear ? SDL_ScaleModeLinear : SDL_ScaleModeNearest); *lin = job.linear; }
    SDL_SetRenderDrawColor(p->ren, 0, 0, 0, 255);
    SDL_RenderClear(p->ren);
    SDL_RenderCopy(p->ren, tx, NULL, job.hasDst ? &job.dst : NULL);
    { const char* ln[9]; for (int i = 0; i < job.nHud && i < 9; i++) ln[i] = job.hud[i];
      drawHudLines(p->ren, ln, job.nHud); }
    double t1 = statsNow();
    SDL_RenderPresent(p->ren);                       // (the vsync wait)
    double t2 = statsNow();
    SDL_GL_MakeCurrent(p->win, NULL);                // let the main thread take the context at a sync point (a no-op without GL)
    p->rtWork = t1 - t0; p->rtWait = t2 - t1; p->rtUp = tu - t0; p->rtNew = 1;
    SDL_AtomicAdd(&p->rtShown, 1);
    if (chk) { frames++; if (fnvBuf(p->stg[b], (size_t)job.w * job.h) != h0) { races++; fprintf(stderr, "[present] RACE: staging buffer %d changed during frame %ld\n", b, frames); }
               if (frames % 300 == 0) fprintf(stderr, "[present] render thread: %ld frames, %ld races\n", frames, races); }
    SDL_LockMutex(p->mu); p->inflight = -1; SDL_CondBroadcast(p->cv); SDL_UnlockMutex(p->mu);
  }
  return 0;
}
void CtPresentSync(CtPresenter* p) {                 // nothing waiting, nothing being shown
  if (!p || !p->rt) return;
  SDL_LockMutex(p->mu);
  while (p->pending >= 0 || p->inflight >= 0) SDL_CondWait(p->cv, p->mu);
  SDL_UnlockMutex(p->mu);
}
void CtPresentSetAsync(CtPresenter* p, int on) {
  if (!p) return;
  if (on) {
    if (p->async || p->backend != CT_BACKEND_SDL || p->logical) return;
    SDL_RendererInfo ri; memset(&ri, 0, sizeof ri); SDL_GetRendererInfo(p->ren, &ri);
    if (ri.flags & SDL_RENDERER_SOFTWARE) return;
#ifdef __SWITCH__
    int allow = getenv("CT_ASYNC_PRESENT") ? atoi(getenv("CT_ASYNC_PRESENT")) != 0 : 1;
#else
    int allow = getenv("CT_ASYNC_PRESENT") ? atoi(getenv("CT_ASYNC_PRESENT")) != 0 : 0;
#endif
    if (!allow) return;
    if (!p->rt) {
      p->mu = SDL_CreateMutex(); p->cv = SDL_CreateCond(); p->pending = p->inflight = -1; p->wbuf = 0; SDL_AtomicSet(&p->rtShown, 0);
      if (!p->mu || !p->cv) return;
      p->rt = SDL_CreateThread(renderThread, "ct-render", p);
      if (!p->rt) return;
    }
    if (!p->tex2) {                                  // the second texture (same size, same scale mode as the first)
      p->tex2 = SDL_CreateTexture(p->ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, p->picW, p->picH);
      if (p->tex2) { SDL_SetTextureScaleMode(p->tex2, p->sdlLinear ? SDL_ScaleModeLinear : SDL_ScaleModeNearest); p->linear2 = p->sdlLinear; }
    }
    SDL_GL_MakeCurrent(p->win, NULL);                // the thread takes the context on its first renderer call
    p->async = 1;
    fprintf(stderr, "[video] render thread on (%s)\n", p->name); fflush(stderr);
  } else if (p->async) {
    CtPresentSync(p);
    p->async = 0;                                    // (the context is free: the main thread's next renderer call takes it)
  }
}
static void presentStop(CtPresenter* p) {
  if (!p->rt) return;
  CtPresentSync(p);
  SDL_LockMutex(p->mu); p->rtQuit = 1; SDL_CondBroadcast(p->cv); SDL_UnlockMutex(p->mu);
  SDL_WaitThread(p->rt, NULL); p->rt = NULL;
  SDL_DestroyMutex(p->mu); SDL_DestroyCond(p->cv);
  p->async = 0;
}

void CtPresentUpload(CtPresenter* p, const uint8_t* rgb) {
  if (!p) return;
  double st0 = statsNow();
#ifdef CT_HAVE_VULKAN
  if (p->backend == CT_BACKEND_VULKAN) { CtVkUpload(p->vk, rgb); return; }
#endif
  if (p->backend == CT_BACKEND_OPENGL) {
    gl.BindTexture(CTGL_TEXTURE_2D, p->glTex);
    gl.PixelStorei(CTGL_UNPACK_ALIGNMENT, 1);
    gl.TexSubImage2D(CTGL_TEXTURE_2D, 0, 0, 0, p->picW, p->picH, CTGL_RGB, CTGL_UNSIGNED_BYTE, rgb);
  } else {
    size_t n = (size_t)p->picW * p->picH;
    if (p->async) {                                  // into the staging buffer the thread is NOT reading (the other one)
      int b = p->wbuf;
      if (p->stgN[b] < n) { free(p->stg[b]); p->stg[b] = (uint32_t*)malloc(n * 4); p->stgN[b] = p->stg[b] ? n : 0; }
      if (p->stg[b]) { CvJob job = { rgb, p->stg[b], p->picW }; CtParallelFor(p->picH, 16, cvRows, &job); p->stgW[b] = p->picW; p->stgH[b] = p->picH; }
    } else {
      if (g_cvN < n) { free(g_cvBuf); g_cvBuf = (uint32_t*)malloc(n * 4); g_cvN = g_cvBuf ? n : 0; }
      if (g_cvBuf) {
        CvJob job = { rgb, g_cvBuf, p->picW };
        CtParallelFor(p->picH, 16, cvRows, &job);
        SDL_UpdateTexture(p->tex, NULL, g_cvBuf, p->picW * 4);
      }
    }
  }
  CtStatsPresent(CT_ST_UPLOAD, statsNow() - st0);
}

// THE PLAIN UPSCALE, FUSED WITH THE RGBA CONVERSION (see present.h). One source row becomes one output row (each pixel k times), copied
// for the other k-1 rows; rows go to the host pool. At 4x this replaces a 3.6 MB RGB24 picture written then read back.
typedef struct { const uint8_t* src; uint32_t* dst; int w, k; } UpJob;
static void upRows(void* ctx, int y0, int y1) {
  const UpJob* j = (const UpJob*)ctx; const int w = j->w, k = j->k, W = w * k;
  for (int y = y0; y < y1; y++) {
    const uint8_t* s = j->src + (size_t)y * w * 3; uint32_t* row = j->dst + (size_t)y * k * W;
    for (int x = 0; x < w; x++, s += 3) {
      uint32_t px = 0xFF000000u | ((uint32_t)s[2] << 16) | ((uint32_t)s[1] << 8) | s[0];
      for (int i = 0; i < k; i++) row[x * k + i] = px;
    }
    for (int r = 1; r < k; r++) memcpy(row + (size_t)r * W, row, (size_t)W * 4);
  }
}
int CtPresentUploadScaled(CtPresenter* p, const uint8_t* rgb, int w, int k) {
  if (!p || p->backend != CT_BACKEND_SDL || k < 1 || p->picW != w * k || p->picH % k) return 0;
  double st0 = statsNow();
  const int rows = p->picH / k;
  size_t n = (size_t)p->picW * p->picH;
  uint32_t* dst;
  if (p->async) {
    int b = p->wbuf;
    if (p->stgN[b] < n) { free(p->stg[b]); p->stg[b] = (uint32_t*)malloc(n * 4); p->stgN[b] = p->stg[b] ? n : 0; }
    if (!p->stg[b]) return 0;
    dst = p->stg[b]; p->stgW[b] = p->picW; p->stgH[b] = p->picH;
  } else {
    if (g_cvN < n) { free(g_cvBuf); g_cvBuf = (uint32_t*)malloc(n * 4); g_cvN = g_cvBuf ? n : 0; }
    if (!g_cvBuf) return 0;
    dst = g_cvBuf;
  }
  UpJob job = { rgb, dst, w, k };
  CtParallelFor(rows, 4, upRows, &job);
  if (!p->async) SDL_UpdateTexture(p->tex, NULL, g_cvBuf, p->picW * 4);
  CtStatsPresent(CT_ST_UPLOAD, statsNow() - st0);
  return 1;
}

void CtPresentSetPicSize(CtPresenter* p, int w, int h) {
  if (!p || w <= 0 || h <= 0 || (w == p->picW && h == p->picH)) return;
#ifdef CT_HAVE_VULKAN
  if (p->backend == CT_BACKEND_VULKAN) { if (CtVkSetPicSize(p->vk, w, h)) { p->picW = w; p->picH = h; } return; }
#endif
  if (p->backend == CT_BACKEND_OPENGL) {
    gl.BindTexture(CTGL_TEXTURE_2D, p->glTex);
    gl.TexImage2D(CTGL_TEXTURE_2D, 0, CTGL_RGB8, w, h, 0, CTGL_RGB, CTGL_UNSIGNED_BYTE, NULL);
    p->picW = w; p->picH = h;
    return;
  }
  CtPresentSync(p);                                  // (the render thread must not be using the texture)
  SDL_Texture* t = SDL_CreateTexture(p->ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, w, h);
  if (!t) return;
  SDL_DestroyTexture(p->tex); p->tex = t;
  SDL_SetTextureScaleMode(p->tex, p->sdlLinear ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
  if (p->tex2) {                                     // (the second texture follows)
    SDL_DestroyTexture(p->tex2);
    p->tex2 = SDL_CreateTexture(p->ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    if (p->tex2) { SDL_SetTextureScaleMode(p->tex2, p->sdlLinear ? SDL_ScaleModeLinear : SDL_ScaleModeNearest); p->linear2 = p->sdlLinear; }
  }
  if (p->logical) SDL_RenderSetLogicalSize(p->ren, w, h);
  p->picW = w; p->picH = h;
  if (p->async) SDL_GL_MakeCurrent(p->win, NULL);    // (the main thread took the context for the new texture: give it back)
}

void CtPresentPicSize(CtPresenter* p, int* w, int* h) { *w = p ? p->picW : 0; *h = p ? p->picH : 0; }

void CtPresentOutputSize(CtPresenter* p, int* w, int* h) {
  *w = p ? p->picW : 0; *h = p ? p->picH : 0;
  if (!p) return;
#ifdef CT_HAVE_VULKAN
  if (p->backend == CT_BACKEND_VULKAN) { CtVkOutputSize(p->vk, w, h); return; }
#endif
  if (p->backend == CT_BACKEND_OPENGL) SDL_GL_GetDrawableSize(p->win, w, h);
  else SDL_GetRendererOutputSize(p->ren, w, h);
}

void CtPresentDraw(CtPresenter* p, const SDL_Rect* dst, int linear) {
  if (!p) return;
  double st0 = statsNow();
#ifdef CT_HAVE_VULKAN
  if (p->backend == CT_BACKEND_VULKAN) { CtVkDraw(p->vk, dst, linear); return; }
#endif
  if (p->backend == CT_BACKEND_OPENGL) {
    int ow, oh; SDL_GL_GetDrawableSize(p->win, &ow, &oh);
    gl.BindFramebuffer(CTGL_DRAW_FRAMEBUFFER, 0);
    gl.Disable(CTGL_SCISSOR_TEST);
    gl.Viewport(0, 0, ow, oh);
    gl.ClearColor(0, 0, 0, 1); gl.Clear(CTGL_COLOR_BUFFER_BIT);
    SDL_Rect d = dst ? *dst : (SDL_Rect){ 0, 0, ow, oh };
    gl.BindFramebuffer(CTGL_READ_FRAMEBUFFER, p->glFbo);
    // GL's rows count up from the bottom: the frame's row 0 (its top) goes to the rectangle's top edge, oh - d.y
    gl.BlitFramebuffer(0, 0, p->picW, p->picH, d.x, oh - d.y, d.x + d.w, oh - d.y - d.h, CTGL_COLOR_BUFFER_BIT,
                       linear ? CTGL_LINEAR : CTGL_NEAREST);
    gl.BindFramebuffer(CTGL_READ_FRAMEBUFFER, 0);
    return;
  }
  if (p->async) { p->phasDst = dst != NULL; if (dst) p->pdst = *dst; p->plinear = linear; return; }   // (the thread draws it)
  if (linear != p->sdlLinear) { SDL_SetTextureScaleMode(p->tex, linear ? SDL_ScaleModeLinear : SDL_ScaleModeNearest); p->sdlLinear = linear; }
  SDL_SetRenderDrawColor(p->ren, 0, 0, 0, 255);
  SDL_RenderClear(p->ren);
  SDL_RenderCopy(p->ren, p->tex, NULL, (dst && !p->logical) ? dst : NULL);
  CtStatsPresent(CT_ST_DRAW, statsNow() - st0);
}

void CtPresentSwap(CtPresenter* p) {
  if (!p) return;
#ifdef CT_HAVE_VULKAN
  if (p->backend == CT_BACKEND_VULKAN) { CtVkSwap(p->vk); return; }
#endif
  double st0 = statsNow();
  if (p->async) {
    int b = p->wbuf; PJob* J = &p->jobs[b];
    J->w = p->stgW[b]; J->h = p->stgH[b]; J->hasDst = p->phasDst; J->dst = p->pdst; J->linear = p->plinear;
    { int n = 0; const char* const* ln = CtStatsHud(&n); if (n > 9) n = 9;
      for (int i = 0; i < n; i++) snprintf(J->hud[i], sizeof J->hud[i], "%s", ln[i]);
      J->nHud = ln ? n : 0; }
    // THE MAILBOX: the main thread never waits for the render thread. The frame goes in as the newest; one that was still waiting (the
    // display is late: its flip fence, the vsync phase) is replaced -- skipped, and counted -- so the engine keeps its own pace.
    SDL_LockMutex(p->mu);
    int replaced = p->pending >= 0;
    p->pending = b;
    for (int i = 0; i < 3; i++) if (i != p->pending && i != p->inflight) { p->wbuf = i; break; }   // (the third buffer is always free)
    SDL_CondBroadcast(p->cv);
    SDL_UnlockMutex(p->mu);
    if (replaced) CtStatsPresentSkipped();
    if (p->rtNew) { CtStatsPresent(CT_ST_RWORK, p->rtWork); CtStatsPresent(CT_ST_RWAIT, p->rtWait); CtStatsPresent(CT_ST_RUP, p->rtUp); p->rtNew = 0; }
    CtStatsShownFrames(SDL_AtomicSet(&p->rtShown, 0));
    CtStatsPresent(CT_ST_SWAP, statsNow() - st0);
    return;
  }
  if (p->backend == CT_BACKEND_OPENGL) SDL_GL_SwapWindow(p->win);
  else { if (!p->logical) drawHud(p->ren); SDL_RenderPresent(p->ren); }
  CtStatsPresent(CT_ST_SWAP, statsNow() - st0);
}

void CtPresentSetVsync(CtPresenter* p, int on) {
  if (!p) return;
  CtPresentSync(p);
#ifdef CT_HAVE_VULKAN
  if (p->backend == CT_BACKEND_VULKAN) { CtVkSetVsync(p->vk, on); return; }
#endif
  if (p->backend == CT_BACKEND_OPENGL) SDL_GL_SetSwapInterval(on ? 1 : 0);
  else SDL_RenderSetVSync(p->ren, on ? 1 : 0);
}

int CtPresentSaveBMP(CtPresenter* p, const char* path) {
  if (!p) return 0;
  CtPresentSync(p);
#ifdef CT_HAVE_VULKAN
  if (p->backend == CT_BACKEND_VULKAN) return CtVkSaveBMP(p->vk, path);
#endif
  int ow, oh; CtPresentOutputSize(p, &ow, &oh);
  SDL_Surface* sf = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ARGB8888);
  if (!sf) return 0;
  int ok = 0;
  if (p->backend == CT_BACKEND_OPENGL) {
    uint8_t* tmp = (uint8_t*)malloc((size_t)ow * oh * 4);
    if (tmp) {
      gl.BindFramebuffer(CTGL_READ_FRAMEBUFFER, 0);
      gl.ReadBuffer(CTGL_BACK);
      gl.PixelStorei(CTGL_PACK_ALIGNMENT, 4);
      gl.ReadPixels(0, 0, ow, oh, CTGL_BGRA, CTGL_UNSIGNED_BYTE, tmp);
      for (int y = 0; y < oh; y++)                       // GL's bottom row first
        memcpy((uint8_t*)sf->pixels + (size_t)y * sf->pitch, tmp + (size_t)(oh - 1 - y) * ow * 4, (size_t)ow * 4);
      free(tmp); ok = 1;
    }
  } else ok = SDL_RenderReadPixels(p->ren, NULL, SDL_PIXELFORMAT_ARGB8888, sf->pixels, sf->pitch) == 0;
  if (ok) ok = SDL_SaveBMP(sf, path) == 0;
  SDL_FreeSurface(sf);
  return ok;
}
