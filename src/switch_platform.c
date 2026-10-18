// switch_platform.c - see switch_platform.h.
#ifdef __SWITCH__
#include "switch_platform.h"
#include "settings.h"
#include "present.h"
#include "bench.h"
#include <switch.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

#define CT_SWITCH_HOME "sdmc:/switch/ct"
static char g_argv0[300];

static FILE* g_log;
static int logWrite(void* cookie, const char* buf, _READ_WRITE_BUFSIZE_TYPE n) {
  (void)cookie;
  size_t w = g_log ? fwrite(buf, 1, (size_t)n, g_log) : (size_t)n;
  if (g_log) fflush(g_log);
  return (int)w;
}

// The log file is closed at exit. Every file left open is a file-system session the homebrew loader's process keeps for the next launch, and a benchmark pass or the leak test relaunches
// through that one process: forty launches that each left one open ended in the loader's 2001-0132 (chain_probe, openlog=1). (Registered first, so it runs last.)
static void logClose(void) { FILE* f = g_log; if (!f) return; fflush(f); g_log = NULL; fclose(f); }
// ---- THE CLOCK PROBES (hoststats): the same fixed loop timed on the main thread's core and, by three small low-priority threads
// pinned one per app core, on all of them. A throttled CPU clock slows every core together; a core shared with another thread
// slows only that core.
static volatile unsigned g_benchSink;
static unsigned benchLoopUs(void) {
  u64 t0 = armGetSystemTick();
  unsigned x = 12345u;
  for (int i = 0; i < 400000; i++) x = x * 1664525u + 1013904223u;
  g_benchSink = x;
  return (unsigned)((armGetSystemTick() - t0) * 1000000ull / armGetSystemTickFreq());
}
static Thread g_probeTh[3]; static volatile unsigned g_probeGen, g_probeUs[3]; static int g_probesUp; static volatile int g_probeQuit;
static void probeMain(void* arg) {
  int core = (int)(intptr_t)arg; unsigned seen = 0;
  for (;;) {
    while (g_probeGen == seen) { if (g_probeQuit) return; svcSleepThread(20000000ll); }
    seen = g_probeGen;
    unsigned best = 0xFFFFFFFFu;
    for (int k = 0; k < 3; k++) { unsigned us = benchLoopUs(); if (us < best) best = us; }   // (min of 3: main's own wakeups inflate one)
    g_probeUs[core] = best;
  }
}
static void probesStart(void) {
  for (int c = 0; c < 3; c++)
    if (R_SUCCEEDED(threadCreate(&g_probeTh[c], probeMain, (void*)(intptr_t)c, NULL, 0x4000, 0x3B, c))) threadStart(&g_probeTh[c]);
  g_probesUp = 1;
}
// ct_boost.ini (next to the .nro): experiments without a rebuild.  boost=0: do not touch the clock (the baseline for a
// measurement); pin=<0-2>: pin the main thread to that app core; async=0: no render thread (present.c).
static int g_optBoost = 1; static int g_optPin = -1; 
#ifdef CT_MESA_UNIFIED
static int g_exitMode = 0;                       // (full teardown now that every thread is joined first: it exits clean from the loader; exit=1 left GPU state the loader could not unmap)
#else
static int g_exitMode = 0;
#endif
static void readBoostIni(void) {
  FILE* f = fopen("ct_boost.ini", "r");
  if (!f) return;
  char line[128];
  while (fgets(line, sizeof line, f)) {
    unsigned v;
    if (sscanf(line, "boost=%u", &v) == 1) g_optBoost = (int)v;
    else if (sscanf(line, "pin=%u", &v) == 1) g_optPin = (int)v;
    else if (sscanf(line, "exit=%u", &v) == 1) g_exitMode = (int)v;
    else if (sscanf(line, "leaktest=%u", &v) == 1) { char b[8]; snprintf(b, sizeof b, "%u", v); setenv("CT_LEAKTEST", b, 1); }
    else if (sscanf(line, "leaktest_n=%u", &v) == 1) { char b[8]; snprintf(b, sizeof b, "%u", v); setenv("CT_LEAKTEST_N", b, 1); }
    else if (sscanf(line, "bench=%u", &v) == 1) { char b[8]; snprintf(b, sizeof b, "%u", v); setenv("CT_BENCH", b, 1); }                // (bench.h)
    else if (sscanf(line, "bench_secs=%u", &v) == 1) { char b[8]; snprintf(b, sizeof b, "%u", v); setenv("CT_BENCH_SECS", b, 1); }
    else if (sscanf(line, "async=%u", &v) == 1) setenv("CT_ASYNC_PRESENT", v ? "1" : "0", 1);   // the render thread (present.c): async=0 turns it off
  }
  fclose(f);
  fprintf(stderr, "[switch] ct_boost.ini: boost=%d pin=%d exit=%d bench=%s bench_secs=%s\n", g_optBoost, g_optPin, g_exitMode, getenv("CT_BENCH") ? getenv("CT_BENCH") : "0 (not set)", getenv("CT_BENCH_SECS") ? getenv("CT_BENCH_SECS") : "default"); fflush(stderr);
}

