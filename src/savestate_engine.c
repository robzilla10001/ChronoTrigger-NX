// savestate_engine.c - see savestate_engine.h.
#include "savestate_engine.h"
#include <stdlib.h>
#include <string.h>

#define WRAM_BYTES 0x20000

static const char* hostUnavailable(void* c) { CtSsHost* h = (CtSsHost*)c; return h->unavailable; }

// ---- LOCN: where the state was taken. A load first has the host go there (a door's worth of load, with no fade) so the rest lands on the right scene ----
static int locnSave(void* c, CtSsBuf* o) {
  CtSsHost* h = (CtSsHost*)c;
  CtSsPutU8(o, (uint32_t)h->mode); CtSsPutU32(o, (uint32_t)h->scene); CtSsPutU32(o, h->vm ? (uint32_t)CtSceneSnapshotSize() : 0);
  return 0;
}
static int locnCheck(void* c, const uint8_t* d, size_t n, uint16_t v) { (void)c; (void)d; (void)v; return n != 1 + 4 + 4; }
static int locnLoad(void* c, const uint8_t* d, size_t n, uint16_t v) {
  (void)v; CtSsHost* h = (CtSsHost*)c; CtSsReader r = CtSsReaderOf(d, n);
  int mode = (int)CtSsGetU8(&r), scene = (int)CtSsGetU32(&r);
  if (r.err) return 1;
  return h->enter ? h->enter(h->enterCtx, mode, scene) : (mode == h->mode && scene == h->scene ? 0 : 1);     // (no host hook: only where it already is)
}
// ---- HOST ----
static int hostSave(void* c, CtSsBuf* o) {
  CtSsHost* h = (CtSsHost*)c;
  CtSsPutU32(o, (uint32_t)*h->frame);
  CtSsPutF64(o, *h->px); CtSsPutF64(o, *h->py); CtSsPutU32(o, (uint32_t)*h->pz); CtSsPutU32(o, (uint32_t)*h->pfac);
  CtSsPutU16(o, *h->padPrev); CtSsPutU16(o, *h->padAuto);
  return 0;
}
static int hostCheck(void* c, const uint8_t* d, size_t n, uint16_t v) { (void)c; (void)d; (void)v; return n != 4 + 8 + 8 + 4 + 4 + 2 + 2; }
static int hostLoad(void* c, const uint8_t* d, size_t n, uint16_t v) {
  (void)v; CtSsHost* h = (CtSsHost*)c; CtSsReader r = CtSsReaderOf(d, n);
  int f = (int)CtSsGetU32(&r); double x = CtSsGetF64(&r), y = CtSsGetF64(&r); int z = (int)CtSsGetU32(&r), fa = (int)CtSsGetU32(&r);
  uint16_t pp = (uint16_t)CtSsGetU16(&r), pa = (uint16_t)CtSsGetU16(&r);
  if (r.err) return 1;
  *h->frame = f; *h->px = x; *h->py = y; *h->pz = z; *h->pfac = fa; *h->padPrev = pp; *h->padAuto = pa;
  return 0;
}
// ---- WRAM ----
static int wramSave(void* c, CtSsBuf* o) { CtSsHost* h = (CtSsHost*)c; CtSsPut(o, h->wram, WRAM_BYTES); return 0; }
static int wramCheck(void* c, const uint8_t* d, size_t n, uint16_t v) { (void)c; (void)d; (void)v; return n != WRAM_BYTES; }
static int wramLoad(void* c, const uint8_t* d, size_t n, uint16_t v) { (void)v; CtSsHost* h = (CtSsHost*)c; if (n != WRAM_BYTES) return 1; memcpy(h->wram, d, n); return 0; }
// ---- SRAM ----
static int sramSave(void* c, CtSsBuf* o) { CtSsHost* h = (CtSsHost*)c; CtSsPut(o, h->sram, h->sramLen); return 0; }
static int sramCheck(void* c, const uint8_t* d, size_t n, uint16_t v) { (void)d; (void)v; return n != ((CtSsHost*)c)->sramLen; }
static int sramLoad(void* c, const uint8_t* d, size_t n, uint16_t v) { (void)v; CtSsHost* h = (CtSsHost*)c; if (n != h->sramLen) return 1; memcpy(h->sram, d, n); return 0; }
// ---- SCNE ----
static int sceneSave(void* c, CtSsBuf* o) {
  CtSsHost* h = (CtSsHost*)c; if (!h->vm) return 0;                       // (the overworld has no scene: an empty part)
  size_t n = CtSceneSnapshotSize(); uint8_t* t = (uint8_t*)malloc(n); if (!t) return 1;
  int ok = CtSceneSnapshot(h->vm, t); if (ok) CtSsPut(o, t, n);
  free(t); return !ok;
}
static int sceneCheck(void* c, const uint8_t* d, size_t n, uint16_t v) { (void)c; (void)d; (void)v; return n != 0 && n != CtSceneSnapshotSize(); }     // (that it is the right scene is the LOCN part's: the load meets the scene it switched to)
static int sceneLoad(void* c, const uint8_t* d, size_t n, uint16_t v) { (void)v; CtSsHost* h = (CtSsHost*)c; if (!n) return 0; return !h->vm || !CtSceneRestoreSnapshot(h->vm, d, n); }
// ---- AUDI (an empty chunk when the run has no sound) ----
static int audioSave(void* c, CtSsBuf* o) {
  CtSsHost* h = (CtSsHost*)c; if (!h->audio) return 0;
  uint8_t* b; int n; if (!CtAudioStateSave(h->audio, &b, &n)) return 0;       // (no ready SPC: an empty part)
  CtSsPut(o, b, (size_t)n); free(b); return 0;
}
static int audioCheck(void* c, const uint8_t* d, size_t n, uint16_t v) { (void)v; CtSsHost* h = (CtSsHost*)c; return n && h->audio ? !CtAudioStateFits(h->audio, d, (int)n) : 0; }
static int audioLoad(void* c, const uint8_t* d, size_t n, uint16_t v) { (void)v; CtSsHost* h = (CtSsHost*)c; if (!n || !h->audio) return 0; return !CtAudioStateLoad(h->audio, d, (int)n, h->wram); }

