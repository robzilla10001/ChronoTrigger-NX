// hoststats.c - see hoststats.h.
#include "hoststats.h"
#include "SDL.h"
#include "switch_platform.h"
#include "settings.h"
#include "bench.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct { double sum, max; long n; } Acc;
static Acc  g_part[CT_ST_N], g_work, g_slept;
static long g_frames, g_over, g_hitch, g_skips, g_aud, g_audDrop, g_audZero, g_presents;
static unsigned g_audMin = 0xFFFFFFFFu, g_audMax;
static double g_runT0 = -1.0;
static double g_periodSum; static long g_periodN;
static double g_winT0 = -1.0, g_prevEnd = -1.0, g_period, g_periodMax;
static double g_fp[CT_ST_N];                      // this frame's own part times (for the spike log)
static int g_shownExt;                           // a render thread counts the frames it shows (CtStatsShownFrames)
static double g_lapT = -1.0;
static char g_mode[12] = "?"; static int g_loc = -1; static double g_mark[2] = { -1, -1 };
static char g_line[10][96]; static const char* g_hud[10]; static double g_fpsShown; static int g_resv[6]; static int g_hudN;

static double nowMs(void) { return (double)SDL_GetPerformanceCounter() * 1000.0 / (double)SDL_GetPerformanceFrequency(); }
static void add(Acc* a, double v) { a->sum += v; a->n++; if (v > a->max) a->max = v; }
static double avg(const Acc* a) { return a->n ? a->sum / (double)a->n : 0.0; }
static double perFrame(const Acc* a) { return g_frames ? a->sum / (double)g_frames : 0.0; }

double CtStatsNow(void) { return nowMs(); }

// ON = the counters run (and log): the Switch build always (ct_stats.txt), elsewhere CT_STATS=1, and anywhere the player turns the
// HUD on in the options overlay (the counters cost a few clock reads a frame). The HUD drawing itself is the setting's alone.
static int statsEnv(void);
int CtStatsOn(void) { return statsEnv() || CtSettingsGet()->statsHud != CT_HUD_OFF || CtBenchMode() != 0; }
static int statsEnv(void) {
  static int on = -1;
  if (on < 0) {
#ifdef __SWITCH__
    on = getenv("CT_STATS") ? atoi(getenv("CT_STATS")) != 0 : 1;
#else
    on = getenv("CT_STATS") ? atoi(getenv("CT_STATS")) != 0 : 0;
#endif
  }
  return on;
}

void CtStatsPresent(int part, double ms) {
  if (!CtStatsOn() || part < 0 || part >= CT_ST_N) return;
  add(&g_part[part], ms); g_fp[part] += ms;
  if (part == CT_ST_SWAP && !g_shownExt) g_presents++;
}
void CtStatsResolution(int rw, int rh, int sw, int sh, int ow, int oh) {
  g_resv[0] = rw; g_resv[1] = rh; g_resv[2] = sw; g_resv[3] = sh; g_resv[4] = ow; g_resv[5] = oh;
}
void CtStatsContext(const char* mode, int loc) {
  if (!CtStatsOn()) return;
  snprintf(g_mode, sizeof g_mode, "%s", mode ? mode : "?"); g_loc = loc;
}
void CtStatsMark(int which) {
  if (!CtStatsOn() || which < 0 || which > 1) return;
  g_mark[which] = nowMs();
  if (which == 0) g_lapT = g_mark[0];
}
void CtStatsReset(void) {
  for (int i = 0; i < CT_ST_N; i++) { g_part[i] = (Acc){0}; g_fp[i] = 0; }
  g_work = (Acc){0}; g_slept = (Acc){0};
  g_frames = g_over = g_hitch = g_skips = g_presents = g_aud = g_audDrop = g_audZero = 0;
  g_audMin = 0xFFFFFFFFu; g_audMax = 0; g_periodMax = 0; g_period = 0; g_periodSum = 0; g_periodN = 0;
  g_mark[0] = g_mark[1] = -1.0; g_lapT = -1.0; g_winT0 = g_runT0 = -1.0; g_prevEnd = -1.0;                  // (the next frame starts the window and the run clock again)
}
void CtStatsClockReset(void) { g_prevEnd = nowMs(); g_mark[0] = g_mark[1] = -1.0; g_lapT = -1.0; for (int i = 0; i < CT_ST_N; i++) g_fp[i] = 0; }
void CtStatsLap(int part) {
  if (!CtStatsOn() || g_lapT < 0) return;
  double t = nowMs(); add(&g_part[part], t - g_lapT); g_lapT = t;
}
void CtStatsShownFrames(long n) { if (!CtStatsOn()) return; g_shownExt = 1; g_presents += n; }
void CtStatsPresentSkipped(void) { if (CtStatsOn()) g_skips++; }
void CtStatsAudio(unsigned q, int dropped) {
  if (!CtStatsOn()) return;
  g_aud++; if (dropped) g_audDrop++; if (q == 0) g_audZero++;
  if (q < g_audMin) g_audMin = q;
  if (q > g_audMax) g_audMax = q;
}

