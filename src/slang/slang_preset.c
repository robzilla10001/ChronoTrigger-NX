// slang_preset.c - the .slangp reader (slang_preset.h).
#include "slang_preset.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char key[96]; char val[CT_SL_PATH]; } Kv;
typedef struct { Kv* kv; int n, cap; } KvList;

static const struct { const char* n; CtSlFormat f; } kFormats[] = {
  { "R8G8B8A8_UNORM", CT_SL_FMT_RGBA8 }, { "R8G8B8A8_SRGB", CT_SL_FMT_RGBA8_SRGB }, { "R16G16B16A16_SFLOAT", CT_SL_FMT_RGBA16F }, { "R32G32B32A32_SFLOAT", CT_SL_FMT_RGBA32F },
  { "R8_UNORM", CT_SL_FMT_R8 }, { "R8G8_UNORM", CT_SL_FMT_RG8 }, { "R16_SFLOAT", CT_SL_FMT_R16F }, { "R16G16_SFLOAT", CT_SL_FMT_RG16F },
  { "R32_SFLOAT", CT_SL_FMT_R32F }, { "R32G32_SFLOAT", CT_SL_FMT_RG32F }, { "A2B10G10R10_UNORM_PACK32", CT_SL_FMT_A2B10G10R10 },
  { "R8G8B8A8_UINT", CT_SL_FMT_RGBA8_UINT }, { "R8G8B8A8_SINT", CT_SL_FMT_RGBA8_SINT }, { "R8_UINT", CT_SL_FMT_R8_UINT }, { "R8G8_UINT", CT_SL_FMT_RG8_UINT },
  { "R16_UINT", CT_SL_FMT_R16_UINT }, { "R16G16_UINT", CT_SL_FMT_RG16_UINT }, { "R16G16B16A16_UINT", CT_SL_FMT_RGBA16_UINT },
  { "R32_UINT", CT_SL_FMT_R32_UINT }, { "R32G32_UINT", CT_SL_FMT_RG32_UINT }, { "R32G32B32A32_UINT", CT_SL_FMT_RGBA32_UINT },
};
CtSlFormat CtSlFormatFromName(const char* name) {
  for (size_t i = 0; i < sizeof kFormats / sizeof kFormats[0]; i++) if (!strcmp(kFormats[i].n, name)) return kFormats[i].f;
  return (CtSlFormat)-1;
}
const char* CtSlFormatName(CtSlFormat f) {
  for (size_t i = 0; i < sizeof kFormats / sizeof kFormats[0]; i++) if (kFormats[i].f == f) return kFormats[i].n;
  return "R8G8B8A8_UNORM";
}

char* CtSlReadFile(const char* path, size_t* len, void* ctx) {
  (void)ctx;
  FILE* f = fopen(path, "rb"); if (!f) return NULL;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  if (n < 0) { fclose(f); return NULL; }
  char* b = (char*)malloc((size_t)n + 1);
  if (!b) { fclose(f); return NULL; }
  size_t got = fread(b, 1, (size_t)n, f); fclose(f);
  b[got] = 0; if (len) *len = got;
  return b;
}

