// hostpool.h - a small shared thread pool for the HOST's pixel work (the title renderer's rows, the scene bake's cell rows).
// Pure speed: every job writes its own disjoint part of the output, so the picture is the same pixels on any thread count.
// CT_THREADS=<n> workers besides the caller (0 = none, the way it always ran). Default: 2 on the Switch (3 app cores),
// 0 elsewhere, so the regression suite and every other host run exactly as before.
#ifndef CT_HOSTPOOL_H
#define CT_HOSTPOOL_H
#ifdef __cplusplus
extern "C" {
#endif
typedef void (*CtParFn)(void* ctx, int i0, int i1);       // work on items [i0, i1)
// Runs fn over [0, n) in chunks of `grain` items, taken by the caller and the workers as they free up; returns when all
// are done. Not reentrant: one CtParallelFor at a time, from one thread.
void CtParallelFor(int n, int grain, CtParFn fn, void* ctx);
int  CtPoolThreads(void);
// ONE background job at a time on its own thread (the sound's frame: SPC + resample + queue). CtAsyncRun starts fn(ctx) and returns at
// once (it first waits for the previous job); CtAsyncJoin waits until it has finished, and does nothing when no job is running -- or when
// called from the job's own thread, so the code the job runs may call the same joining entry points. CtAsyncAvailable = 0 means no thread
// could be made (or the host runs without one): the caller then runs the work itself.
typedef void (*CtAsyncFn)(void* ctx);
int  CtAsyncAvailable(void);
void CtAsyncRun(CtAsyncFn fn, void* ctx);
void CtAsyncJoin(void);
void CtHostThreadsStop(void);                             // end and join the pool workers and the background job's thread (at exit)
#ifdef __cplusplus
}
#endif
#endif
