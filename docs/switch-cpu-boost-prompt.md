# Task: keep the Switch CPU at 1785 MHz in this homebrew app (libnx)

## Problem
This is a Nintendo Switch homebrew app (libnx, devkitA64). Homebrew starts at 1020 MHz. Requesting 1785 MHz
once at startup is NOT enough: the system power manager (apm) lowers the clock again later (observed around
focus / operation-mode / performance-mode changes and while content loads), and the boost sometimes only
"stuck" after several re-requests. The fix that worked in another app is to (a) request the clock, (b) measure
it back, and (c) keep re-asserting it whenever the measured clock is low, reacting immediately to applet events.

Implement exactly this. Do not rely on a one-shot request.

## What to implement

### 1. Initialization (call once, early, after the app starts; before the main loop)
* If `hosversionAtLeast(8,0,0)`: `clkrstInitialize()`, then
  `clkrstOpenSession(&session, PcvModuleId_CpuBus, 3)`, remember the current rate via `clkrstGetClockRate` as the
  "original" clock, then `clkrstSetClockRate(&session, 1785000000)`. Keep the session OPEN for the app's lifetime.
* Else (< 8.0.0): `pcvInitialize()`, remember the original with `pcvGetClockRate(PcvModule_CpuBus, &rate)`, then
  `pcvSetClockRate(PcvModule_CpuBus, 1785000000)`.
* Read the clock back. If it is < 1500 MHz, also call `appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad)` and read
  again.
* Register an applet hook: `appletHook(&cookie, callback, nullptr)`. The callback only sets a volatile flag
  `boostEvent = true` and increments a counter. (Hooks only fire from `appletMainLoop()`, so the main loop must
  call it.)
* Set a "watch" counter to 300 (frames).

### 2. Per-frame maintenance (call once per main-loop iteration, every frame)
```c
#define CPU_BOOST_HZ 1785000000u

static volatile bool boostEvent;       // set by the applet hook
static int boostWatchFrames;           // >0 => check every other frame
static int boostPollCounter;
static int boostFailStreak;
static int boostReapplied, boostHookEvents, boostFallbacks;
static Result boostLastRc;

static uint32_t currentRateMHz(void) {
    u32 rate = 0;
    if (cpuSessionOpen)      clkrstGetClockRate(&cpuSession, &rate);
    else if (pcvBoosted)     pcvGetClockRate(PcvModule_CpuBus, &rate);
    return rate / 1000000;
}

// Sets the rate; if the clkrst session call fails, close/reopen the session once and retry.
static Result setCpuRate(uint32_t hz) {
    Result rc = 0;
    if (cpuSessionOpen) {
        rc = clkrstSetClockRate(&cpuSession, hz);
        if (R_FAILED(rc)) {
            clkrstCloseSession(&cpuSession);
            cpuSessionOpen = false;
            if (R_SUCCEEDED(clkrstOpenSession(&cpuSession, PcvModuleId_CpuBus, 3))) {
                cpuSessionOpen = true;
                rc = clkrstSetClockRate(&cpuSession, hz);
            }
        }
    } else if (pcvBoosted) {
        rc = pcvSetClockRate(PcvModule_CpuBus, hz);
    }
    return rc;
}

void maintainBoost(void) {                      // call every frame
    bool event = boostEvent;
    if (event) { boostEvent = false; boostWatchFrames = 180; }   // watch closely for ~3 s after any system event

    bool watching = boostWatchFrames > 0;
    if (watching) boostWatchFrames--;
    if (!event && ++boostPollCounter < (watching ? 2 : 4)) return;   // otherwise check at ~15 Hz
    boostPollCounter = 0;

    if (currentRateMHz() >= 1500) { boostFailStreak = 0; return; }   // already boosted: nothing to do

    boostLastRc = setCpuRate(CPU_BOOST_HZ);
    boostReapplied++;
    if (currentRateMHz() < 1500) {
        // request accepted but did not stick: also ask the applet manager for its boost
        if (++boostFailStreak >= 3) {
            boostFailStreak = 0;
            appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
            boostFallbacks++;
        }
    } else {
        boostFailStreak = 0;
    }
}

// Call after loading or unloading content / any heavy operation (the clock tended to drop around those).
void boostNudge(void) { boostWatchFrames = 300; }
```
Call `boostNudge()` right after a game/content load finishes and after one is closed.

### 3. Shutdown
`appletUnhook(&cookie)`; set the clock back to the remembered original rate (`clkrstSetClockRate` /
`pcvSetClockRate`); `clkrstCloseSession(&session)` then `clkrstExit()` (or `pcvExit()`).

### 4. Diagnostics (strongly recommended)
Keep a small status string and draw it on screen (or log it): `boost: <MHz> re=<boostReapplied>
ev=<boostHookEvents> fb=<boostFallbacks> rc=<boostLastRc hex>`, plus the measured MHz from `currentRateMHz()`.
How to read it:
* `re` climbing steadily with rc=0: something keeps overriding the clock; maintenance is working.
* non-zero `rc`: the request itself is being refused (note the code).
* `ev` rising: system events are arriving; confirm the clock recovers right after.
* `fb` > 0: the direct request was accepted but ignored, and the applet boost fallback was used.

## Requirements / pitfalls
* Always verify by READING the clock back; never assume the request worked. A success return code did not
  guarantee the clock stuck.
* Re-assert from the main loop, not just at startup. Checking every other frame during the watch window and
  ~15 Hz otherwise cost a negligible amount of time (a few IPC calls per second).
* Keep the clkrst session open for the app's lifetime; reopen it only if a set call fails.
* The main loop must call `appletMainLoop()` each iteration or the applet hooks will never fire.
* Do not use a larger value than 1785000000 unless the user asks; that is the stock maximum.
* Build with the usual libnx link line (`-lnx`); no extra libraries are needed.
* Do not change anything else in the app. Report what you added and where it is called from, and show the
  diagnostics line from a real run on hardware if the user can provide one.
