# Logs and benchmarks

Everything here is the HOST's measurement (`src/bench.c`, `src/hoststats.c`, the presenter in `src/present.c`): it reads clocks only and changes nothing in the game.

## One log per run

Every run writes its own files into `logs/` next to the program (on the Switch: `sdmc:/switch/<folder>/logs/`). `<name>` is the program's file name without `.nro`
(`ct_field`, `ct_field_mesa`), `<stamp>` the start time as `YYYYMMDD-HHMMSS` (a console whose clock is not set gets `runNNNN`, counted in `logs/.runcount`).

| file | what |
|---|---|
| `ct_log_<name>_<stamp>.txt` | stdout + stderr of the run (the Switch). Begins with the program, start time, firmware and applet type. |
| `ct_stats_<name>_<stamp>.txt` | the per-second performance line (the Switch) |
| `bench_<name>_<backend>_<stamp>.txt` | the run's **summary** (below). Rewritten every 30 s, final at the end: a run that is killed still leaves one. |
| `bench_<name>_<backend>_<stamp>.csv` | one row per second: mode, location, engine / shown f/s, frame average and worst, work, CPU / GPU / memory clock, audio drops |
| `bench_present.csv` | the presentation micro-benchmark, one row per (backend, picture size, phase), **appended across runs** (the columns grew in 0.6b: delete an older file before a new pass, so the header matches) |
| `bench_chain_<stamp>.csv` | the same rows for one automated pass over every backend |

## The run summary

