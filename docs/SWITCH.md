# Nintendo Switch build

A homebrew `.nro` of `ct_field` (the interactive engine), built with devkitA64 + libnx + switch-sdl2. It is a different
*host* for the same engine: no game logic differs (CLAUDE.md: the vanilla game is the ROM's). Host-only parts live in
`src/switch_platform.c` (working folder, log, default arguments) and `switch/CMakeLists.txt`.

## Build

The step-by-step version, with Docker, is the README's [Quick start](../README.md#quick-start). In short:

1. `python tools/setup.py --rom <your dump> --no-build` (generates `src/data`, `src/xlat`, `ct_assets.dat`; the ROM is yours, nothing is distributed).
2. `sh tools/get_imgui.sh` (Dear ImGui 1.91.5 into `third_party/imgui`; without it the mods menu is compiled out).
3. With devkitPro (`DEVKITPRO` set; packages libnx, switch-sdl2, switch-mesa, switch-cmake; the `devkitpro/devkita64` Docker image has them all):
   `cmake -S switch -B build-switch -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake && cmake --build build-switch -j`
   -> `build-switch/ct_field.nro` (SDL_Renderer, OpenGL and deko3d; Vulkan too when `third_party/mesa-switch` exists, see *Building with Vulkan*).
4. Optional: `sh switch/build_both.sh` builds `ct_field.nro` and the second configuration `ct_field_mesa.nro` (*Two builds, side by side*) in the Docker container.

## Install

Put these in a folder of the SD card, e.g. `sdmc:/switch/ChronoTrigger-NX/` (the program works in the folder it is started from):

| file | from |
|---|---|
| `ct_field.nro` (or `ct_field_mesa.nro`) | the build |
| `ct_assets.dat` | setup |
| `Chrono_Trigger_USA_SNES.sfc` | setup (your dump, verified; battles and the sound driver read it) |

`ct_sram.srm` (the battery save) and `ct_settings.ini` appear in the same folder, and a `logs/` folder with a NEW log, stats file and benchmark summary for every run, named after the program and the start time (`ct_log_ct_field_mesa_20261018-041530.txt`, ...). See [BENCHMARK.md](BENCHMARK.md).
Launch from the homebrew menu in **application mode** (hold R on a game) for the full memory and CPU budget; from Sphaira or as a forwarder works too.

## Controls

The Joy-Cons/Pro Controller come through SDL's game-controller layer, so the default map applies (by position: bottom
face button = B, right = A, left = Y, top = X; L/R, `-` = Select, `+` = Start; left stick also drives the d-pad).

## Status

