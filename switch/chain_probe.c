// chain_probe.c - the smallest possible program that relaunches itself through the homebrew loader, to tell whether the loader / applet manager limits how many launches one process can chain
// (the benchmark pass and the leak test chain several) independently of anything ChronoTrigger-NX does: libnx's default init and exit, nothing else.
// Put chain_probe.nro in the same folder as the game; it appends to chain_probe.log one line per launch and relaunches itself `N` times (N from chain_probe.n, default 40). Start it from the homebrew menu.
// Knobs, one `key=value` per line in chain_probe.cfg, each adding one thing the game does that the bare probe does not (to find which one the loader cannot take many launches of):
//   threads=<n>  start n threads and join them again          alloc=<MB>  malloc that much, touch it, free it          openlog=1  open a log file and never close it (as the game's log does)
//   stack=1      big thread stack (1 MB) and a 1 MB local array    tls=1  use thread-local storage                          sleep=<ms>  wait before relaunching
// A build with -DBIG_PAD links a 28 MB constant array, like the game's NRO size. With `chain_probe.boost` present it also opens and closes the apm and clkrst services and registers an applet hook, as the game's clock code does, to see if THAT changes the count.
#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static AppletHookCookie g_cookie;
#ifdef BIG_PAD
static const unsigned char g_pad[28 * 1024 * 1024] = { 1, 2, 3 };
#endif
static void thr(void* a) { (void)a; svcSleepThread(1000000ll); }
static void hook(AppletHookType t, void* p) { (void)t; (void)p; }

int main(int argc, char** argv) {
  const char* me = argc > 0 ? argv[0] : "";
  char dir[256]; snprintf(dir, sizeof dir, "%s", me);
  char* sl = strrchr(dir, '/'); if (sl) *sl = 0; else snprintf(dir, sizeof dir, "sdmc:/switch");
  if (chdir(dir) != 0) chdir("sdmc:/");
  int n = 40, i = 0; long t0 = 0;
  FILE* f = fopen("chain_probe.n", "r"); if (f) { if (fscanf(f, "%d", &n) != 1) n = 40; fclose(f); }
  f = fopen("chain_probe.state", "r"); if (f) { if (fscanf(f, "%d %ld", &i, &t0) != 2) i = 0; fclose(f); }
  long now = (long)time(NULL);
  if (now - t0 > 60 || now < t0) i = 0;                          // the last launch was long ago: a fresh test
  i++;
  int boost = access("chain_probe.boost", F_OK) == 0;
  f = fopen("chain_probe.log", "a"); if (f) { fprintf(f, "%ld launch %d of %d boost=%d applet type %d\n", now, i, n, boost, (int)appletGetAppletType()); fclose(f); }
  int cfgThreads = 0, cfgAlloc = 0, cfgOpenlog = 0, cfgSleep = 0;
  f = fopen("chain_probe.cfg", "r");
  if (f) { char l[80]; int v; while (fgets(l, sizeof l, f)) { if (sscanf(l, "threads=%d", &v) == 1) cfgThreads = v; else if (sscanf(l, "alloc=%d", &v) == 1) cfgAlloc = v;
                                                                else if (sscanf(l, "openlog=%d", &v) == 1) cfgOpenlog = v; else if (sscanf(l, "sleep=%d", &v) == 1) cfgSleep = v; } fclose(f); }
#ifdef BIG_PAD
  { volatile unsigned long long sum = 0; for (int k = 0; k < 28 * 1024 * 1024; k += 4096) sum += g_pad[k]; (void)sum; }
#endif
  if (cfgOpenlog) { FILE* keep = fopen("chain_probe.open.log", "a"); if (keep) fprintf(keep, "%d\n", i); }          // (left open on purpose)
  if (cfgAlloc > 0) { unsigned char* m = (unsigned char*)malloc((size_t)cfgAlloc << 20); if (m) { for (size_t k = 0; k < ((size_t)cfgAlloc << 20); k += 4096) m[k] = 1; free(m); } }
  if (cfgThreads > 0) { Thread t[16]; int made = 0; for (int k = 0; k < cfgThreads && k < 16; k++) if (R_SUCCEEDED(threadCreate(&t[k], thr, NULL, NULL, 0x4000, 0x2B, -2)) && R_SUCCEEDED(threadStart(&t[k]))) made++;
                         for (int k = 0; k < made; k++) { threadWaitForExit(&t[k]); threadClose(&t[k]); } }
  ApmCpuBoostMode dummy = ApmCpuBoostMode_Normal; (void)dummy;
  if (boost) {                                                    // what the game's clock code does with services
    if (R_SUCCEEDED(apmInitialize())) { ApmPerformanceMode pm; apmGetPerformanceMode(&pm); apmExit(); }
    if (R_SUCCEEDED(clkrstInitialize())) { ClkrstSession s; if (R_SUCCEEDED(clkrstOpenSession(&s, PcvModuleId_CpuBus, 3))) clkrstCloseSession(&s); clkrstExit(); }
    appletHook(&g_cookie, hook, NULL); appletUnhook(&g_cookie);
  }
  if (i < n) {
    f = fopen("chain_probe.state", "w"); if (f) { fprintf(f, "%d %ld\n", i, (long)time(NULL)); fclose(f); }
    if (cfgSleep > 0) svcSleepThread((s64)cfgSleep * 1000000ll);
    envSetNextLoad(me, me);
  } else {
    remove("chain_probe.state");
    f = fopen("chain_probe.log", "a"); if (f) { fprintf(f, "%ld all %d launches survived\n", (long)time(NULL), n); fclose(f); }
  }
  return 0;
}
