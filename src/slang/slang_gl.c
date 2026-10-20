// slang_gl.c - see slang_gl.h.
#include "slang_gl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define SLGL_API __stdcall
#else
#define SLGL_API
#endif
typedef unsigned int GLu; typedef int GLi; typedef unsigned int GLe; typedef float GLf; typedef char GLc; typedef ptrdiff_t GLsz;

#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_ARRAY_BUFFER 0x8892
#define GL_UNIFORM_BUFFER 0x8A11
#define GL_STREAM_DRAW 0x88E0
#define GL_STATIC_DRAW 0x88E4
#define GL_FLOAT 0x1406
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_SHORT 0x1403
#define GL_UNSIGNED_INT 0x1405
#define GL_TRIANGLE_STRIP 0x0005
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_TEXTURE_BORDER_COLOR 0x1004
#define GL_TEXTURE_BASE_LEVEL 0x813C
#define GL_TEXTURE_MAX_LEVEL 0x813D
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_CLAMP_TO_BORDER 0x812D
#define GL_REPEAT 0x2901
#define GL_MIRRORED_REPEAT 0x8370
#define GL_RGBA 0x1908
#define GL_RED 0x1903
#define GL_RG 0x8227
#define GL_RGBA_INTEGER 0x8D99
#define GL_RED_INTEGER 0x8D94
#define GL_RG_INTEGER 0x8228
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_BLEND 0x0BE2
#define GL_DEPTH_TEST 0x0B71
#define GL_CULL_FACE 0x0B44
#define GL_SCISSOR_TEST 0x0C11
#define GL_FRAMEBUFFER_SRGB 0x8DB9
#define GL_COLOR 0x1800
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_MAJOR_VERSION 0x821B
#define GL_MINOR_VERSION 0x821C
#define GL_NO_ERROR 0
#define GL_INFO_LOG_LENGTH 0x8B84

typedef struct {
  void (SLGL_API* GenTextures)(GLi, GLu*); void (SLGL_API* DeleteTextures)(GLi, const GLu*); void (SLGL_API* BindTexture)(GLe, GLu);
  void (SLGL_API* TexParameteri)(GLe, GLe, GLi); void (SLGL_API* TexImage2D)(GLe, GLi, GLi, GLi, GLi, GLi, GLe, GLe, const void*);
  void (SLGL_API* PixelStorei)(GLe, GLi); void (SLGL_API* GenFramebuffers)(GLi, GLu*); void (SLGL_API* DeleteFramebuffers)(GLi, const GLu*);
  void (SLGL_API* BindFramebuffer)(GLe, GLu); void (SLGL_API* FramebufferTexture2D)(GLe, GLe, GLe, GLu, GLi); GLe (SLGL_API* CheckFramebufferStatus)(GLe);
  void (SLGL_API* BlitFramebuffer)(GLi, GLi, GLi, GLi, GLi, GLi, GLi, GLi, GLu, GLe);
  void (SLGL_API* Viewport)(GLi, GLi, GLi, GLi); void (SLGL_API* Disable)(GLe); void (SLGL_API* Enable)(GLe); void (SLGL_API* ColorMask)(unsigned char, unsigned char, unsigned char, unsigned char);
  GLu (SLGL_API* CreateShader)(GLe); void (SLGL_API* ShaderSource)(GLu, GLi, const GLc* const*, const GLi*); void (SLGL_API* CompileShader)(GLu);
  void (SLGL_API* GetShaderiv)(GLu, GLe, GLi*); void (SLGL_API* GetShaderInfoLog)(GLu, GLi, GLi*, GLc*); void (SLGL_API* DeleteShader)(GLu);
  GLu (SLGL_API* CreateProgram)(void); void (SLGL_API* AttachShader)(GLu, GLu); void (SLGL_API* LinkProgram)(GLu); void (SLGL_API* GetProgramiv)(GLu, GLe, GLi*);
  void (SLGL_API* GetProgramInfoLog)(GLu, GLi, GLi*, GLc*); void (SLGL_API* DeleteProgram)(GLu); void (SLGL_API* UseProgram)(GLu);
  void (SLGL_API* GenBuffers)(GLi, GLu*); void (SLGL_API* DeleteBuffers)(GLi, const GLu*); void (SLGL_API* BindBuffer)(GLe, GLu);
  void (SLGL_API* BufferData)(GLe, GLsz, const void*, GLe); void (SLGL_API* BindBufferBase)(GLe, GLu, GLu);
  void (SLGL_API* GenVertexArrays)(GLi, GLu*); void (SLGL_API* DeleteVertexArrays)(GLi, const GLu*); void (SLGL_API* BindVertexArray)(GLu);
  void (SLGL_API* VertexAttribPointer)(GLu, GLi, GLe, unsigned char, GLi, const void*); void (SLGL_API* EnableVertexAttribArray)(GLu);
  void (SLGL_API* DrawArrays)(GLe, GLi, GLi); void (SLGL_API* ActiveTexture)(GLe);
  void (SLGL_API* GenSamplers)(GLi, GLu*); void (SLGL_API* DeleteSamplers)(GLi, const GLu*); void (SLGL_API* BindSampler)(GLu, GLu);
  void (SLGL_API* SamplerParameteri)(GLu, GLe, GLi); void (SLGL_API* SamplerParameterfv)(GLu, GLe, const GLf*);
  void (SLGL_API* GenerateMipmap)(GLe); GLe (SLGL_API* GetError)(void); void (SLGL_API* GetIntegerv)(GLe, GLi*);
  void (SLGL_API* ClearBufferfv)(GLe, GLi, const GLf*); void (SLGL_API* ClearBufferuiv)(GLe, GLi, const GLu*);
  void (SLGL_API* ReadPixels)(GLi, GLi, GLi, GLi, GLe, GLe, void*); void (SLGL_API* ReadBuffer)(GLe);
} Gl;