Runs on hardware. On the base clocks at the 4x internal resolution it holds about 60 f/s in the title, the field, the overworld and
battles (a soft 60: see the stats below for the frames that spike, mostly the sound driver's song and sample-set loads); the
boosted profiles leave more headroom. The mods menu is described below.

## The CPU clock

Homebrew starts at 1020 MHz and the engine needs 1785. Measured on hardware: the boost-mode request alone holds for 60 s, then the CPU
drops to 1020 MHz for 15 s, every 75 s, whatever the game is doing. So the host asks the clock service for 1785 MHz directly
(`clkrst` on 8.0.0+, `pcv` before), **reads the clock back**, and re-asserts it whenever it reads low: ~15 times a second, every other
frame for ~3 s after an applet event or a long frame (a load), with the boost-mode request as a fallback if a direct request is accepted
but does not stick. The original clock is restored at exit. (docs/switch-cpu-boost-prompt.md describes the method.)

`ct_boost.ini` next to the `.nro` (one `key=value` per line): `boost=0` leaves the clock alone (the baseline for a measurement);
`pin=<0-2>` pins the main thread to that app core; `async=0` turns off the render thread (below); `bench=<0..4>` / `bench_secs=<n>` run the benchmarks ([BENCHMARK.md](BENCHMARK.md)). Set **Performance HUD** in the mods menu (Video tab) to **Full HUD** (every counter) or **FPS only** (just the frame rate; **OFF** draws nothing; saved as `stats_hud=full|fps|off`) to see `CPU <n> MHZ`
live, and the picture's sizes: `RENDER WxH` (the uploaded picture: 256x224 x the internal scale, wider with widescreen) and `SHOWN WxH IN WxH` (the
rectangle it is drawn in, and the window's output), with the options that make them; the run's `logs/ct_stats_*.txt` has the full line: `cpu <MHz> bench <us> core<N> [<c0> <c1> <c2>] re=<re-asserts> ev=<applet events> fb=<fallbacks>
rc=<last result>` (`bench` is a fixed loop: ~610 us boosted, ~1175 us at 1020 MHz).

## The save/load state menu (ZL + ZR)

Squeeze both triggers to pause the game and open a touch-first menu: ten save-state slots, backup and restore of all of them, and the options to keep the
previous state as a `.bak` and to confirm before a save or load. States work everywhere; see [SAVESTATES.md](SAVESTATES.md).

## The mods menu (L3 + R3)

The mods menu (widescreen, internal resolution, frames between, sync mode, controls, ...) is built into the Switch build with
Dear ImGui 1.91.5. Press **L3 + R3** (both sticks) to open it, and again to close it; the left stick / d-pad moves, A selects, B backs out.
The game is paused while it is open, and changes are saved to `ct_settings.ini` on close. Build note: `third_party/imgui` must exist
(`sh tools/get_imgui.sh`). The menu draws through whichever video backend is running (below); it is laid out for a 1080-line surface in every mode.

## Video backends (the mods menu's "Video backend")

Chosen in the Video tab and applied the next time the game starts (it is saved in `ct_settings.ini`, `[video] backend=`):

| Backend | `backend=` | Notes |
|---|---|---|
| SDL_Renderer | `sdl` | The default. Its own performance HUD and the render thread (below). |
| OpenGL 3.3 | `opengl` | Through Mesa; the frame is a texture blitted to the back buffer. The performance HUD is drawn by `present.c` (CPU-rasterised, blitted to the corner). |
| Vulkan | `vulkan` | Mesa's NVK driver from [mesa-switch](https://github.com/danfromtico/mesa-switch) (Mesa 26.2.4, MIT), linked into the game with no loader (`src/present_vk.c`: a surface on the console's NWindow through `VK_NN_vi_surface`). Needs the build step below. |
| deko3d | `deko3d` | The Switch's native graphics API (`src/present_dk.cpp`): the frame is an RGBA texture drawn as one quad; the menus and the HUD draw through a small deko3d renderer of Dear ImGui's draw data. |

Vulkan and deko3d have no screenshots yet. A backend that cannot start falls back to SDL_Renderer with the reason in the run's log. A backend that crashes its own start is
caught too: a marker file (`ct_backend_try`) is written before a non-default backend starts and removed after two seconds of frames, and a start that finds it still there goes
back to SDL_Renderer (and says so in the log). The deko3d shaders are pre-built (`src/dk/dk_shaders.inc`, from the `.glsl` files beside it with `src/dk/build_shaders.sh`).

**Vulkan and deko3d get no SDL window.** The console's SDL video driver connects the screen's only window (the NWindow) to an EGL surface the moment a window is created, which
leaves nothing for deko3d's swapchain or Vulkan's surface (deko3d fails fatally). So with one of those backends the game starts SDL's video subsystem but creates no window, the
backend takes the screen itself, and `switch_platform.c` adds the touch screen as SDL finger events and runs the applet loop. (SDL's "dummy" video driver, which would have
avoided this, is not built into the console's SDL.) The surface is 1920x1080 in every mode, like SDL's own EGL window (the compositor scales it to the handheld panel), and the
mods menu's font is sized from the surface's height, so the menus look the same on every backend.

### Building with Vulkan

```sh
sh switch/get_mesa.sh        # clones and builds mesa-switch in Docker (first run: an image with SPIRV-Tools and a Rust nightly, ~10 GB, a while)
```

This leaves `third_party/mesa-switch/` (the Khronos headers and `nvk_vulkan.o`), and the next `cmake` of `switch/` compiles the Vulkan backend in and
links it; without that folder the build has no working Vulkan and the menu entry falls back to SDL_Renderer. Two things the script does that
mesa-switch's own Docker build does not: it cross-compiles the Rust parts for aarch64 (that build silently produced x86-64 objects), and it combines NVK, its
Rust shader compiler and the Rust runtime into one object in which only the `vk*` API is global, because the console's older Mesa (the libEGL that SDL's
renderer and the OpenGL backend use) defines the same internal symbols. The NRO grows by about 15 MB.

