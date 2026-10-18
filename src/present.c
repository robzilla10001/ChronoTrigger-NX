// present.c - see present.h. The SDL_Renderer backend and the OpenGL 3.3 backend; the OpenGL functions are loaded
// through SDL_GL_GetProcAddress (no GL headers, no loader library, nothing linked).
#include "present.h"
#include "present_vk.h"
#ifdef CT_HAVE_DEKO3D
#include "present_dk.h"
#endif
#ifdef __SWITCH__
#include <glad/glad.h>     // the overlay's GL loader (ImGui's own would dlopen); loaded here once the context exists
#endif
#include "hoststats.h"
#include "hostpool.h"
#include "switch_platform.h"
#include "bench.h"
#include "settings.h"
#include "overlay.h"
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
#define CTGL_TIME_ELAPSED 0x88BF
#define CTGL_QUERY_RESULT 0x8866
#define CTGL_QUERY_RESULT_AVAILABLE 0x8867
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
  void (CTGL_API* GenQueries)(GLsizei_, GLuint_*);                       // (timer queries: optional)
  void (CTGL_API* DeleteQueries)(GLsizei_, const GLuint_*);
  void (CTGL_API* BeginQuery)(GLenum_, GLuint_);
  void (CTGL_API* EndQuery)(GLenum_);
  void (CTGL_API* GetQueryObjectiv)(GLuint_, GLenum_, GLint_*);
  void (CTGL_API* GetQueryObjectui64v)(GLuint_, GLenum_, unsigned long long*);
} gl;
static int glLoad(void) {
#define L(name) if (!(*(void**)&gl.name = SDL_GL_GetProcAddress("gl" #name))) { fprintf(stderr, "[video] OpenGL: no gl%s\n", #name); return 0; }
  L(GenTextures) L(DeleteTextures) L(BindTexture) L(TexParameteri) L(TexImage2D) L(TexSubImage2D) L(PixelStorei)
  L(GenFramebuffers) L(DeleteFramebuffers) L(BindFramebuffer) L(FramebufferTexture2D) L(CheckFramebufferStatus)
  L(BlitFramebuffer) L(ClearColor) L(Clear) L(Viewport) L(Disable) L(ReadBuffer) L(ReadPixels) L(GetString)
#undef L
  *(void**)&gl.GenQueries = SDL_GL_GetProcAddress("glGenQueries"); *(void**)&gl.DeleteQueries = SDL_GL_GetProcAddress("glDeleteQueries");
  *(void**)&gl.BeginQuery = SDL_GL_GetProcAddress("glBeginQuery"); *(void**)&gl.EndQuery = SDL_GL_GetProcAddress("glEndQuery");
  *(void**)&gl.GetQueryObjectiv = SDL_GL_GetProcAddress("glGetQueryObjectiv"); *(void**)&gl.GetQueryObjectui64v = SDL_GL_GetProcAddress("glGetQueryObjectui64v");
  return 1;
}

static void presentStop(struct CtPresenter* p);
// Per-frame GPU time and the time the CPU is blocked on the GPU (bench.h): 1 when a new GPU time has come in since `*seq`, -1 backends have none (SDL_Renderer).
int CtPresentGpuMs(struct CtPresenter* p, long* seq, double* ms);
double CtPresentWaitedMs(struct CtPresenter* p);
// THE SDL RENDER THREAD'S OWN FIGURES (bench.h): 1 when the thread has finished a frame since *seq (then its times for that frame), 0 when not, -1 when there is no render thread.
int CtPresentRenderThread(struct CtPresenter* p, long* seq, double* work, double* up, double* wait);
void CtPresentRenderThreadTotals(struct CtPresenter* p, long* shown, long* replaced);
typedef struct { int w, h, hasDst, linear, nHud; SDL_Rect dst; char hud[9][96]; } PJob;   // one frame's draw, handed to the render thread
struct CtPresenter {
  int backend;
  SDL_Window* win;
  int picW, picH;
  int texW, texH;            // GPU backends: the size of the texture the backend holds. It is the SOURCE size when the GPU does the k-times scaling (CtPresentUploadScaled), else picW x picH
  char name[160];
  // SDL_Renderer
  SDL_Renderer* ren;
  SDL_Texture* tex;
  int logical;               // headless: the logical size does the placing
  int sdlLinear;             // the texture's current scale mode
  // Vulkan
  CtVk* vk;
#ifdef CT_HAVE_DEKO3D
  CtDk* dk;                  // deko3d (the Switch)
#endif
  // OpenGL
  SDL_GLContext ctx;
  GLuint_ glTex, glFbo;
  GLuint_ glQ[2]; int glQi, glQarmed[2]; double glGpuMs; long glGpuSeq;    // timer queries around the draw, alternating: the last finished one is read without waiting
  GLuint_ hudTex, hudFbo; int hudW, hudH; char hudKey[1024];   // the HUD as a small texture blitted at the corner (no ImGui frame, no shader)
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
  volatile long rtSeq, rtShownTotal, rtReplacedTotal;            // cumulative: frames the thread finished, frames it showed, frames replaced before it got to them (the benchmark reads these)
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
#ifdef __SWITCH__
  if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) { fprintf(stderr, "[video] OpenGL: glad could not load the functions\n"); return 0; }
