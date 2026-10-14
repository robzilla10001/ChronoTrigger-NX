// switch_platform.h - the Nintendo Switch host (devkitA64 + libnx + switch-sdl2). Host plumbing only: where the files
// live, the arguments a console launch does not have, a log file. No game behaviour (CLAUDE.md: the vanilla game is
// the ROM's; this is a different host for the same engine).
#ifndef CT_SWITCH_PLATFORM_H
#define CT_SWITCH_PLATFORM_H
#ifdef __SWITCH__
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
#endif
#endif
