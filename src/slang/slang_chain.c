// slang_chain.c - see slang_chain.h.
#include "slang_chain.h"
#include <ctype.h>
#include <math.h>
#include <sys/stat.h>
#include <time.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- what a name in a shader means ---------------------------------------------------------------------------------------------------------------------------------------------------------

enum { TX_NONE = 0, TX_ORIGINAL, TX_SOURCE, TX_HISTORY, TX_PASS, TX_FEEDBACK, TX_LUT };
typedef struct { int kind, idx; } TexRef;

enum { SM_NONE = 0, SM_MVP, SM_OUTPUT_SIZE, SM_FINAL_SIZE, SM_FRAME_COUNT, SM_FRAME_DIR, SM_ORIG_FPS, SM_ROTATION, SM_SUBFRAME, SM_SUBFRAMES, SM_FRAME_DELTA,
       SM_HDR_ENABLE, SM_PAPER_WHITE, SM_MAX_NITS, SM_ASPECT, SM_ASPECT_ROT, SM_TEXSIZE, SM_PARAM };
typedef struct { int kind; TexRef tex; int param; } SemRef;

typedef struct { void* tex; int w, h; int fmt; } Img;

typedef struct Pass {
  CtSlProgram prog; void* gpu;
  char name[CT_SL_NAME];                // the alias the later passes use ("" = none)
  CtSlFormat fmt;
  SemRef ubo[CT_SL_MAX_MEMBERS], push[CT_SL_MAX_MEMBERS];
  TexRef samp[CT_SL_MAX_TEX];
  uint8_t* uboBuf; uint8_t* pushBuf;
  int needFeedback;                     // a later (or this) pass reads this pass's previous output
  int needMip;                          // the next pass samples this pass's output with mipmaps
  Img out[2]; int cur;                  // the render targets; with feedback, out[cur] is rendered into and out[cur ^ 1] is last frame's
  int w, h;                             // this frame's output size
} Pass;

struct CtSlChain {
  CtSlPreset preset; CtSlGpuOps ops; void* dev;
  int n; Pass* pass;                    // n = the passes that run: the preset's, plus the stock pass when the last one reads its own feedback
  int nReal, stockIdx;                  // the preset's own pass count; where the stock pass lives (always built: the capture and a last pass read back next frame use it)
  CtSlFrame lastFrame; int haveLast, captureReq; char capStatus[400];
  int varPass, varNoSrgb, varNearest, varSrcOriginal;                     // the capture's what-if runs of one pass (see CtSlChainCapture): -1 = none     // the last frame's parameters (the capture reruns the chain with them); a capture was asked for
  int nLut; Img lut[CT_SL_MAX_LUTS];
  int nParam; CtSlChainParam* param; float* paramInit;
  int histN; Img* hist;                 // hist[k] = the Original k frames ago (k = 1 .. histN); hist[0] is this frame's copy, made first thing in the frame
  Img black;                            // 1x1 transparent black for names that mean nothing
  char warn[1024]; int nWarn;
};

static void warn(CtSlChain* c, const char* fmt, ...) {
  char b[256]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
  size_t l = strlen(c->warn); if (l + strlen(b) + 3 < sizeof c->warn) snprintf(c->warn + l, sizeof c->warn - l, "%s; ", b);
  c->nWarn++;
  fprintf(stderr, "[slang] warning: %s\n", b); fflush(stderr);
}

static int isNumber(const char* s) { if (!*s) return 0; for (; *s; s++) if (*s < '0' || *s > '9') return 0; return 1; }

// "Source" "Original" "OriginalHistory3" "PassOutput2" "PassFeedback0", an alias, "<alias>Feedback" or a look-up texture's name -> what it is. 0 = nothing of that name.
static int resolveTex(const CtSlChain* c, int pass, const char* nm, TexRef* r) {
  r->kind = TX_NONE; r->idx = 0;
  if (!strcmp(nm, "Original")) { r->kind = TX_ORIGINAL; return 1; }
  if (!strcmp(nm, "Source")) { r->kind = TX_SOURCE; return 1; }
  if (!strncmp(nm, "OriginalHistory", 15) && isNumber(nm + 15)) { int k = atoi(nm + 15); if (k == 0) r->kind = TX_ORIGINAL; else { r->kind = TX_HISTORY; r->idx = k; } return 1; }
  if (!strncmp(nm, "PassOutput", 10) && isNumber(nm + 10)) { r->kind = TX_PASS; r->idx = atoi(nm + 10); return 1; }
  if (!strncmp(nm, "PassFeedback", 12) && isNumber(nm + 12)) { r->kind = TX_FEEDBACK; r->idx = atoi(nm + 12); return 1; }
  for (int i = 0; i < c->preset.nLuts; i++) if (!strcmp(c->preset.lut[i].name, nm)) { r->kind = TX_LUT; r->idx = i; return 1; }
  for (int i = 0; i < c->n; i++) if (c->pass[i].name[0] && !strcmp(c->pass[i].name, nm)) { r->kind = TX_PASS; r->idx = i; return 1; }
  size_t l = strlen(nm);
  if (l > 8 && !strcmp(nm + l - 8, "Feedback")) {
    char base[CT_SL_NAME]; snprintf(base, sizeof base, "%.*s", (int)(l - 8), nm);
    for (int i = 0; i < c->n; i++) if (c->pass[i].name[0] && !strcmp(c->pass[i].name, base)) { r->kind = TX_FEEDBACK; r->idx = i; return 1; }
  }
  (void)pass;
  return 0;
}

static int classify(const CtSlChain* c, int pass, const char* nm, SemRef* s) {
  memset(s, 0, sizeof *s);
  static const struct { const char* n; int k; } kB[] = {
    { "MVP", SM_MVP }, { "OutputSize", SM_OUTPUT_SIZE }, { "FinalViewportSize", SM_FINAL_SIZE }, { "FrameCount", SM_FRAME_COUNT }, { "FrameDirection", SM_FRAME_DIR },
    { "OriginalFPS", SM_ORIG_FPS }, { "Rotation", SM_ROTATION }, { "CurrentSubFrame", SM_SUBFRAME }, { "TotalSubFrames", SM_SUBFRAMES }, { "FrameTimeDelta", SM_FRAME_DELTA },
    { "EnableHDR", SM_HDR_ENABLE }, { "PaperWhiteNits", SM_PAPER_WHITE }, { "MaxNits", SM_MAX_NITS }, { "OriginalAspect", SM_ASPECT }, { "OriginalAspectRotated", SM_ASPECT_ROT },
  };
  for (size_t i = 0; i < sizeof kB / sizeof kB[0]; i++) if (!strcmp(nm, kB[i].n)) { s->kind = kB[i].k; return 1; }
  for (int i = 0; i < c->nParam; i++) if (!strcmp(c->param[i].def.id, nm)) { s->kind = SM_PARAM; s->param = i; return 1; }
  // the numbered sizes put the number AFTER "Size": OriginalHistorySize3, PassOutputSize1, PassFeedbackSize0
  { static const struct { const char* pre; int kind; } kN[] = { { "OriginalHistorySize", TX_HISTORY }, { "PassOutputSize", TX_PASS }, { "PassFeedbackSize", TX_FEEDBACK } };
    for (size_t i = 0; i < sizeof kN / sizeof kN[0]; i++) {
      size_t pl = strlen(kN[i].pre);
      if (!strncmp(nm, kN[i].pre, pl) && isNumber(nm + pl)) { s->kind = SM_TEXSIZE; s->tex.kind = kN[i].kind; s->tex.idx = atoi(nm + pl); if (s->tex.kind == TX_HISTORY && s->tex.idx == 0) s->tex.kind = TX_ORIGINAL; return 1; }
    } }
  size_t l = strlen(nm);
  if (l > 4 && !strcmp(nm + l - 4, "Size")) {
    char base[CT_SL_NAME]; snprintf(base, sizeof base, "%.*s", (int)(l - 4), nm);
    if (resolveTex(c, pass, base, &s->tex)) { s->kind = SM_TEXSIZE; return 1; }
  }
  return 0;
}

// A last pass that reads its own previous frame renders to a texture of the viewport's size; this copies it to the screen.
static const char kStock[] =
  "#version 450\n"
  "layout(std140, set = 0, binding = 0) uniform UBO { mat4 MVP; vec4 OutputSize; vec4 SourceSize; } global;\n"
  "#pragma stage vertex\n"
  "layout(location = 0) in vec4 Position;\nlayout(location = 1) in vec2 TexCoord;\nlayout(location = 0) out vec2 vTexCoord;\n"
  "void main() { gl_Position = global.MVP * Position; vTexCoord = TexCoord; }\n"
  "#pragma stage fragment\n"
  "layout(location = 0) in vec2 vTexCoord;\nlayout(location = 0) out vec4 FragColor;\nlayout(set = 0, binding = 1) uniform sampler2D Source;\n"
  "void main() { FragColor = texture(Source, vTexCoord); }\n";
#define STOCK_PATH ":stock"

// ---- THE SELF-TEST: tiny known shaders with exactly predictable pixels, run through the same chain machinery as any preset. If a backend draws a preset wrong, which of these fail says which layer is
// broken: the rasterization and the texture coordinates, the sampling of a known picture (and its orientation), the sRGB render target, or the uniform / push-constant blocks. ---------------------------------
static const char kTestPreset[] =
  "shaders = 6\n"
  "shader0 = :t_grad\nscale_type0 = absolute\nscale_x0 = 64\nscale_y0 = 64\nfilter_linear0 = false\n"
  "shader1 = :t_copy\nscale_type1 = absolute\nscale_x1 = 64\nscale_y1 = 64\nfilter_linear1 = false\n"
  "shader2 = :t_srgb\nscale_type2 = absolute\nscale_x2 = 64\nscale_y2 = 64\nfilter_linear2 = false\nsrgb_framebuffer2 = true\n"
  "shader3 = :t_ubo\nfilter_linear3 = false\n"
  "shader4 = :t_vtx\nfilter_linear4 = false\n"
  "shader5 = :t_fc\nfilter_linear5 = false\n";