#endif
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
    if (p->glQ[0] && gl.DeleteQueries) gl.DeleteQueries(2, p->glQ);
    if (p->hudFbo) gl.DeleteFramebuffers(1, &p->hudFbo);
    if (p->hudTex) gl.DeleteTextures(1, &p->hudTex);
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
  p->picW = c->picW; p->picH = c->picH; p->texW = c->picW; p->texH = c->picH;
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
  } else if (c->backend == CT_BACKEND_DEKO3D && !c->software) {
#ifdef CT_HAVE_DEKO3D
    p->win = NULL;                                       // (NO SDL window: SDL would put an EGL surface on the screen; deko3d takes the console's native window itself)
    if (CtDkStart(NULL, c->vsync, p->picW, p->picH, &p->dk)) {
      snprintf(p->name, sizeof p->name, "%s", CtDkName(p->dk));
      p->backend = CT_BACKEND_DEKO3D;
      return p;
    }
    fprintf(stderr, "[video] deko3d unavailable -- SDL_Renderer instead\n");
    CtSwitchBackendFailed();                             // (the Switch, on the dummy video driver: relaunches on the console's own; a no-op elsewhere)
    if (p->win) SDL_DestroyWindow(p->win);
    p->win = NULL;
#else
    fprintf(stderr, "[video] built without deko3d (the Switch build only) -- SDL_Renderer instead\n");
#endif
  } else if (c->backend == CT_BACKEND_VULKAN && !c->software) {
#ifdef CT_HAVE_VULKAN
    #ifdef __SWITCH__
    p->win = NULL;                                       // (no SDL window on the console: SDL would put an EGL surface on the screen; the Vulkan surface is made on the NWindow)
#else
    p->win = makeWindow(c, SDL_WINDOW_VULKAN);
#endif
    if ((p->win || CtSwitchNoWindowMode()) && CtVkStart(p->win, c->vsync, p->picW, p->picH, &p->vk)) {
      snprintf(p->name, sizeof p->name, "%s", CtVkName(p->vk));
      p->backend = CT_BACKEND_VULKAN;
      return p;
    }
    fprintf(stderr, "[video] Vulkan unavailable -- SDL_Renderer instead\n");
    CtSwitchBackendFailed();
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
#ifdef CT_HAVE_DEKO3D
  if (p->dk) CtDkStop(p->dk);
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
#ifdef CT_HAVE_DEKO3D
struct CtDk*  CtPresentDk(CtPresenter* p) { return p && p->backend == CT_BACKEND_DEKO3D ? p->dk : NULL; }
#else
struct CtDk*  CtPresentDk(CtPresenter* p) { (void)p; return NULL; }
#endif
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
    p->rtWork = t1 - t0; p->rtWait = t2 - t1; p->rtUp = tu - t0; p->rtNew = 1; p->rtShownTotal++; p->rtSeq++;
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

void CtPresentStopThread(CtPresenter* p) { if (p) presentStop(p); }

// The GPU backends keep the texture at texW x texH, which is picW x picH (the picture as the engine sizes it) or, when the GPU does the plain k-times scaling, the 1x source: the blit / sampler
// stretches it to the same pixels with nearest filtering (floor of floor is floor), so a frame costs w x 224 pixels of conversion and upload instead of (4w) x 896.
static int gpuTexSet(CtPresenter* p, int w, int h) {
  if (w == p->texW && h == p->texH) return 1;
  int ok = 0;
#ifdef CT_HAVE_DEKO3D
  if (p->backend == CT_BACKEND_DEKO3D) ok = CtDkSetPicSize(p->dk, w, h);
#endif
#ifdef CT_HAVE_VULKAN
  if (p->backend == CT_BACKEND_VULKAN) ok = CtVkSetPicSize(p->vk, w, h);
#endif
  if (p->backend == CT_BACKEND_OPENGL) {
    gl.BindTexture(CTGL_TEXTURE_2D, p->glTex);
    gl.TexImage2D(CTGL_TEXTURE_2D, 0, CTGL_RGB8, w, h, 0, CTGL_RGB, CTGL_UNSIGNED_BYTE, NULL);
    ok = 1;
  }
  if (ok) { p->texW = w; p->texH = h; }
  return ok;
}
static void presentUpload(CtPresenter* p, const uint8_t* rgb);
void CtPresentUpload(CtPresenter* p, const uint8_t* rgb) {
  if (!p) return;
  if (p->backend != CT_BACKEND_SDL && !gpuTexSet(p, p->picW, p->picH)) return;
  presentUpload(p, rgb);
}
static void presentUpload(CtPresenter* p, const uint8_t* rgb) {          // (the texture is already the size of `rgb`)
  double st0 = statsNow();
#ifdef CT_HAVE_DEKO3D
  if (p->backend == CT_BACKEND_DEKO3D) { CtDkUpload(p->dk, rgb); CtStatsPresent(CT_ST_UPLOAD, statsNow() - st0); return; }
#endif
#ifdef CT_HAVE_VULKAN
  if (p->backend == CT_BACKEND_VULKAN) { CtVkUpload(p->vk, rgb); CtStatsPresent(CT_ST_UPLOAD, statsNow() - st0); return; }
#endif
  if (p->backend == CT_BACKEND_OPENGL) {
    gl.BindTexture(CTGL_TEXTURE_2D, p->glTex);
    gl.PixelStorei(CTGL_UNPACK_ALIGNMENT, 1);
    gl.TexSubImage2D(CTGL_TEXTURE_2D, 0, 0, 0, p->texW, p->texH, CTGL_RGB, CTGL_UNSIGNED_BYTE, rgb);
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
  if (!p || k < 1 || p->picW != w * k || p->picH % k) return 0;
  if (p->backend != CT_BACKEND_SDL) {                // the GPU backends: upload the 1x source, the GPU stretches it (nearest only: a linear filter would blur what the k-times copy keeps sharp)
    static int off = -1; if (off < 0) off = getenv("CT_NO_GPU_SCALE") != NULL;      // (A/B switch: the CPU upscale)
    if (off || k == 1 || CtSettingsGet()->linearFilter) return 0;
    if (!gpuTexSet(p, w, p->picH / k)) return 0;
    presentUpload(p, rgb);                           // (times itself)
    return 1;
  }
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
#ifdef CT_HAVE_DEKO3D
  if (p->backend == CT_BACKEND_DEKO3D) { p->picW = w; p->picH = h; return; }          // (the backend's texture follows at the next upload: gpuTexSet)
#endif
#ifdef CT_HAVE_VULKAN
  if (p->backend == CT_BACKEND_VULKAN) { p->picW = w; p->picH = h; return; }
#endif
  if (p->backend == CT_BACKEND_OPENGL) { p->picW = w; p->picH = h; return; }
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
#ifdef CT_HAVE_DEKO3D
  if (p->backend == CT_BACKEND_DEKO3D) { CtDkOutputSize(p->dk, w, h); return; }
#endif
#ifdef CT_HAVE_VULKAN
  if (p->backend == CT_BACKEND_VULKAN) { CtVkOutputSize(p->vk, w, h); return; }
#endif
  if (p->backend == CT_BACKEND_OPENGL) SDL_GL_GetDrawableSize(p->win, w, h);
  else SDL_GetRendererOutputSize(p->ren, w, h);
}

void CtPresentDraw(CtPresenter* p, const SDL_Rect* dst, int linear) {
  if (!p) return;
  double st0 = statsNow();
#ifdef CT_HAVE_DEKO3D
  if (p->backend == CT_BACKEND_DEKO3D) { CtDkDraw(p->dk, dst, linear); CtStatsPresent(CT_ST_DRAW, statsNow() - st0); return; }
#endif
#ifdef CT_HAVE_VULKAN
  if (p->backend == CT_BACKEND_VULKAN) { CtVkDraw(p->vk, dst, linear); CtStatsPresent(CT_ST_DRAW, statsNow() - st0); return; }
#endif
  if (p->backend == CT_BACKEND_OPENGL) {
    int ow, oh; SDL_GL_GetDrawableSize(p->win, &ow, &oh);
    gl.BindFramebuffer(CTGL_DRAW_FRAMEBUFFER, 0);
    gl.Disable(CTGL_SCISSOR_TEST);
    gl.Viewport(0, 0, ow, oh);
    if (gl.GenQueries && gl.BeginQuery && gl.GetQueryObjectui64v) {                    // GPU timing: read the other query if it has finished, start this one
      if (!p->glQ[0]) gl.GenQueries(2, p->glQ);
      int o = p->glQi ^ 1;
      if (p->glQarmed[o]) { GLint_ av = 0; gl.GetQueryObjectiv(p->glQ[o], CTGL_QUERY_RESULT_AVAILABLE, &av);
        if (av) { unsigned long long ns = 0; gl.GetQueryObjectui64v(p->glQ[o], CTGL_QUERY_RESULT, &ns); p->glGpuMs = (double)ns / 1e6; p->glGpuSeq++; p->glQarmed[o] = 0; } }
      if (!p->glQarmed[p->glQi]) { gl.BeginQuery(CTGL_TIME_ELAPSED, p->glQ[p->glQi]); p->glQarmed[p->glQi] = 2; }
    }
    gl.ClearColor(0, 0, 0, 1); gl.Clear(CTGL_COLOR_BUFFER_BIT);
    SDL_Rect d = dst ? *dst : (SDL_Rect){ 0, 0, ow, oh };
    gl.BindFramebuffer(CTGL_READ_FRAMEBUFFER, p->glFbo);
    // GL's rows count up from the bottom: the frame's row 0 (its top) goes to the rectangle's top edge, oh - d.y
    gl.BlitFramebuffer(0, 0, p->texW, p->texH, d.x, oh - d.y, d.x + d.w, oh - d.y - d.h, CTGL_COLOR_BUFFER_BIT,
                       linear ? CTGL_LINEAR : CTGL_NEAREST);
    gl.BindFramebuffer(CTGL_READ_FRAMEBUFFER, 0);
    if (p->glQarmed[p->glQi] == 2) { gl.EndQuery(CTGL_TIME_ELAPSED); p->glQarmed[p->glQi] = 1; p->glQi ^= 1; }
    return;
  }
  if (p->async) { p->phasDst = dst != NULL; if (dst) p->pdst = *dst; p->plinear = linear; return; }   // (the thread draws it)
  if (linear != p->sdlLinear) { SDL_SetTextureScaleMode(p->tex, linear ? SDL_ScaleModeLinear : SDL_ScaleModeNearest); p->sdlLinear = linear; }
  SDL_SetRenderDrawColor(p->ren, 0, 0, 0, 255);
  SDL_RenderClear(p->ren);
  SDL_RenderCopy(p->ren, p->tex, NULL, (dst && !p->logical) ? dst : NULL);
  CtStatsPresent(CT_ST_DRAW, statsNow() - st0);
}

// THE HUD ON THE OPENGL BACKEND: rasterised on the CPU (the same 5x7 font as SDL_Renderer's), kept as a small texture and BLITTED to the corner of the back buffer
// before the swap -- no ImGui frame, no shader. (An ImGui frame every game frame, which the other backends use, left the menus without input on the console.)
static void glHudDraw(CtPresenter* p, const char* const* ln, int n) {
  if (!ln || n <= 0) return;
  int ow = 0, oh = 0; SDL_GL_GetDrawableSize(p->win, &ow, &oh);
  int sc = oh >= 1000 ? 3 : oh >= 540 ? 2 : 1, cw = 6 * sc, lh = 9 * sc, maxc = 0;
  for (int i = 0; i < n; i++) { int l = (int)strlen(ln[i]); if (l > maxc) maxc = l; }
  int w = maxc * cw + 6 * sc, h = n * lh + 4 * sc;
  if (w < 1 || h < 1 || w > 4096 || h > 1024) return;
  char key[1024]; int kl = snprintf(key, sizeof key, "%d|%d|", sc, n);
  for (int i = 0; i < n && kl < (int)sizeof key - 100; i++) kl += snprintf(key + kl, sizeof key - (size_t)kl, "%s\n", ln[i]);
  if (!p->hudTex || strcmp(key, p->hudKey)) {                          // new text: draw it again
    uint8_t* px = (uint8_t*)malloc((size_t)w * (size_t)h * 3);
    if (!px) return;
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) { px[3 * i] = 12; px[3 * i + 1] = 12; px[3 * i + 2] = 12; }
    for (int i = 0; i < n; i++)
      for (int k = 0; ln[i][k]; k++) {
        const uint8_t* g = NULL;
        for (size_t f = 0; f < sizeof kFont / sizeof kFont[0]; f++) if (kFont[f].c == ln[i][k]) { g = kFont[f].r; break; }
        if (!g) continue;
        for (int y = 0; y < 7; y++) for (int x = 0; x < 5; x++) if (g[y] & (0x10 >> x))
          for (int dy = 0; dy < sc; dy++) for (int dx = 0; dx < sc; dx++) {
            int X = 3 * sc + k * cw + x * sc + dx, Y = 2 * sc + i * lh + y * sc + dy;
            if (X < w && Y < h) { uint8_t* q = px + ((size_t)Y * (size_t)w + (size_t)X) * 3; q[0] = q[1] = q[2] = 255; }
          }
      }
    if (!p->hudTex) {
      gl.GenTextures(1, &p->hudTex); gl.BindTexture(CTGL_TEXTURE_2D, p->hudTex);
      gl.TexParameteri(CTGL_TEXTURE_2D, CTGL_TEXTURE_MIN_FILTER, CTGL_NEAREST); gl.TexParameteri(CTGL_TEXTURE_2D, CTGL_TEXTURE_MAG_FILTER, CTGL_NEAREST);
      gl.TexParameteri(CTGL_TEXTURE_2D, CTGL_TEXTURE_WRAP_S, CTGL_CLAMP_TO_EDGE); gl.TexParameteri(CTGL_TEXTURE_2D, CTGL_TEXTURE_WRAP_T, CTGL_CLAMP_TO_EDGE);
      gl.GenFramebuffers(1, &p->hudFbo);
    }
    gl.PixelStorei(CTGL_UNPACK_ALIGNMENT, 1);
    gl.BindTexture(CTGL_TEXTURE_2D, p->hudTex);
    gl.TexImage2D(CTGL_TEXTURE_2D, 0, CTGL_RGB8, w, h, 0, CTGL_RGB, CTGL_UNSIGNED_BYTE, px);
    gl.BindFramebuffer(CTGL_READ_FRAMEBUFFER, p->hudFbo);
    gl.FramebufferTexture2D(CTGL_READ_FRAMEBUFFER, CTGL_COLOR_ATTACHMENT0, CTGL_TEXTURE_2D, p->hudTex, 0);
    free(px); p->hudW = w; p->hudH = h; snprintf(p->hudKey, sizeof p->hudKey, "%s", key);
  }
  gl.Disable(CTGL_SCISSOR_TEST);
  gl.BindFramebuffer(CTGL_READ_FRAMEBUFFER, p->hudFbo);
  gl.BindFramebuffer(CTGL_DRAW_FRAMEBUFFER, 0);
  gl.BlitFramebuffer(0, 0, p->hudW, p->hudH, 4, oh - 4, 4 + p->hudW, oh - 4 - p->hudH, CTGL_COLOR_BUFFER_BIT, CTGL_NEAREST);   // (the reversed rows put the top row on top)
  gl.BindFramebuffer(CTGL_READ_FRAMEBUFFER, 0);
}

int CtPresentBackendAvailable(int id) {
  if (id == CT_BACKEND_SDL || id == CT_BACKEND_OPENGL) return 1;
#ifdef CT_HAVE_VULKAN
  if (id == CT_BACKEND_VULKAN) return 1;
#endif
#ifdef CT_HAVE_DEKO3D
  if (id == CT_BACKEND_DEKO3D) return 1;
#endif
  return 0;
}
int CtPresentBackendId(const CtPresenter* p) { return p ? p->backend : -1; }

// THE PRESENTATION MICRO-BENCHMARK (bench.h). The backend's own cost with no engine work behind it: a synthetic picture of each size the game uses is uploaded, drawn into the window
// and swapped, with every call timed, vsync off (what the backend can sustain) and vsync on (how steady it presents), then the HUD, the mods menu and the linear filter at 4x.
static void benchPattern(uint8_t* rgb, int w, int h, int phase) {
  for (int y = 0; y < h; y++) {
    uint8_t* r = rgb + (size_t)y * (size_t)w * 3;
    for (int x = 0; x < w; x++) {
      int v = ((x + phase * 7) ^ (y + phase * 3)) & 255;
      r[3 * x] = (uint8_t)v; r[3 * x + 1] = (uint8_t)(255 - v); r[3 * x + 2] = (uint8_t)((x * 255) / (w ? w : 1));
    }
  }
}
static int g_benchGk;                            // >1: the phase uploads the 1x source and lets CtPresentUploadScaled scale it (the GPU on the GPU backends, the fused pass on SDL)
static void benchUp(CtPresenter* p, const uint8_t* rgb, int w) { if (g_benchGk > 1) CtPresentUploadScaled(p, rgb, w / g_benchGk, g_benchGk); else CtPresentUpload(p, rgb); }
static void benchPhase(CtPresenter* p, const char* phase, int w, int h, int vsync, int linear, int hud, int menu, int frames, uint8_t* const bufs[4]) {
  static const char* const kHud[9] = { "ENGINE 60.0 FPS  SHOWN 60.0 FPS", "FRAME 16.7 AVG 16.9 MAX MS", "WORK 6.1 AVG 9.8 MAX MS  SLEEP 10.5", "AUDIO 0.4 PICT 1.2 TITLE 0.0 UP 0.3 SWAP 0.1 MS",
                                       "OVER 16.7 MS 0  OVER 33 MS 0  SKIP 0  ARENA 0.0 FX 0.0", "AUDIO Q 8000-17000 B DROP 0 EMPTY 0", "CPU 1785 GPU 768 MEM 1600 MHZ  FIELD  LOC 000  LOGIC 1.0 COMPOSE 2.0 MS",
                                       "RENDER 1024X896  SCALE 4X  WIDE OFF  BETWEEN OFF", "SHOWN 1024X896 IN 1920X1080  FIT 8:7 NEAREST" };
  CtPresentSetPicSize(p, w, h);
  CtPresentSetVsync(p, vsync);
  CtStatsHudForce(hud ? kHud : NULL, hud ? 9 : 0);
  int ow = 0, oh = 0; CtPresentOutputSize(p, &ow, &oh);
  double sc = (double)ow / w < (double)oh / h ? (double)ow / w : (double)oh / h; if (sc <= 0) sc = 1;
  SDL_Rect dst; dst.w = (int)(w * sc); dst.h = (int)(h * sc); dst.x = (ow - dst.w) / 2; dst.y = (oh - dst.h) / 2;
  double* t = (double*)calloc((size_t)frames * 10, sizeof(double)); if (!t) return;
  double *tot = t, *up = t + frames, *dr = t + 2 * frames, *sw = t + 3 * frames, *ov = t + 4 * frames, *wt = t + 5 * frames, *gp = t + 6 * frames;
  double *rw = t + 7 * frames, *ru = t + 8 * frames, *rv = t + 9 * frames;                  // the SDL render thread's work, upload and present wait, one per frame it finished
  int ngp = 0, nrt = 0; long gseq = 0, rseq = 0; double gms = 0, a1, a2, a3; CtPresentGpuMs(p, &gseq, &gms);
  for (int i = 0; i < 30; i++) {                                                       // warm-up: the first frames build pipelines, textures and caches
    benchUp(p, bufs[i & 3], w); CtPresentDraw(p, &dst, linear);
    if (menu) { CtOverlayFrame(); CtOverlayRender(); }
    CtPresentSwap(p);
  }
  CtPresentSync(p);
  CtPresentRenderThread(p, &rseq, &a1, &a2, &a3);                                           // (start from here)
  long shown0, repl0; CtPresentRenderThreadTotals(p, &shown0, &repl0);
  double wall0 = statsNow(), prev = wall0, wait0 = CtPresentWaitedMs(p);
  for (int i = 0; i < frames; i++) {
    double a = statsNow();
    benchUp(p, bufs[i & 3], w);
    double b = statsNow();
    CtPresentDraw(p, &dst, linear);
    double c = statsNow(), o = 0;
    if (menu) { CtOverlayFrame(); CtOverlayRender(); o = statsNow() - c; }
    double d = statsNow();
    CtPresentSwap(p);
    double e = statsNow();
    if (p->async) CtPresentSync(p);                        // the SDL render thread: wait for it to show this frame, so none is replaced and the frame's whole cost (and the thread's) is measured
    double e2 = statsNow();
    up[i] = b - a; dr[i] = c - b; ov[i] = o; sw[i] = e - d; tot[i] = e2 - prev; prev = e2;
    { double w1 = CtPresentWaitedMs(p); wt[i] = (w1 - wait0) + (e2 - e); wait0 = w1; }
    if (CtPresentGpuMs(p, &gseq, &gms) == 1 && ngp < frames) gp[ngp++] = gms;
    if (CtPresentRenderThread(p, &rseq, &a1, &a2, &a3) == 1 && nrt < frames) { rw[nrt] = a1; ru[nrt] = a2; rv[nrt] = a3; nrt++; }
  }
  CtPresentSync(p);
  double wall = (statsNow() - wall0) / 1000.0;
  CtBenchRt rt = { rw, ru, rv, nrt, 0, 0 };
  { long sh1, rp1; CtPresentRenderThreadTotals(p, &sh1, &rp1); rt.shown = sh1 - shown0; rt.replaced = rp1 - repl0; }
  CtBenchPresentRow(phase, w, h, ow, oh, frames, wall, tot, up, dr, sw, ov, wt, gp, ngp, nrt >= 0 && p->async ? &rt : NULL);
  CtStatsHudForce(NULL, 0);
  free(t);
}
void CtPresentBench(CtPresenter* p) {
  if (!p) return;
  static const int sizes[4][2] = { { 256, 224 }, { 512, 448 }, { 1024, 896 }, { 1593, 896 } };   // 1x, 2x, 4x, 4x widescreen 16:9
  static const char* const names[4] = { "1x", "2x", "4x", "4x-16:9" };
  fprintf(stderr, "[bench] presentation micro-benchmark: backend %s\n", CtPresentName(p)); fflush(stderr);
  CtPresentSetAsync(p, 1);
  for (int s = 0; s < 4; s++) {
    int w = sizes[s][0], h = sizes[s][1];
    uint8_t* bufs[4]; int ok = 1;
    for (int i = 0; i < 4; i++) { bufs[i] = (uint8_t*)malloc((size_t)w * (size_t)h * 3); if (!bufs[i]) ok = 0; else benchPattern(bufs[i], w, h, i); }
    if (ok) {
      char ph[64];
      snprintf(ph, sizeof ph, "%s vsync-off", names[s]); benchPhase(p, ph, w, h, 0, 0, 0, 0, 300, bufs);
      snprintf(ph, sizeof ph, "%s vsync-on", names[s]);  benchPhase(p, ph, w, h, 1, 0, 0, 0, 240, bufs);
      if (s == 2) {
        benchPhase(p, "4x linear vsync-off", w, h, 0, 1, 0, 0, 300, bufs);
        benchPhase(p, "4x +HUD vsync-off", w, h, 0, 0, 1, 0, 300, bufs);
        benchPhase(p, "4x +mods-menu vsync-off", w, h, 0, 0, 0, 1, 200, bufs);
        g_benchGk = 4;                                                                         // the same 4x picture from the 1x source: scaled by the GPU (SDL: the fused CPU pass)
        benchPhase(p, "4x scaled-up vsync-off", w, h, 0, 0, 0, 0, 300, bufs);
        benchPhase(p, "4x scaled-up vsync-on", w, h, 1, 0, 0, 0, 240, bufs);
        benchPhase(p, "4x scaled-up +HUD vsync-off", w, h, 0, 0, 1, 0, 300, bufs);
        g_benchGk = 0;
      }
    }
    for (int i = 0; i < 4; i++) free(bufs[i]);
  }
  CtPresentSetVsync(p, CtSettingsGet()->vsync);
  fprintf(stderr, "[bench] presentation micro-benchmark done\n"); fflush(stderr);
}

void CtPresentDrawHud(CtPresenter* p) {                  // (the OpenGL backend's HUD; the swap calls it)
  if (!p || p->backend != CT_BACKEND_OPENGL || p->logical) return;
  int n = 0; const char* const* ln = CtStatsHud(&n);
  if (getenv("CT_HUDFAKE")) { static const char* fk[] = { "ENGINE 60.0 F/S SHOWN 60.0 F/S", "FRAME AVG 16.67 MAX 16.7 MS" }; ln = fk; n = 2; }   // (a probe)
  glHudDraw(p, ln, n);
}

static void benchExtra(CtPresenter* p) {                  // once a frame, after the backend's swap: the GPU's time and the CPU's wait, for the run's summary
  static long seq; static double lastWait; double g = 0;
  int has = CtPresentGpuMs(p, &seq, &g) == 1;
  double w = CtPresentWaitedMs(p); CtBenchExtra(g, has, w - lastWait); lastWait = w;
}
void CtPresentSwap(CtPresenter* p) {
  if (!p) return;
  if (p->backend != CT_BACKEND_SDL && p->backend != CT_BACKEND_OPENGL && !p->logical) { int n = 0; const char* const* ln = CtStatsHud(&n); CtOverlayHud(ln, n); }   // the other backends' HUD, drawn by the overlay (SDL_Renderer draws its own, below)
#ifdef CT_HAVE_DEKO3D
  if (p->backend == CT_BACKEND_DEKO3D) { double t0 = statsNow(); CtDkSwap(p->dk); CtStatsPresent(CT_ST_SWAP, statsNow() - t0); benchExtra(p); return; }
#endif
#ifdef CT_HAVE_VULKAN
  if (p->backend == CT_BACKEND_VULKAN) { double t0 = statsNow(); CtVkSwap(p->vk); CtStatsPresent(CT_ST_SWAP, statsNow() - t0); benchExtra(p); return; }
#endif
  double st0 = statsNow();
  if (p->backend != CT_BACKEND_SDL && !p->logical) { int n = 0; const char* const* ln = CtStatsHud(&n); CtOverlayHud(ln, n); }   // (SDL_Renderer draws its own, below)
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
    if (replaced) { CtStatsPresentSkipped(); p->rtReplacedTotal++; }
    if (p->rtNew) { CtStatsPresent(CT_ST_RWORK, p->rtWork); CtStatsPresent(CT_ST_RWAIT, p->rtWait); CtStatsPresent(CT_ST_RUP, p->rtUp); p->rtNew = 0; }
    CtStatsShownFrames(SDL_AtomicSet(&p->rtShown, 0));
    CtStatsPresent(CT_ST_SWAP, statsNow() - st0);
    return;
  }
  if (p->backend == CT_BACKEND_OPENGL) { CtPresentDrawHud(p); SDL_GL_SwapWindow(p->win); benchExtra(p); }
  else { if (!p->logical) drawHud(p->ren); SDL_RenderPresent(p->ren); }
  CtStatsPresent(CT_ST_SWAP, statsNow() - st0);
}