typedef struct { GLu tex, fbo; int w, h, fmt, mip, owned, integer; } Tex;
typedef struct { GLu prog, ubo, push; unsigned uboBinding, pushBinding; int hasUbo, hasPush; } Prog;
typedef struct {
  Gl gl; GLu vao, vbo; GLu samp[32]; GLu bbFbo; int bbH; int srgbOn; GLu usedUnits;
} Dev;

// CtSlFormat -> {internal format, format, type, integer?}
static const struct { GLi internal; GLe fmt, type; int integer; } kFmt[CT_SL_FMT_COUNT] = {
  [CT_SL_FMT_RGBA8] = { 0x8058, GL_RGBA, GL_UNSIGNED_BYTE, 0 }, [CT_SL_FMT_RGBA8_SRGB] = { 0x8C43, GL_RGBA, GL_UNSIGNED_BYTE, 0 },
  [CT_SL_FMT_RGBA16F] = { 0x881A, GL_RGBA, GL_FLOAT, 0 }, [CT_SL_FMT_RGBA32F] = { 0x8814, GL_RGBA, GL_FLOAT, 0 },
  [CT_SL_FMT_R8] = { 0x8229, GL_RED, GL_UNSIGNED_BYTE, 0 }, [CT_SL_FMT_RG8] = { 0x822B, GL_RG, GL_UNSIGNED_BYTE, 0 },
  [CT_SL_FMT_R16F] = { 0x822D, GL_RED, GL_FLOAT, 0 }, [CT_SL_FMT_RG16F] = { 0x822F, GL_RG, GL_FLOAT, 0 },
  [CT_SL_FMT_R32F] = { 0x822E, GL_RED, GL_FLOAT, 0 }, [CT_SL_FMT_RG32F] = { 0x8230, GL_RG, GL_FLOAT, 0 },
  [CT_SL_FMT_A2B10G10R10] = { 0x8059, GL_RGBA, GL_UNSIGNED_BYTE, 0 },
  [CT_SL_FMT_RGBA8_UINT] = { 0x8D7C, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, 1 }, [CT_SL_FMT_RGBA8_SINT] = { 0x8D8E, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, 1 },
  [CT_SL_FMT_R8_UINT] = { 0x8232, GL_RED_INTEGER, GL_UNSIGNED_BYTE, 1 }, [CT_SL_FMT_RG8_UINT] = { 0x8238, GL_RG_INTEGER, GL_UNSIGNED_BYTE, 1 },
  [CT_SL_FMT_R16_UINT] = { 0x8234, GL_RED_INTEGER, GL_UNSIGNED_SHORT, 1 }, [CT_SL_FMT_RG16_UINT] = { 0x823A, GL_RG_INTEGER, GL_UNSIGNED_SHORT, 1 },
  [CT_SL_FMT_RGBA16_UINT] = { 0x8D76, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, 1 },
  [CT_SL_FMT_R32_UINT] = { 0x8236, GL_RED_INTEGER, GL_UNSIGNED_INT, 1 }, [CT_SL_FMT_RG32_UINT] = { 0x823C, GL_RG_INTEGER, GL_UNSIGNED_INT, 1 },
  [CT_SL_FMT_RGBA32_UINT] = { 0x8D70, GL_RGBA_INTEGER, GL_UNSIGNED_INT, 1 },
};