static const char kTestVert[] =
  "#pragma stage vertex\nlayout(location = 0) in vec4 Position;\nlayout(location = 1) in vec2 TexCoord;\nlayout(location = 0) out vec2 vTexCoord;\n"
  "void main() { gl_Position = global.MVP * Position; vTexCoord = TexCoord; }\n#pragma stage fragment\nlayout(location = 0) in vec2 vTexCoord;\nlayout(location = 0) out vec4 FragColor;\n";
static const char kTestGradHead[] = "#version 450\nlayout(std140, set = 0, binding = 0) uniform UBO { mat4 MVP; vec4 OutputSize; } global;\n";
static const char kTestGradBody[] = "void main() { FragColor = vec4(vTexCoord.x, vTexCoord.y, 0.5, 1.0); }\n";
static const char kTestCopyHead[] = "#version 450\nlayout(std140, set = 0, binding = 0) uniform UBO { mat4 MVP; vec4 OutputSize; } global;\n";
static const char kTestCopyBody[] = "layout(set = 0, binding = 1) uniform sampler2D Original;\nvoid main() { FragColor = texture(Original, vTexCoord); }\n";
static const char kTestSrgbBody[] = "void main() { FragColor = vec4(0.5, 0.5, 0.5, 1.0); }\n";
static const char kTestUboHead[] =
  "#version 450\nlayout(std140, set = 0, binding = 0) uniform UBO { mat4 MVP; vec4 OutputSize; float TestA; } global;\nlayout(push_constant) uniform Push { vec4 SourceSize; float TestB; } params;\n"
  "#pragma parameter TestA \"a\" 0.25 0.0 1.0 0.01\n#pragma parameter TestB \"b\" 0.75 0.0 1.0 0.01\n";
static const char kTestUboBody[] = "layout(set = 0, binding = 1) uniform sampler2D Source;\nvoid main() { FragColor = vec4(global.TestA, params.TestB, params.SourceSize.x / 64.0, global.OutputSize.x / 64.0); }\n";
// The vertex stage reading BOTH blocks and handing four varyings (vec2, vec2, vec2, float) to the fragment stage: royale's scanline pass computes its step sizes in the vertex shader.
static const char kTestVtx[] =
  "#version 450\nlayout(std140, set = 0, binding = 0) uniform UBO { mat4 MVP; vec4 OutputSize; float TestA; } global;\nlayout(push_constant) uniform Push { vec4 SourceSize; float TestB; } params;\n"
  "#pragma parameter TestA \"a\" 0.25 0.0 1.0 0.01\n#pragma parameter TestB \"b\" 0.75 0.0 1.0 0.01\n#pragma stage vertex\n"
  "layout(location = 0) in vec4 Position;\nlayout(location = 1) in vec2 TexCoord;\n"
  "layout(location = 0) out vec2 vA;\nlayout(location = 1) out vec2 vB;\nlayout(location = 2) out vec2 vC;\nlayout(location = 3) out float vD;\n"
  "void main() { gl_Position = global.MVP * Position; vA = vec2(global.TestA, params.TestB); vB = params.SourceSize.xy / 256.0; vC = vec2(1.0, 1.0 + float(params.SourceSize.y > 100.0)); vD = global.OutputSize.y / 128.0; }\n"
  "#pragma stage fragment\nlayout(location = 0) in vec2 vA;\nlayout(location = 1) in vec2 vB;\nlayout(location = 2) in vec2 vC;\nlayout(location = 3) in float vD;\nlayout(location = 0) out vec4 FragColor;\n"
  "layout(set = 0, binding = 1) uniform sampler2D Source;\n"
  "void main() { FragColor = vec4(vA.x + vA.y - 0.75, vB.x * 3.0, vD, (vC.x + vC.y) * 0.25); }\n";
// The push block of royale's scanline pass exactly: three vec4 and a uint (52 bytes), the uint last. The chain runs this test at frame 5.
static const char kTestFc[] =
  "#version 450\nlayout(std140, set = 0, binding = 0) uniform UBO { mat4 MVP; vec4 OutputSize; } global;\nlayout(push_constant) uniform Push { vec4 SourceSize; vec4 OriginalSize; vec4 OutputSize; uint FrameCount; } params;\n"
  "#pragma stage vertex\nlayout(location = 0) in vec4 Position;\nlayout(location = 1) in vec2 TexCoord;\nlayout(location = 0) out vec2 vTexCoord;\n"
  "void main() { gl_Position = global.MVP * Position; vTexCoord = TexCoord; }\n#pragma stage fragment\nlayout(location = 0) in vec2 vTexCoord;\nlayout(location = 0) out vec4 FragColor;\n"
  "layout(set = 0, binding = 1) uniform sampler2D Source;\n"
  "void main() { FragColor = vec4(float(params.FrameCount) / 10.0, params.OutputSize.x / 64.0, params.OriginalSize.x / 64.0, float(params.FrameCount % 2u) * 0.5 + 0.25); }\n";
static char* testSource(const char* path) {
  if (!strcmp(path, ":t_vtx")) return strdup(kTestVtx);
  if (!strcmp(path, ":t_fc")) return strdup(kTestFc);
  const char *h = NULL, *b = NULL;
  if (!strcmp(path, ":t_grad")) { h = kTestGradHead; b = kTestGradBody; }
  else if (!strcmp(path, ":t_copy")) { h = kTestCopyHead; b = kTestCopyBody; }
  else if (!strcmp(path, ":t_srgb")) { h = kTestGradHead; b = kTestSrgbBody; }
  else if (!strcmp(path, ":t_ubo")) { h = kTestUboHead; b = kTestUboBody; }
  if (!h) return NULL;
  size_t n = strlen(h) + strlen(kTestVert) + strlen(b) + 1; char* t = (char*)malloc(n);
  if (t) snprintf(t, n, "%s%s%s", h, kTestVert, b);
  return t;
}

typedef struct { CtSlReadFn rd; void* ctx; } ReadWrap;
static char* readWrapped(const char* path, size_t* len, void* ctx) {
  ReadWrap* w = (ReadWrap*)ctx;
  if (!strcmp(path, STOCK_PATH)) { char* t = (char*)malloc(sizeof kStock); if (t) { memcpy(t, kStock, sizeof kStock); if (len) *len = sizeof kStock - 1; } return t; }
  if (path[0] == ':') {                                                         // the self-test's preset and shaders
    char* t = !strcmp(path, ":selftest.slangp") ? strdup(kTestPreset) : testSource(path);
    if (t && len) *len = strlen(t);
    return t;
  }
  return w->rd(path, len, w->ctx);
}

// ---- loading -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

static void imgFree(const CtSlChain* c, Img* i) { if (i->tex) c->ops.texDestroy(c->dev, i->tex); memset(i, 0, sizeof *i); }

void CtSlChainFree(CtSlChain* c) {
  if (!c) return;
  for (int i = 0; i < c->n; i++) {
    Pass* p = &c->pass[i];
    imgFree(c, &p->out[0]); imgFree(c, &p->out[1]);
    if (p->gpu) c->ops.progDestroy(c->dev, p->gpu);
    CtSlProgramFree(&p->prog); free(p->uboBuf); free(p->pushBuf);
  }
  for (int i = 0; i < c->nLut; i++) imgFree(c, &c->lut[i]);
  for (int i = 0; i <= c->histN; i++) if (c->hist) imgFree(c, &c->hist[i]);
  imgFree(c, &c->black);
  free(c->pass); free(c->param); free(c->paramInit); free(c->hist); free(c);
}

#define FAIL(...) do { snprintf(err, errN, __VA_ARGS__); CtSlChainFree(c); return NULL; } while (0)

