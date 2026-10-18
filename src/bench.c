// bench.c - see bench.h.
#include "bench.h"
#include "present.h"
#include "switch_platform.h"
#include "SDL.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

// ---- the run's identity -------------------------------------------------------------------------------------------------------------------------------
static char g_name[64] = "ct_field", g_stamp[32];
void CtRunSetName(const char* argv0) {
  if (!argv0 || !*argv0) return;
  const char* b = strrchr(argv0, '/'); b = b ? b + 1 : argv0;
  const char* bs = strrchr(b, '\\'); if (bs) b = bs + 1;
  snprintf(g_name, sizeof g_name, "%s", b);
  size_t l = strlen(g_name); if (l > 4 && !strcmp(g_name + l - 4, ".nro")) g_name[l - 4] = 0;
  if (!g_name[0]) snprintf(g_name, sizeof g_name, "ct_field");
}
const char* CtRunName(void) { return g_name; }
const char* CtRunStamp(void) {
  if (g_stamp[0]) return g_stamp;
  time_t t = time(NULL); struct tm* lt = t > 1577836800 ? localtime(&t) : NULL;        // (a clock before 2020 is not set)
  if (lt) strftime(g_stamp, sizeof g_stamp, "%Y%m%d-%H%M%S", lt);
  else {                                                                                // no clock: a run counter, kept in a file
    int n = 0; FILE* f = fopen("logs/.runcount", "r"); if (f) { if (fscanf(f, "%d", &n) != 1) n = 0; fclose(f); }
    mkdir("logs", 0777); f = fopen("logs/.runcount", "w"); if (f) { fprintf(f, "%d\n", n + 1); fclose(f); }
    snprintf(g_stamp, sizeof g_stamp, "run%04d", n + 1);
  }
  return g_stamp;
}
void CtRunPath(char* out, size_t n, const char* kind, const char* ext) {
  mkdir("logs", 0777);
  snprintf(out, n, "logs/%s_%s_%s.%s", kind, g_name, CtRunStamp(), ext);
}

// ---- configuration -------------------------------------------------------------------------------------------------------------------------------------
int CtBenchMode(void) { static int m = -1; if (m < 0) { const char* e = getenv("CT_BENCH"); m = e ? atoi(e) : 0; if (m < 0 || m > 4) m = 0; } return m; }
int CtBenchSecs(void) { static int s = -1; if (s < 0) { const char* e = getenv("CT_BENCH_SECS"); s = e ? atoi(e) : (CtBenchMode() == 4 ? 60 : 120); if (s < 5) s = 5; } return s; }

// ---- histograms ------------------------------------------------------------------------------------------------------------------------------------------
#define HB 800                                   // buckets of 0.25 ms: 0..200 ms, the last one holds the rest
typedef struct { unsigned b[HB + 1]; unsigned long long n; double sum, mn, mx; } Hist;
static void hAdd(Hist* h, double v) {
  if (v < 0) v = 0;
  int i = (int)(v * 4.0); if (i > HB) i = HB;
  h->b[i]++; h->sum += v; if (!h->n || v < h->mn) h->mn = v; if (v > h->mx) h->mx = v; h->n++;
}
static double hPct(const Hist* h, double p) {    // the upper edge of the bucket holding the p-th percentile, never above the maximum
  if (!h->n) return 0.0;
  unsigned long long want = (unsigned long long)(p / 100.0 * (double)h->n); if (want < 1) want = 1;
  unsigned long long c = 0;
  for (int i = 0; i <= HB; i++) { c += h->b[i]; if (c >= want) { double e = (double)(i + 1) * 0.25; return e > h->mx ? h->mx : e; } }
  return h->mx;
}
static double hAvg(const Hist* h) { return h->n ? h->sum / (double)h->n : 0.0; }