#ifdef __SWITCH__
static const char* statsPath(void) { static char p[256]; if (!p[0]) CtRunPath(p, sizeof p, "ct_stats", "txt"); return p; }   // one file per run, named after the program and the start time
#endif

static void roll(double now) {
  double secs = (now - g_winT0) / 1000.0;
  double fps = g_frames / secs, pfps = g_presents / secs;
  g_fpsShown = pfps;
  snprintf(g_line[0], sizeof g_line[0], "ENGINE %.1f FPS  SHOWN %.1f FPS", fps, pfps);
  snprintf(g_line[1], sizeof g_line[1], "FRAME %.1f AVG %.1f MAX MS", g_period, g_periodMax);
  snprintf(g_line[2], sizeof g_line[2], "WORK %.1f AVG %.1f MAX MS  SLEEP %.1f", avg(&g_work), g_work.max, avg(&g_slept));
  snprintf(g_line[3], sizeof g_line[3], "AUDIO %.1f PICT %.1f TITLE %.1f UP %.1f SWAP %.1f MS", perFrame(&g_part[CT_ST_AUDIO]), perFrame(&g_part[CT_ST_PICTURE]), perFrame(&g_part[CT_ST_TITLE]), avg(&g_part[CT_ST_UPLOAD]), avg(&g_part[CT_ST_SWAP]));
  snprintf(g_line[4], sizeof g_line[4], "OVER 16.7 MS %ld  OVER 33 MS %ld  SKIP %ld  ARENA %.1f FX %.1f", g_over, g_hitch, g_skips,
           perFrame(&g_part[CT_ST_BWARENA]), perFrame(&g_part[CT_ST_BWFX]));
  snprintf(g_line[5], sizeof g_line[5], "AUDIO Q %u-%u B DROP %ld EMPTY %ld", g_audMin == 0xFFFFFFFFu ? 0 : g_audMin, g_audMax, g_audDrop, g_audZero);
#ifdef __SWITCH__
  snprintf(g_line[6], sizeof g_line[6], "CPU %d GPU %d MEM %d MHZ  %s  LOC %03X  LOGIC %.1f COMPOSE %.1f MS", CtSwitchCpuMHz(), CtSwitchGpuMHz(), CtSwitchMemMHz(), g_mode,
           g_loc < 0 ? 0 : g_loc, perFrame(&g_part[CT_ST_LOGIC]), perFrame(&g_part[CT_ST_COMPOSE]));
#else
  snprintf(g_line[6], sizeof g_line[6], "%s  LOC %03X  LOGIC %.1f COMPOSE %.1f MS", g_mode,
           g_loc < 0 ? 0 : g_loc, perFrame(&g_part[CT_ST_LOGIC]), perFrame(&g_part[CT_ST_COMPOSE]));
#endif
  for (int i = 0; i < 7; i++) g_hud[i] = g_line[i];
  g_hudN = 7;
  fprintf(stderr, "[stats] %dx%d>%dx%d %s %s loc %03X logic %.2f compose %.2f | engine %.1f f/s shown %.1f f/s | frame avg %.2f max %.1f ms | work avg %.2f max %.1f slept avg %.2f | "
          "x vm %.2f bat %.2f load %.2f enter %.2f bake %.2f bake3 %.2f layers %.2f sndload %.2f bakepct %.0f arena %.2f fx %.2f laps base %.2f fx %.2f wide %.2f cmb %.2f hud %.2f asm %.2f rt %.2f/%.2f (up %.2f) audio %.2f picture %.2f title %.2f upload %.2f draw %.2f swap %.2f (max %.1f) ajoin %.2f misc %.2f tirender %.2f tiup %.2f timode %.2f ms/frame | over16.7 %ld over33 %ld skippedPresents %ld | "
          "audio q %u..%u B dropped %ld empty %ld\n",
          g_resv[0], g_resv[1], g_resv[2], g_resv[3], CtSwitchClockInfo(), g_mode, g_loc < 0 ? 0 : g_loc, perFrame(&g_part[CT_ST_LOGIC]), perFrame(&g_part[CT_ST_COMPOSE]),
          fps, pfps, g_period, g_periodMax, avg(&g_work), g_work.max, avg(&g_slept),
          perFrame(&g_part[CT_ST_XVM]), perFrame(&g_part[CT_ST_XBAT]), perFrame(&g_part[CT_ST_XLOAD]), perFrame(&g_part[CT_ST_XENTER]), perFrame(&g_part[CT_ST_XBAKE]), perFrame(&g_part[CT_ST_XBAKEB3]), perFrame(&g_part[CT_ST_XLAYERS]), perFrame(&g_part[CT_ST_XAUD]), avg(&g_part[CT_ST_XBAKEPCT]), perFrame(&g_part[CT_ST_BWARENA]), perFrame(&g_part[CT_ST_BWFX]), perFrame(&g_part[CT_ST_LBASE]), perFrame(&g_part[CT_ST_LFX]), perFrame(&g_part[CT_ST_LWIDE]), perFrame(&g_part[CT_ST_LCMB]), perFrame(&g_part[CT_ST_LHUD]), perFrame(&g_part[CT_ST_LASM]), perFrame(&g_part[CT_ST_RWORK]), perFrame(&g_part[CT_ST_RWAIT]), perFrame(&g_part[CT_ST_RUP]), perFrame(&g_part[CT_ST_AUDIO]), perFrame(&g_part[CT_ST_PICTURE]), perFrame(&g_part[CT_ST_TITLE]), avg(&g_part[CT_ST_UPLOAD]), avg(&g_part[CT_ST_DRAW]), avg(&g_part[CT_ST_SWAP]), g_part[CT_ST_SWAP].max, perFrame(&g_part[CT_ST_XJOIN]), perFrame(&g_part[CT_ST_XMISC]), perFrame(&g_part[CT_ST_TIRENDER]), perFrame(&g_part[CT_ST_TIUP]), avg(&g_part[CT_ST_TIMODE]),
          g_over, g_hitch, g_skips, g_audMin == 0xFFFFFFFFu ? 0 : g_audMin, g_audMax, g_audDrop, g_audZero);
  fflush(stderr);
#ifdef __SWITCH__
  { // its own file, opened and closed each second: it does not depend on how stdout/stderr were redirected
    FILE* sf = fopen(statsPath(), "a");
    if (sf) {
      fprintf(sf, "%.0f s: render %dx%d shown %dx%d | %s | %s loc %03X logic %.2f compose %.2f | engine %.1f shown %.1f | frame avg %.2f max %.1f | work avg %.2f max %.1f slept %.2f | "
              "x vm %.2f bat %.2f load %.2f enter %.2f bake %.2f bake3 %.2f layers %.2f sndload %.2f bakepct %.0f arena %.2f fx %.2f laps base %.2f fx %.2f wide %.2f cmb %.2f hud %.2f asm %.2f rt %.2f/%.2f (up %.2f) audio %.2f picture %.2f title %.2f upload %.2f draw %.2f swap %.2f (max %.1f) ajoin %.2f misc %.2f tirender %.2f tiup %.2f timode %.2f | over16.7 %ld over33 %ld skips %ld | audio q %u..%u drop %ld empty %ld\n",
              (now - g_runT0) / 1000.0, g_resv[0], g_resv[1], g_resv[2], g_resv[3], CtSwitchClockInfo(), g_mode, g_loc < 0 ? 0 : g_loc, perFrame(&g_part[CT_ST_LOGIC]), perFrame(&g_part[CT_ST_COMPOSE]), fps, pfps, g_period, g_periodMax, avg(&g_work), g_work.max, avg(&g_slept),
              perFrame(&g_part[CT_ST_XVM]), perFrame(&g_part[CT_ST_XBAT]), perFrame(&g_part[CT_ST_XLOAD]), perFrame(&g_part[CT_ST_XENTER]), perFrame(&g_part[CT_ST_XBAKE]), perFrame(&g_part[CT_ST_XBAKEB3]), perFrame(&g_part[CT_ST_XLAYERS]), perFrame(&g_part[CT_ST_XAUD]), avg(&g_part[CT_ST_XBAKEPCT]), perFrame(&g_part[CT_ST_BWARENA]), perFrame(&g_part[CT_ST_BWFX]), perFrame(&g_part[CT_ST_LBASE]), perFrame(&g_part[CT_ST_LFX]), perFrame(&g_part[CT_ST_LWIDE]), perFrame(&g_part[CT_ST_LCMB]), perFrame(&g_part[CT_ST_LHUD]), perFrame(&g_part[CT_ST_LASM]), perFrame(&g_part[CT_ST_RWORK]), perFrame(&g_part[CT_ST_RWAIT]), perFrame(&g_part[CT_ST_RUP]), perFrame(&g_part[CT_ST_AUDIO]), perFrame(&g_part[CT_ST_PICTURE]), perFrame(&g_part[CT_ST_TITLE]), avg(&g_part[CT_ST_UPLOAD]), avg(&g_part[CT_ST_DRAW]), avg(&g_part[CT_ST_SWAP]), g_part[CT_ST_SWAP].max, perFrame(&g_part[CT_ST_XJOIN]), perFrame(&g_part[CT_ST_XMISC]), perFrame(&g_part[CT_ST_TIRENDER]), perFrame(&g_part[CT_ST_TIUP]), avg(&g_part[CT_ST_TIMODE]),
              g_over, g_hitch, g_skips, g_audMin == 0xFFFFFFFFu ? 0 : g_audMin, g_audMax, g_audDrop, g_audZero);
      fclose(sf);
    }
  }
#endif
#ifdef __SWITCH__
  CtBenchSecond(g_mode, g_loc, fps, pfps, g_period, g_periodMax, avg(&g_work), CtSwitchCpuMHz(), CtSwitchGpuMHz(), CtSwitchMemMHz(), g_audDrop, g_audZero);
#else
  CtBenchSecond(g_mode, g_loc, fps, pfps, g_period, g_periodMax, avg(&g_work), 0, 0, 0, g_audDrop, g_audZero);
#endif
  for (int i = 0; i < CT_ST_N; i++) g_part[i] = (Acc){0};
  g_work = (Acc){0}; g_slept = (Acc){0};
  g_frames = g_over = g_hitch = g_skips = g_presents = g_aud = g_audDrop = g_audZero = 0;
  g_audMin = 0xFFFFFFFFu; g_audMax = 0; g_periodMax = 0; g_period = 0;
  g_winT0 = now;
}