CtSlChain* CtSlChainLoad(const char* path, const CtSlGpuOps* ops, void* dev, CtSlReadFn rd, void* rdCtx, char* err, size_t errN) {
  char e0[4]; if (!err) { err = e0; errN = sizeof e0; }
  err[0] = 0;
  CtSlChain* c = (CtSlChain*)calloc(1, sizeof *c);
  if (!c) { snprintf(err, errN, "out of memory"); return NULL; }
  c->ops = *ops; c->dev = dev;
  if (!rd) rd = CtSlReadFile;
  ReadWrap rw = { rd, rdCtx }; rd = readWrapped; rdCtx = &rw;
  if (CtSlPresetLoad(&c->preset, path, rd, rdCtx, err, errN)) { CtSlChainFree(c); return NULL; }
  c->n = c->preset.nPasses;
  c->pass = (Pass*)calloc((size_t)c->n + 1, sizeof(Pass));                       // (one spare: the stock pass)
  CtSlSource* src = (CtSlSource*)calloc((size_t)c->n + 1, sizeof(CtSlSource));
  if (!c->pass || !src) { free(src); FAIL("out of memory"); }

  // 1. every pass's source, its name, and the parameters all of them declare
  for (int i = 0; i < c->n; i++) {
    char e[1024];
    if (CtSlSourceLoad(&src[i], c->preset.pass[i].path, rd, rdCtx, e, sizeof e)) { for (int k = 0; k < i; k++) CtSlSourceFree(&src[k]); free(src); FAIL("pass %d: %s", i, e); }
    snprintf(c->pass[i].name, sizeof c->pass[i].name, "%s", c->preset.pass[i].alias[0] ? c->preset.pass[i].alias : src[i].name);
  }
  int cap = 0;
  for (int i = 0; i < c->n; i++) cap += src[i].nParams;
  c->param = (CtSlChainParam*)calloc((size_t)(cap ? cap : 1), sizeof(CtSlChainParam)); c->paramInit = (float*)calloc((size_t)(cap ? cap : 1), sizeof(float));
  for (int i = 0; i < c->n; i++)
    for (int k = 0; k < src[i].nParams; k++) {
      int dup = 0; for (int q = 0; q < c->nParam; q++) if (!strcmp(c->param[q].def.id, src[i].param[k].id)) dup = 1;
      if (dup) continue;
      c->param[c->nParam].def = src[i].param[k]; c->param[c->nParam].value = src[i].param[k].init; c->param[c->nParam].pass = i; c->nParam++;
    }
  for (int k = 0; k < c->preset.nParams; k++)
    for (int q = 0; q < c->nParam; q++) if (!strcmp(c->param[q].def.id, c->preset.param[k].name)) c->param[q].value = c->preset.param[k].value;
  for (int q = 0; q < c->nParam; q++) c->paramInit[q] = c->param[q].value;

  // 2. compile, translate, reflect, resolve semantics, hand to the GPU
  for (int i = 0; i < c->n; i++) {
    Pass* p = &c->pass[i]; char e[4096];
    if (CtSlCompile(&src[i], &p->prog, e, sizeof e)) { for (int k = 0; k < c->n; k++) CtSlSourceFree(&src[k]); free(src); FAIL("pass %d (%s): %s", i, c->preset.pass[i].path, e); }
    if (c->ops.wantsGlsl && CtSlTranslate(&p->prog, c->ops.glslVersion, c->ops.glslEs, e, sizeof e)) { for (int k = 0; k < c->n; k++) CtSlSourceFree(&src[k]); free(src); FAIL("pass %d (%s): %s", i, c->preset.pass[i].path, e); }
    const CtSlPassCfg* cfg = &c->preset.pass[i];
    p->fmt = cfg->floatFb ? CT_SL_FMT_RGBA16F : cfg->srgbFb ? CT_SL_FMT_RGBA8_SRGB : src[i].format >= 0 ? (CtSlFormat)src[i].format : CT_SL_FMT_RGBA8;
    if (i == c->n - 1) p->fmt = CT_SL_FMT_RGBA8;                                   // (the backbuffer)
    for (int k = 0; k < p->prog.ubo.nMembers; k++) if (!classify(c, i, p->prog.ubo.m[k].name, &p->ubo[k])) warn(c, "pass %d: the uniform '%s' means nothing to the chain (left 0)", i, p->prog.ubo.m[k].name);
    for (int k = 0; k < p->prog.push.nMembers; k++) if (!classify(c, i, p->prog.push.m[k].name, &p->push[k])) warn(c, "pass %d: the push constant '%s' means nothing to the chain (left 0)", i, p->prog.push.m[k].name);
    for (int k = 0; k < p->prog.nTex; k++) {
      if (!resolveTex(c, i, p->prog.tex[k].name, &p->samp[k])) warn(c, "pass %d: the texture '%s' means nothing to the chain (black)", i, p->prog.tex[k].name);
      else if ((p->samp[k].kind == TX_PASS && p->samp[k].idx >= i) || (p->samp[k].kind == TX_FEEDBACK && p->samp[k].idx >= c->n)) { warn(c, "pass %d: '%s' reads a pass that has not run yet (black)", i, p->prog.tex[k].name); p->samp[k].kind = TX_NONE; }
    }
    p->uboBuf = (uint8_t*)calloc(1, p->prog.ubo.size ? p->prog.ubo.size : 4); p->pushBuf = (uint8_t*)calloc(1, p->prog.push.size ? p->prog.push.size : 4);
    if (c->ops.maxPushBytes && (int)p->prog.push.size > c->ops.maxPushBytes) warn(c, "pass %d: %u bytes of push constants, the backend takes %d", i, p->prog.push.size, c->ops.maxPushBytes);
    char pe[1024];
    p->gpu = c->ops.progCreate(c->dev, &p->prog, p->fmt, pe, sizeof pe);
    if (!p->gpu) { for (int k = 0; k < c->n; k++) CtSlSourceFree(&src[k]); free(src); FAIL("pass %d (%s): %s", i, c->preset.pass[i].path, pe); }
  }
  for (int k = 0; k < c->n; k++) CtSlSourceFree(&src[k]);
  free(src);

  // 3. what the passes ask of each other: feedback, mipmaps, how much history
  for (int i = 0; i < c->n; i++) {
    Pass* p = &c->pass[i];
    for (int k = 0; k < p->prog.nTex; k++) {
      if (p->samp[k].kind == TX_FEEDBACK) c->pass[p->samp[k].idx].needFeedback = 1;
      if (p->samp[k].kind == TX_HISTORY && p->samp[k].idx > c->histN) c->histN = p->samp[k].idx;
    }
    for (int k = 0; k < p->prog.ubo.nMembers; k++) { const SemRef* s = &p->ubo[k]; if (s->kind == SM_TEXSIZE) { if (s->tex.kind == TX_FEEDBACK) c->pass[s->tex.idx].needFeedback = 1; if (s->tex.kind == TX_HISTORY && s->tex.idx > c->histN) c->histN = s->tex.idx; } }
    for (int k = 0; k < p->prog.push.nMembers; k++) { const SemRef* s = &p->push[k]; if (s->kind == SM_TEXSIZE) { if (s->tex.kind == TX_FEEDBACK) c->pass[s->tex.idx].needFeedback = 1; if (s->tex.kind == TX_HISTORY && s->tex.idx > c->histN) c->histN = s->tex.idx; } }
    if (c->preset.pass[i].mipmap && i > 0) c->pass[i - 1].needMip = 1;
  }
  c->nReal = c->n; c->stockIdx = c->n; c->varPass = -1;
  if (c->n < CT_SL_MAX_PASSES) {                                                  // the stock pass, always built: it puts a texture on the screen. A last pass that is read back next frame renders to a texture
    // (and needs it; the debug view of a pass's output needs it too)
    int i = c->n; Pass* p = &c->pass[i]; char e[1024]; CtSlSource ss;
    CtSlPassCfg* cfg = &c->preset.pass[i]; memset(cfg, 0, sizeof *cfg);
    snprintf(cfg->path, sizeof cfg->path, "%s", STOCK_PATH); cfg->filter = CT_SL_FILTER_NEAREST; cfg->wrap = CT_SL_WRAP_EDGE; cfg->hasScale = 1; cfg->scaleX = cfg->scaleY = 1.0f;
    cfg->scaleTypeX = cfg->scaleTypeY = CT_SL_SCALE_VIEWPORT;
    if (CtSlSourceLoad(&ss, STOCK_PATH, rd, rdCtx, e, sizeof e)) FAIL("stock pass: %s", e);
    if (CtSlCompile(&ss, &p->prog, e, sizeof e) || (c->ops.wantsGlsl && CtSlTranslate(&p->prog, c->ops.glslVersion, c->ops.glslEs, e, sizeof e))) { CtSlSourceFree(&ss); FAIL("stock pass: %s", e); }
    CtSlSourceFree(&ss);
    p->fmt = CT_SL_FMT_RGBA8;
    for (int k = 0; k < p->prog.ubo.nMembers; k++) classify(c, i, p->prog.ubo.m[k].name, &p->ubo[k]);
    for (int k = 0; k < p->prog.nTex; k++) resolveTex(c, i, p->prog.tex[k].name, &p->samp[k]);
    p->uboBuf = (uint8_t*)calloc(1, p->prog.ubo.size ? p->prog.ubo.size : 4); p->pushBuf = (uint8_t*)calloc(1, 4);
    p->gpu = c->ops.progCreate(c->dev, &p->prog, p->fmt, e, sizeof e);
    if (!p->gpu) FAIL("stock pass: %s", e);
    if (c->pass[c->n - 1].needFeedback) c->n++;                                   // (it runs: the old last pass is a middle pass now)
  } else if (c->pass[c->n - 1].needFeedback) { warn(c, "the last pass's own feedback is not available (too many passes)"); c->pass[c->n - 1].needFeedback = 0; }

  // 4. the look-up textures
  const uint8_t blk[4] = { 0, 0, 0, 0 };
  c->black.tex = c->ops.texFromRgba(c->dev, blk, 1, 1, 0); c->black.w = c->black.h = 1;
  for (int i = 0; i < c->preset.nLuts; i++) {
    int w = 0, h = 0; uint8_t* px = CtSlLoadImage(c->preset.lut[i].path, &w, &h);
    if (!px) { warn(c, "the texture '%s' (%s) could not be read (black)", c->preset.lut[i].name, c->preset.lut[i].path); c->lut[i].tex = NULL; c->lut[i].w = c->lut[i].h = 1; c->nLut++; continue; }
    c->lut[i].tex = c->ops.texFromRgba(c->dev, px, w, h, c->preset.lut[i].mipmap); c->lut[i].w = w; c->lut[i].h = h; c->nLut++;
    free(px);
  }
  if (c->histN) { c->hist = (Img*)calloc((size_t)c->histN + 1, sizeof(Img)); for (int k = 0; k <= c->histN; k++) { c->hist[k].tex = NULL; c->hist[k].w = c->hist[k].h = 1; } }
  return c;
}

// ---- running -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

static void orthoMvp(float m[16], int flipY) {
  memset(m, 0, 16 * sizeof(float));
  m[0] = 2; m[5] = flipY ? -2.0f : 2.0f; m[10] = 1; m[12] = -1; m[13] = flipY ? 1.0f : -1.0f; m[15] = 1;
}

static int scaled(int type, float scale, int srcSize, int vpSize) {
  float v = type == CT_SL_SCALE_ABSOLUTE ? scale : type == CT_SL_SCALE_VIEWPORT ? (float)vpSize * scale : (float)srcSize * scale;
  int r = (int)lroundf(v); return r < 1 ? 1 : r;
}

typedef struct { const CtSlFrame* f; int pass; int srcW, srcH; } RunCtx;

static Img* texImg(CtSlChain* c, const RunCtx* r, TexRef t, Img* srcImg, Img* origImg) {
  switch (t.kind) {
    case TX_ORIGINAL: return origImg;
    case TX_SOURCE: return srcImg;
    case TX_HISTORY: return t.idx <= c->histN ? &c->hist[t.idx] : &c->black;
    case TX_PASS: return t.idx < r->pass ? &c->pass[t.idx].out[c->pass[t.idx].needFeedback ? c->pass[t.idx].cur : 0] : &c->black;
    case TX_FEEDBACK: return (t.idx < c->n && c->pass[t.idx].needFeedback) ? &c->pass[t.idx].out[c->pass[t.idx].cur ^ 1] : &c->black;
    case TX_LUT: return t.idx < c->nLut && c->lut[t.idx].tex ? &c->lut[t.idx] : &c->black;
    default: return &c->black;
  }
}

