// bench.h - per-run logs and the HOST's benchmark: a new, named log for every run, histograms of the frame (every part of it) for the whole run and per mode, a
// per-second CSV, and a presentation micro-benchmark that isolates each video backend's own cost (upload, draw, swap, the HUD, the mods menu) at the picture sizes the game
// uses. Reads clocks only; changes no game state. See docs/BENCHMARK.md.
//
// FILES (in logs/, next to the program; <name> = the program's file name without .nro, <stamp> = YYYYMMDD-HHMMSS of the start):
//   ct_log_<name>_<stamp>.txt      stdout + stderr of the run (the Switch; elsewhere the terminal)
//   ct_stats_<name>_<stamp>.txt    the per-second performance line (the Switch)
//   bench_<name>_<backend>_<stamp>.txt  the run's summary: averages, min/max and percentiles of every part of the frame, overall and per mode
//   bench_<name>_<backend>_<stamp>.csv  one row per second of the run
//   bench_present.csv              the presentation micro-benchmark, one row per (backend, picture size, phase), appended across runs
//   bench_chain_<stamp>.csv        the same rows for one automated pass over every backend (bench=3 / 4)
//
// SWITCHES (ct_boost.ini on the Switch, the environment elsewhere): bench=<0..4>  (CT_BENCH)
//   0 off (the logs and the summary are always written)   1 the game runs for bench_secs (CT_BENCH_SECS, default 120) and ends, summary written
//   2 the presentation micro-benchmark for the selected backend, then ends
//   3 the micro-benchmark for EVERY backend in turn (the program relaunches itself with the next one; the player's choice is not changed)
//   4 as 3, and the game runs for bench_secs (default 60) on each backend too
#ifndef CT_BENCH_H
#define CT_BENCH_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
// ---- the run's identity and files
void        CtRunSetName(const char* argv0);                       // the program's path as launched ("sdmc:/switch/x/ct_field_mesa.nro"): its base name tags every file
const char* CtRunName(void);
const char* CtRunStamp(void);                                      // fixed at the first call
void        CtRunPath(char* out, size_t n, const char* kind, const char* ext);   // "logs/<kind>_<name>_<stamp>.<ext>", the folder made
// ---- configuration
int         CtBenchMode(void);                                     // 0..4
int         CtBenchSecs(void);
// ---- what the run is (shown in the summary)
void        CtBenchSetBackend(int id, const char* name);           // present.h CT_BACKEND_*, and its human name
void        CtBenchInfo(const char* line);                         // one more line of context for the header (settings, firmware, clocks)
// ---- the samples
void        CtBenchFrame(const char* mode, double periodMs, double workMs, double upMs, double drawMs, double swapMs, double logicMs, double composeMs, double hudMs, double renderThreadMs, double rtUpMs, double rtWaitMs);
void        CtBenchExtra(double gpuMs, int hasGpu, double waitMs);       // once a frame from the presenter: the GPU's time for a finished frame (hasGpu), and the CPU's blocked time since the last call
void        CtBenchSecond(const char* mode, int loc, double engineFps, double shownFps, double periodAvg, double periodMax, double workAvg, int cpuMHz, int gpuMHz, int memMHz,
                          long audioDrops, long audioEmpty);
void        CtBenchWrite(int final);                               // the summary file (cheap: histograms), at once; `final` marks the end of the run
int         CtBenchEngineExpired(void);                            // bench=1/4: the game's time is up
// ---- the presentation micro-benchmark's rows
typedef struct { const double *work, *up, *wait; int n; long shown, replaced; } CtBenchRt;   // the SDL render thread's own times for the frames it finished, and its counts
void        CtBenchPresentRow(const char* phase, int picW, int picH, int outW, int outH, int frames, double wallSecs, const double* totalMs, const double* upMs,
                              const double* drawMs, const double* swapMs, const double* ovlMs, const double* waitMs, const double* gpuMs, int nGpu, const CtBenchRt* rt);
// ---- the automated pass over every backend (the Switch relaunches itself; elsewhere it is one pass over the selected backend)
int         CtBenchChainActive(void);
int         CtBenchChainExpected(void);                            // the backend this run must use (CT_BACKEND_*), -1 when not in a chain
void        CtBenchChainStart(int* backend);                       // early in main: sets the backend for this run from the chain's state (a crashed one is recorded and skipped)
void        CtBenchChainFailed(const char* why);                   // this run's backend could not start: record it, go on to the next
void        CtBenchOneShotDone(void);                              // bench=1/2 has run: switch it off in ct_boost.ini (the Switch)
void        CtBenchChainNext(void);                                // the run is over: the next backend (relaunch), or the end of the chain
#ifdef __cplusplus
}
#endif
#endif
