# ChronoTrigger-NX

A native C re-implementation of **Chrono Trigger** (SNES, USA, 1995), forked from [TemplarServant/BelthasarProject](https://github.com/TemplarServant/BelthasarProject).

<p align="center">
  <img src="docs/media/widescreen-battle.gif" alt="A battle in the port with the widescreen mod on: Marle casts a spell on Zombor" width="796">
  <br><sub>The C engine running a battle from the attract demo, widescreen mod on (<a href="docs/media/widescreen-battle.mp4">MP4</a>).</sub>
</p>

> **Legal notice.** This repository contains **no part of the game**: no program code, graphics, music, text or
> data from the cartridge. To use it you need your own dump of **Chrono Trigger (USA)**, made from your cartridge
> with a cartridge reader -- how: [docs/ROM.md](docs/ROM.md#dumping-your-cartridge), following the preservation
> community's guide <https://dumping.guide/carts/nintendo/snes>. The engine, the oracle and the setup flow check the
> dump before they use it:
>
> | | |
> |---|---|
> | Size | 4,194,304 bytes (4 MiB), headerless. A 512-byte copier header is accepted and stripped. |
> | SHA-1 | `de5822f4f2f7a55acb8926d4c0eaa63d5d989312` (the No-Intro dump) |
> | CRC32 | `2D206BF7` |
>
> Everything derived from the ROM (the asset container, generated tables, translated stages, test states) is
> produced on your machine from your dump and is never distributed. See [docs/ROM.md](docs/ROM.md).
>
> Chrono Trigger is © Square Enix (originally Square Co., Ltd.). This project is not affiliated with or endorsed by
> Square Enix or Nintendo.

## What it is

From the parent project:

> BelthasarProject is not a byte-matching disassembly and not an emulator. It is a portable C engine that behaves like the
> game: the field, the event scripts, battles, menus, the overworld and the sound, all re-implemented routine by
> routine from the 65816 code. The method is the one [snesrev/zelda3](https://github.com/snesrev/zelda3) proved:
> - The original ROM, run in an embedded SNES core (the **oracle**, built on
>   [LakeSnes](https://github.com/elzo-d/LakeSnes)), is the only source of truth.
> - Every behaviour is derived from the ROM's disassembly and the oracle's RAM. Nothing is guessed, tuned by eye, or
>   special-cased per scene.
> - A routine counts as ported when the oracle and the C code agree, and a regression check pins that agreement.
> The rules behind this, and the decisions that shaped them, are in [docs/METHODOLOGY.md](docs/METHODOLOGY.md) and
> [docs/DECISIONS.md](docs/DECISIONS.md).
> 

## Screenshots

Every picture is the C engine's own output during the attract demo (not the emulator), made by
[`tools/release/make_media.py`](tools/release/make_media.py).

| | |
|---|---|
| ![The title screen](docs/media/title.png) | ![Frog's tech over three enemies in a battle](docs/media/battle-tech.png) |
| The title sequence | A battle tech, vanilla 256x224 |

With the widescreen mod (16:9), the field, the overworld and battles show more of the map at the sides:

| | |
|---|---|
| ![A floating continent over the clouds, widescreen](docs/media/widescreen-zeal.png) | ![A battle on a bridge, widescreen](docs/media/widescreen-battle.png) |
| ![The party in a forest, widescreen](docs/media/widescreen-forest.png) | ![The party before a castle gate, widescreen](docs/media/widescreen-castle.png) |

The mods are switched on in the F1 overlay. With every option at its default, the game is the vanilla game:

<p align="center"><img src="docs/media/options-overlay.png" alt="The F1 options overlay, Video tab" width="796"></p>

## Status

The game boots like the cartridge does: the title sequence, the attract demo, the load screen, and a new game into
Crono's bedroom. From there the field, battles, menus, shops, the overworld and the music run on the C engine. The
project's owner plays it, reports what looks wrong, and each report is traced back to the ROM before it is fixed.
It is **not yet verified playable from start to finish**.

| Area | State | Notes |
|---|---|---|
| Field | Works | Event-script VM and scheduler, collision, movers, sprite priority and draw list, camera, textbox, doors and location changes, chests, naming screen, minigames met so far (the fair, the tent of horrors), iris and window wipes, the staff roll's caption plane. The field VM covers 217 opcodes, 99.9% of all script sites. The remaining sites are two ops that reset the stack (the engine runs them as the reboot they are, but no sweep can grade them) and the ROM's own panic stub. |
| Battle | Works | ATB, the menu pass, physical attacks, techs (single, dual, triple), items, status effects, enemy AI from the ROM's condition tables, counters, death dissolve, rewards, level-up, running away. Enemy census: 253 of 255 enemy fights identical to the cartridge under the timing rule. |
| Menus | Works | All six X-menu screens (Item, Equip, Tech, Config, Save, Form), shops, the load screen. Saving writes the 8 KB battery image to `ct_sram.srm`. |
| Overworld | Works | Camera and tilemap streaming, the ambient-object VM (birds, boats), the ferry, party followers, zoned music. |
| Sound | Works, with debt | The bank `$C7` driver upload, the song loader, the command channel. The SPC700 itself still runs on LakeSnes's SPC700/DSP core (see [Not done](#not-done)). |
| Title and attract | Works | The title sequence is the overlay's own machine; the attract demo is audited against the cartridge frame by frame. |
| Mods (F1) | Works | Widescreen, internal resolution, frames between (60+ fps), free sync. Opt-in; with every option at its default the game is the vanilla game. |

### Not done

- **Part of the suite still needs private states.** Everything the build needs is made from your ROM, and 582 of
  the 826 regression checks run from it; the other 244 read a state from the owner's playthrough that no recipe
  reproduces yet, and SKIP in a fresh clone. See [docs/ROADMAP.md](docs/ROADMAP.md#1-the-public-release-and-what-it-left-open).
- **Some pieces still run on emulated hardware.** The sound processor runs on LakeSnes's SPC700/DSP. Five
  timing-sensitive stages (the motorcycle race and other `$C3` sequences, the window wipe, an overworld window, the
  gate warp, an HDMA table builder) are ROM programs translated instruction by instruction and run over the vendored
  65816/PPU. The project's rule is that nothing in the port should be emulation, so these are debt to replace with
  native C.
- **Open divergences.** A list of known differences from the cartridge (a few frames of timing in some battle
  exits and location loads, a handful of pictures) is kept in [docs/ROADMAP.md](docs/ROADMAP.md#2-engine).
- **No continuous-playthrough check.** Each subsystem is graded on its own scenes; nobody has yet graded the whole
  game end to end.

## Quick start

### Requirements

> *In progress*