static void putMember(uint8_t* buf, const CtSlMember* m, const float* v, int nv, uint32_t u, int isUint, int isInt) {
  if (m->offset + m->size > 0x10000) return;
  uint8_t* d = buf + m->offset;
  if (m->cols > 1) { if (m->size >= 64 && nv >= 16) memcpy(d, v, 64); return; }
  if (m->base == CT_SL_T_UINT || isUint) { if (m->vec == 1) memcpy(d, &u, 4); return; }
  if (m->base == CT_SL_T_INT || isInt) { int32_t s = (int32_t)u; if (m->vec == 1) memcpy(d, &s, 4); return; }
  int n = m->vec < nv ? m->vec : nv; memcpy(d, v, (size_t)n * 4);
}

static void fillBlock(CtSlChain* c, Pass* p, const CtSlBlock* b, const SemRef* sem, uint8_t* buf, const RunCtx* r, const float mvp[16], Img* srcImg, Img* origImg, int vpW, int vpH) {
  const CtSlFrame* f = r->f;
  for (int k = 0; k < b->nMembers; k++) {
    const CtSlMember* m = &b->m[k]; const SemRef* s = &sem[k];
    float v[16] = { 0 }; int nv = 4; uint32_t u = 0;
    switch (s->kind) {
      case SM_MVP: memcpy(v, mvp, 64); nv = 16; break;
      case SM_OUTPUT_SIZE: v[0] = (float)p->w; v[1] = (float)p->h; v[2] = 1.0f / (float)p->w; v[3] = 1.0f / (float)p->h; break;
      case SM_FINAL_SIZE: v[0] = (float)vpW; v[1] = (float)vpH; v[2] = 1.0f / (float)vpW; v[3] = 1.0f / (float)vpH; break;
      case SM_FRAME_COUNT: { unsigned mod = c->preset.pass[r->pass].frameCountMod; uint64_t n = f->frame; if (mod) n %= mod; u = (uint32_t)n; v[0] = (float)u; nv = 1; break; }
      case SM_FRAME_DIR: u = 1; v[0] = 1; nv = 1; break;
      case SM_ORIG_FPS: v[0] = f->fps > 0 ? f->fps : 60.0f; nv = 1; break;
      case SM_ROTATION: u = 0; nv = 1; break;
      case SM_SUBFRAME: case SM_SUBFRAMES: u = 1; v[0] = 1; nv = 1; break;
      case SM_FRAME_DELTA: v[0] = 16.667f; u = 16667; nv = 1; break;
      case SM_HDR_ENABLE: u = 0; nv = 1; break;
      case SM_PAPER_WHITE: v[0] = 200.0f; u = 200; nv = 1; break;
      case SM_MAX_NITS: v[0] = 1000.0f; u = 1000; nv = 1; break;
      case SM_ASPECT: case SM_ASPECT_ROT: v[0] = f->aspect > 0 ? f->aspect : (float)f->ow / (float)f->oh; nv = 1; break;
      case SM_TEXSIZE: {
        Img* im = texImg(c, r, s->tex, srcImg, origImg);
        int w = im->w ? im->w : 1, h = im->h ? im->h : 1;
        if (s->tex.kind == TX_PASS && s->tex.idx < r->pass) { w = c->pass[s->tex.idx].w; h = c->pass[s->tex.idx].h; }       // (this frame's size, even before the target is touched)
        v[0] = (float)w; v[1] = (float)h; v[2] = 1.0f / (float)w; v[3] = 1.0f / (float)h; break;
      }
      case SM_PARAM: v[0] = c->param[s->param].value; u = (uint32_t)(int32_t)lroundf(v[0]); nv = 1; break;
      default: continue;
    }
    putMember(buf, m, v, nv, u, 0, 0);
  }
}

static int ensureTarget(CtSlChain* c, Img* o, int w, int h, CtSlFormat fmt, int mip) {
  if (o->tex && o->w == w && o->h == h && o->fmt == (int)fmt) return 1;
  if (o->tex) c->ops.texDestroy(c->dev, o->tex);
  o->tex = c->ops.texCreate(c->dev, w, h, fmt, mip); o->w = w; o->h = h; o->fmt = (int)fmt;
  return o->tex != NULL;
}

// One run of the passes. upto < 0: the normal frame, every pass, the last into the backbuffer. upto >= 0 and `cap` (CAPTURE): the passes up to the real pass `upto`, all offscreen, the end
// of it (that pass's output, put through the stock pass; or the last pass itself) rendered into the RGBA8 texture `cap` -- without touching the frame's state (feedback, history).
static int execute(CtSlChain* c, const CtSlFrame* f, int upto, Img* cap) {
  Img orig = { f->original, f->ow, f->oh };
  if (c->histN && !cap) {                          // the history first, before any pass opens: this frame's Original goes into hist[0], and hist[k] is the frame k ago
    Img old = c->hist[c->histN];
    for (int k = c->histN; k > 0; k--) c->hist[k] = c->hist[k - 1];
    c->hist[0] = old;
    if (!ensureTarget(c, &c->hist[0], f->ow, f->oh, CT_SL_FMT_RGBA8, 0)) return 1;
    c->ops.texCopy(c->dev, c->hist[0].tex, f->original, f->ow, f->oh);
  }
  Img* srcImg = &orig; int srcW = f->ow, srcH = f->oh;
  RunCtx r = { f, 0, srcW, srcH };
  int order[CT_SL_MAX_PASSES + 2], nOrd = 0;
  int stockShows = cap && upto >= -2 && upto < c->nReal - 1 && upto != -1;           // capturing a middle pass: the stock pass puts its output into `cap`. upto = -2: the INPUT picture alone, sampled by the stock pass
  if (stockShows) { for (int i = 0; i <= upto; i++) order[nOrd++] = i; order[nOrd++] = c->stockIdx; }
  else for (int i = 0; i < c->n; i++) order[nOrd++] = i;
  for (int oi = 0; oi < nOrd; oi++) {
    int i = order[oi];
    Pass* p = &c->pass[i]; const CtSlPassCfg* cfg = &c->preset.pass[i];
    int final = oi == nOrd - 1;                                                    // the pass that ends the run
    int var = i == c->varPass;                                                      // (the capture's what-if run of this pass)
    CtSlFormat passFmt = (var && c->varNoSrgb && p->fmt == CT_SL_FMT_RGBA8_SRGB) ? CT_SL_FMT_RGBA8 : p->fmt;
    Img* src = (var && c->varSrcOriginal) ? &orig : srcImg;
    int toScreen = final && !cap;
    r.pass = i; r.srcW = srcW; r.srcH = srcH;
    if (final) { if (stockShows) { p->w = srcW; p->h = srcH; } else { p->w = f->dst.w; p->h = f->dst.h; } }
    else {
      p->w = scaled(cfg->scaleTypeX, cfg->scaleX, srcW, f->dst.w); p->h = scaled(cfg->scaleTypeY, cfg->scaleY, srcH, f->dst.h);
      if (!ensureTarget(c, &p->out[0], p->w, p->h, passFmt, p->needMip)) return 1;
      if (p->needFeedback && !ensureTarget(c, &p->out[1], p->w, p->h, passFmt, p->needMip)) return 1;
    }
    if (final && cap && !ensureTarget(c, cap, p->w, p->h, CT_SL_FMT_RGBA8, 0)) return 1;
    float mvp[16]; orthoMvp(mvp, toScreen ? c->ops.flipBackbuffer : c->ops.flipOffscreen);
    fillBlock(c, p, &p->prog.ubo, p->ubo, p->uboBuf, &r, mvp, src, &orig, f->dst.w, f->dst.h);
    fillBlock(c, p, &p->prog.push, p->push, p->pushBuf, &r, mvp, src, &orig, f->dst.w, f->dst.h);
    CtSlBind binds[CT_SL_MAX_TEX]; int nb = 0;
    for (int k = 0; k < p->prog.nTex; k++) {
      Img* im = texImg(c, &r, p->samp[k], src, &orig);
      CtSlBind* b = &binds[nb++]; b->binding = p->prog.tex[k].binding; b->tex = im->tex ? im->tex : c->black.tex;
      if (p->samp[k].kind == TX_LUT) { const CtSlLutCfg* l = &c->preset.lut[p->samp[k].idx]; b->filter = l->filter; b->wrap = l->wrap; b->mip = l->mipmap; }
      else { b->filter = cfg->filter == CT_SL_FILTER_UNSPEC ? (f->defaultLinear ? CT_SL_FILTER_LINEAR : CT_SL_FILTER_NEAREST) : cfg->filter; b->wrap = cfg->wrap; b->mip = (cfg->mipmap && p->samp[k].kind == TX_SOURCE) ? 1 : 0; }
      if (var && c->varNearest) b->filter = CT_SL_FILTER_NEAREST;
    }
    if (cfg->mipmap && i > 0 && srcImg->tex && c->pass[i - 1].needMip) c->ops.texGenMips(c->dev, srcImg->tex);
    CtSlRect vp = toScreen ? f->dst : (CtSlRect){ 0, 0, p->w, p->h };
    void* target = toScreen ? NULL : (final ? cap->tex : p->out[p->needFeedback ? p->cur : 0].tex);
    if (c->ops.passBegin(c->dev, target, &vp)) return 1;
    c->ops.passDraw(c->dev, p->gpu, binds, nb, p->uboBuf, p->prog.ubo.size, p->pushBuf, p->prog.push.size);
    c->ops.passEnd(c->dev, target);
    srcImg = final && cap ? cap : &p->out[p->needFeedback ? p->cur : 0]; srcW = p->w; srcH = p->h;
  }
  if (!cap) for (int i = 0; i < c->n; i++) if (c->pass[i].needFeedback) c->pass[i].cur ^= 1;       // this frame's output is next frame's feedback
  return 0;
}

