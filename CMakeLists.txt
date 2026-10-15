cmake_minimum_required(VERSION 3.15)
project(chrono_trigger_c C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# Oracle core: vendored LakeSnes (snes/*.c). main.c from LakeSnes is NOT included
# (it pulls in SDL); the M0 harness is headless.
file(GLOB SNES_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/snes/*.c")

add_executable(ct
  src/main.c
  src/ct_cpu_infra.c src/rom_verify.c
  src/ct_shadow.c  # --shadow: the routine-level shadow (docs/SHADOW_PROPOSAL.md)
  src/ct_lake_state.c
  src/harness_drive.c
  src/boot.c
  src/lz.c
  src/scene.c
  src/assets.c
  src/ct_vm.c
  src/oam.c
  src/move.c
  src/input.c
  src/player.c
  src/collision.c
  src/world.c
  src/audio.c
  src/battle.c   # --sweepaitarget: the first bank-$C1 micro-execution harness needs the battle port
  src/objslot.c  # FD/E39C, the palette-effect records -- one body for the battle and the field VM
  src/tileanim.c # FD/DE98 + FD/E022, the field's animated BG tiles (--sweepbganim)
  src/seq_xlat.c src/xlat/seq_c3.c src/xlat/wipe_c3.c src/xlat/hdma_fd.c src/xlat/owwin_c6.c src/xlat/gwarp_fd.c  # --xlatcmp: the race's program translated from the ROM, lockstepped per instruction
  src/seq_ppu.c  # --seqppucmp: the sequence stages' vendored-PPU picture, graded against the oracle's own frame
  ${SNES_SRC}
)
target_include_directories(ct PRIVATE snes src)
if(UNIX)
  target_link_libraries(ct m)   # lround (ct_vm.c); MSVC's CRT carries libm itself
endif()

if(MSVC)
  target_compile_definitions(ct PRIVATE _CRT_SECURE_NO_WARNINGS)
  # The LakeSnes core emits many benign implicit-conversion warnings; quiet them.
  target_compile_options(ct PRIVATE /W2 /wd4244 /wd4267 /wd4018 /wd4146 /wd4334 /wd4101)
endif()

# Interactive SDL2 viewer for the C scene engine. Built only when SDL2 is present
# in third_party/SDL2 (downloaded, git-ignored). The headless 'ct' stays SDL-free;

# PC-side test targets live in cmake/pc_targets.cmake (local only, git-ignored); without it only the oracle is built.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/pc_targets.cmake OPTIONAL)
