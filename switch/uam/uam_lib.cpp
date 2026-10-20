// uam_lib.cpp - see uam_lib.h. Replaces uam's main.cpp: the same DekoCompiler, with the DKSH written to memory instead of a file.
#include "compiler_iface.h"
#include "uam_lib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

namespace {
int g_optLevel = 1;                                                          // uam defaults to 3, but levels 2 and 3 miscompile crt-royale (the scanline distance came out constant); 1 renders it correctly
struct Job { int stage; const char* glsl; void** dksh; size_t* size; char* log; size_t logN; int rc; };

void run(Job* j) {
  pipeline_stage st = j->stage == CT_UAM_VERTEX ? pipeline_stage_vertex : pipeline_stage_fragment;
  DekoCompiler compiler{st, g_optLevel};
  if (!compiler.CompileGlsl(j->glsl)) { if (j->log && j->logN) snprintf(j->log, j->logN, "uam: the %s shader did not compile (the compiler's message is in the program log)", j->stage == CT_UAM_VERTEX ? "vertex" : "fragment"); j->rc = 1; return; }
  char* buf = NULL; size_t n = 0;
  FILE* f = open_memstream(&buf, &n);
  if (!f) { if (j->log && j->logN) snprintf(j->log, j->logN, "uam: no memory stream"); j->rc = 1; return; }
  compiler.OutputDkshTo(f);
  fclose(f);
  if (!buf || !n) { free(buf); if (j->log && j->logN) snprintf(j->log, j->logN, "uam: no output"); j->rc = 1; return; }
  *j->dksh = buf; *j->size = n; j->rc = 0;
}
}  // namespace

#ifdef __SWITCH__
// ONE persistent worker thread (16 MB of stack) for every compile, instead of a thread per compile: a capture recompiles dozens of shaders, and a thread's kernel handle and stack mapping per compile
// added up (the loader failed on the way out afterwards). The thread must be joined before the program exits: CtUamShutdown (the deko3d backend's teardown).
namespace {
Thread g_thread; Mutex g_mu; CondVar g_cvJob, g_cvDone; Job* g_job = NULL; bool g_up = false, g_quit = false, g_done = false;
void worker(void*) {
  mutexLock(&g_mu);
  for (;;) {
    while (!g_job && !g_quit) condvarWait(&g_cvJob, &g_mu);
    if (g_quit) break;
    Job* j = g_job; g_job = NULL;
    mutexUnlock(&g_mu); run(j); mutexLock(&g_mu);
    g_done = true; condvarWakeAll(&g_cvDone);
  }
  mutexUnlock(&g_mu);
}
}
#endif
extern "C" void CtUamSetOptLevel(int level) { g_optLevel = level < 0 ? 0 : level > 3 ? 3 : level; }
extern "C" void CtUamShutdown(void) {
#ifdef __SWITCH__
  if (!g_up) return;
  mutexLock(&g_mu); g_quit = true; condvarWakeAll(&g_cvJob); mutexUnlock(&g_mu);
  threadWaitForExit(&g_thread); threadClose(&g_thread); g_up = false; g_quit = false;
#endif
}
extern "C" int CtUamCompile(int stage, const char* glsl, void** dksh, size_t* size, char* log, size_t logN) {
  *dksh = NULL; *size = 0;
  if (log && logN) log[0] = 0;
  Job j = { stage, glsl, dksh, size, log, logN, 1 };
#ifdef __SWITCH__
  if (!g_up) {
    mutexInit(&g_mu); condvarInit(&g_cvJob); condvarInit(&g_cvDone);
    if (R_SUCCEEDED(threadCreate(&g_thread, worker, NULL, NULL, 16 * 1024 * 1024, 0x2C, -2)) && R_SUCCEEDED(threadStart(&g_thread))) g_up = true;
  }
  if (g_up) {
    mutexLock(&g_mu); g_job = &j; g_done = false; condvarWakeOne(&g_cvJob);
    while (!g_done) condvarWait(&g_cvDone, &g_mu);
    mutexUnlock(&g_mu);
    return j.rc;
  }
#endif
  run(&j);
  return j.rc;
}
