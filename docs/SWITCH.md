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
   -> `build-switch/ct_field.nro`.

## Install

Put these in a folder of the SD card, e.g. `sdmc:/switch/ChronoTrigger-NX/` (the program works in the folder it is started from):

| file | from |
|---|---|
| `ct_field.nro` | the build |
| `ct_assets.dat` | setup |
| `Chrono_Trigger_USA_SNES.sfc` | setup (your dump, verified; battles and the sound driver read it) |

`ct_sram.srm` (the battery save), `ct_settings.ini`, `ct_log.txt` (stdout/stderr) and `ct_stats.txt` (the per-second performance line) appear in the same folder.
Launch from the homebrew menu in **application mode** (hold R on a game) for the full memory and CPU budget.

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
`pin=<0-2>` pins the main thread to that app core; `async=0` turns off the render thread (below). Set **Performance HUD** in the mods menu (Video tab) to **Full HUD** (every counter) or **FPS only** (just the frame rate; **OFF** draws nothing; saved as `stats_hud=full|fps|off`) to see `CPU <n> MHZ`
live, and the picture's sizes: `RENDER WxH` (the uploaded picture: 256x224 x the internal scale, wider with widescreen) and `SHOWN WxH IN WxH` (the
rectangle it is drawn in, and the window's output), with the options that make them; `ct_stats.txt` has the full line: `cpu <MHz> bench <us> core<N> [<c0> <c1> <c2>] re=<re-asserts> ev=<applet events> fb=<fallbacks>
rc=<last result>` (`bench` is a fixed loop: ~610 us boosted, ~1175 us at 1020 MHz).

## The mods menu (L3 + R3)

The mods menu (widescreen, internal resolution, frames between, sync mode, controls, ...) is built into the Switch build with
Dear ImGui 1.91.5. Press **L3 + R3** (both sticks) to open it, and again to close it; the left stick / d-pad moves, A selects, B backs out.
The game is paused while it is open, and changes are saved to `ct_settings.ini` on close. Build note: `third_party/imgui` must exist
(`sh tools/get_imgui.sh`);
the Switch target is SDL_Renderer only (`CT_NO_GL`).

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

Any frame over 40 ms appends one line to `ct_stats.txt` (and the log): `SPIKE <t>s <mode> loc <id> work <ms> | logic .. (vm .. battle .. load ..) compose ..
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
