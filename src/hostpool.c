// hostpool.c - see hostpool.h.
#include "hostpool.h"
#include "SDL.h"
#include <stdlib.h>

static struct {
  CtParFn fn; void* ctx; int n, grain; SDL_atomic_t next;
} g_job;
static SDL_sem *g_go, *g_done;
static SDL_Thread* g_th[4];
static volatile int g_quit;                                 // CtHostThreadsStop: the workers leave their loops
static int g_nth = -1;

static void work(void) {
  for (;;) {
    int i0 = SDL_AtomicAdd(&g_job.next, g_job.grain);
    if (i0 >= g_job.n) return;
    int i1 = i0 + g_job.grain < g_job.n ? i0 + g_job.grain : g_job.n;
    g_job.fn(g_job.ctx, i0, i1);
  }
}
static int worker(void* arg) {
  (void)arg;
  for (;;) { SDL_SemWait(g_go); if (g_quit) return 0; work(); SDL_SemPost(g_done); }
}
int CtPoolThreads(void) {
  if (g_nth >= 0) return g_nth;
  const char* e = getenv("CT_THREADS");
#ifdef __SWITCH__
  int n = e ? atoi(e) : 2;
#else
  int n = e ? atoi(e) : 0;
#endif
  if (n < 0) n = 0;
  if (n > 4) n = 4;
  g_go = SDL_CreateSemaphore(0); g_done = SDL_CreateSemaphore(0);
  int made = 0;
  if (g_go && g_done)
    for (; made < n; made++) { g_th[made] = SDL_CreateThread(worker, "ct-pool", NULL); if (!g_th[made]) break; }
  g_nth = made;
  return g_nth;
}
void CtParallelFor(int n, int grain, CtParFn fn, void* ctx) {
  if (n <= 0) return;
  if (grain < 1) grain = 1;
  int nth = CtPoolThreads();
  if (nth == 0 || n <= grain) { fn(ctx, 0, n); return; }     // no pool, or one chunk: just run it
  g_job.fn = fn; g_job.ctx = ctx; g_job.n = n; g_job.grain = grain;
  SDL_AtomicSet(&g_job.next, 0);
  for (int i = 0; i < nth; i++) SDL_SemPost(g_go);
  work();
  for (int i = 0; i < nth; i++) SDL_SemWait(g_done);
}

// ---- the background job ----
static SDL_Thread* g_at; static SDL_sem *g_aGo, *g_aDone; static SDL_threadID g_aTid;
static CtAsyncFn g_aFn; static void* g_aCtx; static int g_aPending; static int g_aInit = -1;       /* g_aPending: touched by the starting thread only */
static int asyncWorker(void* arg) {
  (void)arg; g_aTid = SDL_ThreadID();
  for (;;) { SDL_SemWait(g_aGo); if (g_quit) return 0; g_aFn(g_aCtx); SDL_SemPost(g_aDone); }
}
int CtAsyncAvailable(void) {
  if (g_aInit >= 0) return g_aInit;
  const char* e = getenv("CT_ASYNC_AUDIO");
#ifdef __SWITCH__
  int want = e ? atoi(e) : 1;
#else
  int want = e ? atoi(e) : 0;
#endif
  g_aInit = 0;
  if (!want) return 0;
  g_aGo = SDL_CreateSemaphore(0); g_aDone = SDL_CreateSemaphore(0);
  if (g_aGo && g_aDone) { g_at = SDL_CreateThread(asyncWorker, "ct-audio", NULL); g_aInit = g_at ? 1 : 0; }
  return g_aInit;
}
void CtAsyncJoin(void) {
  if (g_aInit <= 0 || !g_aPending) return;
  if (SDL_ThreadID() == g_aTid) return;                      // the job itself
  SDL_SemWait(g_aDone); g_aPending = 0;
}
void CtAsyncRun(CtAsyncFn fn, void* ctx) {
  if (!CtAsyncAvailable()) { fn(ctx); return; }
  CtAsyncJoin();
  g_aFn = fn; g_aCtx = ctx; g_aPending = 1;
  SDL_SemPost(g_aGo);
}

// Ends every thread this file started and waits for them: a program that leaves threads running when it returns to the homebrew loader makes the loader's
// unmapping of the program fail (the system's "an error has occurred" dialog). Safe to call twice; nothing here is used afterwards.
void CtHostThreadsStop(void) {
  CtAsyncJoin();
  g_quit = 1;
  if (g_at) { SDL_SemPost(g_aGo); SDL_WaitThread(g_at, NULL); g_at = NULL; }
  for (int i = 0; i < g_nth && i < 4; i++) if (g_th[i]) SDL_SemPost(g_go);
  for (int i = 0; i < g_nth && i < 4; i++) if (g_th[i]) { SDL_WaitThread(g_th[i], NULL); g_th[i] = NULL; }
  g_nth = 0; g_aInit = 0;
}
