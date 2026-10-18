// switch_platform.h - the Nintendo Switch host (devkitA64 + libnx + switch-sdl2). Host plumbing only: where the files
// live, the arguments a console launch does not have, a log file. No game behaviour (CLAUDE.md: the vanilla game is
// the ROM's; this is a different host for the same engine).
#ifndef CT_SWITCH_PLATFORM_H
#define CT_SWITCH_PLATFORM_H
#ifdef __SWITCH__
#include "SDL.h"
// Call first in main(). Makes the executable's folder (sdmc:/switch/ct/) the working directory, where
// Chrono_Trigger_USA_SNES.sfc, ct_assets.dat, ct_sram.srm and ct_settings.ini live, sends stdout/stderr to ct_log.txt
// there, and, launched with no arguments (the homebrew menu passes only the path), hands main() the player's own:
// --assets ct_assets.dat --boot.
void CtSwitchInit(int* argc, char*** argv);
// A line in ct_log.txt, flushed at once: how far a start got before a crash (no debugger on the console).
void CtSwitchTrace(const char* what);
// About once a second: read the CPU clock the system left us, put it back at the boost rate if it dropped, and report it
// (hoststats prints it). The 1785 MHz the engine needs is not kept by a one-time request: the system takes it back.
void CtSwitchClockTick(void);
void CtSwitchBoostFrame(void);                     // once per main-loop frame: re-assert the CPU clock whenever it reads low
void CtSwitchBoostNudge(void);                     // after a content load / a long frame: watch the clock closely for a while
int  CtSwitchCpuMHz(void);
int  CtSwitchGpuMHz(void);
int  CtSwitchMemMHz(void);                         // the last memory clock read, MHz (0 = unknown)                         // the last GPU clock read, MHz (0 = unknown)                         // the last clock read, MHz (0 = unknown)
// A fixed CPU benchmark (microseconds; ~constant at a constant clock, ~2x when the CPU is throttled) and the system's current
// performance mode/configuration, as text for the stats: "bench 212us apm 1/0x92220009".
const char* CtSwitchClockInfo(void);
// THE CONSOLE'S SDL VIDEO DRIVER CONNECTS THE SCREEN'S ONLY WINDOW (the NWindow) TO AN EGL SURFACE THE MOMENT A WINDOW IS CREATED, which leaves nothing for deko3d or
// Vulkan to present on (deko3d's swapchain create fails, fatally). (SDL's "dummy" video driver, the first remedy tried, is not built into the console's SDL.) So when
// the player chose one of those backends no SDL window is made at all: the video subsystem still starts, the backend takes the screen itself, and
// switch_platform.c adds the touch screen as SDL finger events (the console's driver reports touches against a window). Every event poll goes through CtSwitchPollEvent.
void CtSwitchNoWindow(void);                       // call before the presenter is made, only for those backends: no SDL window will be created
int  CtSwitchNoWindowMode(void);                     // 1 = this run uses it
int  CtSwitchPollEvent(SDL_Event* e);              // SDL_PollEvent, plus the applet loop and the touch screen when the dummy driver is in use
// A clean end: stdio flushed and, in application mode, the process exited at once (threads of the graphics stacks that cannot be joined would otherwise
// still run while the loader unmaps the program: an instruction abort at exit).
void CtSwitchExit(void);
int  CtSwitchExitMode(void);                       // ct_boost.ini `exit=`: 0 full teardown + return from main, 1 no teardown + return (default), 2 no teardown + end the process, 3 teardown + end the process
void CtSwitchLogFlush(void);
void CtSwitchReleaseGpu(void);                     // close any nvdrv sessions the graphics stack left open (the next program, the launcher, would run out)
void CtSwitchThreadsStop(void);                    // join the clock probes (the host pool and the sound thread: hostpool.h CtHostThreadsStop)
int  CtSwitchFastExitOk(void);                     // 1 = in application mode: the process may end at once, with no teardown of the graphics stack (its destructors are what crashed)
// SELF-HEALING BACKEND CHOICE. A backend other than SDL_Renderer that does not get through its start (it crashes, or returns "no window") would leave the player
// with a program that cannot launch. So a marker file is written before the window is made and removed once frames have been presented (or the run ends
// cleanly); a start that finds the marker still there goes back to SDL_Renderer, saves that, and says so in the log.
void CtSwitchBackendTryBegin(void);
void CtSwitchBackendTryEnd(void);
int  CtSwitchBackendTryPending(void);
// The dummy-driver backends (deko3d, Vulkan) could not start: save SDL_Renderer as the choice and relaunch the game (through the homebrew loader), which
// is a clean start on the console's video driver. Does not return.
void CtSwitchBackendFailed(void);
#else
#define CtSwitchInit(argc, argv) ((void)0)
#define CtSwitchTrace(what) ((void)0)
#define CtSwitchClockTick() ((void)0)
#define CtSwitchBoostFrame() ((void)0)
#define CtSwitchBoostNudge() ((void)0)
#define CtSwitchCpuMHz() 0
#define CtSwitchGpuMHz() 0
#define CtSwitchMemMHz() 0
#define CtSwitchClockInfo() ""
#define CtSwitchNoWindow() ((void)0)
#define CtSwitchNoWindowMode() 0
#define CtSwitchExit() ((void)0)
#define CtSwitchFastExitOk() 0
#define CtSwitchExitMode() 0
#define CtSwitchLogFlush() ((void)0)
#define CtSwitchThreadsStop() ((void)0)
#define CtSwitchReleaseGpu() ((void)0)
#define CtSwitchBackendTryBegin() ((void)0)
#define CtSwitchBackendTryEnd() ((void)0)
#define CtSwitchBackendTryPending() 0
#define CtSwitchBackendFailed() ((void)0)
#endif
#endif