void CtPresentSetVsync(CtPresenter* p, int on) {
  if (!p) return;
  CtPresentSync(p);
#ifdef CT_HAVE_DEKO3D
  if (p->backend == CT_BACKEND_DEKO3D) { CtDkSetVsync(p->dk, on); return; }
#endif
#ifdef CT_HAVE_VULKAN
  if (p->backend == CT_BACKEND_VULKAN) { CtVkSetVsync(p->vk, on); return; }
#endif
  if (p->backend == CT_BACKEND_OPENGL) SDL_GL_SetSwapInterval(on ? 1 : 0);
  else SDL_RenderSetVSync(p->ren, on ? 1 : 0);
}

int CtPresentSaveBMP(CtPresenter* p, const char* path) {
  if (!p) return 0;
  CtPresentSync(p);
#ifdef CT_HAVE_DEKO3D
  if (p->backend == CT_BACKEND_DEKO3D) return 0;          // (no read-back yet: the other backends take the screenshots)
#endif
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

int CtPresentGpuMs(CtPresenter* p, long* seq, double* ms) {
  if (!p || !seq || !ms) return 0;
  long cur = *seq; double v = 0;
#ifdef CT_HAVE_DEKO3D
  if (p->backend == CT_BACKEND_DEKO3D) { v = CtDkGpuMs(p->dk, &cur); }
  else
#endif
#ifdef CT_HAVE_VULKAN
  if (p->backend == CT_BACKEND_VULKAN) { v = CtVkGpuMs(p->vk, &cur); }
  else
#endif
  if (p->backend == CT_BACKEND_OPENGL) { v = p->glGpuMs; cur = p->glGpuSeq; }
  else return -1;
  if (cur == *seq) return 0;
  *seq = cur; *ms = v; return 1;
}
double CtPresentWaitedMs(CtPresenter* p) {
  if (!p) return 0;
#ifdef CT_HAVE_DEKO3D
  if (p->backend == CT_BACKEND_DEKO3D) return CtDkWaitedMs(p->dk);
#endif
#ifdef CT_HAVE_VULKAN
  if (p->backend == CT_BACKEND_VULKAN) return CtVkWaitedMs(p->vk);
#endif
  return 0;
}

int CtPresentRenderThread(CtPresenter* p, long* seq, double* work, double* up, double* wait) {
  if (!p || !p->async || !seq) return -1;
  long cur = p->rtSeq; if (cur == *seq) return 0;
  *seq = cur; *work = p->rtWork; *up = p->rtUp; *wait = p->rtWait; return 1;
}
void CtPresentRenderThreadTotals(CtPresenter* p, long* shown, long* replaced) { if (shown) *shown = p ? p->rtShownTotal : 0; if (replaced) *replaced = p ? p->rtReplacedTotal : 0; }