enum { M_PERIOD, M_WORK, M_UP, M_DRAW, M_SWAP, M_LOGIC, M_COMPOSE, M_HUD, M_RT, M_PRESENT, M_GPU, M_WAIT, M_RTUP, M_RTWAIT, M_N };
static const char* const kMetric[M_N] = { "frame", "work", "upload", "draw", "swap", "logic", "compose", "hud", "render-thread", "present(up+draw+swap)", "gpu (frame, by GPU clock)", "cpu blocked on gpu", "render-thread texture upload", "render-thread present wait" };
#define MAXMODES 9                               // 0 = every frame, then each mode the game reports
typedef struct { char name[16]; Hist h[M_N]; unsigned long long over17, over33, over50, over100; } ModeStats;
static ModeStats g_m[MAXMODES]; static int g_nm = 1;

static int modeIx(const char* mode) {
  if (!mode) mode = "?";
  for (int i = 1; i < g_nm; i++) if (!strcmp(g_m[i].name, mode)) return i;
  if (g_nm >= MAXMODES) return 0;
  snprintf(g_m[g_nm].name, sizeof g_m[g_nm].name, "%s", mode);
  return g_nm++;
}

// ---- what the run is -----------------------------------------------------------------------------------------------------------------------------
static int g_backendId = -1; static char g_backendName[160], g_backendKey[16] = "unknown", g_info[16][200]; static int g_ninfo;
static const char* backendKey(int id) { return id == CT_BACKEND_OPENGL ? "opengl" : id == CT_BACKEND_VULKAN ? "vulkan" : id == CT_BACKEND_DEKO3D ? "deko3d" : "sdl"; }
void CtBenchSetBackend(int id, const char* name) {
  g_backendId = id; snprintf(g_backendName, sizeof g_backendName, "%s", name ? name : "?");
  snprintf(g_backendKey, sizeof g_backendKey, "%s", backendKey(id));
}
void CtBenchInfo(const char* line) { if (g_ninfo < 16 && line) snprintf(g_info[g_ninfo++], sizeof g_info[0], "%s", line); }

// ---- samples ---------------------------------------------------------------------------------------------------------------------------------------
static double nowMs(void) { return (double)SDL_GetPerformanceCounter() * 1000.0 / (double)SDL_GetPerformanceFrequency(); }
static int g_lastMode;                                   // the mode of the latest frame: where the presenter's per-frame extras go
static double g_t0 = -1.0, g_lastWrite;
static unsigned long long g_secN; static double g_fpsSum, g_fpsMin = 1e9, g_engSum, g_cpuSum, g_gpuSum, g_memSum, g_periodAvgSum, g_periodMaxMax; static unsigned long long g_audDrop, g_audEmpty;
static char g_csvPath[256]; static int g_csvHeader;