// ---- THE CPU CLOCK. Homebrew starts at 1020 MHz; the engine needs 1785. Measured on hardware (hoststats): the boost-mode
// request alone holds for 60 s and then lapses for 15 s, every 75 s, whatever the game does. The fix is the one a sibling
// project found (docs/switch-cpu-boost-prompt.md): ask the clock service for the rate directly, READ IT BACK, and keep
// re-asserting it whenever it reads low, at once on any applet event. (An earlier version of this file opened the clkrst session
// with PcvModule_CpuBus -- the wrong enum, clkrst takes PcvModuleId_CpuBus -- so it never reached the service at all.)
#define CT_CPU_HZ 1785000000u
static ClkrstSession g_cpu; static int g_cpuOpen, g_pcv;
static u32 g_origHz;
static volatile int g_boostEvent; static int g_watch, g_poll, g_failStreak, g_reapplied, g_hookEvents, g_fallbacks, g_boostUp;
static Result g_boostRc;
static AppletHookCookie g_hookCookie;
static unsigned cpuMHz(void) {
  u32 hz = 0;
  if (g_cpuOpen) clkrstGetClockRate(&g_cpu, &hz);
  else if (g_pcv) pcvGetClockRate(PcvModule_CpuBus, &hz);
  return hz / 1000000u;
}
static Result cpuSetRate(u32 hz) {            // if the session call fails, close/reopen the session once and retry
  Result rc = 0;
  if (g_cpuOpen) {
    rc = clkrstSetClockRate(&g_cpu, hz);
    if (R_FAILED(rc)) {
      clkrstCloseSession(&g_cpu); g_cpuOpen = 0;
      if (R_SUCCEEDED(clkrstOpenSession(&g_cpu, PcvModuleId_CpuBus, 3))) { g_cpuOpen = 1; rc = clkrstSetClockRate(&g_cpu, hz); }
    }
  } else if (g_pcv) rc = pcvSetClockRate(PcvModule_CpuBus, hz);
  return rc;
}
// THE CLOCK PROFILES (settings.h CT_CLK_*, the options overlay's "Clocks"): CPU / GPU / memory targets, applied live and re-asserted while
// the game runs, each read back. Measured so far: the GPU request works (307 -> 460 MHz, held 223 s) yet the render thread's upload still
// stepped between 3 and 15 ms at that constant GPU clock -- the memory clock is the other governed clock, so it is set as well.
typedef struct { const char* name; u32 cpu, gpu, mem; } ClkProfile;
static const ClkProfile kProf[4] = {
  { "base",   1020000000u,  460800000u, 1331200000u },     // safe, stock: 1x resolution
  { "erista", 1785000000u,  614400000u, 1600000000u },     // boost: 2-4x, generally regarded as safe
  { "mariko", 1785000000u,  768000000u, 1600000000u },
  { "max",    1963500000u,  921600000u, 1600000000u },     // may degrade battery life; a cushion for stability
};
static ClkrstSession g_gpu, g_mem; static int g_gpuOpen, g_memOpen;
static u32 g_gpuOrigHz, g_memOrigHz, g_tgtCpu = CT_CPU_HZ, g_tgtGpu, g_tgtMem;
static int g_prof = -1, g_gpuRe, g_memRe; static unsigned g_gpuMHz, g_memMHz;
static u32 modRead(ClkrstSession* s, int open) { u32 hz = 0; if (open) clkrstGetClockRate(s, &hz); return hz; }
static int within3(u32 hz, u32 want) { return hz + hz / 33 >= want && hz <= want + want / 33; }
static void sessionsOpen(void) {
  if (!g_cpuOpen) return;                                  // (the clock service is clkrst, 8.0.0+)
  Result rg = clkrstOpenSession(&g_gpu, PcvModuleId_GPU, 3); g_gpuOpen = R_SUCCEEDED(rg);
  Result rm = clkrstOpenSession(&g_mem, PcvModuleId_EMC, 3); g_memOpen = R_SUCCEEDED(rm);
  g_gpuOrigHz = modRead(&g_gpu, g_gpuOpen); g_memOrigHz = modRead(&g_mem, g_memOpen);
  g_gpuMHz = g_gpuOrigHz / 1000000u; g_memMHz = g_memOrigHz / 1000000u;
  fprintf(stderr, "[switch] clocks: gpu session rc=0x%X (original %u MHz), memory session rc=0x%X (original %u MHz)\n",
          (unsigned)rg, g_gpuMHz, (unsigned)rm, g_memMHz); fflush(stderr);
}
static int hardwareProfile(void) {                         // Erista (Icosa, Copper) or Mariko (Hoag, Iowa, Calcio, Aula)
  u64 hw = 0; int prof = CT_CLK_ERISTA;
  if (R_SUCCEEDED(splInitialize())) {
    if (R_SUCCEEDED(splGetConfig(SplConfigItem_HardwareType, &hw))) prof = hw >= 2 ? CT_CLK_MARIKO : CT_CLK_ERISTA;
    splExit();
  }
  fprintf(stderr, "[switch] clocks: hardware type %llu -> default profile %s\n", (unsigned long long)hw, kProf[prof].name); fflush(stderr);
  return prof;
}
static void applyProfile(int p) {
  const ClkProfile* P = &kProf[p];
  g_tgtCpu = P->cpu; g_tgtGpu = P->gpu; g_tgtMem = P->mem;
  Result rc = cpuSetRate(g_tgtCpu), rg = 0, rm = 0;
  if (g_gpuOpen) rg = clkrstSetClockRate(&g_gpu, g_tgtGpu);
  if (g_memOpen) rm = clkrstSetClockRate(&g_mem, g_tgtMem);
  g_gpuMHz = modRead(&g_gpu, g_gpuOpen) / 1000000u; g_memMHz = modRead(&g_mem, g_memOpen) / 1000000u;
  fprintf(stderr, "[switch] clocks: profile %s -> cpu %u gpu %u mem %u MHz requested; read back cpu %u gpu %u mem %u MHz (rc cpu 0x%X gpu 0x%X mem 0x%X)\n",
          P->name, (unsigned)(P->cpu / 1000000u), (unsigned)(P->gpu / 1000000u), (unsigned)(P->mem / 1000000u), cpuMHz(), g_gpuMHz, g_memMHz,
          (unsigned)rc, (unsigned)rg, (unsigned)rm);
  fflush(stderr);
  g_prof = p;
}
static void modMaintain(void) {                            // GPU and memory: read back, re-assert when off target
  if (g_gpuOpen && g_tgtGpu) {
    u32 hz = modRead(&g_gpu, 1); g_gpuMHz = hz / 1000000u;
    if (!within3(hz, g_tgtGpu)) { clkrstSetClockRate(&g_gpu, g_tgtGpu); g_gpuRe++; }
  }
  if (g_memOpen && g_tgtMem) {
    u32 hz = modRead(&g_mem, 1); g_memMHz = hz / 1000000u;
    if (!within3(hz, g_tgtMem)) { clkrstSetClockRate(&g_mem, g_tgtMem); g_memRe++; }
  }
}
static void boostHook(AppletHookType type, void* param) { (void)type; (void)param; g_boostEvent = 1; g_hookEvents++; }
void CtSwitchBoostNudge(void) { g_watch = 300; }          // content loaded/closed: the clock tends to drop around those
void CtSwitchBoostFrame(void) {                            // once per main-loop frame
  if (!g_boostUp) return;
  int event = g_boostEvent;
  if (event) { g_boostEvent = 0; g_watch = 180; }          // watch closely for ~3 s after any system event
  int watching = g_watch > 0;
  if (watching) g_watch--;
  if (!event && ++g_poll < (watching ? 2 : 4)) return;     // otherwise ~15 Hz
  g_poll = 0;
  { CtSettings* st = CtSettingsGet();                      // the player's profile (the options overlay's Clocks)
    if (st->clocks < 0) st->clocks = hardwareProfile();    //   not chosen yet: by hardware, and the dropdown shows it
    int p = st->clocks > 3 ? 3 : st->clocks;
    if (p != g_prof) applyProfile(p); }
  modMaintain();
  { unsigned now = cpuMHz(), want = g_tgtCpu / 1000000u;
    if (now * 100u >= want * 97u && now * 100u <= want * 103u) { g_failStreak = 0; return; }   // on target
    g_boostRc = cpuSetRate(g_tgtCpu); g_reapplied++;
    now = cpuMHz();
    if (g_tgtCpu >= 1500000000u && now < 1500) {           // accepted but did not stick: the applet manager's boost as well
      if (++g_failStreak >= 3) { g_failStreak = 0; appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad); g_fallbacks++; }
    } else g_failStreak = 0; }
}
static int apmUp = -1;
static void boostShutdown(void) {
  CtSwitchLogResources("atexit: before the boost shutdown");
  if (apmUp == 1) { apmExit(); apmUp = 0; }            // (a session left open per run adds up across the relaunches of a benchmark pass)
  if (!g_boostUp) return;
  appletUnhook(&g_hookCookie);
  if (g_gpuOpen) { if (g_gpuOrigHz) clkrstSetClockRate(&g_gpu, g_gpuOrigHz); clkrstCloseSession(&g_gpu); g_gpuOpen = 0; }
  if (g_memOpen) { if (g_memOrigHz) clkrstSetClockRate(&g_mem, g_memOrigHz); clkrstCloseSession(&g_mem); g_memOpen = 0; }
  if (g_origHz) cpuSetRate(g_origHz);
  if (g_cpuOpen) { clkrstCloseSession(&g_cpu); g_cpuOpen = 0; clkrstExit(); }
  else if (g_pcv) { g_pcv = 0; pcvExit(); }
  g_boostUp = 0;
  CtSwitchLogResources("atexit: after the boost shutdown"); fflush(stderr);
}
static void boostInit(void) {
  if (!g_optBoost) { fprintf(stderr, "[switch] boost: off (ct_boost.ini boost=0)\n"); fflush(stderr); return; }
  Result rcInit = 0, rcOpen = 0, rcSet = 0;
  const char* api = "clkrst";
  if (hosversionAtLeast(8, 0, 0)) {
    rcInit = clkrstInitialize();
    if (R_SUCCEEDED(rcInit)) { rcOpen = clkrstOpenSession(&g_cpu, PcvModuleId_CpuBus, 3); g_cpuOpen = R_SUCCEEDED(rcOpen); }
  } else {
    api = "pcv";
    rcInit = pcvInitialize(); g_pcv = R_SUCCEEDED(rcInit);
  }
  g_origHz = 0;
  { u32 hz = 0;
    if (g_cpuOpen) clkrstGetClockRate(&g_cpu, &hz); else if (g_pcv) pcvGetClockRate(PcvModule_CpuBus, &hz);
    g_origHz = hz; }
  unsigned before = cpuMHz();
  if (g_cpuOpen || g_pcv) rcSet = cpuSetRate(CT_CPU_HZ);
  unsigned after = cpuMHz();
  if ((g_cpuOpen || g_pcv) && after < 1500) { appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad); after = cpuMHz(); }
  fprintf(stderr, "[switch] boost init (%s): init rc=0x%X open rc=0x%X set rc=0x%X; clock %u -> %u MHz (original %u)\n",
          api, (unsigned)rcInit, (unsigned)rcOpen, (unsigned)rcSet, before, after, (unsigned)(g_origHz / 1000000u));
  fflush(stderr);
  if (!g_cpuOpen && !g_pcv) {                                // no clock service at all: the boost mode request is all there is
    appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
    fprintf(stderr, "[switch] boost: no clock service, boost-mode request only\n"); fflush(stderr);
  }
  sessionsOpen();
  appletHook(&g_hookCookie, boostHook, NULL);
  g_boostUp = 1; g_watch = 300;
  atexit(boostShutdown);
}