int CtSlChainRun(CtSlChain* c, const CtSlFrame* f) {
  if (!c || !f || !f->original || f->ow < 1 || f->oh < 1 || f->dst.w < 1 || f->dst.h < 1) return 1;
  c->lastFrame = *f; c->haveLast = 1;
  return execute(c, f, -1, NULL);
}

// ---- CAPTURE: every pass's output, as images and numbers ----------------------------------------------------------------------------------------------------------------------------------


// ---- THE PROBE: a pass that comes out black although its input did not, bisected inside its own shader. The fragment GLSL's main() declares its intermediate values as locals; the capture recompiles the pass once per
// local with the output replaced by that value (NaN shown magenta, infinity cyan, anything else as itself) and counts what came back. The first one that is black, NaN or infinite is where the shader goes wrong.
// Needs the GLSL: the backends that compile it (OpenGL, deko3d); Vulkan takes SPIR-V and is skipped. ----------------------------------------------------------------------------------------------------------
#define PROBE_MAX 36
static int blockInstance(const char* g, const char* key, char* out, size_t n);
// `var` is a local / varying's name, or one of the derived expressions the probe list adds (fract(X*SourceSize), X*SourceSize/4096, gl_FragCoord.xy/4096). scale: the value is divided by it (1 = as it is).
static char* probePatch(const char* glsl, const char* var, const char* type, const char* outName, float scale) {
  const char* end = strrchr(glsl, '}'); if (!end) return NULL;                      // main() is the last function SPIRV-Cross writes
  const char* conv = !strcmp(type, "float") ? "vec3(%s)" : !strcmp(type, "vec2") ? "vec3(%s, 0.0)" : !strcmp(type, "vec3") ? "%s" : "%s.xyz";
  char expr[200]; snprintf(expr, sizeof expr, "%s", var);
  { char inst[64] = ""; blockInstance(glsl, "uniform Push", inst, sizeof inst);
    char x[64];
    if (!strncmp(var, "fract(", 6) && sscanf(var + 6, "%63[^*]*SourceSize)", x) == 1) snprintf(expr, sizeof expr, "fract(%s * %s.SourceSize.xy)", x, inst);
    else if (strstr(var, "*SourceSize/4096") && sscanf(var, "%63[^*]*SourceSize/4096", x) == 1) snprintf(expr, sizeof expr, "(%s * %s.SourceSize.xy / 4096.0)", x, inst);
    else if (!strcmp(var, "gl_FragCoord.xy/4096")) snprintf(expr, sizeof expr, "(gl_FragCoord.xy / 4096.0)"); }
  char pv[260]; snprintf(pv, sizeof pv, conv, expr);
  char add[1100];
  // colour = the value itself, clamped to 0..1 (so its min / max / mean can be read back exactly); alpha says what it was: 1 normal, 0.25 NaN, 0.5 infinity
  snprintf(add, sizeof add, "    vec3 probe_v = %s;\n    bool probe_n = any(notEqual(probe_v, probe_v));\n    bool probe_i = any(greaterThan(abs(probe_v), vec3(1.0e30)));\n"
           "    %s = vec4(probe_n ? vec3(0.0) : clamp(abs(probe_v) / %.1f, 0.0, 1.0), probe_n ? 0.25 : (probe_i ? 0.5 : 1.0));\n", pv, outName, (double)scale);
  size_t n = strlen(glsl) + strlen(add) + 2; char* o = (char*)malloc(n); if (!o) return NULL;
  size_t head = (size_t)(end - glsl); memcpy(o, glsl, head); strcpy(o + head, add); strcat(o, end);
  return o;
}
static void probeBlackPass(CtSlChain* c, const CtSlFrame* f, int k, FILE* sf) {
  Pass* p = &c->pass[k]; char line[400];
  if (!c->ops.wantsGlsl || !p->prog.glsl[1]) { snprintf(line, sizeof line, "PROBE pass %d: skipped (this backend does not compile GLSL)", k); fprintf(stderr, "[slang] %s\n", line); if (sf) fprintf(sf, "%s\n", line); return; }
  const char* g = p->prog.glsl[1];
  char outName[64] = "FragColor";
  { const char* o = strstr(g, "layout(location = 0) out vec4 "); if (o) { o += strlen("layout(location = 0) out vec4 "); size_t i = 0; while (o[i] && o[i] != ';' && i + 1 < sizeof outName) { outName[i] = o[i]; i++; } outName[i] = 0; } }
  const char* mp = strstr(g, "void main()"); if (!mp) return;
  struct { char name[96]; char type[8]; } cand[PROBE_MAX * 4 + 8]; int nc = 0;
  for (const char* ln = strchr(mp, '\n'); ln && nc < PROBE_MAX * 4; ) {                  // the locals declared at the top level of main (four spaces of indent)
    ln++; if (!strncmp(ln, "    ", 4) && ln[4] != ' ' && ln[4] != '}') {
      const char* q = ln + 4; char type[8] = ""; int t = 0;
      if (!strncmp(q, "float ", 6)) { strcpy(type, "float"); t = 6; } else if (!strncmp(q, "vec2 ", 5)) { strcpy(type, "vec2"); t = 5; } else if (!strncmp(q, "vec3 ", 5)) { strcpy(type, "vec3"); t = 5; } else if (!strncmp(q, "vec4 ", 5)) { strcpy(type, "vec4"); t = 5; }
      if (t) { size_t i = 0; char nm[64]; q += t; while ((isalnum((unsigned char)q[i]) || q[i] == '_') && i + 1 < sizeof nm) { nm[i] = q[i]; i++; } nm[i] = 0;
        if (i && strncmp(nm, "param", 5) && nm[0] != '_' && (q[i] == ' ' || q[i] == ';')) { snprintf(cand[nc].name, 64, "%s", nm); strcpy(cand[nc].type, type); nc++; } }
    }
    ln = strchr(ln, '\n');
  }
  for (const char* in = g; (in = strstr(in, "layout(location = ")) != NULL; in += 10) {            // the fragment stage's own inputs (the varyings), after the locals
    const char* ln = in; const char* e = strchr(ln, ';'); const char* nl = strchr(ln, '\n'); if (!e || (nl && nl < e)) continue;
    const char* kw = strstr(ln, ") in "); if (!kw || kw > e) continue; kw += 5;
    char type[8] = ""; int t = 0;
    if (!strncmp(kw, "float ", 6)) { strcpy(type, "float"); t = 6; } else if (!strncmp(kw, "vec2 ", 5)) { strcpy(type, "vec2"); t = 5; } else if (!strncmp(kw, "vec3 ", 5)) { strcpy(type, "vec3"); t = 5; } else if (!strncmp(kw, "vec4 ", 5)) { strcpy(type, "vec4"); t = 5; }
    if (!t || nc >= PROBE_MAX * 4) continue;
    size_t i = 0; char nm[64]; kw += t; while ((isalnum((unsigned char)kw[i]) || kw[i] == '_') && i + 1 < sizeof nm) { nm[i] = kw[i]; i++; } nm[i] = 0;
    if (i) { snprintf(cand[nc].name, 64, "%s", nm); strcpy(cand[nc].type, type); nc++; }
  }
  { char inst[64] = ""; if (blockInstance(g, "uniform Push", inst, sizeof inst) && strstr(g, "SourceSize")) {                    // DERIVED values, whose exact numbers show how an interpolated coordinate behaves
      for (int i = 0; i < nc; i++) if (!strcmp(cand[i].type, "vec2") && strstr(cand[i].name, "uv") && !strstr(cand[i].name, "step") && !strstr(cand[i].name, "scanline")) {
        if (nc + 2 >= PROBE_MAX * 4 + 8) break;
        snprintf(cand[nc].name, sizeof cand[nc].name, "fract(%s*SourceSize)", cand[i].name); strcpy(cand[nc].type, "vec2"); nc++;
        snprintf(cand[nc].name, sizeof cand[nc].name, "%s*SourceSize/4096", cand[i].name); strcpy(cand[nc].type, "vec2"); nc++;
        break;
      } }
    if (nc < PROBE_MAX * 4 + 8) { snprintf(cand[nc].name, sizeof cand[nc].name, "gl_FragCoord.xy/4096"); strcpy(cand[nc].type, "vec2"); nc++; } }
  if (nc > PROBE_MAX) { int keep = PROBE_MAX; for (int i = 0; i < keep; i++) cand[i] = cand[(long)i * nc / keep]; nc = keep; }          // (spread over the shader, not the first 36)
  snprintf(line, sizeof line, "PROBE pass %d (%s): %d intermediate values, recompiled one by one (this takes a while)", k, c->pass[k].name[0] ? c->pass[k].name : "?", nc); fprintf(stderr, "[slang] %s\n", line); if (sf) fprintf(sf, "%s\n", line);
  void* orig = p->gpu;
  for (int i = 0; i < nc; i++) {
    for (int wide = 0; wide < 2; wide++) {
    char* patched = probePatch(g, cand[i].name, cand[i].type, outName, wide ? 2048.0f : 1.0f); if (!patched) break;
    CtSlProgram tmp = p->prog; tmp.glsl[1] = patched; char e[512];
    void* gp = c->ops.progCreate(c->dev, &tmp, p->fmt, e, sizeof e);
    free(patched);
    if (!gp) { snprintf(line, sizeof line, "  %-28s %-5s did not compile: %.150s", cand[i].name, cand[i].type, e); fprintf(stderr, "[slang] %s\n", line); if (sf) fprintf(sf, "%s\n", line); continue; }
    p->gpu = gp;
    Img cap = { 0 }; uint8_t* px = NULL; int w = 0, h = 0; int saturated = 0;
    if (c->ops.frameBegin) c->ops.frameBegin(c->dev);
    int rc = execute(c, f, k, &cap);
    if (c->ops.frameEnd) c->ops.frameEnd(c->dev);
    if (!rc && cap.tex) px = c->ops.texReadback(c->dev, cap.tex, &w, &h);
    if (cap.tex) c->ops.texDestroy(c->dev, cap.tex);
    p->gpu = orig; c->ops.progDestroy(c->dev, gp);
    if (!px) { snprintf(line, sizeof line, "  %-28s %-5s no output", cand[i].name, cand[i].type); }
    else {
      size_t n = (size_t)w * h, nan = 0, inf = 0; int mn[3] = { 255, 255, 255 }, mx[3] = { 0, 0, 0 }; double sm[3] = { 0, 0, 0 };
      int nch = !strcmp(cand[i].type, "float") ? 1 : !strcmp(cand[i].type, "vec2") ? 2 : 3;
      for (size_t q = 0; q < n; q++) { const uint8_t* t = px + 4 * q;
        if (t[3] < 100) nan++; else if (t[3] < 200) inf++;
        for (int ch = 0; ch < 3; ch++) { if (t[ch] < mn[ch]) mn[ch] = t[ch]; if (t[ch] > mx[ch]) mx[ch] = t[ch]; sm[ch] += t[ch]; } }
      saturated = 0; for (int ch = 0; ch < nch; ch++) if (mx[ch] >= 254) saturated = 1;
      double sc = wide ? 2048.0 : 1.0;
      int l = snprintf(line, sizeof line, "  %-30s %-5s%s NaN %5.1f%% inf %5.1f%% |", cand[i].name, cand[i].type, wide ? " WIDE" : "", 100.0 * (double)nan / (double)n, 100.0 * (double)inf / (double)n);
      for (int ch = 0; ch < nch && l < (int)sizeof line - 60; ch++) l += snprintf(line + l, sizeof line - (size_t)l, " %c min %.4g max %.4g mean %.4g |", "xyz"[ch], mn[ch] / 255.0 * sc, mx[ch] / 255.0 * sc, sm[ch] / 255.0 / (double)n * sc);
      free(px);
    }
    fprintf(stderr, "[slang] %s\n", line); if (sf) fprintf(sf, "%s\n", line);
    if (wide || !saturated) break;                                                    // a value that hit 1.0 is read again divided by 2048 (a size, a pixel coordinate)
    }
  }
  p->gpu = orig;
  fflush(stderr);
}