Histograms (0.25 ms buckets, so percentiles are exact to a quarter millisecond) of every part of the frame, for all frames and for each mode the game reports (`TITLE`/`FIELD`/`OW`/`BATTLE`/`MENU`):
**frame** (the time between engine frames; 60 Hz is 16.67 ms), **work** (what the host spent in the frame), **upload / draw / swap** (the presenter's calls on the main thread; swap
includes the vsync wait), **logic / compose** (the engine's two halves), **hud**, **render-thread** (SDL_Renderer's own thread) and **present** (upload + draw + swap): average,
minimum, p50, p95, p99, p99.9, maximum; the frames over 16.7 / 33.3 / 50 / 100 ms; and over the run the shown and engine frame rate, the clocks and the audio blocks dropped. Two more parts need a backend with GPU timestamps: **gpu** (the GPU's time for a frame) and **cpu blocked on gpu**. The header says
which backend, build, settings and firmware it was.

## Benchmark modes (`ct_boost.ini`: `bench=<0..4>`, `bench_secs=<n>`; elsewhere `CT_BENCH`, `CT_BENCH_SECS`)

| `bench=` | |
|---|---|
| 0 | off (the logs and the summary are written every run) |
| 1 | the game runs for `bench_secs` (default 120 s: the title and the attract demo play by themselves) and the program ends with the summary |
| 2 | the **presentation micro-benchmark** for the selected backend, then the program ends |
| 3 | the micro-benchmark for **every** backend (SDL_Renderer, OpenGL, Vulkan and deko3d when built in) in turn: the program relaunches itself with the next backend each time; your saved choice is not changed; each run is written to `bench_chain_<stamp>.csv`; a backend that cannot start, or crashes while starting, is recorded as `FAILED` and skipped |
| 4 | as 3, and the game runs for `bench_secs` (default 60 s) on each backend too |

**If a launch in the middle of a pass fails** (a pass relaunches the program several times through the homebrew loader and a launch can fail before the program starts), the pass has not been lost:
`ct_bench_state` next to the program remembers the step, and starting the program again carries on from it (`bench=3` is still in `ct_boost.ini`). Delete `ct_bench_state` to start the pass over.

A benchmark is a one-shot: it sets `bench=0` in `ct_boost.ini` when it has run (3 and 4 after the last backend). On the Switch, edit `ct_boost.ini` next to the program (`bench=3`) and start it.

### The presentation micro-benchmark

No engine work behind it: synthetic pictures of the sizes the game uses (1x 256x224, 2x, 4x 1024x896 and 4x widescreen 16:9 1593x896) are uploaded, drawn into the window and swapped, every
call timed. For each size, vsync off (what the backend can sustain) and vsync on (how steadily it presents); at 4x also the linear filter, the performance HUD, and the mods menu open.
Columns: `frame_*` (the interval between frames), `upload_*`, `draw_*`, `swap_*` and `overlay_*` (the menu) in ms, `fps`, `wait_*` (how long the CPU was **blocked on the GPU**: fences and
the acquire; Vulkan and deko3d) and `gpu_*` (the **GPU's own time for a frame**, from its timestamps: deko3d and Vulkan for the whole frame, OpenGL for the draw; `gpu_samples` is how many finished frames were
read). Reading them:

* **On the Switch the display is the limit, not the GPU.** deko3d (and the console's compositor in general) will not run faster than the 60 Hz panel even with vsync off, so `fps` reads 60.0 and `frame_avg` 16.67 for every size:
  the backend's real cost is `gpu_*` (how long the GPU worked on each frame) and the CPU's share is `upload + draw + swap - wait`. A backend that is fast has a small `gpu_avg` and `wait_avg`
  (the CPU is not held up). `upload` on deko3d includes the wait for the frame slot's fence, which is why it reads about a frame at a locked 60 f/s.

* **SDL_Renderer runs a render thread** (the Switch's default). The micro-benchmark waits for it to show each frame (so none is replaced), and the SDL rows carry the thread's own figures:
  `rt_work_*` (its texture upload + clear + copy: the work it does per frame), `rt_up_*` (the texture upload inside that), `rt_wait_*` (its `RenderPresent`: the display wait and the flush), `shown_fps`
  (frames the thread really presented a second), `replaced` (frames dropped before it got to them; 0 here by construction, nonzero in a game run when the display is late) and `rt_samples`. The main thread's
  `wait_*` for SDL is the time spent waiting for the render thread. A game run's summary has the same parts as `render-thread`, `render-thread texture upload` and `render-thread present wait`. For the
  other backends `shown_fps` is the main thread's rate and the `rt_*` columns are 0.
* `swap` with vsync on is the wait for the display: a backend that holds 60.0 f/s has `frame_avg` 16.67 and a small `frame_p99`; a jittery one shows it in `frame_p95` / `frame_max`.
* `upload` is the RGB24 to RGBA conversion plus the copy to the GPU: it grows with the picture's area, and is the first thing to optimise on a backend where it dominates.

## The process's kernel resources

Each run logs `resources (<point>): memory used/total, regions, per-type counts and sizes` at start-up, after each stage, in the exit block and at `atexit` (an application cannot read its resource-limit handle:
`svcGetInfo(InfoType_ResourceLimit)` returns `0xE401`, logged once). A benchmark pass relaunches the program through the homebrew loader several times inside ONE process, so whatever a run leaves open is
what the next run and, after the last, the launcher have less of. Memory, mappings and threads were checked this way and are identical at the start and end of every run on every backend; they do not leak.

**The leak that was found:** the run's log file was never closed. Each file left open is a file-system session the loader's process keeps; the budget ran out after 4 to 19 launches (the loader's `2001-0132`, "out of
resource", on the way back to the launcher). `switch/chain_probe.c` (a bare self-relaunching program with knobs: `openlog=1` leaves one file open per launch) reproduced it in isolation, while threads, 512 MB
allocations, a 28 MB NRO, the clock services and slow relaunches all survived 40 launches. The log is closed at exit now (`logClose` in `switch_platform.c`): `leaktest=3` survives 40 launches, and a `bench=3` pass
(44 rows, four backends) completes and returns to the launcher cleanly. **Any new file or service the program opens must be closed before it exits.**

**What is left is not ours to see.** A forwarder-launched application (the way the NRO is run on the console) has an application's tight kernel limits, shared with the loader. Asking the kernel for one more session or event is refused (`2001-0132`, the loader's own crash code) at the start of every launch and
after the full teardown alike, so the process has no spare ones, and the loader's return to the launcher can fail (intermittently: the same pass has run clean and crashed) when it needs one. Memory, mappings, threads and open files are identical in clean and crashing exits, on every backend;
the log lines count what we can (`open files`, memory, regions) and nothing more can be measured from inside. Keep a pass's launches few (the default four) and everything opened closed.

### The leak test (`leaktest=<stage>`, `leaktest_n=<n>` in `ct_boost.ini`)

The homebrew loader runs every launch in ONE process, so a session or handle a run leaves open is one fewer for the next; the loader's `2001-0132` ("limit reached") at the end of a benchmark pass is that running out
(the memory and mappings do not leak: `resources (...)` lines are identical start to end). `leaktest` relaunches the program `leaktest_n` times (default 40), each time running only as far as the stage and tearing it down:
`0` libnx + our init (boost, clock probes), `1` + SDL, `2` + the sound, `3` + the video backend and the overlay. `logs/leaktest.csv` gets a line per launch: the last line before the loader's error is how many launches
survived, and a stage that survives fewer launches than the one before it names the code that leaks. (`boost=0` leaves the clock code out of stage 0, the bare libnx baseline.) Delete `ct_leak_state` to restart a test.

While `leaktest` is set the game runs only the leak test (`bench` is ignored): remove the line afterwards.

## Where to look when optimising

1. `bench_chain_*.csv` (or `bench_present.csv`): rank the backends by `frame_p99` at `4x vsync-off` and `4x-16:9`; the difference between `4x` and `4x +HUD` is the HUD's cost, `4x +mods-menu` the menu's.
2. A game run's summary (`bench=1`): `work` against `frame` says how much headroom a mode has; `render-thread` and `upload` say whether the presenter is the limit.
3. `bench_*.csv` per second: the seconds where `frame_max_ms` or `audio_drops` jump are the spikes; `ct_log_*.txt` has the `[stats] SPIKE` line for each, with the breakdown of what was in that frame.