// ---- HVAR: the host loop's registered variables, by name ----
#define SS_MAXVARS 1024
static struct { const char* name; void* p; size_t n; } g_vars[SS_MAXVARS]; static int g_nvars;
void CtSsVarAdd(const char* name, void* p, size_t n) {
  for (int i = 0; i < g_nvars; i++) if (!strcmp(g_vars[i].name, name)) { g_vars[i].p = p; g_vars[i].n = n; return; }     // (re-registered: the new address)
  if (g_nvars < SS_MAXVARS && name && p && n) { g_vars[g_nvars].name = name; g_vars[g_nvars].p = p; g_vars[g_nvars].n = n; g_nvars++; }
}
static int varSave(void* c, CtSsBuf* o) {
  (void)c; CtSsPutU32(o, (uint32_t)g_nvars);
  for (int i = 0; i < g_nvars; i++) { size_t l = strlen(g_vars[i].name); CtSsPutU8(o, (uint32_t)l); CtSsPut(o, g_vars[i].name, l); CtSsPutU32(o, (uint32_t)g_vars[i].n); CtSsPut(o, g_vars[i].p, g_vars[i].n); }
  return 0;
}
static int varWalk(const uint8_t* d, size_t n, int apply) {                                // 0 = ok; apply = copy the bytes in
  CtSsReader r = CtSsReaderOf(d, n); uint32_t cnt = CtSsGetU32(&r);
  for (uint32_t i = 0; i < cnt && !r.err; i++) {
    uint32_t l = CtSsGetU8(&r); const uint8_t* nm = CtSsGet(&r, l); uint32_t sz = CtSsGetU32(&r); const uint8_t* data = CtSsGet(&r, sz);
    if (r.err) break;
    for (int k = 0; k < g_nvars; k++)
      if (strlen(g_vars[k].name) == l && !memcmp(g_vars[k].name, nm, l)) { if (g_vars[k].n != sz) return 1; if (apply) memcpy(g_vars[k].p, data, sz); break; }
  }
  return r.err;
}
static int varCheck(void* c, const uint8_t* d, size_t n, uint16_t v) { (void)c; (void)v; return varWalk(d, n, 0); }
static int varLoad(void* c, const uint8_t* d, size_t n, uint16_t v) { (void)c; (void)v; return varWalk(d, n, 1); }

static void metaFn(void* c, CtSsMeta* m) {
  CtSsHost* h = (CtSsHost*)c; static uint8_t th[CT_SS_THUMB_W * CT_SS_THUMB_H * 3];
  m->frame = (uint32_t)*h->frame; m->loc = (uint16_t)h->loc; m->mode = (uint8_t)h->mode;
  if (h->fb && h->fbW >= CT_SS_THUMB_W && h->fbH >= CT_SS_THUMB_H) {          // a box average of the picture, down to the slot's thumbnail
    int bx = h->fbW / CT_SS_THUMB_W, by = h->fbH / CT_SS_THUMB_H;
    for (int y = 0; y < CT_SS_THUMB_H; y++) for (int x = 0; x < CT_SS_THUMB_W; x++) for (int k = 0; k < 3; k++) {
      unsigned sum = 0;
      for (int j = 0; j < by; j++) for (int i = 0; i < bx; i++) sum += h->fb[((size_t)(y * by + j) * h->fbW + (size_t)(x * bx + i)) * 3 + k];
      th[((size_t)y * CT_SS_THUMB_W + x) * 3 + k] = (uint8_t)(sum / (unsigned)(bx * by));
    }
    m->thumbW = CT_SS_THUMB_W; m->thumbH = CT_SS_THUMB_H; m->thumb = th;
  }
}

void CtSsEngineBind(CtSs* s, CtSsHost* h) {
  CtSsProvider locn = { "LOCN", 1, 1, 1, NULL, locnSave, locnCheck, locnLoad };
  CtSsProvider host = { "HOST", 1, 1, 1, hostUnavailable, hostSave, hostCheck, hostLoad };
  CtSsProvider wram = { "WRAM", 1, 1, 1, NULL, wramSave, wramCheck, wramLoad };
  CtSsProvider sram = { "SRAM", 1, 1, 1, NULL, sramSave, sramCheck, sramLoad };
  CtSsProvider scne = { "SCNE", 1, 1, 1, NULL, sceneSave, sceneCheck, sceneLoad };
  CtSsProvider hvar = { "HVAR", 1, 1, 0, NULL, varSave, varCheck, varLoad };
  CtSsProvider audi = { "AUDI", 1, 1, 0, NULL, audioSave, audioCheck, audioLoad };
  CtSsRegister(s, &locn, h); CtSsRegister(s, &host, h); CtSsRegister(s, &wram, h); CtSsRegister(s, &sram, h); CtSsRegister(s, &scne, h); CtSsRegister(s, &hvar, h); CtSsRegister(s, &audi, h);
  CtSsSetMetaFn(s, metaFn, h);
}