static char g_clockInfo[160] = "";
const char* CtSwitchClockInfo(void) { return g_clockInfo; }
static void clockSample(void) {
  if (apmUp < 0) apmUp = R_SUCCEEDED(apmInitialize()) ? 1 : 0;
  unsigned us = benchLoopUs();                       // 400k dependent integer ops: ~0.6 ms boosted, ~1.2 ms at 1020 MHz
  ApmPerformanceMode pm = ApmPerformanceMode_Invalid; u32 conf = 0;
  if (apmUp) { apmGetPerformanceMode(&pm); apmGetPerformanceConfiguration(pm, &conf); }
  static unsigned probeTick;
  if (g_probesUp && (probeTick++ % 3) == 0) g_probeGen++;                // the helpers re-measure about once a second
  snprintf(g_clockInfo, sizeof g_clockInfo, "cpu %uMHz gpu %uMHz mem %uMHz clk=%s gre=%d mre=%d bench %uus core%u [%u %u %u] re=%d ev=%d fb=%d rc=0x%X apm %d/0x%08X",
           cpuMHz(), g_gpuMHz, g_memMHz, g_prof >= 0 ? kProf[g_prof].name : "-", g_gpuRe, g_memRe, us, (unsigned)svcGetCurrentProcessorNumber(), g_probeUs[0], g_probeUs[1], g_probeUs[2],
           g_reapplied, g_hookEvents, g_fallbacks, (unsigned)g_boostRc, (int)pm, (unsigned)conf);
}