void CtSlJoinPath(char* out, size_t n, const char* base, const char* rel) {
  char tmp[CT_SL_PATH * 2];
  if (rel[0] == '/' || (rel[0] && rel[1] == ':') || !strncmp(rel, "sdmc:", 5)) snprintf(tmp, sizeof tmp, "%s", rel);
  else {
    const char* s1 = strrchr(base, '/'); const char* s2 = strrchr(base, '\\'); const char* sl = s1 > s2 ? s1 : s2;
    size_t dl = sl ? (size_t)(sl - base) + 1 : 0;
    snprintf(tmp, sizeof tmp, "%.*s%s", (int)dl, base, rel);
  }
  for (char* c = tmp; *c; c++) if (*c == '\\') *c = '/';
  // normalise "a/./b" and "a/../b"
  char* seg[128]; int ns = 0; char* save = tmp; char root[8] = ""; size_t rl = 0;
  if (tmp[0] == '/') { root[0] = '/'; root[1] = 0; rl = 1; save = tmp + 1; }
  else { const char* colon = strchr(tmp, ':'); if (colon && colon - tmp < 8 && colon[1] == '/') { rl = (size_t)(colon - tmp) + 2; memcpy(root, tmp, rl); root[rl] = 0; save = tmp + rl; } }
  for (char* t = save; t && *t;) {
    char* e = strchr(t, '/'); if (e) *e = 0;
    if (!strcmp(t, "..")) { if (ns > 0 && strcmp(seg[ns - 1], "..")) ns--; else if (!rl && ns < 128) seg[ns++] = t; }
    else if (strcmp(t, ".") && *t && ns < 128) seg[ns++] = t;
    t = e ? e + 1 : NULL;
  }
  size_t o = 0; o += (size_t)snprintf(out + o, n - o, "%s", root);
  for (int i = 0; i < ns && o < n; i++) o += (size_t)snprintf(out + o, n - o, "%s%s", i ? "/" : "", seg[i]);
  if (o >= n) out[n - 1] = 0;
}

static void kvSet(KvList* l, const char* k, const char* v) {
  for (int i = 0; i < l->n; i++) if (!strcmp(l->kv[i].key, k)) { snprintf(l->kv[i].val, sizeof l->kv[i].val, "%s", v); return; }
  if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 64; l->kv = (Kv*)realloc(l->kv, (size_t)l->cap * sizeof(Kv)); if (!l->kv) { l->n = l->cap = 0; return; } }
  snprintf(l->kv[l->n].key, sizeof l->kv[l->n].key, "%s", k); snprintf(l->kv[l->n].val, sizeof l->kv[l->n].val, "%s", v); l->n++;
}
static const char* kvGet(const KvList* l, const char* k) { for (int i = 0; i < l->n; i++) if (!strcmp(l->kv[i].key, k)) return l->kv[i].val; return NULL; }
static const char* kvGetf(const KvList* l, const char* fmt, const char* a, int i) {
  char k[160]; if (a) snprintf(k, sizeof k, fmt, a, i); else snprintf(k, sizeof k, fmt, i);
  return kvGet(l, k);
}

static int truthy(const char* v) { return v && (!strcmp(v, "true") || !strcmp(v, "True") || !strcmp(v, "TRUE") || !strcmp(v, "1")); }

