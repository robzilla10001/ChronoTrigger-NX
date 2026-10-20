// slang_source.c - see slang_source.h.
#include "slang_source.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char* p; size_t n, cap; } Buf;
static void bput(Buf* b, const char* s, size_t n) {
  if (b->n + n + 1 > b->cap) { size_t c = b->cap ? b->cap : 4096; while (c < b->n + n + 1) c *= 2; char* q = (char*)realloc(b->p, c); if (!q) return; b->p = q; b->cap = c; }
  memcpy(b->p + b->n, s, n); b->n += n; b->p[b->n] = 0;
}

typedef struct { Buf vs, fs; int stage; /* 0 = both, 1 = vertex, 2 = fragment */ CtSlSource* out; CtSlReadFn rd; void* ctx; char* err; size_t errN; int firstLine; } Ctx;

static int pragmaParam(CtSlSource* o, const char* s) {
  // #pragma parameter ID "DESCRIPTION" INITIAL MINIMUM MAXIMUM [STEP]
  CtSlParam p; memset(&p, 0, sizeof p);
  while (*s == ' ' || *s == '\t') s++;
  size_t i = 0; while (*s && !isspace((unsigned char)*s) && i < sizeof p.id - 1) p.id[i++] = *s++;
  p.id[i] = 0; while (*s == ' ' || *s == '\t') s++;
  if (*s == '"') { s++; i = 0; while (*s && *s != '"' && i < sizeof p.desc - 1) p.desc[i++] = *s++; p.desc[i] = 0; while (*s && *s != '"') s++; if (*s == '"') s++; }
  char* e; p.init = strtof(s, &e); if (e == s) return 0; s = e;
  p.min = strtof(s, &e); if (e == s) return 0; s = e;
  p.max = strtof(s, &e); if (e == s) return 0; s = e;
  p.step = strtof(s, &e); if (e == s) p.step = (p.max - p.min) / 100.0f;
  for (int k = 0; k < o->nParams; k++) if (!strcmp(o->param[k].id, p.id)) return 1;       // (an included file may name it too)
  if (o->nParams == o->capParams) { int nc = o->capParams ? o->capParams * 2 : 64; CtSlParam* q = (CtSlParam*)realloc(o->param, (size_t)nc * sizeof(CtSlParam)); if (!q) return 0; o->param = q; o->capParams = nc; }
  o->param[o->nParams++] = p;
  return 1;
}

static int process(Ctx* c, const char* path, int depth) {
  if (depth > 16) { snprintf(c->err, c->errN, "#include nested too deep at %s", path); return 1; }
  size_t len = 0; char* text = c->rd(path, &len, c->ctx);
  if (!text) { snprintf(c->err, c->errN, "cannot read %s", path); return 1; }
  char* line = text;
  while (line && *line) {
    char* nl = strchr(line, '\n'); char* next = NULL; size_t ll;
    if (nl) { ll = (size_t)(nl - line); next = nl + 1; } else ll = strlen(line);
    if (ll && line[ll - 1] == '\r') ll--;
    char tmp[2048]; size_t tl = ll < sizeof tmp - 1 ? ll : sizeof tmp - 1; memcpy(tmp, line, tl); tmp[tl] = 0;
    const char* s = tmp; while (*s == ' ' || *s == '\t') s++;
    int emit = 1, blank = 0;
    if (!strncmp(s, "#include", 8) && (s[8] == ' ' || s[8] == '\t' || s[8] == '"')) {
      const char* q = strchr(s, '"');
      if (q) {
        char inc[CT_SL_PATH]; size_t r = 0; q++; while (*q && *q != '"' && r < sizeof inc - 1) inc[r++] = *q++; inc[r] = 0;
        char full[CT_SL_PATH]; CtSlJoinPath(full, sizeof full, path, inc);
        if (process(c, full, depth + 1)) {
          if (!strncmp(c->err, "cannot read ", 12)) { char t[CT_SL_PATH + 16]; snprintf(t, sizeof t, "%s", c->err); snprintf(c->err, c->errN, "%s (an #include of %s: copy the folders the preset's shaders refer to, e.g. the library's include/)", t, path); }
          free(text); return 1;
        }
        emit = 0;
      }
    } else if (!strncmp(s, "#pragma", 7) && (s[7] == ' ' || s[7] == '\t')) {
      const char* a = s + 7; while (*a == ' ' || *a == '\t') a++;
      if (!strncmp(a, "stage", 5)) {
        const char* w = a + 5; while (*w == ' ' || *w == '\t') w++;
        if (!strncmp(w, "vertex", 6)) { c->stage = 1; c->out->hasVertex = 1; blank = 1; }
        else if (!strncmp(w, "fragment", 8)) { c->stage = 2; c->out->hasFragment = 1; blank = 1; }
      } else if (!strncmp(a, "name", 4) && isspace((unsigned char)a[4])) {
        const char* w = a + 4; while (*w == ' ' || *w == '\t') w++;
        size_t i = 0; while (*w && !isspace((unsigned char)*w) && i < sizeof c->out->name - 1) c->out->name[i++] = *w++;
        c->out->name[i] = 0; blank = 1;
      } else if (!strncmp(a, "format", 6) && isspace((unsigned char)a[6])) {
        const char* w = a + 6; while (*w == ' ' || *w == '\t') w++;
        char f[64]; size_t i = 0; while (*w && !isspace((unsigned char)*w) && i < sizeof f - 1) f[i++] = *w++;
        f[i] = 0; c->out->format = (int)CtSlFormatFromName(f); blank = 1;
      } else if (!strncmp(a, "parameter", 9) && isspace((unsigned char)a[9])) {
        pragmaParam(c->out, a + 9); blank = 1;
      }
    }
    if (emit) {
      // a line goes to the active stage(s); the stage that does not get it gets an empty line, so the line numbers stay the file's (an include's lines shift them: that is the one drift)
      int toV = c->stage != 2, toF = c->stage != 1;
      if (blank) toV = toF = 0;
      bput(&c->vs, toV ? tmp : "", toV ? tl : 0); bput(&c->vs, "\n", 1);
      bput(&c->fs, toF ? tmp : "", toF ? tl : 0); bput(&c->fs, "\n", 1);
    }
    line = next;
  }
  free(text);
  return 0;
}

int CtSlSourceLoad(CtSlSource* s, const char* path, CtSlReadFn rd, void* rdCtx, char* err, size_t errN) {
  memset(s, 0, sizeof *s); s->format = -1;
  char e0[4]; if (!err) { err = e0; errN = sizeof e0; }
  err[0] = 0;
  Ctx c; memset(&c, 0, sizeof c); c.out = s; c.rd = rd ? rd : CtSlReadFile; c.ctx = rdCtx; c.err = err; c.errN = errN;
  if (process(&c, path, 0)) { free(c.vs.p); free(c.fs.p); free(s->param); s->param = NULL; s->nParams = s->capParams = 0; return 1; }
  s->vs = c.vs.p ? c.vs.p : strdup(""); s->fs = c.fs.p ? c.fs.p : strdup("");
  // the very first line has to be #version (the spec); the compiler would say so, but not by which file
  const char* v = s->fs; while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r') v++;
  if (strncmp(v, "#version", 8)) { snprintf(err, errN, "%s: the first line must be #version", path); CtSlSourceFree(s); return 1; }
  return 0;
}
void CtSlSourceFree(CtSlSource* s) { if (!s) return; free(s->vs); free(s->fs); free(s->param); s->vs = s->fs = NULL; s->param = NULL; s->nParams = s->capParams = 0; }