// What the GPU actually sees in the uniform and push-constant blocks: the failing pass's float / uint members, four at a time, shown as colours (value / 8, clamped), read back and compared with the bytes the chain wrote.
static char* patchAssign(const char* glsl, const char* outName, const char* expr) {
  const char* end = strrchr(glsl, '}'); if (!end) return NULL;
  size_t n = strlen(glsl) + strlen(expr) + strlen(outName) + 24; char* o = (char*)malloc(n); if (!o) return NULL;
  size_t head = (size_t)(end - glsl); memcpy(o, glsl, head); snprintf(o + head, n - head, "    %s = %s;\n", outName, expr); strcat(o, end);
  return o;
}
static int blockInstance(const char* g, const char* key, char* out, size_t n) {           // "uniform UBO { ... } global;" -> "global"
  const char* u = strstr(g, key); if (!u) return 0;
  const char* e = strchr(u, '}'); if (!e) return 0; e++; while (*e == ' ') e++;
  size_t i = 0; while ((isalnum((unsigned char)e[i]) || e[i] == '_') && i + 1 < n) { out[i] = e[i]; i++; } out[i] = 0;
  return i > 0;
}
static void probeBlocks(CtSlChain* c, const CtSlFrame* f, int k, FILE* sf) {
  Pass* p = &c->pass[k]; const char* g = p->prog.glsl[1]; char line[500];
  if (!c->ops.wantsGlsl || !g) return;
  char outName[64] = "FragColor";
  { const char* o = strstr(g, "layout(location = 0) out vec4 "); if (o) { o += strlen("layout(location = 0) out vec4 "); size_t i = 0; while (o[i] && o[i] != ';' && i + 1 < sizeof outName) { outName[i] = o[i]; i++; } outName[i] = 0; } }
  char uName[64] = "", pName[64] = ""; int hasU = blockInstance(g, "uniform UBO", uName, sizeof uName), hasP = blockInstance(g, "uniform Push", pName, sizeof pName);
  struct { char expr[110]; char name[64]; int off; int isPush; int isUint; double scale; } mem[220]; int nm = 0;
  for (int b = 0; b < 2; b++) {
    const CtSlBlock* bl = b ? &p->prog.push : &p->prog.ubo; const char* inst = b ? pName : uName; if (b ? !hasP : !hasU) continue;
    const uint8_t* cb = b ? p->pushBuf : p->uboBuf;
    for (int i = 0; i < bl->nMembers && nm < 216; i++) {
      const CtSlMember* m = &bl->m[i]; if (m->cols > 1 || m->arrayN || m->vec < 1 || (m->base != CT_SL_T_FLOAT && m->base != CT_SL_T_UINT)) continue;
      for (int comp = 0; comp < m->vec && nm < 216; comp++) {                                    // a vector member is read component by component
        int off = (int)m->offset + 4 * comp; double v;
        if (m->base == CT_SL_T_UINT) { uint32_t u; memcpy(&u, cb + off, 4); v = (double)u; } else { float fl; memcpy(&fl, cb + off, 4); v = (double)fl; }
        double sc = fabs(v) > 7.5 ? 2048.0 : 8.0;                                                // a size is read /2048, a plain parameter /8
        char acc[32] = ""; if (m->vec > 1) snprintf(acc, sizeof acc, ".%c", "xyzw"[comp]);
        snprintf(mem[nm].expr, sizeof mem[nm].expr, "clamp(abs(float(%s.%s%s)) / %.1f, 0.0, 1.0)", inst, m->name, acc, sc);
        snprintf(mem[nm].name, sizeof mem[nm].name, "%s.%s%s", b ? "push" : "ubo", m->name, acc);
        mem[nm].off = off; mem[nm].isPush = b; mem[nm].isUint = m->base == CT_SL_T_UINT; mem[nm].scale = sc; nm++;
      }
    }
  }
  snprintf(line, sizeof line, "BLOCKS pass %d: %d uniform / push values (vectors component by component) read back by the GPU, four per recompile (expected = the value the chain wrote, scaled, as a byte)", k, nm); fprintf(stderr, "[slang] %s\n", line); if (sf) fprintf(sf, "%s\n", line);
  int bad = 0; void* orig = p->gpu;
  for (int base = 0; base < nm; base += 4) {
    char expr[512]; int cnt = nm - base < 4 ? nm - base : 4; char parts[4][96];
    for (int j = 0; j < 4; j++) snprintf(parts[j], sizeof parts[j], "%s", j < cnt ? mem[base + j].expr : "0.0");
    snprintf(expr, sizeof expr, "vec4(%s, %s, %s, %s)", parts[0], parts[1], parts[2], parts[3]);
    char* patched = patchAssign(g, outName, expr); if (!patched) continue;
    CtSlProgram tmp = p->prog; tmp.glsl[1] = patched; char e[512];
    void* gp = c->ops.progCreate(c->dev, &tmp, p->fmt, e, sizeof e); free(patched);
    if (!gp) { snprintf(line, sizeof line, "  group at %d did not compile: %.150s", base, e); fprintf(stderr, "[slang] %s\n", line); if (sf) fprintf(sf, "%s\n", line); continue; }
    p->gpu = gp;
    Img cap = { 0 }; uint8_t* px = NULL; int w = 0, h = 0;
    if (c->ops.frameBegin) c->ops.frameBegin(c->dev);
    int rc = execute(c, f, k, &cap);
    if (c->ops.frameEnd) c->ops.frameEnd(c->dev);
    if (!rc && cap.tex) px = c->ops.texReadback(c->dev, cap.tex, &w, &h);
    if (cap.tex) c->ops.texDestroy(c->dev, cap.tex);
    p->gpu = orig; c->ops.progDestroy(c->dev, gp);
    if (!px) continue;
    const uint8_t* q = px + 4 * ((size_t)(h / 2) * w + w / 2);                                   // (every pixel is the same: a uniform)
    for (int j = 0; j < cnt; j++) {
      const uint8_t* cb = mem[base + j].isPush ? p->pushBuf : p->uboBuf; double v;
      if (mem[base + j].isUint) { uint32_t u; memcpy(&u, cb + mem[base + j].off, 4); v = (double)u; } else { float fl; memcpy(&fl, cb + mem[base + j].off, 4); v = (double)fl; }
      double ex = fabs(v) / mem[base + j].scale; if (ex > 1) ex = 1; int e8 = (int)(ex * 255.0 + 0.5);
      int mismatch = abs((int)q[j] - e8) > 2; if (mismatch) bad++;
      if (mismatch || getenv("CT_SLANG_VARIANTS")) { snprintf(line, sizeof line, "  %-40s @%-4d value %-12.5g expected %3d got %3d %s", mem[base + j].name, mem[base + j].off, v, e8, q[j], mismatch ? "<-- MISMATCH" : ""); fprintf(stderr, "[slang] %s\n", line); if (sf) fprintf(sf, "%s\n", line); }
    }
    free(px);
  }
  p->gpu = orig;
  snprintf(line, sizeof line, "BLOCKS pass %d: %d of %d members did not match what the chain wrote", k, bad, nm); fprintf(stderr, "[slang] %s\n", line); if (sf) fprintf(sf, "%s\n", line);
  fflush(stderr);
}

