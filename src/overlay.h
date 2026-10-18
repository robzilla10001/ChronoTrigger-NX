// overlay.h - the F1 options overlay (Dear ImGui over the game's picture). C interface; src/overlay.cpp implements it
// when third_party/imgui is present (CT_HAVE_IMGUI), src/overlay_stub.c otherwise (no overlay, F1 does nothing).
// The host (field.c) runs it MODAL: the game stands still while it is open, and the options it edits are
// settings.h's.
#ifndef CT_OVERLAY_H
#define CT_OVERLAY_H
#include "SDL.h"
#include "present.h"
#ifdef __cplusplus
extern "C" {
#endif

// What a frame of the overlay changed, for the host to apply.
enum {
  CT_OVL_VIDEO   = 1,   // picture options (scale, aspect, filter, vsync, fullscreen, window size)
  CT_OVL_CLOSE   = 2,   // the player closed the overlay
  CT_OVL_QUIT    = 4,   // the player asked to leave the game
  CT_OVL_RESTART = 8,   // an option that takes effect on the next start changed (the render driver)
};

int  CtOverlayInit(CtPresenter* pr);                      // 1 = the overlay is available (on pr's backend)
void CtOverlayShutdown(void);
int  CtOverlayAvailable(void);
// 1 = the overlay consumed the event (a key being bound, typing in a widget): the host must not act on it.
int  CtOverlayProcessEvent(const SDL_Event* e);
// Build this frame's options window; returns CT_OVL_* flags.
int  CtOverlayFrame(void);
void CtOverlayHud(const char* const* lines, int n);       // the performance HUD (hoststats) over the frame being recorded, for the backends that have no HUD of their own; no-op when n <= 0
void CtOverlayRefreshPads(void);                          // tell ImGui's backend to re-read the connected controllers (a menu opening, a device event)
void CtOverlayRender(void);                               // draw it (after the host drew the game picture)
void CtOverlaySetPadName(const char* name);               // the connected controller, or NULL
void CtOverlaySelectTab(int tab);                         // (CT_OVERLAY_SHOT) open on tab 0..3 next frame

// THE SAVE/LOAD STATE MENU (ZL + ZR): a second window of the same overlay, built for the touch screen -- large buttons, a slot list, a 2x2 grid:
// save, load, back up every slot, restore a backup. The host runs it MODAL like the options (the game stands still). What it asks of the game goes
// through savestate.h's request queue; the flag below asks the host to run that queue NOW (the game is between two frames), so the answer is on the menu at once.
enum {
  CT_SMENU_CLOSE   = 1,   // the player chose "Return to game"
  CT_SMENU_SERVICE = 2,   // a save / load was queued: the host runs it before the next frame of the menu
};
void CtStateMenuOpen(void);                               // reset the menu (popups shut, message cleared) as it is shown
int  CtStateMenuFrame(void);                              // build this frame's window; CT_SMENU_* flags (draw with CtOverlayRender)
int  CtStateMenuPopupOpen(void);                          // 1 = a confirmation / list is on top: B closes that first, not the menu
void CtStateMenuDebugPopup(int which);                    // (CT_OVERLAY_SHOT) 1 = the restore list, 2 = a save confirmation, 3 = the restore confirmation
#ifdef __cplusplus
}
#endif
#endif
