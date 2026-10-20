# Slang shaders

ct_field can run RetroArch's **slang** shader presets (`.slangp` with their `.slang` files; the library is [libretro/slang-shaders](https://github.com/libretro/slang-shaders)) over the picture, on the GPU:
CRT filters, scanlines, bloom, phosphor masks, upscalers, multi-pass chains with look-up textures, feedback and history. The shaders are compiled **on the console** when a preset is chosen.

**Status.** OpenGL, Vulkan and deko3d all run presets, from the same chain: each backend is one file implementing the GPU interface (below). OpenGL and Vulkan are verified against each other pixel for pixel
on a PC (software GL and Vulkan, `tools/slangtool`). On the console, OpenGL and deko3d are verified with crt-royale (12 passes); the Vulkan runner has not been run on hardware yet.
deko3d's shaders are compiled by uam on the console, at **optimisation level 1**: uam's default level 3 (and 2) miscompiles crt-royale's scanline pass (a distance came out constant, so the pass and its successors were black),
found with the capture's "compiler optimisation level" what-if. The compiles run on ONE persistent 16 MB thread, joined at exit (a thread per compile eventually made the loader fail on the way out, error 2001-0132).
Known: a `uint` push member (`FrameCount`) reads wrong on deko3d's GPU; no shipped preset is affected so far.

## Using it

1. Copy the presets you want, **with the folders they reference**, into `shaders/` next to the game (`sdmc:/switch/chronotrigger-nx/shaders/`). The simplest is the whole library:
   `git clone --depth 1 https://github.com/libretro/slang-shaders shaders` (presets use relative paths such as `../include/...`, so keep the library's layout; it is large, ~130 MB, and you only need the folders a preset uses; the library's `include/` folder is needed by most multi-pass presets).
2. Any of the three GPU video backends works (mods menu, Video tab; the SDL_Renderer backend has no shader support). Open the Video tab: *Shaders (slang presets)* has a file browser over `shaders/`. Pick a `.slangp`; **None** turns it off.
   The choice is kept in `ct_settings.ini` (`shader=shaders/crt/crt-lottes.slangp`) and applied at start.
3. *Capture every pass to logs/ (debugging)*, under the parameters, is a one-shot toggle: it runs the preset one pass at a time, reads each pass's output back and saves it as a PNG in `logs/slang_<preset>_<time>/` with a `summary.txt` (size, mean brightness, how much is non-black, whether a pass is identical to the one before, flagged ALL BLACK when empty); the same table goes in the log. When a preset draws wrong, the first pass that differs from the same pass on another backend is the bug (OpenGL and Vulkan produce identical tables).
4. *Shader parameters* lists the shader's own knobs (`#pragma parameter`) as sliders, live. *Reset parameters* returns to the preset's values.

The game's picture is handed to the chain at its **native size** (256x224, or the widescreen width) whatever the *Internal resolution* option says: that is what CRT shaders are written for, and the shader does all the scaling to the screen.
The preset's filter is the one it asks for; where it says nothing, the *Filter* option decides.

Compiling a big preset (crt-royale: 12 passes) takes a few seconds on the console, once, at selection or at start; the status line in the menu shows how long it took and any warnings, or why a preset did not load
(a missing file is named, with the file that asked for it).

**What it costs.** Light presets are free; heavy ones are GPU-bound. Measured at the Base clock profile (GPU 460 MHz), engine frames per second with the shader on: zfast-crt-curvature 59, vhs 54, crt-yo6-flat-trinitron 51,
anamorphic 50, simple-crt 34, newpixie-crt 31, lottesRVM 29, crt-royale 20, crt-maximus-royale (22 passes) 16, a 16-pass HDR bezel preset 7. The mods menu's *Clocks* (Mariko boost / Max performance) raise the GPU clock.

## What is supported

The format of RetroArch's `spec/SHADER_SPEC.md`, in full: `#include`, `#pragma stage / name / format / parameter`, any number of passes, `scale_type` source / viewport / absolute, `float_framebuffer`, `srgb_framebuffer`,
all render-target formats, `wrap_mode`, `mipmap_input`, `alias`, `frame_count_mod`, look-up textures (`textures = ...`, PNG), the history (`OriginalHistoryN`), feedback (`PassFeedbackN`, `<alias>Feedback`), `#reference`d presets,
parameters set by the preset. The names a shader may use are the spec's (`MVP`, `OutputSize`, `FinalViewportSize`, `FrameCount`, `Source`, `Original`, `PassOutputN`, `<alias>Size`, ...) plus `FrameDirection`, `OriginalFPS`, `Rotation`,
`CurrentSubFrame`, `TotalSubFrames`, `OriginalAspect`, `OriginalAspectRotated` and the HDR names (always SDR). A name the chain does not know is logged as a warning and left 0, as RetroArch does.