static int levelsFor(int w, int h) { int n = 1; while (w > 1 || h > 1) { w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; n++; } return n; }

static void* texCreate(void* d, int w, int h, CtSlFormat fmt, int mip) {
  Dev* dv = (Dev*)d; Gl* g = &dv->gl;
  if ((unsigned)fmt >= CT_SL_FMT_COUNT || !kFmt[fmt].internal) fmt = CT_SL_FMT_RGBA8;
  Tex* t = (Tex*)calloc(1, sizeof *t); if (!t) return NULL;
  t->w = w; t->h = h; t->fmt = fmt; t->mip = mip; t->owned = 1; t->integer = kFmt[fmt].integer;
  g->GenTextures(1, &t->tex); g->BindTexture(GL_TEXTURE_2D, t->tex);
  g->PixelStorei(GL_UNPACK_ALIGNMENT, 1);
  int levels = mip ? levelsFor(w, h) : 1;
  for (int l = 0, lw = w, lh = h; l < levels; l++, lw = lw > 1 ? lw / 2 : 1, lh = lh > 1 ? lh / 2 : 1)
    g->TexImage2D(GL_TEXTURE_2D, l, kFmt[fmt].internal, lw, lh, 0, kFmt[fmt].fmt, kFmt[fmt].type, NULL);
  g->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0); g->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels - 1);
  g->GenFramebuffers(1, &t->fbo); g->BindFramebuffer(GL_FRAMEBUFFER, t->fbo);
  g->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
  if (g->CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
    if (t->integer) { const GLu z[4] = { 0, 0, 0, 0 }; g->ClearBufferuiv(GL_COLOR, 0, z); } else { const GLf z[4] = { 0, 0, 0, 0 }; g->ClearBufferfv(GL_COLOR, 0, z); }
  }
  g->BindFramebuffer(GL_FRAMEBUFFER, 0);
  return t;
}
static void* texFromRgba(void* d, const uint8_t* rgba, int w, int h, int mip) {
  Dev* dv = (Dev*)d; Gl* g = &dv->gl;
  Tex* t = (Tex*)calloc(1, sizeof *t); if (!t) return NULL;
  t->w = w; t->h = h; t->fmt = CT_SL_FMT_RGBA8; t->mip = mip; t->owned = 1;
  g->GenTextures(1, &t->tex); g->BindTexture(GL_TEXTURE_2D, t->tex); g->PixelStorei(GL_UNPACK_ALIGNMENT, 1);
  g->TexImage2D(GL_TEXTURE_2D, 0, 0x8058, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  if (mip) g->GenerateMipmap(GL_TEXTURE_2D);
  else { g->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0); g->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0); }
  g->GenFramebuffers(1, &t->fbo); g->BindFramebuffer(GL_FRAMEBUFFER, t->fbo);
  g->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
  g->BindFramebuffer(GL_FRAMEBUFFER, 0);
  return t;
}
static void texDestroy(void* d, void* tex) {
  Dev* dv = (Dev*)d; Tex* t = (Tex*)tex; if (!t) return;
  if (t->owned) { if (t->fbo) dv->gl.DeleteFramebuffers(1, &t->fbo); if (t->tex) dv->gl.DeleteTextures(1, &t->tex); }
  free(t);
}
static void texCopy(void* d, void* dst, void* src, int w, int h) {
  Dev* dv = (Dev*)d; Gl* g = &dv->gl; Tex* a = (Tex*)dst; Tex* b = (Tex*)src;
  g->BindFramebuffer(GL_READ_FRAMEBUFFER, b->fbo); g->BindFramebuffer(GL_DRAW_FRAMEBUFFER, a->fbo);
  g->Disable(GL_SCISSOR_TEST); g->BlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  g->BindFramebuffer(GL_FRAMEBUFFER, 0);
}
static void texGenMips(void* d, void* tex) { Dev* dv = (Dev*)d; Tex* t = (Tex*)tex; if (!t || !t->mip) return; dv->gl.BindTexture(GL_TEXTURE_2D, t->tex); dv->gl.GenerateMipmap(GL_TEXTURE_2D); }