void CtSwitchClockTick(void) { clockSample(); }          // (the bench + status line, ~3 times a second)
int CtSwitchCpuMHz(void) { return (int)cpuMHz(); }
int CtSwitchGpuMHz(void) { return (int)g_gpuMHz; }
int CtSwitchMemMHz(void) { return (int)g_memMHz; }

void CtSwitchTrace(const char* what) { fprintf(stderr, "[switch] %s\n", what); fflush(stderr); }

void CtSwitchInit(int* argc, char*** argv) {
  if (*argc > 0 && (*argv)[0]) snprintf(g_argv0, sizeof g_argv0, "%s", (*argv)[0]);
  static char* defArgv[] = { "ct_field", "--assets", "ct_assets.dat", "--boot", NULL };
  // The homebrew menu launches "sdmc:/switch/ct/ct_field.nro" (argv[0]); take its folder, else the default one.
  char dir[256]; snprintf(dir, sizeof dir, "%s", CT_SWITCH_HOME);
  if (*argc > 0 && (*argv)[0] && !strncmp((*argv)[0], "sdmc:/", 6)) {
    snprintf(dir, sizeof dir, "%s", (*argv)[0]);
    char* slash = strrchr(dir, '/'); if (slash) *slash = 0;
  }
  mkdir("sdmc:/switch", 0777); mkdir(dir, 0777);
  if (chdir(dir) != 0) chdir("sdmc:/");
  // ONE FILE for both streams. Two FILEs on one file keep two offsets, and the SD card's driver does not append per write:
  // they overwrote each other. funopen gives stdout and stderr the same sink, a single handle that writes in order.
  char logPath[256]; CtRunPath(logPath, sizeof logPath, "ct_log", "txt");        // a NEW log for every run: logs/ct_log_<program>_<start time>.txt
  g_log = fopen(logPath, "w");
  if (g_log) atexit(logClose);
  if (g_log) {
    FILE* sink = funopen(NULL, NULL, logWrite, NULL, NULL);
    if (sink) { stdout = sink; stderr = sink; }
  }
  { char sp[256]; CtRunPath(sp, sizeof sp, "ct_stats", "txt"); FILE* t = fopen(sp, "w"); if (t) fclose(t); }   // (hoststats.c appends to it once a second)
  setvbuf(stdout, NULL, _IOLBF, 0);
  { char b[300]; snprintf(b, sizeof b, "init: folder %s argc %d, log %s", dir, *argc, logPath); CtSwitchTrace(b); }
  { u32 hv = hosversionGet(); char b[200]; snprintf(b, sizeof b, "run: program %s, started %s, firmware %u.%u.%u, applet type %d", CtRunName(), CtRunStamp(), HOSVER_MAJOR(hv), HOSVER_MINOR(hv), HOSVER_MICRO(hv), (int)appletGetAppletType());
    CtSwitchTrace(b); CtBenchInfo(b); }
  // THE FILES THE ENGINE NEEDS, checked before anything starts: ct_assets.dat (the field and menu) and the ROM (battles,
  // the sound driver, scene loads: CtRomImage exits when it is absent). A missing one is a dialog that names it, not a
  // silent return to the launcher.
  static const char* need[] = { "ct_assets.dat", "Chrono_Trigger_USA_SNES.sfc" };
  for (int i = 0; i < 2; i++) {
    FILE* t = fopen(need[i], "rb");
    if (t) { fclose(t); continue; }
    char full[300], det[600];
    snprintf(full, sizeof full, "Missing file: %s\nin folder: %s", need[i], dir);
    snprintf(det, sizeof det, "%s\n\nPut ct_assets.dat and Chrono_Trigger_USA_SNES.sfc (your own dump, see docs/SWITCH.md) "
             "next to the .nro.", full);
    CtSwitchTrace(full);
    ErrorSystemConfig e;
    if (R_SUCCEEDED(errorSystemCreate(&e, "Chrono Trigger cannot start", det))) errorSystemShow(&e);
    exit(1);
  }
  // The CPU at its boost clock (1785 MHz vs the default 1020): the engine is CPU-bound here (hoststats: 19-41 ms a frame).
  readBoostIni();
  if (g_optPin >= 0 && g_optPin <= 2) {
    Result r = svcSetThreadCoreMask(CUR_THREAD_HANDLE, g_optPin, 1u << g_optPin);
    fprintf(stderr, "[switch] main thread pinned to core %d: rc=0x%X\n", g_optPin, (unsigned)r); fflush(stderr);
  }
  probesStart();
  boostInit();
  CtSwitchClockTick();
  CtSwitchLogResources("after init + clocks");
  if (*argc <= 1) { *argc = 4; *argv = defArgv; }
}