void CtBenchFrame(const char* mode, double per, double work, double up, double draw, double swap, double logic, double compose, double hud, double rt, double rtUp, double rtWait) {
  if (g_t0 < 0) g_t0 = nowMs();
  double v[M_N] = { per, work, up, draw, swap, logic, compose, hud, rt, up + draw + swap, 0, 0, rtUp, rtWait };
  int mi = modeIx(mode); g_lastMode = mi;
  for (int k = 0; k < 2; k++) {
    int ix = k ? mi : 0; if (k && !mi) break;
    for (int m = 0; m < M_PRESENT + 1; m++) hAdd(&g_m[ix].h[m], v[m]);
    if (rt > 0 || rtUp > 0) { hAdd(&g_m[ix].h[M_RTUP], rtUp); hAdd(&g_m[ix].h[M_RTWAIT], rtWait); }
    if (per > 1000.0 / 60.0 + 0.5) g_m[ix].over17++;
    if (per > 2000.0 / 60.0 + 1.0) g_m[ix].over33++;
    if (per > 50.0) g_m[ix].over50++;
    if (per > 100.0) g_m[ix].over100++;
  }
}
void CtBenchExtra(double gpuMs, int hasGpu, double waitMs) {
  if (g_t0 < 0) return;
  for (int k = 0; k < 2; k++) {
    int ix = k ? g_lastMode : 0; if (k && !ix) break;
    if (hasGpu) hAdd(&g_m[ix].h[M_GPU], gpuMs);
    hAdd(&g_m[ix].h[M_WAIT], waitMs);
  }
}
void CtBenchSecond(const char* mode, int loc, double eng, double shown, double perAvg, double perMax, double workAvg, int cpu, int gpu, int mem, long audDrop, long audEmpty) {
  if (g_t0 < 0) g_t0 = nowMs();
  g_secN++; g_fpsSum += shown; if (shown < g_fpsMin) g_fpsMin = shown; g_engSum += eng; g_cpuSum += cpu; g_gpuSum += gpu; g_memSum += mem;
  g_periodAvgSum += perAvg; if (perMax > g_periodMaxMax) g_periodMaxMax = perMax; g_audDrop += (unsigned long long)audDrop; g_audEmpty += (unsigned long long)audEmpty;
  if (!g_csvPath[0]) { char k[48]; snprintf(k, sizeof k, "bench_%s", g_name); mkdir("logs", 0777);
                       snprintf(g_csvPath, sizeof g_csvPath, "logs/bench_%s_%s_%s.csv", g_name, g_backendKey, CtRunStamp()); }
  FILE* f = fopen(g_csvPath, g_csvHeader ? "a" : "w");
  if (f) {
    if (!g_csvHeader) { fputs("t_s,mode,loc,engine_fps,shown_fps,frame_avg_ms,frame_max_ms,work_avg_ms,cpu_mhz,gpu_mhz,mem_mhz,audio_drops,audio_empty\n", f); g_csvHeader = 1; }
    fprintf(f, "%.0f,%s,%03X,%.2f,%.2f,%.3f,%.2f,%.3f,%d,%d,%d,%ld,%ld\n", (nowMs() - g_t0) / 1000.0, mode ? mode : "?", loc < 0 ? 0 : loc, eng, shown, perAvg, perMax, workAvg, cpu, gpu, mem, audDrop, audEmpty);
    fclose(f);
  }
  if (nowMs() - g_lastWrite > 30000.0) { g_lastWrite = nowMs(); CtBenchWrite(0); }          // (a run that is killed still leaves a summary no older than half a minute)
}
int CtBenchEngineExpired(void) {
  int m = CtBenchMode(); if (m != 1 && m != 4) return 0;
  return g_t0 >= 0 && (nowMs() - g_t0) / 1000.0 >= (double)CtBenchSecs();
}