static GLu compile(Gl* g, GLe type, const char* src, char* err, size_t errN, const char* what) {
  GLu s = g->CreateShader(type); const GLc* p = src; g->ShaderSource(s, 1, &p, NULL); g->CompileShader(s);
  GLi ok = 0; g->GetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) { char log[2048] = ""; g->GetShaderInfoLog(s, sizeof log - 1, NULL, log); snprintf(err, errN, "%s shader: %s", what, log); g->DeleteShader(s); return 0; }
  return s;
}
static void* progCreate(void* d, const CtSlProgram* p, CtSlFormat fmt, char* err, size_t errN) {
  Dev* dv = (Dev*)d; Gl* g = &dv->gl; (void)fmt;
  if (!p->glsl[0] || !p->glsl[1]) { snprintf(err, errN, "no GLSL for the pass"); return NULL; }
  GLu vs = compile(g, GL_VERTEX_SHADER, p->glsl[0], err, errN, "vertex"); if (!vs) return NULL;
  GLu fs = compile(g, GL_FRAGMENT_SHADER, p->glsl[1], err, errN, "fragment"); if (!fs) { g->DeleteShader(vs); return NULL; }
  Prog* pr = (Prog*)calloc(1, sizeof *pr);
  pr->prog = g->CreateProgram(); g->AttachShader(pr->prog, vs); g->AttachShader(pr->prog, fs); g->LinkProgram(pr->prog);
  g->DeleteShader(vs); g->DeleteShader(fs);
  GLi ok = 0; g->GetProgramiv(pr->prog, GL_LINK_STATUS, &ok);
  if (!ok) { char log[2048] = ""; g->GetProgramInfoLog(pr->prog, sizeof log - 1, NULL, log); snprintf(err, errN, "link: %s", log); g->DeleteProgram(pr->prog); free(pr); return NULL; }
  pr->hasUbo = p->ubo.present; pr->uboBinding = p->ubo.binding; pr->hasPush = p->push.present; pr->pushBinding = p->pushBinding;
  if (pr->hasUbo) g->GenBuffers(1, &pr->ubo);
  if (pr->hasPush) g->GenBuffers(1, &pr->push);
  return pr;
}
static void progDestroy(void* d, void* prog) {
  Dev* dv = (Dev*)d; Prog* p = (Prog*)prog; if (!p) return;
  if (p->ubo) dv->gl.DeleteBuffers(1, &p->ubo);
  if (p->push) dv->gl.DeleteBuffers(1, &p->push);
  dv->gl.DeleteProgram(p->prog); free(p);
}