// Parse `text` (a preset) into the list; #reference first (the referenced preset's keys, then this file's override them).
static int parseInto(KvList* l, const char* path, CtSlReadFn rd, void* ctx, int depth, char* err, size_t errN) {
  if (depth > 8) { snprintf(err, errN, "#reference nested too deep at %s", path); return 1; }
  size_t len = 0; char* text = rd(path, &len, ctx);
  if (!text) { snprintf(err, errN, "cannot read %s", path); return 1; }
  char* line = text; int rc = 0;
  while (line && *line && !rc) {
    char* nl = strpbrk(line, "\r\n"); char* next = NULL;
    if (nl) { next = nl + 1; while (*next == '\r' || *next == '\n') next++; *nl = 0; }
    char* s = line; while (*s == ' ' || *s == '\t') s++;
    if (!strncmp(s, "#reference", 10)) {
      char* q = s + 10; while (*q == ' ' || *q == '\t') q++;
      char ref[CT_SL_PATH]; size_t r = 0;
      if (*q == '"') { q++; while (*q && *q != '"' && r < sizeof ref - 1) ref[r++] = *q++; } else while (*q && !isspace((unsigned char)*q) && r < sizeof ref - 1) ref[r++] = *q++;
      ref[r] = 0;
      char full[CT_SL_PATH]; CtSlJoinPath(full, sizeof full, path, ref);
      rc = parseInto(l, full, rd, ctx, depth + 1, err, errN);
    } else if (*s && *s != '#' && *s != ';') {
      char* eq = strchr(s, '=');
      if (eq) {
        char* ke = eq; while (ke > s && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
        char key[96]; size_t kl = (size_t)(ke - s); if (kl >= sizeof key) kl = sizeof key - 1; memcpy(key, s, kl); key[kl] = 0;
        char* v = eq + 1; while (*v == ' ' || *v == '\t') v++;
        char val[CT_SL_PATH]; size_t vl = 0;
        if (*v == '"') { v++; while (*v && *v != '"' && vl < sizeof val - 1) val[vl++] = *v++; }
        else { while (*v && vl < sizeof val - 1) val[vl++] = *v++; while (vl && (val[vl - 1] == ' ' || val[vl - 1] == '\t')) vl--; const char* hash = strstr(val, " #"); if (hash) vl = (size_t)(hash - val); }
        val[vl] = 0;
        // an entry's path is relative to the file it is in, which may be a #reference'd one: store it already joined
        int isPath = (!strncmp(key, "shader", 6) && isdigit((unsigned char)key[6]));
        if (isPath) { char full[CT_SL_PATH]; CtSlJoinPath(full, sizeof full, path, val); kvSet(l, key, full); }
        else if (!strcmp(key, "textures")) kvSet(l, key, val);
        else kvSet(l, key, val);
        // a LUT's file is a path too, but which keys are LUTs is only known once `textures` is: remember the folder with the key
        char fk[110]; snprintf(fk, sizeof fk, "%s#dir", key); kvSet(l, fk, path);
      }
    }
    line = next;
  }
  free(text);
  return rc;
}

static int scaleTypeOf(const char* v, int dflt) {
  if (!v) return dflt;
  if (!strcmp(v, "source")) return CT_SL_SCALE_SOURCE;
  if (!strcmp(v, "viewport")) return CT_SL_SCALE_VIEWPORT;
  if (!strcmp(v, "absolute")) return CT_SL_SCALE_ABSOLUTE;
  return dflt;
}
static int wrapOf(const char* v) {
  if (!v) return CT_SL_WRAP_BORDER;
  if (!strcmp(v, "clamp_to_edge")) return CT_SL_WRAP_EDGE;
  if (!strcmp(v, "repeat")) return CT_SL_WRAP_REPEAT;
  if (!strcmp(v, "mirrored_repeat")) return CT_SL_WRAP_MIRROR;
  return CT_SL_WRAP_BORDER;                                  // "clamp_to_border" and the default
}

int CtSlPresetLoad(CtSlPreset* p, const char* path, CtSlReadFn rd, void* ctx, char* err, size_t errN) {
  memset(p, 0, sizeof *p);
  if (!rd) rd = CtSlReadFile;
  char e0[8]; if (!err) { err = e0; errN = sizeof e0; }
  err[0] = 0;
  snprintf(p->path, sizeof p->path, "%s", path);
  KvList l = { 0 };
  if (parseInto(&l, path, rd, ctx, 0, err, errN)) { free(l.kv); return 1; }
  const char* sn = kvGet(&l, "shaders");
  int n = sn ? atoi(sn) : 0;
  if (n < 1 || n > CT_SL_MAX_PASSES) { snprintf(err, errN, "%s: shaders = %d (1..%d expected)", path, n, CT_SL_MAX_PASSES); free(l.kv); return 1; }
  p->nPasses = n;
  for (int i = 0; i < n; i++) {
    CtSlPassCfg* c = &p->pass[i];
    const char* sh = kvGetf(&l, "shader%d", NULL, i);
    if (!sh || !*sh) { snprintf(err, errN, "%s: shader%d is missing", path, i); free(l.kv); return 1; }
    snprintf(c->path, sizeof c->path, "%s", sh);
    const char* v;
    if ((v = kvGetf(&l, "alias%d", NULL, i))) snprintf(c->alias, sizeof c->alias, "%s", v);
    v = kvGetf(&l, "filter_linear%d", NULL, i); c->filter = v ? (truthy(v) ? CT_SL_FILTER_LINEAR : CT_SL_FILTER_NEAREST) : CT_SL_FILTER_UNSPEC;
    c->wrap = wrapOf(kvGetf(&l, "wrap_mode%d", NULL, i));
    c->mipmap = truthy(kvGetf(&l, "mipmap_input%d", NULL, i));
    c->floatFb = truthy(kvGetf(&l, "float_framebuffer%d", NULL, i));
    c->srgbFb = truthy(kvGetf(&l, "srgb_framebuffer%d", NULL, i));
    if ((v = kvGetf(&l, "frame_count_mod%d", NULL, i))) c->frameCountMod = (unsigned)atoi(v);
    const char* st = kvGetf(&l, "scale_type%d", NULL, i), *sx = kvGetf(&l, "scale_type_x%d", NULL, i), *sy = kvGetf(&l, "scale_type_y%d", NULL, i);
    const char* f = kvGetf(&l, "scale%d", NULL, i), *fx = kvGetf(&l, "scale_x%d", NULL, i), *fy = kvGetf(&l, "scale_y%d", NULL, i);
    c->hasScale = st || sx || sy || f || fx || fy;
    int dflt = (i == n - 1 && !c->hasScale) ? CT_SL_SCALE_VIEWPORT : CT_SL_SCALE_SOURCE;
    c->scaleTypeX = scaleTypeOf(sx ? sx : st, dflt); c->scaleTypeY = scaleTypeOf(sy ? sy : st, dflt);
    c->scaleX = fx ? (float)atof(fx) : f ? (float)atof(f) : 1.0f; c->scaleY = fy ? (float)atof(fy) : f ? (float)atof(f) : 1.0f;
  }
  const char* tx = kvGet(&l, "textures");
  if (tx && *tx) {
    char names[1024]; snprintf(names, sizeof names, "%s", tx);
    for (char* t = strtok(names, ";"); t && p->nLuts < CT_SL_MAX_LUTS; t = strtok(NULL, ";")) {
      while (*t == ' ') t++;
      const char* fv = kvGet(&l, t);
      if (!fv) continue;
      CtSlLutCfg* c = &p->lut[p->nLuts];
      snprintf(c->name, sizeof c->name, "%s", t);
      char dk[110], dir[CT_SL_PATH]; snprintf(dk, sizeof dk, "%s#dir", t);
      const char* from = kvGet(&l, dk); snprintf(dir, sizeof dir, "%s", from ? from : path);
      CtSlJoinPath(c->path, sizeof c->path, dir, fv);
      char k[160];
      snprintf(k, sizeof k, "%s_linear", t); const char* v = kvGet(&l, k); c->filter = v ? (truthy(v) ? CT_SL_FILTER_LINEAR : CT_SL_FILTER_NEAREST) : CT_SL_FILTER_LINEAR;
      snprintf(k, sizeof k, "%s_wrap_mode", t); c->wrap = wrapOf(kvGet(&l, k));
      snprintf(k, sizeof k, "%s_mipmap", t); c->mipmap = truthy(kvGet(&l, k));
      p->nLuts++;
    }
  }
  const char* pr = kvGet(&l, "parameters");
  if (pr && *pr) {
    char names[2048]; snprintf(names, sizeof names, "%s", pr);
    for (char* t = strtok(names, ";"); t && p->nParams < CT_SL_MAX_PARAMS; t = strtok(NULL, ";")) {
      while (*t == ' ') t++;
      const char* v = kvGet(&l, t);
      if (!v) continue;
      snprintf(p->param[p->nParams].name, CT_SL_NAME, "%s", t); p->param[p->nParams].value = (float)atof(v); p->nParams++;
    }
  }
  free(l.kv);
  return 0;
}
