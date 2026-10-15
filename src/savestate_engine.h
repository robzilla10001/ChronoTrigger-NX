// savestate_engine.h - the engine's providers for the save-state framework (savestate.h): what a state of the running field is made of.
//
//   HOST  the host loop's own variables (the frame counter, the lead's position and facing)
//   WRAM  the flat 128 KB image ($7E and bank $7F) the scene VM, the overworld and the menus all work on
//   SRAM  the 8 KB battery image the save screen reads and writes
//   SCNE  the scene VM (objects, scheduler, movers; its pointers into the host stay as they are)
//   AUDI  the sound processor: SPC700, DSP, ARAM and the clock's carry
//
// Where a state can be taken is the host's call: it sets `unavailable` each frame (NULL = a location in free play). Parts that do not exist yet
// (the battle, the menus, the overworld, the title's own machines, a fade in progress, a door being taken) are not guessed at: the state is
// refused with the reason, rather than saved incomplete. Each such part arrives as another provider, with the host's variables it needs.
#ifndef CT_SAVESTATE_ENGINE_H
#define CT_SAVESTATE_ENGINE_H
#include <stddef.h>
#include <stdint.h>
#include "savestate.h"
#include "ct_vm.h"
#include "audio.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct CtSsHost {
  uint8_t* wram;                 // CtWramSys(): the flat 128 KB image
  uint8_t* sram; size_t sramLen;
  CtScene* vm;                   // the live scene; NULL off a location
  CtAudio* audio;                // NULL when the run has no sound
  double *px, *py; int *pz, *pfac;   // the lead
  int* frame;                    // the engine's frame counter
  uint16_t *padPrev, *padAuto;   // the pad latch's memory of the frame before (edges), and the battle's copy of it
  const char* unavailable;       // NULL = a state can be taken now; else why not
  int mode, loc;                 // CT_SS_MODE_*, the location (for the slot list)
  int scene;                     // the field scene the host is in (the LOCN part)
  int (*enter)(void* ctx, int mode, int scene);   // the host switches to a mode / scene before the rest of a state is restored; 0 = ok
  void* enterCtx;
  const uint8_t* fb; int fbW, fbH;   // the last composed picture (RGB24), for the slot's thumbnail
} CtSsHost;
// The host loop's own variables (the `HVAR` part): registered by NAME with their address and size, saved and restored by name. A state keeps what it finds:
// a variable added since the state was written keeps its current value on load, one removed is skipped, one whose size changed refuses the load.
// Only plain values and arrays of them (no pointers). Register before the first state is taken (field.c includes src/ss_main_vars.inc for main()'s).
void CtSsVarAdd(const char* name, void* p, size_t n);
#define SSV(x)      CtSsVarAdd(#x, &(x), sizeof(x))       /* a variable or array by its own name */
#define SSVN(x, n)  CtSsVarAdd(#x, (void*)(x), (n))       /* a buffer the variable points at, n bytes */
// Registers the providers (once) on `s`, with `h` as their context. `h` must outlive `s`.
void CtSsEngineBind(CtSs* s, CtSsHost* h);
#ifdef __cplusplus
}
#endif
#endif