void CtStatsFrame(double workMs, double sleptMs) {
  if (!CtStatsOn()) return;
  double now = nowMs();
  if (g_winT0 < 0) { g_winT0 = g_runT0 = now; g_prevEnd = now; return; }
  add(&g_work, workMs); add(&g_slept, sleptMs);
  g_frames++;
  if (workMs > 1000.0 / 60.0) g_over++;
  if (workMs > 2000.0 / 60.0) g_hitch++;
  if (g_mark[0] >= g_prevEnd) {                      // this frame marked its picture build: logic / compose
    add(&g_part[CT_ST_LOGIC], g_mark[0] - g_prevEnd); g_fp[CT_ST_LOGIC] = g_mark[0] - g_prevEnd;
    if (g_mark[1] >= g_mark[0]) { add(&g_part[CT_ST_COMPOSE], g_mark[1] - g_mark[0]); g_fp[CT_ST_COMPOSE] = g_mark[1] - g_mark[0]; }
  }
  if (workMs > 40.0 && g_runT0 >= 0) {               // THE SPIKE LOG: a frame this long is one the player sees; say what was in it
    char b[640];
    snprintf(b, sizeof b, "SPIKE %.1fs %s loc %03X work %.1f ms | logic %.1f (vm %.1f battle %.1f load %.1f enter %.1f [boot pre %.1f stage %.1f lists %.1f rest %.1f strip %.1f] bake %.1f bake3 %.1f layers %.1f sndload %.1f) compose %.1f (base %.1f fx %.1f wide %.1f cmb %.1f hud %.1f asm %.1f) picture %.1f title %.1f audio %.1f convert %.1f swap %.1f ajoin %.1f misc %.1f",
             (now - g_runT0) / 1000.0, g_mode, g_loc < 0 ? 0 : g_loc, workMs, g_fp[CT_ST_LOGIC], g_fp[CT_ST_XVM], g_fp[CT_ST_XBAT], g_fp[CT_ST_XLOAD], g_fp[CT_ST_XENTER], g_fp[CT_ST_XB1], g_fp[CT_ST_XB2], g_fp[CT_ST_XB3], g_fp[CT_ST_XB4], g_fp[CT_ST_XB5], g_fp[CT_ST_XBAKE], g_fp[CT_ST_XBAKEB3], g_fp[CT_ST_XLAYERS], g_fp[CT_ST_XAUD],
             g_fp[CT_ST_COMPOSE], g_fp[CT_ST_LBASE], g_fp[CT_ST_LFX], g_fp[CT_ST_LWIDE], g_fp[CT_ST_LCMB], g_fp[CT_ST_LHUD], g_fp[CT_ST_LASM],
             g_fp[CT_ST_PICTURE], g_fp[CT_ST_TITLE], g_fp[CT_ST_AUDIO], g_fp[CT_ST_UPLOAD], g_fp[CT_ST_SWAP], g_fp[CT_ST_XJOIN], g_fp[CT_ST_XMISC]);
    fprintf(stderr, "[stats] %s\n", b); fflush(stderr);
#ifdef __SWITCH__
    { FILE* sf = fopen(statsPath(), "a"); if (sf) { fprintf(sf, "%s\n", b); fclose(sf); } }
#endif
  }
  double per = now - g_prevEnd; g_prevEnd = now;
  CtBenchFrame(g_mode, per, workMs, g_fp[CT_ST_UPLOAD], g_fp[CT_ST_DRAW], g_fp[CT_ST_SWAP], g_fp[CT_ST_LOGIC], g_fp[CT_ST_COMPOSE], g_fp[CT_ST_LHUD], g_fp[CT_ST_RWORK], g_fp[CT_ST_RUP], g_fp[CT_ST_RWAIT]);
  for (int i = 0; i < CT_ST_N; i++) g_fp[i] = 0;
  g_mark[0] = g_mark[1] = -1;
  g_periodSum += per; g_periodN++;
  if (per > g_periodMax) g_periodMax = per;
  if (now - g_winT0 >= 1000.0) {
    g_period = g_periodN ? g_periodSum / (double)g_periodN : 0.0; g_periodSum = 0; g_periodN = 0;
    roll(now);
  }
}