static void writeMode(FILE* f, const ModeStats* s, const char* label) {
  const Hist* p = &s->h[M_PERIOD]; if (!p->n) return;
  double secs = p->sum / 1000.0;
  fprintf(f, "\n== %s: %llu frames, %.1f s of frames, %.2f engine f/s\n", label, p->n, secs, secs > 0 ? (double)p->n / secs : 0.0);
  fprintf(f, "   over 16.7 ms: %llu (%.2f%%)   over 33.3 ms: %llu (%.2f%%)   over 50 ms: %llu   over 100 ms: %llu\n", s->over17, 100.0 * (double)s->over17 / (double)p->n,
          s->over33, 100.0 * (double)s->over33 / (double)p->n, s->over50, s->over100);
  fprintf(f, "   %-22s %8s %8s %8s %8s %8s %8s %8s\n", "part (ms)", "avg", "min", "p50", "p95", "p99", "p99.9", "max");
  for (int m = 0; m < M_N; m++) {
    const Hist* h = &s->h[m]; if (!h->n || (h->mx <= 0.0 && m != M_PERIOD)) continue;
    fprintf(f, "   %-22s %8.3f %8.3f %8.2f %8.2f %8.2f %8.2f %8.2f\n", kMetric[m], hAvg(h), h->mn, hPct(h, 50), hPct(h, 95), hPct(h, 99), hPct(h, 99.9), h->mx);
  }
}
void CtBenchWrite(int final) {
  if (g_m[0].h[M_PERIOD].n == 0 && !final) return;
  char path[256]; snprintf(path, sizeof path, "logs/bench_%s_%s_%s.txt", g_name, g_backendKey, CtRunStamp()); mkdir("logs", 0777);
  FILE* f = fopen(path, "w"); if (!f) return;
  fprintf(f, "Chrono Trigger NX -- run summary%s\nprogram %s, started %s, backend %s (%s)\n", final ? "" : " (partial: the run is still going or was ended abruptly)", g_name, CtRunStamp(), g_backendKey, g_backendName);
  for (int i = 0; i < g_ninfo; i++) fprintf(f, "%s\n", g_info[i]);
  fprintf(f, "bench mode %d%s\n", CtBenchMode(), CtBenchMode() == 1 || CtBenchMode() == 4 ? " (timed run)" : "");
  if (g_secN) fprintf(f, "\nper-second figures over %llu s: shown f/s avg %.2f min %.2f, engine f/s avg %.2f, frame avg %.3f ms (worst second max %.1f ms), clocks cpu %.0f gpu %.0f mem %.0f MHz avg, "
                         "audio blocks dropped %llu, queue empty %llu\n", g_secN, g_fpsSum / (double)g_secN, g_fpsMin, g_engSum / (double)g_secN, g_periodAvgSum / (double)g_secN, g_periodMaxMax,
                         g_cpuSum / (double)g_secN, g_gpuSum / (double)g_secN, g_memSum / (double)g_secN, g_audDrop, g_audEmpty);
  writeMode(f, &g_m[0], "ALL FRAMES");
  for (int i = 1; i < g_nm; i++) writeMode(f, &g_m[i], g_m[i].name);
  fprintf(f, "\nHistograms are 0.25 ms buckets: percentiles are the bucket's upper edge. 'frame' is the time between engine frames (60 Hz = 16.67 ms), 'work' the time the host spent in\n"
             "a frame, 'upload/draw/swap' the presenter's calls on the main thread (swap includes the vsync wait), 'render-thread' the SDL_Renderer thread's own work.\n");
  fclose(f);
}