### Two builds, side by side

`sh switch/build_both.sh` builds the game twice, so the known-good configuration is never replaced by the experimental one:

| NRO | Graphics stack | Use |
|---|---|---|
| `ct_field.nro` | The console's own (older) Mesa for SDL_Renderer and OpenGL; NVK Vulkan beside it as one isolated object. | The known-good build. |
| `ct_field_mesa.nro` | **Everything on mesa-switch's Mesa 26.2**: its EGL / OpenGL / GLES (what SDL_Renderer and the OpenGL backend draw through) and its NVK Vulkan, one stack and one GPU runtime, no libdrm_nouveau. Shows in the Homebrew Menu as "Chrono Trigger (C) - Mesa". | The newest drivers. Run on hardware with all four backends; `ct_field.nro` stays the reference. |

Both read the same `ct_settings.ini` and saves, so you can switch between them freely. The second one is `-DCT_MESA_UNIFIED=ON` in `switch/CMakeLists.txt`,
its libraries come from `sh switch/get_mesa_unified.sh` (`third_party/mesa-switch-unified`). With it OpenGL can also run Mesa's Zink (OpenGL on top of NVK):
set `MESA_SWITCH_GL_DRIVER=zink` before the game starts (the default is Mesa's native NVC0 driver).

## How the game exits (`ct_boost.ini`, `exit=`)

The default (`exit=0`) is right for both NROs. The setting exists for diagnosis; every step is written to the run's log (`exit mode N ...`, `exit: overlay down`, ...).

| `exit=` | What happens |
|---|---|
| 0 (default) | every thread joined, every teardown (overlay, presenter / EGL / GPU, SDL), the GPU services released, then return from `main` |
| 1 | the same without the graphics teardown |
| 2 | no teardown, the process is ended at once (`svcExitProcess`; application mode only) |
| 3 | every teardown, then the process is ended |

What the exit has to get right, found with crash reports from the homebrew loader (hbmenu, Sphaira; a forwarder, the game as its own title, never had the trouble because the
process simply ends):

* **Every thread of ours must be gone.** The loader panics (`2001-0106`, a break inside `hbl`) when it unmaps a program that still has running threads: the three clock probes, the two
  pool workers, the sound thread and the render thread are all stopped and joined first.
* **The GPU must be torn down, and its services released.** Skipping the graphics teardown (`exit=1`) leaves GPU mappings the loader cannot unmap on the Mesa 26 build; after a full
  teardown the loader's next program (Sphaira) failed with `2001-0132`, out of resource, until the exit also closed any nvdrv sessions the runtime left open (`CtSwitchReleaseGpu`).
* **Every file we open must be closed.** The run's log was left open once; the loader's process kept that file-system session for the next launch and ran out (`2001-0132`) after 4 to 19 chained launches. The log is closed at exit now ([BENCHMARK.md](BENCHMARK.md#the-processs-kernel-resources)).
* `svcExitProcess` (`exit=2`, `3`) skips the applet's own clean exit and ends in the system's error dialog.

## The render thread

The SDL renderer work (texture upload, draw, the HUD, the swap) runs on its own thread, so it overlaps the next frame's engine work. Hand-off is a
**mailbox of three staging buffers**: the main thread converts the picture to RGBA into the free one, records the draw and publishes it as the
*newest* frame, and never waits; the thread always shows the newest, and a frame still waiting when a newer one arrives is skipped (counted as
`skippedPresents`). Measured why: the thread's texture upload took up to 14.8 ms of a 16.7 ms frame at every clock setting -- it was waiting (the
display's flip fence, whose phase against the engine's 60.000 Hz drifts), not working -- and a one-frame hand-off turned that wait into engine
stalls (7-8 of the 9 dip seconds in each run). Uploads alternate between two textures. The thread releases the GL context after each frame; the
options menu, a texture resize, a screenshot and shutdown wait for it first. On by default on the Switch (`async=0` in `ct_boost.ini` or
`CT_ASYNC_PRESENT=0` turns it off; `CT_ASYNC_PRESENT=1` turns it on elsewhere, SDL_Renderer on a GPU renderer only). Stats: `swap` is the main
thread's time in the hand-off (~0); `rt <work>/<wait> (up <upload>)` is the thread's own timing; `shown` counts frames the thread presented.
`CT_PRESENT_CHECK=1` reports a staging buffer that changed under the thread.

## The spike log

Any frame over 40 ms appends one line to the run's stats file (and the log): `SPIKE <t>s <mode> loc <id> work <ms> | logic .. (vm .. battle .. load ..) compose ..
(base fx wide cmb hud asm) picture .. title .. audio .. convert .. swap ..` -- where the frame's time went (vm = the event VM, battle = the battle
module's tick, load = a location load).

## Clock profiles (the mods menu's "Clocks")

Video tab -> **Clocks**: four profiles, applied live (the host sets the CPU, GPU and memory clocks through the clock service, reads each back,
and re-asserts it whenever it drifts; the original clocks are restored at exit). Saved as `clocks=base|erista|mariko|max` in `ct_settings.ini`;
first run (`auto`) picks Erista or Mariko by the console's hardware type.

| profile | CPU | GPU | memory | |
|---|---|---|---|---|
| Base (safe, stock) | 1020 MHz | 460 MHz | 1331 MHz | limited to 1x resolution |
| Erista boost | 1785 MHz | 614 MHz | 1600 MHz | 2-4x resolution; generally regarded as safe |
| Mariko boost | 1785 MHz | 768 MHz | 1600 MHz | 2-4x resolution; generally regarded as safe |
| Max performance | 1963 MHz | 921 MHz | 1600 MHz | may degrade battery life; a comfortable cushion for stability |

(The boost profiles keep the CPU at the 1785 MHz stock maximum the engine needs; only Base lowers it.) Why the memory clock is in the list:
the render thread's texture upload stepped between 3 and 15 ms for tens of seconds at a time at a constant GPU clock -- a memory-bound copy
under a governed memory clock. Each stats line shows `cpu .. gpu .. mem .. clk=<profile> gre=<GPU re-asserts> mre=<memory re-asserts>`; the
HUD shows `CPU n GPU n MEM n MHZ`.

## The windowed scene bake

A scene's picture is baked once for the whole map and re-baked whenever its tiles or palette animate (the forest: every few frames, and in battle it
was 2.5 ms a frame on the base clocks, the largest steady cost there). The picture is read only around the camera, so on the Switch a bake now
recomputes the camera's rectangle only (the view plus the widescreen's sides plus 88 px; ~29% of the forest's canvas), in place, and is renewed when the
camera leaves the part that is current (the view plus 40 px). A rectangle that runs past a canvas edge is baked as the pieces on each side (maps wrap: the host reads the same pixels from the
other side -- the forest's battle camera sits at the map's corner, which is why the first version, that fell back to a full bake there, never helped in battle). A view as
large as the canvas, and everything that is not the main loop's bake (a location load), is a full bake. Proven byte-identical to a full bake inside every piece (135 scenes, 1059 windowed bakes, 704 of them across an edge, every output:
picture, priority mask, coverage, motion coverage, pre-math copy), with the coverage check proven sound (8472 claims against a pixel bitmap) and frame-identical in 256 scenes with the camera scrolling. `CT_BAKE_WINDOW=0` turns
it off, `=1` turns it on elsewhere. Stats: `bakepct` = the share of the canvas a bake recomputed (100 = a full bake).