typedef struct { double mean, alpha; double nonBlack; unsigned hash; int r, g, b; } ImgStats;
static void imgStats(const uint8_t* px, int w, int h, ImgStats* st) {
  double sum = 0, sa = 0, sr = 0, sg = 0, sb = 0; size_t nb = 0, n = (size_t)w * h; unsigned hs = 2166136261u;
  for (size_t i = 0; i < n; i++) {
    int r = px[4 * i], g = px[4 * i + 1], b = px[4 * i + 2], a = px[4 * i + 3];
    sr += r; sg += g; sb += b; sum += r + g + b; sa += a; if (r > 8 || g > 8 || b > 8) nb++;
    if ((i & 15) == 0) { hs = (hs ^ (unsigned)(r | g << 8 | b << 16)) * 16777619u; }
  }
  st->mean = n ? sum / (3.0 * (double)n) : 0; st->alpha = n ? sa / (double)n : 0; st->nonBlack = n ? 100.0 * (double)nb / (double)n : 0; st->hash = hs;
  st->r = n ? (int)(sr / (double)n) : 0; st->g = n ? (int)(sg / (double)n) : 0; st->b = n ? (int)(sb / (double)n) : 0;
}
static void safeName(char* out, size_t n, const char* in) {
  size_t o = 0; for (; *in && o + 1 < n; in++) out[o++] = (isalnum((unsigned char)*in) || *in == '-' || *in == '_') ? *in : '_'; out[o] = 0;
}


// The picture the self-test starts from: red grows to the right, green downwards, blue is a one-pixel checker; opaque.
static void testPattern(uint8_t* px) {
  for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) { uint8_t* q = px + 4 * (y * 64 + x); q[0] = (uint8_t)(x * 4); q[1] = (uint8_t)(y * 4); q[2] = ((x ^ y) & 1) ? 255 : 0; q[3] = 255; }
}
// 0 = every check passed. `out` gets one line per check. The images go to <dir>/selftest_<n>.png.
static int selfTest(const CtSlGpuOps* ops, void* dev, const char* dir, FILE* sf) {
  char err[1024]; int fails = 0;
  CtSlChain* t = CtSlChainLoad(":selftest.slangp", ops, dev, NULL, NULL, err, sizeof err);
  if (!t) { fprintf(stderr, "[slang] self-test: could not load: %s\n", err); if (sf) fprintf(sf, "self-test: could not load: %s\n", err); return 1; }
  uint8_t pat[64 * 64 * 4]; testPattern(pat);
  void* orig = ops->texFromRgba(dev, pat, 64, 64, 0);
  CtSlFrame f = { orig, 64, 64, 5, { 0, 0, 64, 64 }, 0, 60.0f, 1.0f };
  static const char* const kName[6] = { "gradient (rasterization, texture coordinates, orientation)", "copy of a known picture (upload, sampling, orientation)", "sRGB render target (write 0.5, read back decoded)", "uniform block, push constants and size semantics", "the VERTEX stage reading both blocks, four varyings to the fragment stage", "royale's push block: three vec4 and a uint FrameCount (frame 5)" };
  for (int k = 0; k < 6; k++) {
    char line[500]; Img cap = { 0 }; uint8_t* px = NULL; int w = 0, h = 0;
    if (ops->frameBegin) ops->frameBegin(dev);
    int rc = execute(t, &f, k, &cap);
    if (ops->frameEnd) ops->frameEnd(dev);
    if (!rc && cap.tex) px = ops->texReadback(dev, cap.tex, &w, &h);
    if (cap.tex) ops->texDestroy(dev, cap.tex);
    if (!px || w < 16 || h < 16) { snprintf(line, sizeof line, "SELFTEST %d %-62s FAIL: no output", k, kName[k]); fails++; }
    else {
      int bad = 0, bx = -1, by = -1, got[4] = { 0 }, want[4] = { 0 }; int maxErr = 0;
      for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        const uint8_t* q = px + 4 * ((size_t)y * w + x); int e[4];
        if (k == 0) { e[0] = (int)((x + 0.5) / w * 255.0 + 0.5); e[1] = (int)((y + 0.5) / h * 255.0 + 0.5); e[2] = 128; e[3] = 255; }
        else if (k == 1) { const uint8_t* o = pat + 4 * (y * 64 + x); e[0] = o[0]; e[1] = o[1]; e[2] = o[2]; e[3] = o[3]; }
        else if (k == 2) { e[0] = e[1] = e[2] = 128; e[3] = 255; }
        else if (k == 3) { e[0] = 64; e[1] = 191; e[2] = 255; e[3] = 255; }
        else if (k == 4) { e[0] = 64; e[1] = 191; e[2] = 128; e[3] = 128; }
        else { e[0] = 128; e[1] = 255; e[2] = 255; e[3] = 191; }                         // FrameCount 5: 5/10 = 0.5; 5 % 2 = 1 -> 0.75
        int tol = k == 1 ? 1 : 3;
        for (int ch = 0; ch < 4; ch++) { int d = abs((int)q[ch] - e[ch]); if (d > maxErr) maxErr = d; if (d > tol && !bad) { bx = x; by = y; for (int c2 = 0; c2 < 4; c2++) { got[c2] = q[c2]; want[c2] = e[c2]; } } if (d > tol) { bad++; break; } }
      }
      if (!bad) snprintf(line, sizeof line, "SELFTEST %d %-62s PASS (max error %d)", k, kName[k], maxErr);
      else { fails++; snprintf(line, sizeof line, "SELFTEST %d %-62s FAIL: %d of %d pixels wrong, first at (%d,%d): got %d,%d,%d,%d expected %d,%d,%d,%d", k, kName[k], bad, w * h, bx, by, got[0], got[1], got[2], got[3], want[0], want[1], want[2], want[3]); }
      char path[600]; snprintf(path, sizeof path, "%s/selftest_%d.png", dir, k);
      for (size_t i = 0; i < (size_t)w * h; i++) px[4 * i + 3] = 255;
      CtSlSavePng(path, px, w, h);
    }
    fprintf(stderr, "[slang] %s\n", line); if (sf) fprintf(sf, "%s\n", line);
    free(px);
    if (k == 0) {                                                                    // the gradient again, to tell WHERE a failure comes from: its own output read straight from memory, and a second run
      for (int pass2 = 0; pass2 < 2; pass2++) {
        uint8_t* q2 = NULL; int w2 = 0, h2 = 0; const char* what = pass2 == 0 ? "gradient, the pass's own output read directly (no second draw)" : "gradient, a second run of the same chain";
        if (pass2 == 0) { Pass* p0 = &t->pass[0]; Img* o = &p0->out[p0->needFeedback ? p0->cur ^ 1 : 0]; if (o->tex) q2 = ops->texReadback(dev, o->tex, &w2, &h2); }
        else {
          Img cap2 = { 0 };
          if (ops->frameBegin) ops->frameBegin(dev);
          int rc2 = execute(t, &f, 0, &cap2);
          if (ops->frameEnd) ops->frameEnd(dev);
          if (!rc2 && cap2.tex) q2 = ops->texReadback(dev, cap2.tex, &w2, &h2);
          if (cap2.tex) ops->texDestroy(dev, cap2.tex);
        }
        int bad2 = -1;
        if (q2 && w2 >= 16) { bad2 = 0; for (int y = 0; y < h2; y++) for (int x = 0; x < w2; x++) { const uint8_t* q = q2 + 4 * ((size_t)y * w2 + x); int e0 = (int)((x + 0.5) / w2 * 255.0 + 0.5), e1 = (int)((y + 0.5) / h2 * 255.0 + 0.5);
            if (abs(q[0] - e0) > 3 || abs(q[1] - e1) > 3 || abs(q[2] - 128) > 3) bad2++; } }
        if (bad2 < 0) snprintf(line, sizeof line, "SELFTEST 0%c %-62s no output", 'b' + pass2, what);
        else { snprintf(line, sizeof line, "SELFTEST 0%c %-62s %s%s", 'b' + pass2, what, bad2 ? "FAIL: " : "PASS", ""); if (bad2) { size_t l = strlen(line); snprintf(line + l, sizeof line - l, "%d of %d pixels wrong", bad2, w2 * h2); } }
        fprintf(stderr, "[slang] %s\n", line); if (sf) fprintf(sf, "%s\n", line);
        free(q2);
      }
    }
  }
  if (orig) ops->texDestroy(dev, orig);
  CtSlChainFree(t);
  return fails;
}

void CtSlChainCaptureRequest(CtSlChain* c) { if (c) { c->captureReq = 1; snprintf(c->capStatus, sizeof c->capStatus, "capturing... (the screen freezes for a moment)"); } }
int CtSlChainCapturePending(const CtSlChain* c) { return c && c->captureReq && c->haveLast && c->ops.texReadback; }
const char* CtSlChainCaptureStatus(const CtSlChain* c) { return c ? c->capStatus : ""; }

// The usual place: logs/slang_<preset>_<time>/ (the program's log folder).
int CtSlChainCaptureAuto(CtSlChain* c) {
  if (!c) return 1;
  const char* b = strrchr(c->preset.path, '/'); b = b ? b + 1 : c->preset.path;
  char nm[96]; size_t o = 0; for (; b[o] && b[o] != '.' && o + 1 < sizeof nm; o++) nm[o] = (isalnum((unsigned char)b[o]) || b[o] == '-' || b[o] == '_') ? b[o] : '_'; nm[o] = 0;
  char dir[300]; mkdir("logs", 0777); snprintf(dir, sizeof dir, "logs/slang_%s_%ld", nm, (long)time(NULL));
  return CtSlChainCapture(c, dir);
}