// ---- the micro-benchmark's rows ------------------------------------------------------------------------------------------------------------
static int cmpd(const void* a, const void* b) { double x = *(const double*)a, y = *(const double*)b; return x < y ? -1 : x > y; }
static void stat6(const double* v, int n, double* avg, double* p50, double* p95, double* p99, double* mx) {
  *avg = *p50 = *p95 = *p99 = *mx = 0; if (!v || n <= 0) return;
  double* s = (double*)malloc((size_t)n * sizeof *s); if (!s) return;
  memcpy(s, v, (size_t)n * sizeof *s); qsort(s, (size_t)n, sizeof *s, cmpd);
  double sum = 0; for (int i = 0; i < n; i++) sum += s[i];
  *avg = sum / n; *p50 = s[n / 2]; *p95 = s[(int)(0.95 * (n - 1))]; *p99 = s[(int)(0.99 * (n - 1))]; *mx = s[n - 1]; free(s);
}
static void appendRow(const char* path, const char* row) {
  mkdir("logs", 0777);
  struct stat st; int fresh = stat(path, &st) != 0;
  FILE* f = fopen(path, "a"); if (!f) return;
  if (fresh) fputs("stamp,program,backend,phase,pic_w,pic_h,out_w,out_h,frames,wall_s,fps,frame_avg,frame_p50,frame_p95,frame_p99,frame_max,upload_avg,upload_p95,draw_avg,draw_p95,swap_avg,swap_p95,overlay_avg,overlay_p95,wait_avg,wait_p95,gpu_avg,gpu_p95,gpu_p99,gpu_max,gpu_samples,shown_fps,replaced,rt_work_avg,rt_work_p95,rt_up_avg,rt_up_p95,rt_wait_avg,rt_wait_p95,rt_samples\n", f);
  fputs(row, f); fclose(f);
}
static char g_chainCsv[256]; static int g_chainOn;
void CtBenchPresentRow(const char* phase, int pw, int ph, int ow, int oh, int frames, double wall, const double* tot, const double* up, const double* dr, const double* sw, const double* ov, const double* wt, const double* gp, int ngp, const CtBenchRt* rt) {
  double rwa = 0, rw95 = 0, rua = 0, ru95 = 0, rva = 0, rv95 = 0, shownFps = wall > 0 ? frames / wall : 0; long replaced = 0; int nrt = 0;
  double wa, w50, w95, w99, wmx, ga, g50, g95, g99, gmx;
  double ta, t50, t95, t99, tmx, ua, u50, u95, u99, umx, da, d50, d95, d99, dmx, sa, s50, s95, s99, smx, oa, o50, o95, o99, omx;
  stat6(tot, frames, &ta, &t50, &t95, &t99, &tmx); stat6(up, frames, &ua, &u50, &u95, &u99, &umx); stat6(dr, frames, &da, &d50, &d95, &d99, &dmx);
  stat6(sw, frames, &sa, &s50, &s95, &s99, &smx); stat6(ov, frames, &oa, &o50, &o95, &o99, &omx);
  stat6(wt, frames, &wa, &w50, &w95, &w99, &wmx); stat6(gp, ngp, &ga, &g50, &g95, &g99, &gmx);
  if (rt) { double a, b, c, d, e; nrt = rt->n; stat6(rt->work, rt->n, &rwa, &a, &rw95, &b, &c); stat6(rt->up, rt->n, &rua, &d, &ru95, &e, &c); stat6(rt->wait, rt->n, &rva, &a, &rv95, &b, &c);
            shownFps = wall > 0 ? (double)rt->shown / wall : 0; replaced = rt->replaced; }
  double fps = wall > 0 ? frames / wall : 0;
  char row[1100];
  snprintf(row, sizeof row, "%s,%s,%s,%s,%d,%d,%d,%d,%d,%.2f,%.1f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%.1f,%ld,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d\n", CtRunStamp(), g_name, g_backendKey, phase, pw, ph, ow, oh, frames, wall, fps,
           ta, t50, t95, t99, tmx, ua, u95, da, d95, sa, s95, oa, o95, wa, w95, ga, g95, g99, gmx, ngp, shownFps, replaced, rwa, rw95, rua, ru95, rva, rv95, nrt);
  appendRow("logs/bench_present.csv", row);
  if (g_chainOn && g_chainCsv[0]) appendRow(g_chainCsv, row);
  fprintf(stderr, "[bench] %-8s %-24s %4dx%-4d in %4dx%-4d: %7.1f f/s | frame avg %.3f p95 %.3f p99 %.3f max %.2f | upload %.3f draw %.3f swap %.3f overlay %.3f | cpu blocked %.3f | gpu %.3f p95 %.3f (%d)%s ms\n", g_backendKey, phase, pw, ph, ow, oh, fps,
          ta, t95, t99, tmx, ua, da, sa, oa, wa, ga, g95, ngp, "");
  if (rt) fprintf(stderr, "[bench]          render thread: %.1f shown f/s (%ld frames replaced) | work %.3f p95 %.3f (texture upload %.3f p95 %.3f) | present wait %.3f p95 %.3f ms | %d frames\n", shownFps, replaced, rwa, rw95, rua, ru95, rva, rv95, nrt);
  fflush(stderr);
  fflush(stderr);
}

