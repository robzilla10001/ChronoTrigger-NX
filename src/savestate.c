// savestate.c - see savestate.h.
#include "savestate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>

#define SS_MAX_PROV 32
#define SS_MAGIC "CTNX"

struct CtSs {
  CtSsProvider prov[SS_MAX_PROV]; void* ctx[SS_MAX_PROV]; int n;
  CtSsMetaFn metaFn; void* metaCtx;
  char dir[256]; int backup;
  int reqKind, reqSlot;
  char msg[160]; int msgOk; unsigned msgSerial;
};

// ---- CRC-32 (IEEE) ----
uint32_t CtSsCrc32(const uint8_t* d, size_t n) {
  static uint32_t T[256]; static int init;
  if (!init) { for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; T[i] = c; } init = 1; }
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) c = T[(c ^ d[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

// ---- buffers ----
void CtSsPut(CtSsBuf* b, const void* d, size_t n) {
  if (b->err || !n) return;
  if (b->len + n > b->cap) {
    size_t c = b->cap ? b->cap : 4096; while (c < b->len + n) c *= 2;
    uint8_t* q = (uint8_t*)realloc(b->p, c);
    if (!q) { b->err = 1; return; }
    b->p = q; b->cap = c;
  }
  memcpy(b->p + b->len, d, n); b->len += n;
}
void CtSsPutU8(CtSsBuf* b, uint32_t v)  { uint8_t t[1] = { (uint8_t)v }; CtSsPut(b, t, 1); }
void CtSsPutU16(CtSsBuf* b, uint32_t v) { uint8_t t[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; CtSsPut(b, t, 2); }
void CtSsPutU32(CtSsBuf* b, uint32_t v) { uint8_t t[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }; CtSsPut(b, t, 4); }
void CtSsPutF64(CtSsBuf* b, double v)   { uint8_t t[8]; memcpy(t, &v, 8); CtSsPut(b, t, 8); }   // (IEEE doubles: the same on every target here)
CtSsReader CtSsReaderOf(const uint8_t* d, size_t n) { CtSsReader r = { d, n, 0, 0 }; return r; }
const uint8_t* CtSsGet(CtSsReader* r, size_t n) {
  if (r->err || n > r->len - r->pos) { r->err = 1; return NULL; }
  const uint8_t* q = r->p + r->pos; r->pos += n; return q;
}
uint32_t CtSsGetU8(CtSsReader* r)  { const uint8_t* q = CtSsGet(r, 1); return q ? q[0] : 0; }
uint32_t CtSsGetU16(CtSsReader* r) { const uint8_t* q = CtSsGet(r, 2); return q ? (uint32_t)(q[0] | q[1] << 8) : 0; }
uint32_t CtSsGetU32(CtSsReader* r) { const uint8_t* q = CtSsGet(r, 4); return q ? (uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24 : 0; }
double   CtSsGetF64(CtSsReader* r) { const uint8_t* q = CtSsGet(r, 8); double v = 0; if (q) memcpy(&v, q, 8); return v; }

// ---- instances ----
CtSs* CtSsNew(void) { CtSs* s = (CtSs*)calloc(1, sizeof *s); if (s) strcpy(s->dir, "states"); return s; }
void CtSsFree(CtSs* s) { free(s); }
CtSs* CtSsDefault(void) { static CtSs* d; if (!d) d = CtSsNew(); return d; }
int CtSsRegister(CtSs* s, const CtSsProvider* p, void* ctx) {
  if (!s || !p || s->n >= SS_MAX_PROV || strlen(p->tag) != 4 || !p->save || !p->load) return 1;
  for (int i = 0; i < s->n; i++) if (!memcmp(s->prov[i].tag, p->tag, 4)) return 1;
  s->prov[s->n] = *p; s->ctx[s->n] = ctx; s->n++; return 0;
}
void CtSsSetMetaFn(CtSs* s, CtSsMetaFn fn, void* ctx) { s->metaFn = fn; s->metaCtx = ctx; }
void CtSsSetBackup(CtSs* s, int on) { s->backup = on != 0; }
void CtSsSetDir(CtSs* s, const char* dir) { snprintf(s->dir, sizeof s->dir, "%s", dir && *dir ? dir : "states"); }
static void say(CtSs* s, int ok, const char* fmt, const char* a) { snprintf(s->msg, sizeof s->msg, fmt, a ? a : ""); s->msgOk = ok; s->msgSerial++; }

// ---- save ----
void CtSsFreeImage(uint8_t* img) { free(img); }
int CtSsSaveImage(CtSs* s, uint8_t** out, size_t* len, const char** why) {
  static const char* kOom = "out of memory";
  *out = NULL; *len = 0;
  for (int i = 0; i < s->n; i++)
    if (s->prov[i].unavailable) { const char* r = s->prov[i].unavailable(s->ctx[i]); if (r) { if (why) *why = r; return 1; } }
  CtSsMeta m; memset(&m, 0, sizeof m); m.unixTime = (uint32_t)time(NULL);
  if (s->metaFn) s->metaFn(s->metaCtx, &m);
  size_t thumbBytes = m.thumb ? (size_t)m.thumbW * m.thumbH * 3 : 0;
  CtSsBuf b = { 0 };
  CtSsPut(&b, SS_MAGIC, 4); CtSsPutU16(&b, CT_SS_FMT); CtSsPutU16(&b, (uint32_t)s->n);
  CtSsPutU32(&b, m.unixTime); CtSsPutU32(&b, m.frame); CtSsPutU16(&b, m.loc); CtSsPutU8(&b, m.mode); CtSsPutU8(&b, 0);
  CtSsPutU8(&b, thumbBytes ? m.thumbW : 0); CtSsPutU8(&b, thumbBytes ? m.thumbH : 0); CtSsPutU32(&b, (uint32_t)thumbBytes);
  if (thumbBytes) CtSsPut(&b, m.thumb, thumbBytes);
  for (int i = 0; i < s->n; i++) {
    CtSsBuf c = { 0 };
    int r = s->prov[i].save(s->ctx[i], &c);
    if (r || c.err || b.err) { free(c.p); free(b.p); if (why) *why = b.err || c.err ? kOom : "a part of the game could not be saved"; return 1; }
    CtSsPut(&b, s->prov[i].tag, 4); CtSsPutU16(&b, s->prov[i].version); CtSsPutU16(&b, 0);
    CtSsPutU32(&b, (uint32_t)c.len); CtSsPutU32(&b, CtSsCrc32(c.p, c.len)); CtSsPut(&b, c.p, c.len);
    free(c.p);
  }
  CtSsPutU32(&b, CtSsCrc32(b.p, b.len));
  if (b.err) { free(b.p); if (why) *why = kOom; return 1; }
  *out = b.p; *len = b.len; return 0;
}

// ---- parse ----
typedef struct { char tag[4]; uint16_t ver; const uint8_t* d; size_t n; } SsChunk;
typedef struct { CtSsMeta m; int nChunks; SsChunk* ch; } SsParsed;
static const char* parse(const uint8_t* img, size_t len, SsParsed* P, uint8_t* thumb, size_t thumbCap, int wantChunks) {
  memset(P, 0, sizeof *P);
  if (len < 4 + 4 + 12 + 6 + 4 || memcmp(img, SS_MAGIC, 4)) return "not a save state";
  if (wantChunks != 2) {                                // (2 = the slot list: the header of a prefix of the file, no checksums)
    uint32_t want = CtSsCrc32(img, len - 4);
    CtSsReader t = CtSsReaderOf(img + len - 4, 4);
    if (CtSsGetU32(&t) != want) return "the file is damaged (checksum)";
  }
  CtSsReader r = CtSsReaderOf(img, wantChunks == 2 ? len : len - 4);
  r.pos = 4;
  uint32_t fmt = CtSsGetU16(&r), nCh = CtSsGetU16(&r);
  if (fmt != CT_SS_FMT) return "a state from another version of the framework";
  P->m.unixTime = CtSsGetU32(&r); P->m.frame = CtSsGetU32(&r); P->m.loc = (uint16_t)CtSsGetU16(&r); P->m.mode = (uint8_t)CtSsGetU8(&r); CtSsGetU8(&r);
  P->m.thumbW = (uint8_t)CtSsGetU8(&r); P->m.thumbH = (uint8_t)CtSsGetU8(&r);
  uint32_t tb = CtSsGetU32(&r);
  if (r.err || tb != (uint32_t)P->m.thumbW * P->m.thumbH * 3) return "the file is damaged (header)";
  const uint8_t* tp = CtSsGet(&r, tb);
  if (r.err) return "the file is damaged (thumbnail)";
  if (thumb && tp) memcpy(thumb, tp, tb < thumbCap ? tb : thumbCap);
  P->m.thumb = tp;
  P->nChunks = (int)nCh;
  if (wantChunks != 1) return NULL;
  P->ch = (SsChunk*)calloc(nCh ? nCh : 1, sizeof(SsChunk));
  if (!P->ch) return "out of memory";
  for (uint32_t i = 0; i < nCh; i++) {
    const uint8_t* tag = CtSsGet(&r, 4);
    uint32_t ver = CtSsGetU16(&r); CtSsGetU16(&r);
    uint32_t n = CtSsGetU32(&r), crc = CtSsGetU32(&r);
    const uint8_t* d = CtSsGet(&r, n);
    if (r.err) { free(P->ch); P->ch = NULL; return "the file is damaged (chunk table)"; }
    if (CtSsCrc32(d, n) != crc) { free(P->ch); P->ch = NULL; return "the file is damaged (chunk checksum)"; }
    memcpy(P->ch[i].tag, tag, 4); P->ch[i].ver = (uint16_t)ver; P->ch[i].d = d; P->ch[i].n = n;
  }
  if (r.pos != r.len) { free(P->ch); P->ch = NULL; return "the file is damaged (trailing bytes)"; }
  return NULL;
}
static int readInfo(const uint8_t* img, size_t len, CtSsMeta* m, uint8_t* thumb, size_t thumbCap, int mode) {
  SsParsed P; const char* e = parse(img, len, &P, thumb, thumbCap, mode);
  if (e) return 1;
  if (m) { *m = P.m; m->thumb = NULL; }
  return 0;
}
int CtSsReadInfo(const uint8_t* img, size_t len, CtSsMeta* m, uint8_t* thumb, size_t thumbCap) { return readInfo(img, len, m, thumb, thumbCap, 0); }

// ---- load ----
int CtSsLoadImage(CtSs* s, const uint8_t* img, size_t len, const char** why, int* partial) {
  static char buf[96];
  if (partial) *partial = 0;
  SsParsed P; const char* e = parse(img, len, &P, NULL, 0, 1);
  if (e) { if (why) *why = e; return 1; }
  const SsChunk* use[SS_MAX_PROV];
  for (int i = 0; i < s->n; i++) {
    const CtSsProvider* p = &s->prov[i]; use[i] = NULL;
    for (int k = 0; k < P.nChunks; k++) if (!memcmp(P.ch[k].tag, p->tag, 4)) { use[i] = &P.ch[k]; break; }
    if (!use[i]) { if (p->required) { snprintf(buf, sizeof buf, "the state has no '%s' part", p->tag); free(P.ch); if (why) *why = buf; return 1; } continue; }
    if (use[i]->ver > p->version) { snprintf(buf, sizeof buf, "'%s' was saved by a newer build", p->tag); free(P.ch); if (why) *why = buf; return 1; }
    if (use[i]->ver < p->minVersion) { snprintf(buf, sizeof buf, "'%s' was saved by an older build", p->tag); free(P.ch); if (why) *why = buf; return 1; }
    if (p->check && p->check(s->ctx[i], use[i]->d, use[i]->n, use[i]->ver)) {
      snprintf(buf, sizeof buf, "the '%s' part does not fit this game", p->tag); free(P.ch); if (why) *why = buf; return 1;
    }
  }
  for (int i = 0; i < s->n; i++) {                      // every check passed: apply, in registration order
    if (!use[i]) continue;
    if (s->prov[i].load(s->ctx[i], use[i]->d, use[i]->n, use[i]->ver)) {
      snprintf(buf, sizeof buf, "loading the '%s' part failed", s->prov[i].tag); free(P.ch);
      if (why) *why = buf;
      if (partial) *partial = i > 0;
      return 1;
    }
  }
  free(P.ch); return 0;
}

// ---- slots ----
void CtSsStampNow(char* out, size_t n) {
  time_t t = time(NULL); struct tm* lt = localtime(&t);
  if (!lt || !strftime(out, n, "%m/%d/%Y %I:%M:%S%p", lt)) snprintf(out, n, "?");
}
static void slotPath(CtSs* s, int slot, char* out, size_t n, const char* suffix) { snprintf(out, n, "%s/state%d.state%s", s->dir, slot, suffix); }
static void bakPath(CtSs* s, int slot, char* out, size_t n) { snprintf(out, n, "%s/state%d.bak", s->dir, slot); }
static int fileExists(const char* p) { FILE* f = fopen(p, "rb"); if (f) fclose(f); return f != NULL; }
// Write `data` to `path` through a temporary name. 0 = ok.
static int writeAtomic(const char* path, const uint8_t* a, size_t an, const uint8_t* b, size_t bn) {
  char tmp[400]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
  FILE* f = fopen(tmp, "wb");
  int ok = f && (!an || fwrite(a, 1, an, f) == an) && (!bn || fwrite(b, 1, bn, f) == bn);
  if (f && fclose(f)) ok = 0;
  if (ok && rename(tmp, path)) { remove(path); ok = !rename(tmp, path); }       // (a platform whose rename will not replace)
  if (!ok) remove(tmp);
  return !ok;
}
int CtSsSaveSlot(CtSs* s, int slot, const char** why) {
  if (slot < 0 || slot >= CT_SS_SLOTS) { if (why) *why = "no such slot"; return 1; }
  uint8_t* img; size_t len;
  if (CtSsSaveImage(s, &img, &len, why)) return 1;                      // (refused: nothing touched, the old file and its .bak stay)
  mkdir(s->dir, 0777);
  char fin[300], bak[300], line[64]; slotPath(s, slot, fin, sizeof fin, ""); bakPath(s, slot, bak, sizeof bak);
  if (s->backup && fileExists(fin)) { remove(bak); rename(fin, bak); }  // the current state becomes the .bak; an older .bak is replaced silently
  CtSsStampNow(line, sizeof line - 1);
  size_t ln = strlen(line); line[ln++] = '\n';
  int bad = writeAtomic(fin, (const uint8_t*)line, ln, img, len);
  CtSsFreeImage(img);
  if (bad) { if (why) *why = "the file could not be written"; return 1; }
  return 0;
}
static uint8_t* slurp(const char* path, size_t* len) {
  FILE* f = fopen(path, "rb"); if (!f) return NULL;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t* b = n > 0 && n < (64L << 20) ? (uint8_t*)malloc((size_t)n) : NULL;
  if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
  fclose(f); if (b) *len = (size_t)n; return b;
}
// Where the image starts: after the first line (at most 64 bytes of it; a file with no line break in them is not one of ours).
static size_t afterFirstLine(const uint8_t* d, size_t n) { for (size_t i = 0; i < n && i < 64; i++) if (d[i] == '\n') return i + 1; return 0; }
int CtSsLoadSlot(CtSs* s, int slot, const char** why, int* partial) {
  if (partial) *partial = 0;
  if (slot < 0 || slot >= CT_SS_SLOTS) { if (why) *why = "no such slot"; return 1; }
  char fin[300]; slotPath(s, slot, fin, sizeof fin, "");
  size_t len = 0; uint8_t* img = slurp(fin, &len);
  if (!img) { if (why) *why = "the slot is empty"; return 1; }
  size_t skip = afterFirstLine(img, len);
  if (!skip || skip >= len) { free(img); if (why) *why = "the slot is empty"; return 1; }
  int r = CtSsLoadImage(s, img + skip, len - skip, why, partial);
  free(img); return r;
}
int CtSsSlotStamp(CtSs* s, int slot, char* out, size_t n) {
  if (slot < 0 || slot >= CT_SS_SLOTS || n < 2) return 1;
  char fin[300]; slotPath(s, slot, fin, sizeof fin, "");
  FILE* f = fopen(fin, "rb"); if (!f) return 1;
  char line[80]; size_t got = fread(line, 1, sizeof line - 1, f); fclose(f);
  size_t i = 0; while (i < got && line[i] != '\n') i++;
  if (i == 0 || i >= got) return 1;                                      // blank, or not a line of ours
  if (i >= n) i = n - 1;
  memcpy(out, line, i); out[i] = 0; return 0;
}
int CtSsSlotInfo(CtSs* s, int slot, CtSsMeta* m, uint8_t* thumb, size_t thumbCap) {
  if (slot < 0 || slot >= CT_SS_SLOTS) return 1;
  char fin[300]; slotPath(s, slot, fin, sizeof fin, "");
  FILE* f = fopen(fin, "rb"); if (!f) return 1;                // (the list reads the header and thumbnail only, not the whole state)
  static uint8_t head[64 + 512 + CT_SS_THUMB_W * CT_SS_THUMB_H * 3 + 256];
  size_t n = fread(head, 1, sizeof head, f); fclose(f);
  size_t skip = afterFirstLine(head, n);
  if (!skip) return 1;
  return readInfo(head + skip, n - skip, m, thumb, thumbCap, 2);
}

// ---- backups: a stored zip of every state<N>.state / state<N>.bak ----
static int validName(const char* nm) {            // exactly state<digit>.state or state<digit>.bak: nothing else is ever written back
  return strlen(nm) >= 10 && !strncmp(nm, "state", 5) && nm[5] >= '0' && nm[5] <= '9' && nm[6] == '.' &&
         (!strcmp(nm + 7, "state") || !strcmp(nm + 7, "bak"));
}
static void zipPut16(CtSsBuf* b, uint32_t v) { CtSsPutU16(b, v); }
static void zipPut32(CtSsBuf* b, uint32_t v) { CtSsPutU32(b, v); }
typedef struct { char name[24]; uint32_t crc, size, off; } ZipEnt;
static int backupDirPath(CtSs* s, char* out, size_t n) { snprintf(out, n, "%s/backups", s->dir); return 0; }
int CtSsBackupAll(CtSs* s, char* name, size_t n, const char** why) {
  CtSsBuf z = { 0 }; ZipEnt ent[2 * CT_SS_SLOTS]; int ne = 0;
  time_t t = time(NULL); struct tm* lt = localtime(&t); struct tm now = lt ? *lt : (struct tm){ 0 };
  uint32_t dt = (uint32_t)(((now.tm_hour & 31) << 11) | ((now.tm_min & 63) << 5) | ((now.tm_sec / 2) & 31));
  uint32_t dd = (uint32_t)((((now.tm_year + 1900 - 1980) & 127) << 9) | (((now.tm_mon + 1) & 15) << 5) | (now.tm_mday & 31));
  for (int i = 0; i < CT_SS_SLOTS; i++) for (int k = 0; k < 2; k++) {
    char path[300], nm[24]; snprintf(nm, sizeof nm, "state%d.%s", i, k ? "bak" : "state");
    snprintf(path, sizeof path, "%s/%s", s->dir, nm);
    size_t len = 0; uint8_t* d = slurp(path, &len);
    if (!d) continue;
    ZipEnt* e = &ent[ne++]; snprintf(e->name, sizeof e->name, "%s", nm); e->crc = CtSsCrc32(d, len); e->size = (uint32_t)len; e->off = (uint32_t)z.len;
    zipPut32(&z, 0x04034B50u); zipPut16(&z, 20); zipPut16(&z, 0); zipPut16(&z, 0); zipPut16(&z, dt); zipPut16(&z, dd);
    zipPut32(&z, e->crc); zipPut32(&z, e->size); zipPut32(&z, e->size); zipPut16(&z, (uint32_t)strlen(nm)); zipPut16(&z, 0);
    CtSsPut(&z, nm, strlen(nm)); CtSsPut(&z, d, len);
    free(d);
  }
  if (!ne) { free(z.p); if (why) *why = "there are no saved states to back up"; return 1; }
  uint32_t cdOff = (uint32_t)z.len;
  for (int i = 0; i < ne; i++) {
    zipPut32(&z, 0x02014B50u); zipPut16(&z, 20); zipPut16(&z, 20); zipPut16(&z, 0); zipPut16(&z, 0); zipPut16(&z, dt); zipPut16(&z, dd);
    zipPut32(&z, ent[i].crc); zipPut32(&z, ent[i].size); zipPut32(&z, ent[i].size); zipPut16(&z, (uint32_t)strlen(ent[i].name));
    zipPut16(&z, 0); zipPut16(&z, 0); zipPut16(&z, 0); zipPut16(&z, 0); zipPut32(&z, 0); zipPut32(&z, ent[i].off);
    CtSsPut(&z, ent[i].name, strlen(ent[i].name));
  }
  uint32_t cdLen = (uint32_t)z.len - cdOff;
  zipPut32(&z, 0x06054B50u); zipPut16(&z, 0); zipPut16(&z, 0); zipPut16(&z, (uint32_t)ne); zipPut16(&z, (uint32_t)ne); zipPut32(&z, cdLen); zipPut32(&z, cdOff); zipPut16(&z, 0);
  if (z.err) { free(z.p); if (why) *why = "out of memory"; return 1; }
  char dir[300], path[420], stamp[48]; backupDirPath(s, dir, sizeof dir);
  mkdir(s->dir, 0777); mkdir(dir, 0777);
  if (!lt || !strftime(stamp, sizeof stamp, "%m-%d-%Y %I-%M-%S%p", &now)) snprintf(stamp, sizeof stamp, "backup");   // (the menu's stamp, with / and : made safe for a file name)
  for (int k = 0; ; k++) {                                                // (two backups in one second: the second gets a number)
    if (k) snprintf(path, sizeof path, "%s/%s (%d).zip", dir, stamp, k + 1); else snprintf(path, sizeof path, "%s/%s.zip", dir, stamp);
    if (!fileExists(path) || k > 99) break;
  }
  int bad = writeAtomic(path, z.p, z.len, NULL, 0);
  free(z.p);
  if (bad) { if (why) *why = "the archive could not be written"; return 1; }
  if (name) snprintf(name, n, "%s", strrchr(path, '/') + 1);
  return 0;
}
// "10-15-2026 01-25-30AM.zip" -> "10/15/2026 01:25:30AM" (anything else is shown as its file name)
static void shownName(const char* file, char* out, size_t n) {
  snprintf(out, n, "%.*s", (int)(n - 1), file);
  size_t l = strlen(out); if (l > 4 && !strcmp(out + l - 4, ".zip")) out[l - 4] = 0;
  if (strlen(out) >= 21 && out[2] == '-' && out[5] == '-' && out[13] == '-' && out[16] == '-') { out[2] = out[5] = '/'; out[13] = out[16] = ':'; }
}
static int cmpBackup(const void* a, const void* b) { long x = ((const CtSsBackup*)a)->mtime, y = ((const CtSsBackup*)b)->mtime; return x < y ? 1 : x > y ? -1 : strcmp(((const CtSsBackup*)b)->file, ((const CtSsBackup*)a)->file); }
int CtSsListBackups(CtSs* s, CtSsBackup* out, int max) {
  char dir[300]; backupDirPath(s, dir, sizeof dir);
  DIR* d = opendir(dir); if (!d) return 0;
  int n = 0; struct dirent* e;
  while ((e = readdir(d)) && n < max) {
    size_t l = strlen(e->d_name);
    if (l < 5 || l >= sizeof out[0].file || strcmp(e->d_name + l - 4, ".zip")) continue;
    char path[640]; if (strlen(dir) + l + 2 > sizeof path) continue;
    snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
    struct stat st; if (stat(path, &st)) continue;
    memcpy(out[n].file, e->d_name, l + 1); shownName(e->d_name, out[n].shown, sizeof out[n].shown); out[n].mtime = (long)st.st_mtime; n++;
  }
  closedir(d);
  qsort(out, (size_t)n, sizeof out[0], cmpBackup);
  return n;
}
int CtSsRestoreBackup(CtSs* s, const char* file, const char** why) {
  static const char* bad = "that archive is damaged or is not one of ours";
  if (!file || strchr(file, '/') || strchr(file, '\\') || strstr(file, "..")) { if (why) *why = bad; return 1; }
  char dir[300], path[420]; backupDirPath(s, dir, sizeof dir); snprintf(path, sizeof path, "%s/%s", dir, file);
  size_t len = 0; uint8_t* z = slurp(path, &len);
  if (!z) { if (why) *why = "the archive could not be read"; return 1; }
  // the end-of-central-directory record: the last 22 bytes (we write no comment)
  if (len < 22 || z[len - 22] != 0x50 || z[len - 21] != 0x4B || z[len - 20] != 5 || z[len - 19] != 6) { free(z); if (why) *why = bad; return 1; }
  CtSsReader er = CtSsReaderOf(z + len - 22, 22); er.pos = 10;
  uint32_t nEnt = CtSsGetU16(&er); uint32_t cdLen = CtSsGetU32(&er), cdOff = CtSsGetU32(&er);
  if (er.err || nEnt == 0 || nEnt > 2 * CT_SS_SLOTS || cdOff > len || cdLen > len - cdOff) { free(z); if (why) *why = bad; return 1; }
  struct { char name[24]; const uint8_t* d; uint32_t n; } got[2 * CT_SS_SLOTS];
  CtSsReader cr = CtSsReaderOf(z + cdOff, cdLen);
  for (uint32_t i = 0; i < nEnt; i++) {
    if (CtSsGetU32(&cr) != 0x02014B50u) { free(z); if (why) *why = bad; return 1; }
    CtSsGetU16(&cr); CtSsGetU16(&cr); CtSsGetU16(&cr); uint32_t method = CtSsGetU16(&cr); CtSsGetU16(&cr); CtSsGetU16(&cr);
    uint32_t crc = CtSsGetU32(&cr), cs = CtSsGetU32(&cr), us = CtSsGetU32(&cr), nl = CtSsGetU16(&cr), xl = CtSsGetU16(&cr), cl = CtSsGetU16(&cr);
    CtSsGetU16(&cr); CtSsGetU16(&cr); CtSsGetU32(&cr); uint32_t lo = CtSsGetU32(&cr);
    const uint8_t* nm = CtSsGet(&cr, nl); CtSsGet(&cr, (size_t)xl + cl);
    if (cr.err || method != 0 || cs != us || nl == 0 || nl >= sizeof got[0].name) { free(z); if (why) *why = bad; return 1; }
    memcpy(got[i].name, nm, nl); got[i].name[nl] = 0;
    if (!validName(got[i].name)) { free(z); if (why) *why = bad; return 1; }           // nothing but state<N>.state / .bak is ever extracted
    CtSsReader lr = CtSsReaderOf(z, len); lr.pos = lo;
    if (CtSsGetU32(&lr) != 0x04034B50u) { free(z); if (why) *why = bad; return 1; }
    for (int k = 0; k < 4; k++) CtSsGetU16(&lr);                                       // version, flags, method, time
    CtSsGetU16(&lr); CtSsGetU32(&lr); CtSsGetU32(&lr); CtSsGetU32(&lr);
    uint32_t lnl = CtSsGetU16(&lr), lxl = CtSsGetU16(&lr);
    CtSsGet(&lr, (size_t)lnl + lxl);
    const uint8_t* data = CtSsGet(&lr, us);
    if (lr.err || !data || CtSsCrc32(data, us) != crc) { free(z); if (why) *why = bad; return 1; }
    got[i].d = data; got[i].n = us;
  }
  mkdir(s->dir, 0777);
  for (uint32_t i = 0; i < nEnt; i++) {                                                 // the whole archive checked: extract over the current files
    char dst[420]; snprintf(dst, sizeof dst, "%s/%s", s->dir, got[i].name);
    if (writeAtomic(dst, got[i].d, got[i].n, NULL, 0)) { free(z); if (why) *why = "a file could not be written"; return 1; }
  }
  free(z); return 0;
}

// ---- requests ----
void CtSsRequest(CtSs* s, int kind, int slot) { s->reqKind = kind; s->reqSlot = slot; }
int CtSsPending(CtSs* s) { return s->reqKind; }
const char* CtSsMessage(CtSs* s, int* ok, unsigned* serial) { if (ok) *ok = s->msgOk; if (serial) *serial = s->msgSerial; return s->msg; }
int CtSsService(CtSs* s) {
  int k = s->reqKind, slot = s->reqSlot; s->reqKind = CT_SS_NONE;
  if (k == CT_SS_NONE) return 0;
  const char* why = "failed"; char nm[8]; snprintf(nm, sizeof nm, "%d", slot);          // (the slots are numbered 0-9 as the menu shows them)
  if (k == CT_SS_SAVE) {
    if (CtSsSaveSlot(s, slot, &why)) { char t[150]; snprintf(t, sizeof t, "Slot %s: not saved (%s)", nm, why); say(s, 0, "%s", t); return 0; }
    say(s, 1, "Saved to slot %s", nm); return CT_SS_SAVE;
  }
  int partial = 0;
  if (CtSsLoadSlot(s, slot, &why, &partial)) {
    char t[150]; snprintf(t, sizeof t, "Slot %s: not loaded (%s)%s", nm, why, partial ? " -- the game may be inconsistent, reload a state or restart" : "");
    say(s, 0, "%s", t); return partial ? CT_SS_LOAD : 0;       // a partial load changed the game: the host must still re-sync
  }
  say(s, 1, "Loaded slot %s", nm); return CT_SS_LOAD;
}

// ---- self test ----
typedef struct { uint32_t counter; uint8_t blob[5000]; int unavail; int failCheck; int loads; uint16_t lastVer; } TProv;
static const char* tUnavail(void* c) { return ((TProv*)c)->unavail ? "busy" : NULL; }
static int tSave(void* c, CtSsBuf* o) { TProv* t = (TProv*)c; CtSsPutU32(o, t->counter); CtSsPut(o, t->blob, sizeof t->blob); return 0; }
static int tCheck(void* c, const uint8_t* d, size_t n, uint16_t v) { (void)d; (void)v; return ((TProv*)c)->failCheck || n != 4 + 5000; }
static int tLoad(void* c, const uint8_t* d, size_t n, uint16_t v) { TProv* t = (TProv*)c; CtSsReader r = CtSsReaderOf(d, n); t->counter = CtSsGetU32(&r); memcpy(t->blob, CtSsGet(&r, sizeof t->blob), sizeof t->blob); t->loads++; t->lastVer = v; return 0; }
static void tMeta(void* c, CtSsMeta* m) { (void)c; static uint8_t th[CT_SS_THUMB_W * CT_SS_THUMB_H * 3]; m->frame = 1234; m->loc = 0x1B1; m->mode = CT_SS_MODE_FIELD; m->thumbW = CT_SS_THUMB_W; m->thumbH = CT_SS_THUMB_H; for (size_t i = 0; i < sizeof th; i++) th[i] = (uint8_t)i; m->thumb = th; }
static void ssTestClean(CtSs* s) {                         // the self-test's folder, emptied (also what an interrupted run left behind)
  char rm[200]; CtSsBackup lb[32];
  for (int i = 0; i < CT_SS_SLOTS; i++) { snprintf(rm, sizeof rm, "ctss_selftest_dir/state%d.state", i); remove(rm); snprintf(rm, sizeof rm, "ctss_selftest_dir/state%d.bak", i); remove(rm); snprintf(rm, sizeof rm, "ctss_selftest_dir/state%d.state.tmp", i); remove(rm); }
  int nl = CtSsListBackups(s, lb, 32); for (int i = 0; i < nl; i++) { snprintf(rm, sizeof rm, "ctss_selftest_dir/backups/%s", lb[i].file); remove(rm); }
  remove("ctss_selftest_dir/backups/bad.zip"); remove("ctss_selftest_dir/backups/evil.zip");
  remove("ctss_selftest_dir/backups"); remove("ctss_selftest_dir");
}
#define CHECK(c, what) do { if (!(c)) { fprintf(stderr, "[savestate selftest] FAILED: %s\n", what); fails++; } } while (0)
int CtSsSelfTest(void) {
  int fails = 0;
  static TProv A, B, Z; const char* why = NULL; int partial = 0;
  CtSsProvider pa = { "AAAA", 2, 1, 1, tUnavail, tSave, tCheck, tLoad }, pb = { "BBBB", 1, 1, 0, NULL, tSave, tCheck, tLoad };
  CtSsProvider pz = { "ZZZZ", 1, 1, 0, NULL, tSave, tCheck, tLoad };
  CtSs* s = CtSsNew(); CtSsRegister(s, &pa, &A); CtSsRegister(s, &pb, &B); CtSsSetMetaFn(s, tMeta, NULL);
  CHECK(CtSsRegister(s, &pa, &A) != 0, "a duplicate tag is refused");
  A.counter = 77; B.counter = 99; for (size_t i = 0; i < sizeof A.blob; i++) { A.blob[i] = (uint8_t)(i * 7); B.blob[i] = (uint8_t)(i * 3); }
  uint8_t* img; size_t len;
  CHECK(!CtSsSaveImage(s, &img, &len, &why), "save");
  if (!img) { CtSsFree(s); return 1; }
  CtSsMeta mi; static uint8_t th[CT_SS_THUMB_W * CT_SS_THUMB_H * 3];
  CHECK(!CtSsReadInfo(img, len, &mi, th, sizeof th) && mi.frame == 1234 && mi.loc == 0x1B1 && mi.thumbW == CT_SS_THUMB_W && th[100] == 100, "header, meta and thumbnail read back");
  // round trip into mutated state
  TProv A0 = A, B0 = B; A.counter = 1; B.counter = 2; memset(A.blob, 0, sizeof A.blob); memset(B.blob, 0, sizeof B.blob);
  CHECK(!CtSsLoadImage(s, img, len, &why, &partial) && A.counter == 77 && B.counter == 99 && !memcmp(A.blob, A0.blob, sizeof A.blob) && !memcmp(B.blob, B0.blob, sizeof B.blob) && A.lastVer == 2, "round trip");
  // truncation at many lengths and bit flips at many offsets: refused, and nothing applied
  A.loads = B.loads = 0;
  int refusedT = 0, refusedF = 0, nT = 0, nF = 0;
  for (size_t cut = 0; cut < len; cut += (len / 150) + 1) { nT++; if (CtSsLoadImage(s, img, cut, &why, &partial)) refusedT++; }
  for (size_t at = 0; at < len; at += (len / 150) + 1) { img[at] ^= 0x10; nF++; if (CtSsLoadImage(s, img, len, &why, &partial)) refusedF++; img[at] ^= 0x10; }
  CHECK(refusedT == nT && refusedF == nF && A.loads == 0 && B.loads == 0, "truncated / flipped files are refused and apply nothing");
  // one provider's check refuses: no provider's load runs (atomic)
  B.failCheck = 1; A.loads = B.loads = 0;
  CHECK(CtSsLoadImage(s, img, len, &why, &partial) && A.loads == 0 && B.loads == 0 && !partial, "a failing check applies nothing, not even the providers before it");
  B.failCheck = 0;
  // unavailable: nothing written
  A.unavail = 1; uint8_t* i2 = NULL; size_t l2 = 0;
  CHECK(CtSsSaveImage(s, &i2, &l2, &why) && !i2 && why && !strcmp(why, "busy"), "an unavailable provider blocks the save and says why");
  A.unavail = 0;
  // an unknown chunk is skipped; a missing required chunk refuses; a newer / older chunk refuses
  CtSs* s2 = CtSsNew(); CtSsRegister(s2, &pa, &Z); CtSsRegister(s2, &pz, &Z);
  uint8_t* i3; size_t l3; CtSsSaveImage(s2, &i3, &l3, &why);
  CHECK(!CtSsLoadImage(s, i3, l3, &why, &partial), "an unknown chunk (ZZZZ) is skipped, a missing optional one (BBBB) is fine");
  CtSs* s3 = CtSsNew(); CtSsRegister(s3, &pb, &Z);
  uint8_t* i4; size_t l4; CtSsSaveImage(s3, &i4, &l4, &why);
  CHECK(CtSsLoadImage(s, i4, l4, &why, &partial) && why && strstr(why, "AAAA"), "a state missing a required part is refused");
  CtSsProvider newer = pa; newer.version = 3; newer.minVersion = 3; CtSs* s4 = CtSsNew(); CtSsRegister(s4, &newer, &Z);
  uint8_t* i5; size_t l5; CtSsSaveImage(s4, &i5, &l5, &why);
  CHECK(CtSsLoadImage(s, i5, l5, &why, &partial) && why && strstr(why, "newer"), "a chunk from a newer build is refused");
  CtSsProvider older = pa; older.version = 1; older.minVersion = 1; CtSs* s5 = CtSsNew(); CtSsRegister(s5, &older, &Z);
  uint8_t* i6; size_t l6; CtSsSaveImage(s5, &i6, &l6, &why);
  CtSsProvider strict = pa; strict.minVersion = 2; CtSs* s6 = CtSsNew(); CtSsRegister(s6, &strict, &A);
  CHECK(CtSsLoadImage(s6, i6, l6, &why, &partial) && why && strstr(why, "older"), "a chunk older than the provider's minimum is refused");
  CHECK(!CtSsLoadImage(s, i6, l6, &why, &partial) && A.lastVer == 1, "an older readable chunk loads and is told its version");
  // slots, the first line, the .bak, the request queue
  CtSsSetDir(s, "ctss_selftest_dir"); ssTestClean(s);
  A.counter = 77;
  CtSsRequest(s, CT_SS_SAVE, 2);
  CHECK(CtSsPending(s) == CT_SS_SAVE && CtSsService(s) == CT_SS_SAVE && !CtSsPending(s), "a save request runs once at the safe point");
  int ok; unsigned ser; CtSsMessage(s, &ok, &ser); CHECK(ok && ser == 1, "its message");
  char stamp[48];
  CHECK(!CtSsSlotStamp(s, 2, stamp, sizeof stamp) && strlen(stamp) == 21 && stamp[2] == '/' && stamp[13] == ':' && (!strcmp(stamp + 19, "AM") || !strcmp(stamp + 19, "PM")),
        "a saved slot's first line is its time, MM/DD/YYYY HH:MM:SS + AM/PM");
  CHECK(CtSsSlotStamp(s, 1, stamp, sizeof stamp) != 0, "an empty slot has no time stamp");
  CtSsMeta sm; CHECK(!CtSsSlotInfo(s, 2, &sm, NULL, 0) && sm.frame == 1234 && CtSsSlotInfo(s, 1, &sm, NULL, 0), "slot 2 holds a state (the header reads past the first line), slot 1 is empty");
  { FILE* bf = fopen("ctss_selftest_dir/state5.state", "wb"); if (bf) fclose(bf); }
  CHECK(CtSsSlotStamp(s, 5, stamp, sizeof stamp) != 0, "a blank file is no state");
  remove("ctss_selftest_dir/state5.state");
  A.counter = 5; CtSsRequest(s, CT_SS_LOAD, 2);
  CHECK(CtSsService(s) == CT_SS_LOAD && A.counter == 77, "a load request restores the slot");
  CtSsRequest(s, CT_SS_LOAD, 1); CHECK(CtSsService(s) == 0 && (CtSsMessage(s, &ok, &ser), !ok), "loading an empty slot fails with a message");
  // the backup option: the old file becomes .bak, an older .bak is replaced silently, the new state is written; a refused save touches nothing
  CtSsSetBackup(s, 1);
  A.counter = 11; CtSsRequest(s, CT_SS_SAVE, 2); CtSsService(s);                          // slot 2 now holds 11; the 77 became state2.bak
  A.counter = 22; CtSsRequest(s, CT_SS_SAVE, 2); CtSsService(s);                          // slot 2 holds 22; the .bak holds 11 (77 is gone, silently)
  { size_t bl = 0; uint8_t* bk = slurp("ctss_selftest_dir/state2.bak", &bl);
    size_t sk = bk ? afterFirstLine(bk, bl) : 0;
    A.counter = 0;
    CHECK(bk && sk && !CtSsLoadImage(s, bk + sk, bl - sk, &why, &partial) && A.counter == 11, "the previous state is kept as state2.bak (the one before it was replaced silently)");
    free(bk); }
  A.counter = 0; CtSsRequest(s, CT_SS_LOAD, 2); CtSsService(s); CHECK(A.counter == 22, "the new state replaced the old in the slot");
  A.unavail = 1; CtSsRequest(s, CT_SS_SAVE, 2); CtSsService(s); A.unavail = 0;
  A.counter = 0; CtSsRequest(s, CT_SS_LOAD, 2); CtSsService(s); CHECK(A.counter == 22, "a refused save leaves the slot and its .bak alone");
  CtSsSetBackup(s, 0);
  remove("ctss_selftest_dir/state2.bak"); A.counter = 33; CtSsRequest(s, CT_SS_SAVE, 2); CtSsService(s);
  CHECK(!fileExists("ctss_selftest_dir/state2.bak"), "with the option off no .bak is made");
  // backup all / restore all
  CtSsSetBackup(s, 1); A.counter = 44; CtSsRequest(s, CT_SS_SAVE, 2); CtSsService(s); A.counter = 55; CtSsRequest(s, CT_SS_SAVE, 7); CtSsService(s);
  char an[96]; const char* bw = NULL;
  CHECK(!CtSsBackupAll(s, an, sizeof an, &bw) && strstr(an, ".zip"), "backup all makes a zip named after the time, in backups/");
  CtSsBackup list[8]; int nb = CtSsListBackups(s, list, 8);
  CHECK(nb == 1 && !strcmp(list[0].file, an) && list[0].shown[2] == '/' && list[0].shown[13] == ':', "restore lists it, with the menu's time format");
  A.counter = 99; CtSsRequest(s, CT_SS_SAVE, 2); CtSsService(s); remove("ctss_selftest_dir/state7.state");        // slot 2 changed, slot 7 gone
  CHECK(!CtSsRestoreBackup(s, an, &bw), "restore extracts the archive over the current files");
  A.counter = 0; CtSsRequest(s, CT_SS_LOAD, 2); CtSsService(s); CHECK(A.counter == 44, "slot 2 is back to the backed-up state");
  A.counter = 0; CtSsRequest(s, CT_SS_LOAD, 7); CtSsService(s); CHECK(A.counter == 55, "slot 7 is back");
  { char zp[200]; snprintf(zp, sizeof zp, "ctss_selftest_dir/backups/%s", an);
    size_t zl = 0; uint8_t* zb = slurp(zp, &zl);
    zb[zl / 2] ^= 0x40; FILE* zf = fopen("ctss_selftest_dir/backups/bad.zip", "wb"); fwrite(zb, 1, zl, zf); fclose(zf); zb[zl / 2] ^= 0x40;
    A.counter = 66; CtSsRequest(s, CT_SS_SAVE, 2); CtSsService(s);
    CHECK(CtSsRestoreBackup(s, "bad.zip", &bw) != 0, "a damaged archive is refused");
    A.counter = 0; CtSsRequest(s, CT_SS_LOAD, 2); CtSsService(s); CHECK(A.counter == 66, "...and extracts nothing");
    for (size_t i = 0; i + 12 <= zl; i++) if (!memcmp(zb + i, "state2.state", 12)) zb[i + 5] = '?';             // a name that is not ours
    zf = fopen("ctss_selftest_dir/backups/evil.zip", "wb"); fwrite(zb, 1, zl, zf); fclose(zf); free(zb);
    CHECK(CtSsRestoreBackup(s, "evil.zip", &bw) != 0 && CtSsRestoreBackup(s, "../x.zip", &bw) != 0, "an archive with a foreign file name, or a path out of backups/, is refused"); }
  ssTestClean(s);
  CtSsFreeImage(img); CtSsFreeImage(i3); CtSsFreeImage(i4); CtSsFreeImage(i5); CtSsFreeImage(i6);
  CtSsFree(s); CtSsFree(s2); CtSsFree(s3); CtSsFree(s4); CtSsFree(s5); CtSsFree(s6);
  printf("savestate-selftest: %s (%d failure%s)\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
  return fails != 0;
}
