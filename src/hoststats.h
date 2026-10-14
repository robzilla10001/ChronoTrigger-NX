// hoststats.h - the HOST's performance counters (not the game's): how fast the engine's frames and the presenter's
// frames really run, so a slow device can be measured instead of guessed at. Reads clocks only; changes no game state.
// On by default on the Switch build, elsewhere with CT_STATS=1. Once a second it logs one "[stats]" line to stderr
// (ct_log.txt on the Switch) and, on the SDL_Renderer backend, draws the same numbers over the picture.
#ifndef CT_HOSTSTATS_H
#define CT_HOSTSTATS_H
#ifdef __cplusplus
extern "C" {
#endif
int         CtStatsOn(void);
double      CtStatsNow(void);                       // ms, for the section timers
// where the time of a presented frame goes (present.c times these): 0 upload, 1 draw (clear+copy), 2 swap (vsync wait)
enum { CT_ST_UPLOAD, CT_ST_DRAW, CT_ST_SWAP, CT_ST_AUDIO, CT_ST_PICTURE, CT_ST_TITLE, CT_ST_LOGIC, CT_ST_COMPOSE, CT_ST_BWARENA, CT_ST_BWFX, CT_ST_RWORK, CT_ST_RWAIT, CT_ST_LBASE, CT_ST_LFX, CT_ST_LWIDE, CT_ST_LCMB, CT_ST_LHUD, CT_ST_LASM, CT_ST_RUP, CT_ST_XBAT, CT_ST_XVM, CT_ST_XLOAD, CT_ST_XB1, CT_ST_XB2, CT_ST_XB3, CT_ST_XB4, CT_ST_XB5, CT_ST_XENTER, CT_ST_XBAKE, CT_ST_XBAKEB3, CT_ST_XLAYERS, CT_ST_XAUD, CT_ST_XBAKEPCT, CT_ST_XJOIN, CT_ST_XMISC, CT_ST_TIRENDER, CT_ST_TIUP, CT_ST_TIMODE, CT_ST_N };   // XJOIN: the wait for the sound thread   // TIRENDER / TIUP / TIMODE: the title's full-size render, its hand-over to the presenter, the title mode shown (7 pendulum, 2 logo)   // XMISC: the Switch clock upkeep after the pace (counted in logic)   // XBAKEPCT: per bake, the percent of the scene canvas it recomputed (100 = a full bake)   // XAUD: the sound-driver handshakes (song / sample-set loads)   // XBAKE/XBAKEB3: the scene bake / its BG3-off variant; XLAYERS: the layer canvases rebuilt   // XB1-5: CtBattleBegin's stages; XENTER: the whole battle-entry call   // X*: inside the frame's logic: the battle tick, the event VM, a location load   // RWORK: the render thread's upload + draw; RWAIT: its present (the vsync wait)   // AUDIO: the SPC/DSP step + samples; PICTURE: the picture's build (raster/scale) before its upload
void        CtStatsPresent(int part, double ms);
// once per engine frame (hostPace): ms of work since the last frame ended, ms slept to the 60 Hz deadline
void        CtStatsFrame(double workMs, double sleptMs);
void        CtStatsAudio(unsigned queuedBytes, int dropped);   // the device queue before a block, and whether the gate dropped it
void        CtStatsPresentSkipped(void);
void        CtStatsShownFrames(long n);            // frames a render thread really presented (once called, "shown" counts these, not Swap calls)
// what the frame is (the pacing sites say): "OW" overworld, "FIELD", "BATTLE", "MENU", and the location id
void        CtStatsContext(const char* mode, int loc);
// the picture's sizes, once a frame it is drawn: RENDERED (the uploaded picture), the rectangle it is SHOWN in, the window's output
void        CtStatsResolution(int renderW, int renderH, int shownW, int shownH, int outW, int outH);
// two marks inside an engine frame: 0 = its logic is done and the picture build begins, 1 = the picture is built.
// The next CtStatsFrame reports logic (previous frame's end -> mark 0) and compose (mark 0 -> mark 1).
void        CtStatsMark(int which);
void        CtStatsClockReset(void);               // a pause that is not a frame's work (the options menu): the next frame starts now
// LAPS inside a frame's picture build: the time since mark 0 or the previous lap goes to `part` (the battle compose's steps)
void        CtStatsLap(int part);
// the last full second as text lines for the HUD (NULL until one exists or when off); `n` receives the line count
const char* const* CtStatsHud(int* n);
#ifdef __cplusplus
}
#endif
#endif