// ---- the automated pass over every backend ---------------------------------------------------------------------------------------------------
#define STATE_FILE "ct_bench_state"
static int g_order[4], g_nOrder, g_idx = -1; static char g_chainStamp[32];
static void buildOrder(void) {
  g_nOrder = 0;
  for (int id = 0; id < 4; id++) if (CtPresentBackendAvailable(id)) g_order[g_nOrder++] = id;
}
static void stateWrite(void) {
  FILE* f = fopen(STATE_FILE, "w"); if (!f) return;
  fprintf(f, "stamp=%s\nidx=%d\nmode=%d\nsecs=%d\n", g_chainStamp, g_idx, CtBenchMode(), CtBenchSecs()); fclose(f);
}
static int stateRead(void) {
  FILE* f = fopen(STATE_FILE, "r"); if (!f) return 0;
  char line[96]; int ok = 0;
  while (fgets(line, sizeof line, f)) {
    int v; char s[32];
    if (sscanf(line, "stamp=%31s", s) == 1) { snprintf(g_chainStamp, sizeof g_chainStamp, "%s", s); ok |= 1; }
    else if (sscanf(line, "idx=%d", &v) == 1) { g_idx = v; ok |= 2; }
  }
  fclose(f); return ok == 3;
}
int CtBenchChainActive(void) { return g_chainOn; }
int CtBenchChainExpected(void) { return g_chainOn && g_idx >= 0 && g_idx < g_nOrder ? g_order[g_idx] : -1; }
static void chainEnd(const char* msg) {
  remove(STATE_FILE); g_chainOn = 0; g_idx = -1;
  CtSwitchBoostIniSet("bench", "0");                    // one-shot: it does not start again at the next launch
  fprintf(stderr, "[bench] chain %s: %s (results: %s)\n", g_chainStamp, msg, g_chainCsv); fflush(stderr);
}
void CtBenchChainStart(int* backend) {
#ifdef __SWITCH__
  int mode = CtBenchMode();
  if (mode < 3) { remove(STATE_FILE); return; }
  buildOrder();
  if (!g_nOrder) return;
  if (!stateRead()) { snprintf(g_chainStamp, sizeof g_chainStamp, "%s", CtRunStamp()); g_idx = 0; stateWrite(); }
  snprintf(g_chainCsv, sizeof g_chainCsv, "logs/bench_chain_%s.csv", g_chainStamp); mkdir("logs", 0777);
  g_chainOn = 1;
  if (CtSwitchBackendTryPending()) {                    // the previous run of this chain never got through its start: that backend crashed
    CtSwitchBackendTryEnd();
    char row[400]; snprintf(row, sizeof row, "%s,%s,%s,FAILED (crashed while starting),0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0\n", g_chainStamp, g_name, g_idx >= 0 && g_idx < g_nOrder ? backendKey(g_order[g_idx]) : "?");
    appendRow(g_chainCsv, row); g_idx++; stateWrite();
  }
  if (g_idx >= g_nOrder) { chainEnd("complete"); return; }
  if (backend) *backend = g_order[g_idx];
  fprintf(stderr, "[bench] chain %s: step %d of %d, backend %s\n", g_chainStamp, g_idx + 1, g_nOrder, backendKey(g_order[g_idx])); fflush(stderr);
#else
  (void)backend;
#endif
}
static void relaunchOrEnd(void) {
#ifdef __SWITCH__
  g_idx++;
  if (g_idx >= g_nOrder) { chainEnd("complete"); return; }
  stateWrite();
  const char* me = CtSwitchSelfPath();
  if (me && *me) envSetNextLoad(me, me);
  fprintf(stderr, "[bench] chain %s: next backend %s\n", g_chainStamp, backendKey(g_order[g_idx])); fflush(stderr);
#endif
}
void CtBenchChainFailed(const char* why) {
  if (!g_chainOn) return;
  char row[400]; snprintf(row, sizeof row, "%s,%s,%s,FAILED (%s),0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0\n", g_chainStamp, g_name, g_idx >= 0 && g_idx < g_nOrder ? backendKey(g_order[g_idx]) : "?", why ? why : "could not start");
  appendRow(g_chainCsv, row);
  relaunchOrEnd();
}
void CtBenchChainNext(void) { if (g_chainOn) relaunchOrEnd(); }

// A single benchmark run (bench=1 or 2) is a one-shot as well.
void CtBenchOneShotDone(void) { if (!g_chainOn && (CtBenchMode() == 1 || CtBenchMode() == 2)) CtSwitchBoostIniSet("bench", "0"); }