// ---- the dummy video driver's stand-ins (switch_platform.h) ------------------------------------------------------------------------------------------
static int g_noWindow, g_touchInit;
void CtSwitchNoWindow(void) { g_noWindow = 1; }          // (no window is created: see switch_platform.h)
int  CtSwitchNoWindowMode(void) { return g_noWindow; }
// The touch screen, as the console's video driver reports it: a finger down / moved / up, positions normalised to the 1280x720 screen.
static struct { u32 id; float x, y; int live; } g_fing[16];
static void touchPoll(void) {
  if (!g_touchInit) { hidInitializeTouchScreen(); g_touchInit = 1; }
  HidTouchScreenState st; memset(&st, 0, sizeof st);
  if (!hidGetTouchScreenStates(&st, 1)) return;
  int seen[16] = { 0 };
  for (int i = 0; i < st.count && i < 16; i++) {
    float x = (float)st.touches[i].x / 1280.0f, y = (float)st.touches[i].y / 720.0f; u32 id = st.touches[i].finger_id;
    int slot = -1; for (int k = 0; k < 16; k++) if (g_fing[k].live && g_fing[k].id == id) { slot = k; break; }
    SDL_Event e; memset(&e, 0, sizeof e);
    e.tfinger.touchId = 1; e.tfinger.fingerId = (SDL_FingerID)id + 1; e.tfinger.x = x; e.tfinger.y = y; e.tfinger.pressure = 1.0f;
    if (slot < 0) {
      for (int k = 0; k < 16; k++) if (!g_fing[k].live) { slot = k; break; }
      if (slot < 0) continue;
      g_fing[slot].live = 1; g_fing[slot].id = id; g_fing[slot].x = x; g_fing[slot].y = y; seen[slot] = 1;
      e.type = SDL_FINGERDOWN; SDL_PushEvent(&e);
    } else {
      seen[slot] = 1;
      if (g_fing[slot].x != x || g_fing[slot].y != y) { e.type = SDL_FINGERMOTION; e.tfinger.dx = x - g_fing[slot].x; e.tfinger.dy = y - g_fing[slot].y; SDL_PushEvent(&e); }
      g_fing[slot].x = x; g_fing[slot].y = y;
    }
  }
  for (int k = 0; k < 16; k++) if (g_fing[k].live && !seen[k]) {
    SDL_Event e; memset(&e, 0, sizeof e); e.type = SDL_FINGERUP; e.tfinger.touchId = 1; e.tfinger.fingerId = (SDL_FingerID)g_fing[k].id + 1;
    e.tfinger.x = g_fing[k].x; e.tfinger.y = g_fing[k].y; SDL_PushEvent(&e); g_fing[k].live = 0;
  }
}
int CtSwitchPollEvent(SDL_Event* e) {
  if (g_noWindow) {
    static Uint32 last = 0xFFFFFFFFu; Uint32 now = SDL_GetTicks();
    if (now != last) {                                      // once per millisecond at most: a drain loop calls this many times
      last = now;
      if (!appletMainLoop()) { SDL_Event q; memset(&q, 0, sizeof q); q.type = SDL_QUIT; SDL_PushEvent(&q); }
      touchPoll();
    }
  }
  return SDL_PollEvent(e);
}
void CtSwitchBackendTryBegin(void) { FILE* f = fopen("ct_backend_try", "w"); if (f) { fputs("1", f); fclose(f); } }
void CtSwitchBackendTryEnd(void) { remove("ct_backend_try"); }
int  CtSwitchBackendTryPending(void) { FILE* f = fopen("ct_backend_try", "r"); if (!f) return 0; fclose(f); return 1; }
const char* CtSwitchSelfPath(void) { return g_argv0; }
// WHAT THE PROCESS HOLDS, in the log. (The kernel's resource-limit handle is not available to an application: svcGetInfo says so once, in the first line.) A program that is relaunched
// through the homebrew loader several times (a benchmark pass) shares one process, so what one run leaves mapped or open is what the next, and the launcher after the last, has less of
// ("2001-0132", out of resource, in the loader at the end of a pass). Walking the address space with svcQueryMemory shows leaked mappings by TYPE: the heap, aliases (memory mapped
// elsewhere), stacks and thread-local pages (one stack per live thread), shared and transfer memory, I/O mappings. Memory used / total is the kernel's own count.
// (An application's kernel budget cannot be read -- svcGetInfo ResourceLimit returns 0xE401 -- and asking the kernel for one more session or event is refused at once, at the start of
// every launch and after the full teardown alike (2001-0132, measured in a forwarder-launched application): the loader's process has no spare ones. See docs/BENCHMARK.md.)
void CtSwitchLogResources(const char* tag) {
  static int limitTried;
  if (!limitTried) {
    limitTried = 1; u64 h = 0; Result rc = svcGetInfo(&h, InfoType_ResourceLimit, CUR_PROCESS_HANDLE, 0);
    char b[120]; snprintf(b, sizeof b, "resource-limit handle: rc=0x%X%s", (unsigned)rc, R_SUCCEEDED(rc) ? "" : " (not available to an application)"); CtSwitchTrace(b);
  }
  u64 used = 0, total = 0, nonSys = 0;
  svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0); svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&nonSys, InfoType_UsedNonSystemMemorySize, CUR_PROCESS_HANDLE, 0);
  static const char* const tn[0x16] = { "unmapped", "io", "normal", "code-static", "code-mutable", "heap", "shared", "weird", "module-static", "module-mutable", "ipc0", "stack", "tls", "transfer-iso",
                                        "transfer", "process", "reserved", "ipc1", "ipc3", "kstack", "code-ro", "code-rw" };
  unsigned cnt[0x16] = { 0 }; unsigned long long bytes[0x16] = { 0 }; unsigned regions = 0;
  u64 addr = 0;
  for (int guard = 0; guard < 200000; guard++) {
    MemoryInfo mi; u32 pi; if (R_FAILED(svcQueryMemory(&mi, &pi, addr))) break;
    unsigned t = mi.type & 0xFF; if (t < 0x16) { cnt[t]++; bytes[t] += mi.size; } regions++;
    u64 next = mi.addr + mi.size; if (next <= addr || next >= (1ull << 39)) break; addr = next;
  }
  int fds = 0; { struct stat st; for (int fd = 3; fd < 256; fd++) if (fstat(fd, &st) == 0) fds++; }          // (open files beyond stdin/out/err: each is a file-system session of the loader's process)
  char b[760]; int n = snprintf(b, sizeof b, "resources (%s): %d open files; memory used %llu / %llu MB (non-system %llu MB); %u regions;", tag, fds, (unsigned long long)(used >> 20), (unsigned long long)(total >> 20), (unsigned long long)(nonSys >> 20), regions);
  for (int t = 2; t < 0x16; t++) if (cnt[t] && n < (int)sizeof b - 60) n += snprintf(b + n, sizeof b - (size_t)n, " %s %u/%lluk;", tn[t], cnt[t], bytes[t] >> 10);
  CtSwitchTrace(b);
  if (!strcmp(tag, "start") || !strcmp(tag, "end of run")) CtBenchInfo(b);
}
// Rewrites one `key=value` line of ct_boost.ini (adds it when absent): a benchmark is a one-shot, so it switches itself off when it has run.
void CtSwitchBoostIniSet(const char* key, const char* val) {
  char lines[64][128]; int n = 0, found = 0;
  FILE* f = fopen("ct_boost.ini", "r");
  if (f) { while (n < 63 && fgets(lines[n], sizeof lines[0], f)) { size_t kl = strlen(key); if (!strncmp(lines[n], key, kl) && lines[n][kl] == '=') { snprintf(lines[n], sizeof lines[0], "%s=%s\n", key, val); found = 1; } n++; } fclose(f); }
  if (!found && n < 64) { snprintf(lines[n++], sizeof lines[0], "%s=%s\n", key, val); }
  f = fopen("ct_boost.ini", "w"); if (!f) return;
  for (int i = 0; i < n; i++) fputs(lines[i], f);
  fclose(f);
}
void CtSwitchBackendFailed(void) {
  if (CtBenchChainActive()) { CtBenchChainFailed("the backend could not start"); fflush(stdout); fflush(stderr); exit(0); }     // (a benchmark pass: record it and go on; the player's choice stays)
  fprintf(stderr, "[video] the chosen backend could not start: saving SDL_Renderer as the choice and relaunching\n"); fflush(stderr);
  CtSettingsGet()->backend = CT_BACKEND_SDL; CtSettingsSave();
  CtSwitchBackendTryEnd();
  if (g_argv0[0]) envSetNextLoad(g_argv0, g_argv0);
  fflush(stdout); fflush(stderr);
  exit(0);
}
int CtSwitchExitMode(void) { return g_exitMode; }
// The three clock-probe threads: stopped and joined (a thread left running when the program returns to the homebrew loader makes the loader's unmapping fail).
void CtSwitchThreadsStop(void) {
  if (!g_probesUp) return;
  g_probeQuit = 1;
  for (int c = 0; c < 3; c++) { threadWaitForExit(&g_probeTh[c]); threadClose(&g_probeTh[c]); }
  g_probesUp = 0;
}
// Whatever is left of the GPU services once the graphics stack is torn down. Mesa's Horizon runtime opens the nvdrv sessions through libnx's reference-counted
// calls and does not always balance them; the sessions are then still open when the homebrew loader starts the next program (Sphaira), which can run out of them
// ("2001-0132" out of resource, in the launcher, right after our exit). Each of these is a no-op at a zero count and closes the service at the last one.
void CtSwitchReleaseGpu(void) {
  for (int i = 0; i < 16; i++) { nvFenceExit(); nvMapExit(); nvGpuExit(); }
  for (int i = 0; i < 16; i++) nvExit();
}
// THE LEAK TEST (docs/BENCHMARK.md): ct_boost.ini `leaktest=<stage>` relaunches the program through the homebrew loader `leaktest_n` times (default 40), each time running only as far as the
// stage (0 libnx + our init, 1 + SDL, 2 + the sound, 3 + the video backend and the overlay) and tearing it down again; logs/leaktest.csv gets one line per launch. The loader's process is shared
// by every launch, so a leak of sessions or handles per launch ends the sequence with "2001-0132" after a number of launches that depends on how much each leaves behind: the last line of the
// file is how many launches survived, and the stage where it drops names the code that leaks.
static int g_leakStage = -2, g_leakN = 40, g_leakI;
int CtSwitchLeakStage(void) {
  if (g_leakStage != -2) return g_leakStage;
  const char* e = getenv("CT_LEAKTEST"); g_leakStage = e ? atoi(e) : -1;
  const char* n = getenv("CT_LEAKTEST_N"); if (n) g_leakN = atoi(n); if (g_leakN < 1) g_leakN = 1;
  if (g_leakStage < 0) { remove("ct_leak_state"); return g_leakStage; }
  FILE* f = fopen("ct_leak_state", "r"); long t0 = 0; int i = 0, st = -1;
  if (f) { if (fscanf(f, "%d %d %ld", &st, &i, &t0) != 3) { i = 0; st = -1; } fclose(f); }
  long now = (long)time(NULL);
  if (st != g_leakStage || now - t0 > 60 || now < t0) i = 0;                  // another stage, or the last launch was long ago: a fresh test
  g_leakI = i + 1;
  mkdir("logs", 0777);
  FILE* c = fopen("logs/leaktest.csv", "a"); if (c) { fprintf(c, "%ld,stage %d,launch %d of %d\n", now, g_leakStage, g_leakI, g_leakN); fclose(c); }
  return g_leakStage;
}
void CtSwitchLeakNext(void) {
  if (g_leakStage < 0) return;
  fprintf(stderr, "[leaktest] stage %d: launch %d of %d done\n", g_leakStage, g_leakI, g_leakN); fflush(stderr);
  if (g_leakI >= g_leakN) { remove("ct_leak_state"); fprintf(stderr, "[leaktest] stage %d: all %d launches survived\n", g_leakStage, g_leakN); return; }
  FILE* f = fopen("ct_leak_state", "w"); if (f) { fprintf(f, "%d %d %ld\n", g_leakStage, g_leakI, (long)time(NULL)); fclose(f); }
  if (g_argv0[0]) envSetNextLoad(g_argv0, g_argv0);
}
void CtSwitchLogFlush(void) { CtSwitchBackendTryEnd(); fflush(stdout); fflush(stderr); if (g_log) fflush(g_log); }
int CtSwitchFastExitOk(void) { AppletType t = appletGetAppletType(); return t == AppletType_Application || t == AppletType_SystemApplication; }
void CtSwitchExit(void) {
  CtSwitchBackendTryEnd();
  fflush(stdout); fflush(stderr);
  if (g_log) fflush(g_log);
  AppletType t = appletGetAppletType();
  if (t == AppletType_Application || t == AppletType_SystemApplication) svcExitProcess();
}
#endif