// Runs the chain once per pass into a private texture, reads each back, writes <dir>/NN_<pass>.png and <dir>/summary.txt, and logs the table. Call it at the START of a frame, before the host
// records anything (the GPU is idle there, and the picture is the last frame's). 0 = done.
int CtSlChainCapture(CtSlChain* c, const char* dir) {
  if (!c) return 1;
  c->captureReq = 0;
  if (!c->haveLast || !c->ops.texReadback) { snprintf(c->capStatus, sizeof c->capStatus, "capture: not available on this backend"); return 1; }
  CtSlFrame f = c->lastFrame;
  mkdir(dir, 0777);
  char path[600]; snprintf(path, sizeof path, "%s/summary.txt", dir);
  FILE* sf = fopen(path, "w");
  char line[400]; int nOk = 0;
  snprintf(line, sizeof line, "slang capture of %s: %d passes, picture %dx%d, output rectangle %dx%d at %d,%d", c->preset.path, c->nReal, f.ow, f.oh, f.dst.w, f.dst.h, f.dst.x, f.dst.y);
  fprintf(stderr, "[slang] %s\n", line); if (sf) fprintf(sf, "%s\n", line);
  snprintf(line, sizeof line, "%-3s %-34s %-11s %-9s %-9s %-14s %s", "#", "pass", "size", "mean", "non-black", "mean R,G,B", "alpha / note");
  fprintf(stderr, "[slang] %s\n", line); if (sf) fprintf(sf, "%s\n", line);
  unsigned prevHash = 0; int havePrev = 0; double prevNonBlack = 0; int variantsDone = 0;
  for (int k = -2; k < c->nReal; k++) {                                            // -2: the picture sampled by a shader (stock pass); -1: the picture read straight from its memory; then every pass
    uint8_t* px = NULL; int w = 0, h = 0; char nm[96] = "input, read directly"; Img cap = { 0 };
    if (k == -1) px = c->ops.texReadback(c->dev, f.original, &w, &h);
    else if (k == -2) {
      snprintf(nm, sizeof nm, "input, sampled by a shader");
      if (c->ops.frameBegin) c->ops.frameBegin(c->dev);
      int rc = execute(c, &f, -2, &cap);
      if (c->ops.frameEnd) c->ops.frameEnd(c->dev);
      if (!rc && cap.tex) px = c->ops.texReadback(c->dev, cap.tex, &w, &h);
      if (cap.tex) c->ops.texDestroy(c->dev, cap.tex);
    }
    else {
      CtSlChainPassInfo(c, k, nm, sizeof nm, NULL, NULL);
      if (c->ops.frameBegin) c->ops.frameBegin(c->dev);
      int rc = execute(c, &f, k, &cap);
      if (c->ops.frameEnd) c->ops.frameEnd(c->dev);
      if (!rc && cap.tex) px = c->ops.texReadback(c->dev, cap.tex, &w, &h);
      if (cap.tex) c->ops.texDestroy(c->dev, cap.tex);
    }
    if (!px) { snprintf(line, sizeof line, "%-3d %-34s could not be read back", k, nm); fprintf(stderr, "[slang] %s\n", line); if (sf) fprintf(sf, "%s\n", line); continue; }
    ImgStats st; imgStats(px, w, h, &st);
    const char* note = !havePrev ? "" : st.hash == prevHash ? "SAME as the one before" : "";
    snprintf(line, sizeof line, "%-3d %-34.34s %4dx%-6d %-9.1f %-8.1f%% %3d,%3d,%3d    alpha %.0f %s%s", k, nm, w, h, st.mean, st.nonBlack, st.r, st.g, st.b, st.alpha, note, st.nonBlack < 0.05 ? "  ALL BLACK" : "");
    fprintf(stderr, "[slang] %s\n", line); if (sf) fprintf(sf, "%s\n", line);
    char fn[96]; safeName(fn, sizeof fn, nm);
    snprintf(path, sizeof path, "%s/%02d_%s.png", dir, k + 2, fn);
    for (size_t i = 0; i < (size_t)w * h; i++) px[4 * i + 3] = 255;                 // (opaque: a viewer would show the colours of transparent pixels as nothing)
    if (CtSlSavePng(path, px, w, h)) nOk++;
    prevHash = st.hash; havePrev = 1;
    free(px);
    if (k >= 1 && !variantsDone && ((st.nonBlack < 0.05 && prevNonBlack > 5.0) || (getenv("CT_SLANG_VARIANTS") && k == atoi(getenv("CT_SLANG_VARIANTS"))))) {            // the first pass that comes out black although its input did not: ask WHY, by running it with one thing changed
      variantsDone = 1;
      static const struct { const char* what; int noSrgb, nearest, srcOrig; } kVar[] = {
        { "target RGBA8 instead of sRGB", 1, 0, 0 }, { "NEAREST filtering of its inputs", 0, 1, 0 }, { "both of those", 1, 1, 0 }, { "reading the ORIGINAL picture instead of the previous pass", 0, 0, 1 } };
      for (size_t v = 0; v < sizeof kVar / sizeof kVar[0]; v++) {
        c->varPass = k; c->varNoSrgb = kVar[v].noSrgb; c->varNearest = kVar[v].nearest; c->varSrcOriginal = kVar[v].srcOrig;
        Img vc = { 0 }; uint8_t* vp = NULL; int vw = 0, vh = 0;
        if (c->ops.frameBegin) c->ops.frameBegin(c->dev);
        int vrc = execute(c, &f, k, &vc);
        if (c->ops.frameEnd) c->ops.frameEnd(c->dev);
        if (!vrc && vc.tex) vp = c->ops.texReadback(c->dev, vc.tex, &vw, &vh);
        if (vc.tex) c->ops.texDestroy(c->dev, vc.tex);
        ImgStats vs2 = { 0 }; if (vp) imgStats(vp, vw, vh, &vs2);
        snprintf(line, sizeof line, "WHAT-IF pass %d (%s): %s -> mean %.1f, non-black %.1f%%", k, nm, kVar[v].what, vs2.mean, vs2.nonBlack);
        fprintf(stderr, "[slang] %s\n", line); if (sf) fprintf(sf, "%s\n", line);
        char vfn[600]; snprintf(vfn, sizeof vfn, "%s/whatif_%02d_%d.png", dir, k, (int)v);
        if (vp) { for (size_t q = 0; q < (size_t)vw * vh; q++) vp[4 * q + 3] = 255; CtSlSavePng(vfn, vp, vw, vh); }
        free(vp);
      }
      c->varPass = -1; c->varNoSrgb = c->varNearest = c->varSrcOriginal = 0;
      if (c->ops.setCompilerOpt && c->ops.wantsGlsl) {                                       // the shader compiler's own optimiser, as the suspect: recompile the pass at lower levels
        void* origGpu = c->pass[k].gpu;
        for (int lvl = 0; lvl <= 2; lvl++) {
          c->ops.setCompilerOpt(lvl);
          char e[512]; void* gp = c->ops.progCreate(c->dev, &c->pass[k].prog, c->pass[k].fmt, e, sizeof e);
          if (!gp) { snprintf(line, sizeof line, "WHAT-IF pass %d: compiler optimisation level %d did not compile: %.120s", k, lvl, e); fprintf(stderr, "[slang] %s\n", line); if (sf) fprintf(sf, "%s\n", line); continue; }
          c->pass[k].gpu = gp;
          Img vc = { 0 }; uint8_t* vp = NULL; int vw = 0, vh = 0;
          if (c->ops.frameBegin) c->ops.frameBegin(c->dev);
          int vrc = execute(c, &f, k, &vc);
          if (c->ops.frameEnd) c->ops.frameEnd(c->dev);
          if (!vrc && vc.tex) vp = c->ops.texReadback(c->dev, vc.tex, &vw, &vh);
          if (vc.tex) c->ops.texDestroy(c->dev, vc.tex);
          c->pass[k].gpu = origGpu; c->ops.progDestroy(c->dev, gp);
          ImgStats v3 = { 0 }; if (vp) imgStats(vp, vw, vh, &v3);
          snprintf(line, sizeof line, "WHAT-IF pass %d (%s): shader compiler optimisation level %d (the app uses 1) -> mean %.1f, non-black %.1f%%%s", k, nm, lvl, v3.mean, v3.nonBlack, v3.nonBlack > 1.0 ? "   <-- RENDERS" : "");
          fprintf(stderr, "[slang] %s\n", line); if (sf) fprintf(sf, "%s\n", line);
          free(vp);
        }
        c->ops.setCompilerOpt(1);
      }
      probeBlocks(c, &f, k, sf);
      probeBlackPass(c, &f, k, sf);
    }
    prevNonBlack = st.nonBlack;
  }
  int stFail = selfTest(&c->ops, c->dev, dir, sf);
  if (sf) fclose(sf);
  fflush(stderr);
  snprintf(c->capStatus, sizeof c->capStatus, "DONE: captured %d images and summary.txt to %s/ (the table is in the log too). Self-test: %s", nOk, dir, stFail ? "SOME CHECKS FAILED (see the log)" : "all passed");
  return 0;
}

int CtSlChainPasses(const CtSlChain* c) { return c ? c->nReal : 0; }
// What pass i is: its alias (or file name), and the size it rendered at last frame (0x0 before the first).
void CtSlChainPassInfo(const CtSlChain* c, int i, char* name, size_t n, int* w, int* h) {
  if (name && n) name[0] = 0;
  if (w) *w = 0;
  if (h) *h = 0;
  if (!c || i < 0 || i >= c->nReal) return;
  if (name && n) { const char* path = c->preset.pass[i].path; const char* b = strrchr(path, '/'); snprintf(name, n, "%s", c->pass[i].name[0] ? c->pass[i].name : b ? b + 1 : path); }
  if (w) *w = c->pass[i].w;
  if (h) *h = c->pass[i].h;
}
const char* CtSlChainPath(const CtSlChain* c) { return c ? c->preset.path : ""; }
int CtSlChainParamCount(const CtSlChain* c) { return c ? c->nParam : 0; }
const CtSlChainParam* CtSlChainParamAt(const CtSlChain* c, int i) { return c && i >= 0 && i < c->nParam ? &c->param[i] : NULL; }
void CtSlChainParamSet(CtSlChain* c, int i, float v) {
  if (!c || i < 0 || i >= c->nParam) return;
  if (v < c->param[i].def.min) v = c->param[i].def.min;
  if (v > c->param[i].def.max) v = c->param[i].def.max;
  c->param[i].value = v;
}
void CtSlChainParamsReset(CtSlChain* c) { if (c) for (int i = 0; i < c->nParam; i++) c->param[i].value = c->paramInit[i]; }
const char* CtSlChainWarnings(const CtSlChain* c) { return c ? c->warn : ""; }