static int passBegin(void* d, void* target, const CtSlRect* vp) {
  Dev* dv = (Dev*)d; Gl* g = &dv->gl; Tex* t = (Tex*)target;
  g->BindFramebuffer(GL_FRAMEBUFFER, t ? t->fbo : dv->bbFbo);
  g->Disable(GL_SCISSOR_TEST); g->Disable(GL_BLEND); g->Disable(GL_DEPTH_TEST); g->Disable(GL_CULL_FACE); g->ColorMask(1, 1, 1, 1);
  int srgb = t && t->fmt == CT_SL_FMT_RGBA8_SRGB;
  if (srgb != dv->srgbOn) { if (srgb) g->Enable(GL_FRAMEBUFFER_SRGB); else g->Disable(GL_FRAMEBUFFER_SRGB); dv->srgbOn = srgb; }
  if (t) g->Viewport(vp->x, vp->y, vp->w, vp->h);
  else g->Viewport(vp->x, dv->bbH - vp->y - vp->h, vp->w, vp->h);           // (GL counts rows from the bottom)
  return 0;
}
static GLu sampler(Dev* dv, int filter, int wrap, int mip) {
  int key = (filter ? 1 : 0) | (wrap & 3) << 1 | (mip ? 1 : 0) << 3;
  if (dv->samp[key]) return dv->samp[key];
  Gl* g = &dv->gl; GLu s; g->GenSamplers(1, &s);
  g->SamplerParameteri(s, GL_TEXTURE_MIN_FILTER, filter ? (mip ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR) : (mip ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST));
  g->SamplerParameteri(s, GL_TEXTURE_MAG_FILTER, filter ? GL_LINEAR : GL_NEAREST);
  GLi w = wrap == CT_SL_WRAP_EDGE ? GL_CLAMP_TO_EDGE : wrap == CT_SL_WRAP_REPEAT ? GL_REPEAT : wrap == CT_SL_WRAP_MIRROR ? GL_MIRRORED_REPEAT : GL_CLAMP_TO_BORDER;
  g->SamplerParameteri(s, GL_TEXTURE_WRAP_S, w); g->SamplerParameteri(s, GL_TEXTURE_WRAP_T, w);
  const GLf border[4] = { 0, 0, 0, 0 }; g->SamplerParameterfv(s, GL_TEXTURE_BORDER_COLOR, border);
  return dv->samp[key] = s;
}
static void passDraw(void* d, void* prog, const CtSlBind* binds, int nBinds, const void* ubo, size_t uboN, const void* push, size_t pushN) {
  Dev* dv = (Dev*)d; Gl* g = &dv->gl; Prog* p = (Prog*)prog;
  g->UseProgram(p->prog);
  for (int i = 0; i < nBinds; i++) {
    const Tex* t = (const Tex*)binds[i].tex; GLu unit = binds[i].binding;
    if (unit >= 32 || !t) continue;
    g->ActiveTexture(GL_TEXTURE0 + unit); g->BindTexture(GL_TEXTURE_2D, t->tex);
    g->BindSampler(unit, sampler(dv, binds[i].filter, binds[i].wrap, binds[i].mip && t->mip)); dv->usedUnits |= 1u << unit;
  }
  if (p->hasUbo) { g->BindBuffer(GL_UNIFORM_BUFFER, p->ubo); g->BufferData(GL_UNIFORM_BUFFER, (GLsz)uboN, ubo, GL_STREAM_DRAW); g->BindBufferBase(GL_UNIFORM_BUFFER, p->uboBinding, p->ubo); }
  if (p->hasPush) { g->BindBuffer(GL_UNIFORM_BUFFER, p->push); g->BufferData(GL_UNIFORM_BUFFER, (GLsz)pushN, push, GL_STREAM_DRAW); g->BindBufferBase(GL_UNIFORM_BUFFER, p->pushBinding, p->push); }
  g->BindVertexArray(dv->vao);
  g->DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  g->BindVertexArray(0);
  g->BindBuffer(GL_UNIFORM_BUFFER, 0);
  for (int u = 0; u < 32; u++) if (dv->usedUnits & (1u << u)) { g->ActiveTexture(GL_TEXTURE0 + u); g->BindSampler(u, 0); }
  dv->usedUnits = 0;
  g->ActiveTexture(GL_TEXTURE0);
  g->UseProgram(0);
}
static void passEnd(void* d, void* target) {
  Dev* dv = (Dev*)d; Tex* t = (Tex*)target;
  if (t && t->mip) { dv->gl.BindTexture(GL_TEXTURE_2D, t->tex); dv->gl.GenerateMipmap(GL_TEXTURE_2D); }
  if (dv->srgbOn) { dv->gl.Disable(GL_FRAMEBUFFER_SRGB); dv->srgbOn = 0; }
  dv->gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
}

