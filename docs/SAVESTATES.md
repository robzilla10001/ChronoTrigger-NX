# Save states

A host feature (like the mods): quick save and quick load of the running game into ten slots. The ROM has no such thing, so nothing here
changes what the game does; the game is the same game whether or not a state is ever taken, and nothing runs until one is asked for.

**Status: a state can be taken anywhere** (the field, the menus, the overworld, a battle, a fade or door in progress, a translated stage such as the
motorcycle race or the gate warp, the title and the load screen). Each mode is proven by the round trip below. Not yet covered by a test: shops, the naming
screen, game over and the ending sequences; if one of them misbehaves after a load, that is a missing part (see *Adding a part*), please report it.

## Using it

Squeeze **ZL + ZR** (both triggers) to open the **SAVE/LOAD STATE MENU**; the game pauses, as it does for the mods menu. It is built for the touch
screen (large boxes; the d-pad and A work too), in the same window as the mods menu:

* **backup current state before overwriting?** (on by default): a save first renames the slot's current file to `state<N>.bak`, replacing any older `.bak`
  without asking, and then writes the new state. A save that is refused for any reason (see below) touches nothing.
* **confirm on load/save?** (on by default): the SAVE and LOAD boxes ask YES / NO first.
* A **slot** list, Slot 0 to Slot 9, and a 2x2 grid:

  | | |
  |---|---|
  | **SAVE STATE (SLOT #)** | **LOAD STATE (SLOT #)**, with the time that state was saved, `MM/DD/YYYY HH:MM:SS` + AM/PM, or `NO STATE CURRENTLY SAVED` |
  | **BACKUP ALL SAVES**: every `state<N>.state` and `state<N>.bak` into one zip named after the date and time, in `states/backups/` | **RESTORE ALL SAVES**: lists the archives in `states/backups/`, you pick one, it asks to confirm, and extracts it over the current save states |

* **Return to game** (or ZL + ZR again, or B) closes the menu. There is no quit button here.

The result of each action is on the menu, under the buttons. Saves are `states/state0.state` .. `state9.state` next to the program; the **first line of a
file is the time it was saved** (text, shown in the LOAD box and never used for anything else), then the state. The name carries no time on purpose, so a
state is not found and overwritten by accident. The archives are ordinary zip files (stored, not compressed); the time in their name has `-` for `/` and
`:` because a file name cannot hold those. Restoring checks the whole archive first and extracts only files named `state<N>.state` / `state<N>.bak`.

A state is a dump of this build's structures: **it loads in the build that wrote it** (a build that changed a part says so and refuses; the file is not trusted
blindly, see *Safety*). The two toggles are saved in `ct_settings.ini` (`[states]`).

## How a state is made

`src/savestate.{c,h}` is the container; it knows nothing about Chrono Trigger. A state is a list of **chunks**, one per **provider**:

| tag | what it holds | where |
|---|---|---|
| `HOST` | the frame counter, the lead's position and facing, the pad latch's memory of the frame before | `savestate_engine.c` |
| `WRAM` | the flat 128 KB image ($7E and bank $7F): everything the VM, the menus and the overworld keep | |
| `SRAM` | the 8 KB battery image the save screen reads | |
| `SCNE` | the scene VM's own state (objects, scheduler, movers); its pointers into the host stay as they are | `ct_vm.c` `CtSceneSnapshot` |
| `AUDI` | the SPC700, DSP, ARAM (the vendored state handler), the clock's fractional carry and what the host told the loader clock; the output FIFO starts empty | `audio.c` `CtAudioStateSave` |
| `LOCN` | the mode and the location; a load enters the location first (the same scene is kept live) | `field.c` `fieldSsEnter` |
| `HVAR` | ~250 locals of `main()` and the file statics of `field.c` / `battle.c` / `snes.c`, by name and size (`src/ss_main_vars.inc`, `src/ss_field_vars.inc`) | `savestate_engine.c` `CtSsVarAdd` |
| `WRLD` `TITL` `TBPT` `SDAT` `BTLH` `FCPT` `STGE` | the overworld, the title intro, the text-box buffer, the scene picture data, the battle host, the collision source, and a running translated stage (its whole vendored machine) | `field.c` |

File layout (little endian), after the one text line of the slot file: `"CTNX"`, format, chunk count, a header (time, frame, location, mode), a 64x56 thumbnail, then per chunk
`tag[4] version flags length crc32 data`, then a CRC-32 of everything before it.

A provider gives the framework its tag, the chunk **version** it writes and the **minVersion** it still reads, whether the chunk is required, and four
functions: `unavailable` (why a state cannot be taken now, or NULL), `save`, `check` and `load`.

## Safety

* **Saving** asks every provider first; if one says no, nothing is written and the reason is shown.
* **Loading is two-phase.** The whole file is verified (header, every chunk's CRC, the trailer) and every provider's `check()` passes before any
  `load()` touches the game. A damaged, truncated, foreign or newer-than-this-build file leaves the game exactly as it was. (`CtSsSelfTest` flips bits at
  150 offsets and cuts the file at 150 lengths and asserts that every one is refused and nothing was applied.)
* Unknown chunks are skipped; a missing required chunk, or one older than a provider's `minVersion` or newer than its version, refuses the load.
* Requests run at one safe point of the frame (`fieldSaveStateRun`, at the start of the loop, the same place the menu runs), so a state is always taken
  and restored between whole frames; the round trip below proves exactly that point.

## Proving it

* `ct_field --savestate-selftest`: the container's own checks (no game needed).
* `CT_SS_ROUNDTRIP=<A>:<B>` (e.g. `60:90`), with `--selftest --scene N --frames A+B+..`: one process saves a state at frame A, runs B frames hashing the
  WRAM, the picture and the sound processor after each, loads the state back at frame A+B and runs the same B frames again. The two runs must be identical;
  if not, the first frame that differs and the first differing WRAM byte say what the state is missing. The lead is driven from the frame number alone (a
  script's own cursor is state a load would not rewind). Both are in the regression suite (`savestate-container-selftest`, `savestate-roundtrip-lead-walking`).
* **Mutation discipline.** A green round trip means little unless it can go red. Skipping the WRAM, the scene or the sound load each makes it diverge on the first
  frame (done when this was written); do the same for any provider you add. Two parts are saved but not yet *proven* needed by this scene: the lead's
  position (the VM re-derives it) and the pad latch's memory (only the battle reads it).

Also: `CT_SS_FORCE=1` (probe a mode regardless of availability), `CT_SS_XSAVE=<frame>:<n>:<path>` / `CT_SS_XLOAD=<path>` (save in one process, load in a fresh one and
compare the next frames: proves the file holds everything), `CT_HIDDEN_WINDOW=1` (needed for battles). The matrix of scenarios (walk, menu, doors, overworld, title,
load screen, new game, talk, eight battle points, attract at many frames) passes.

## Adding a part (what the next modes need)

1. Find the state: the host variables and statics the mode keeps outside the WRAM (the round trip's first divergent byte points at them).
2. Write the provider (`save` / `check` / `load`, versioned) next to the code that owns the state; keep `check` free of side effects.
3. Register it in `CtSsEngineBind`, and let the host lift the matching reason in the availability test in `field.c` (the *SAVE STATES* block).
4. Extend the round trip to run in that mode and mutation-check it.

Host-only state not restored: the camera's smoothing, the frames-between interpolation history (a load clears it) and the audio output queue.

Also open: a quick save / quick load without the menu, and an autosave slot.