static const char* const* g_forceLines; static int g_forceN;
void CtStatsHudForce(const char* const* lines, int n) { g_forceLines = lines; g_forceN = lines ? n : 0; }
const char* const* CtStatsHud(int* n) {
  if (g_forceLines && g_forceN > 0) { if (n) *n = g_forceN; return g_forceLines; }
  const int mode = CtSettingsGet()->statsHud;
  if (mode == CT_HUD_OFF || !g_hudN) { if (n) *n = 0; return NULL; }
  if (mode == CT_HUD_FPS) {                           // the frame rate alone: the pictures shown a second
    snprintf(g_line[9], sizeof g_line[9], "FPS %.1f", g_fpsShown);
    g_hud[0] = g_line[9];
    if (n) *n = 1;
    return g_hud;
  }
  const CtSettings* st = CtSettingsGet();
  static const char* const wide[] = { "OFF", "16:9", "16:10", "21:9" };
  static const char* const scl[] = { "FIT", "INTEGER", "STRETCH" };
  snprintf(g_line[7], sizeof g_line[7], "RENDER %dX%d  SCALE %dX  WIDE %s  BETWEEN %s", g_resv[0], g_resv[1], st->renderScale,
           wide[st->widescreen & 3], st->interpolate ? "ON" : "OFF");
  snprintf(g_line[8], sizeof g_line[8], "SHOWN %dX%d IN %dX%d  %s %s %s", g_resv[2], g_resv[3], g_resv[4], g_resv[5],
           scl[st->scaleMode % 3], st->aspect == CT_ASPECT_4_3 ? "4:3" : "8:7", st->linearFilter ? "LINEAR" : "NEAREST");
  for (int i = 0; i < 9; i++) g_hud[i] = g_line[i];          // (the FPS-only path reuses slot 0: set them all, every call)
  if (n) *n = 9;
  return g_hud;
}