int CtSlGlCreate(CtSlGlLoader load, CtSlGpuOps* ops, void** devOut, char* err, size_t errN) {
  Dev* dv = (Dev*)calloc(1, sizeof *dv); if (!dv) { snprintf(err, errN, "out of memory"); return 1; }
  Gl* g = &dv->gl;
#define L(n) if (!(*(void**)&g->n = load("gl" #n))) { snprintf(err, errN, "OpenGL: no gl%s", #n); free(dv); return 1; }
  L(GenTextures) L(DeleteTextures) L(BindTexture) L(TexParameteri) L(TexImage2D) L(PixelStorei) L(GenFramebuffers) L(DeleteFramebuffers) L(BindFramebuffer) L(FramebufferTexture2D)
  L(CheckFramebufferStatus) L(BlitFramebuffer) L(Viewport) L(Disable) L(Enable) L(ColorMask) L(CreateShader) L(ShaderSource) L(CompileShader) L(GetShaderiv) L(GetShaderInfoLog)
  L(DeleteShader) L(CreateProgram) L(AttachShader) L(LinkProgram) L(GetProgramiv) L(GetProgramInfoLog) L(DeleteProgram) L(UseProgram) L(GenBuffers) L(DeleteBuffers) L(BindBuffer)
  L(BufferData) L(BindBufferBase) L(GenVertexArrays) L(DeleteVertexArrays) L(BindVertexArray) L(VertexAttribPointer) L(EnableVertexAttribArray) L(DrawArrays) L(ActiveTexture)
  L(GenSamplers) L(DeleteSamplers) L(BindSampler) L(SamplerParameteri) L(SamplerParameterfv) L(GenerateMipmap) L(GetError) L(GetIntegerv) L(ClearBufferfv) L(ClearBufferuiv)
  L(ReadPixels) L(ReadBuffer)
#undef L
  GLi maj = 0, min = 0; g->GetIntegerv(GL_MAJOR_VERSION, &maj); g->GetIntegerv(GL_MINOR_VERSION, &min);
  if (maj < 4 || (maj == 4 && min < 3)) { snprintf(err, errN, "OpenGL %d.%d: the slang shaders need 4.3", maj, min); free(dv); return 1; }
  // the quad: Position (x, y, 0, 1) and TexCoord (x, y) for x, y in {0, 1}; (0, 0) is the top left
  static const float quad[4][6] = { { 0, 0, 0, 1, 0, 0 }, { 1, 0, 0, 1, 1, 0 }, { 0, 1, 0, 1, 0, 1 }, { 1, 1, 0, 1, 1, 1 } };
  g->GenVertexArrays(1, &dv->vao); g->BindVertexArray(dv->vao);
  g->GenBuffers(1, &dv->vbo); g->BindBuffer(GL_ARRAY_BUFFER, dv->vbo); g->BufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
  g->VertexAttribPointer(0, 4, GL_FLOAT, 0, 6 * sizeof(float), (const void*)0); g->EnableVertexAttribArray(0);
  g->VertexAttribPointer(1, 2, GL_FLOAT, 0, 6 * sizeof(float), (const void*)(4 * sizeof(float))); g->EnableVertexAttribArray(1);
  g->BindVertexArray(0); g->BindBuffer(GL_ARRAY_BUFFER, 0);
  memset(ops, 0, sizeof *ops);
  ops->name = "OpenGL"; ops->flipBackbuffer = 1; ops->wantsGlsl = 1; ops->glslVersion = 430; ops->glslEs = 0; ops->maxPushBytes = 0;
  ops->texCreate = texCreate; ops->texFromRgba = texFromRgba; ops->texDestroy = texDestroy; ops->texCopy = texCopy; ops->texGenMips = texGenMips;
  ops->progCreate = progCreate; ops->progDestroy = progDestroy; ops->passBegin = passBegin; ops->passDraw = passDraw; ops->passEnd = passEnd;
  ops->texReadback = CtSlGlReadback;                                         // (OpenGL needs no private frame)
  *devOut = dv; return 0;
}
void CtSlGlDestroy(void* d) {
  Dev* dv = (Dev*)d; if (!dv) return;
  for (int i = 0; i < 32; i++) if (dv->samp[i]) dv->gl.DeleteSamplers(1, &dv->samp[i]);
  dv->gl.DeleteVertexArrays(1, &dv->vao); dv->gl.DeleteBuffers(1, &dv->vbo); free(dv);
}
void CtSlGlSetBackbuffer(void* d, unsigned fbo, int height) { Dev* dv = (Dev*)d; dv->bbFbo = fbo; dv->bbH = height; }
void* CtSlGlWrap(void* d, unsigned tex, unsigned fbo, int w, int h) { (void)d; Tex* t = (Tex*)calloc(1, sizeof *t); if (t) { t->tex = tex; t->fbo = fbo; t->w = w; t->h = h; t->fmt = CT_SL_FMT_RGBA8; } return t; }
void CtSlGlWrapSet(void* wr, unsigned tex, unsigned fbo, int w, int h) { Tex* t = (Tex*)wr; if (t) { t->tex = tex; t->fbo = fbo; t->w = w; t->h = h; } }
void CtSlGlWrapFree(void* wr) { free(wr); }
void* CtSlGlUpload(void* d, const uint8_t* rgba, int w, int h) { return texFromRgba(d, rgba, w, h, 0); }
uint8_t* CtSlGlReadback(void* d, void* tex, int* w, int* h) {
  Dev* dv = (Dev*)d; Gl* g = &dv->gl; Tex* t = (Tex*)tex; if (!t) return NULL;
  uint8_t* px = (uint8_t*)malloc((size_t)t->w * t->h * 4); if (!px) return NULL;
  g->BindFramebuffer(GL_READ_FRAMEBUFFER, t->fbo); g->ReadBuffer(GL_COLOR_ATTACHMENT0); g->PixelStorei(GL_UNPACK_ALIGNMENT, 1);
  g->ReadPixels(0, 0, t->w, t->h, GL_RGBA, GL_UNSIGNED_BYTE, px);
  g->BindFramebuffer(GL_FRAMEBUFFER, 0); *w = t->w; *h = t->h; return px;
}