Not supported: JPEG look-up textures (a few bezels; they render black), rotation, multiple sub-frames.

## How it is built (src/slang)

```
.slangp --slang_preset.c--> passes, scale, filter, wrap, LUTs, parameter values
.slang  --slang_source.c--> #include resolved, split into vertex + fragment text, #pragma metadata
        --slang_compile.cpp:  glslang --> SPIR-V ; SPIRV-Cross --> reflection (which uniform / push constant / sampler is what, by NAME)
                                                      SPIRV-Cross --> GLSL (version 430; push constants become a second UBO next to the real one)
        --slang_chain.c--> sizes every pass's render target per frame, resolves every name to a meaning, fills the uniform blocks, runs the passes in order,
                           keeps the feedback and history textures
        --slang_gpu.h--> the functions a backend provides: texCreate, texFromRgba, texDestroy, texCopy, texGenMips, progCreate, progDestroy, passBegin, passDraw, passEnd
        --slang_gl.c--> OpenGL 4.3: the GLSL goes to the driver (any GL loader: SDL on the console, OSMesa on a PC)
        --slang_vk.c--> Vulkan: the SPIR-V as it is; passes recorded into the backend's command buffer, the last one inside the backend's own render pass (the overlay follows in it)
        --slang_dk.cpp--> deko3d: the GLSL compiled to DKSH ON THE CONSOLE by uam (libuam.a, built by switch/get_uam.sh), render targets, a descriptor table, uniform blocks pushed through the command stream
```

`slang_gpu.h` states the conventions every backend keeps (texture row 0 is the top, uv (0,0) is the top-left, the MVP is Vulkan's y-down).
A backend is one file implementing those functions and one hook in its present_*.c: Vulkan takes the SPIR-V as it is (`wantsGlsl = 0`), OpenGL and deko3d take the GLSL.
deko3d's render targets have row 0 at clip +y, so its runner asks the chain to flip every pass (`flipOffscreen`); OpenGL flips only the final pass; Vulkan none.
A preset is (re)loaded at the start of a frame on Vulkan and deko3d (nothing of the old one is in flight then; what died is freed after the GPU is idle), at once on OpenGL.

Libraries: glslang and SPIRV-Cross are built for the Switch by `sh switch/get_slang.sh` (into `third_party/slang-switch`, ~12 MB, sources in `third_party/_slang_src`; `switch/build_both.sh` does it when missing).
**Build them with the toolchain's flags** (the script keeps them: `-mtp=soft`, the software thread pointer libnx needs for `thread_local`; a build that replaces the compiler flags crashes in glslang's allocator).
uam (the deko3d shader compiler: Mesa's GLSL front end and Nouveau's Maxwell code generator) comes from `sh switch/get_uam.sh` as one relocatable object exporting only `CtUamCompile`, `CtUamSetOptLevel` and `CtUamShutdown`:
its old Mesa must not meet the Mesa 26.2 of `ct_field_mesa.nro`, which links with duplicate definitions allowed (the first copy of a name wins).
Without `third_party/slang-switch`, or with `-DCT_WITHOUT_SLANG=ON`, the program builds without shader support and nothing else changes; without `libuam.a` deko3d runs without them. The libraries add about 4 MB to the NRO, uam about 4 more.

## Testing on a PC (no console needed): tools/slangtool

`sh tools/build_slangtool.sh` builds `slangtool` in the Docker image (glslang + SPIRV-Cross for the PC, Mesa's software OpenGL 4.5, lavapipe Vulkan, libpng). (`tools/` is git-ignored: it stays on disk.)

```
slangtool -v preset.slangp                  every pass compiled; the uniforms, push constants and samplers each shader asks for
slangtool -all slang-shaders                every preset of the library: compile + translate to GLSL      (2658 of 2658 today)
slangtool -chain -all slang-shaders         every preset through the chain with a GPU that draws nothing   (names, sizes, feedback, history; unknown names listed)
slangtool -render preset.slangp in.png out.png [1920x1080] [frames] [Param=value ...]
                                            the whole pipeline on a software OpenGL: a PNG you can look at (crt-royale, guest-advanced, ... all render)
slangtool -render-vk ...                    the same on a software Vulkan (lavapipe): matches the OpenGL picture to within a pixel's rounding
slangtool -dump <dir> -glslver 430 [-all] ... the GLSL of every pass, to run through the real `uam` (99% of a 1500-shader sample compiled; the rest was the push-block binding, fixed).
                                            (DON'T dump the whole library: tens of GB.)
```
