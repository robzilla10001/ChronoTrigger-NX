# The Nintendo Switch build of ct_field (devkitA64 + libnx + switch-sdl2). A standalone project so the PC build's
# CMakeLists.txt is untouched:
#   cmake -S switch -B build-switch -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake && cmake --build build-switch
# Needs the generated sources (src/data/*.h, src/xlat/*.c: python tools/setup.py --rom <your ROM>). Output:
# build-switch/ct_field.nro -> sdmc:/switch/ct/, next to ct_assets.dat and Chrono_Trigger_USA_SNES.sfc (docs/SWITCH.md).
# Host-only differences: no Vulkan, no OpenGL backend (CT_NO_GL), SDL_Renderer for video. The mods menu (the F1 overlay on PC)
# is Dear ImGui 1.91.5 from third_party/imgui (python/ps1 tools/get_imgui.ps1), opened on a controller with L3 + R3.
cmake_minimum_required(VERSION 3.15)
project(chrono_trigger_switch C CXX)
set(CMAKE_C_STANDARD 11)
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
file(GLOB SNES_SRC CONFIGURE_DEPENDS ${ROOT}/snes/*.c)
file(GLOB XLAT_SRC CONFIGURE_DEPENDS ${ROOT}/src/xlat/*.c)
set(CT_SRC field scene lz assets ct_vm collision player world battle objslot tileanim menu menu_cost field_cost oam
    settings present hoststats hostpool motion fieldnative move audio title_intro seq_ppu seq_xlat ct_cpu_infra rom_verify ct_lake_state
    switch_platform savestate savestate_engine)
list(TRANSFORM CT_SRC PREPEND ${ROOT}/src/)
list(TRANSFORM CT_SRC APPEND .c)
set(IMGUI ${ROOT}/third_party/imgui)
if(NOT EXISTS ${IMGUI}/imgui.h)
  message(FATAL_ERROR "third_party/imgui is missing: Dear ImGui 1.91.5 (tools/get_imgui.ps1) -- the mods menu needs it")
endif()
add_executable(ct_field ${CT_SRC} ${XLAT_SRC} ${SNES_SRC} ${ROOT}/src/overlay.cpp
  ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
  ${IMGUI}/backends/imgui_impl_sdl2.cpp ${IMGUI}/backends/imgui_impl_sdlrenderer2.cpp)
target_compile_definitions(ct_field PRIVATE CT_HAVE_IMGUI CT_NO_GL IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)
target_include_directories(ct_field PRIVATE ${IMGUI})
set_source_files_properties(${ROOT}/src/overlay.cpp ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp
  ${IMGUI}/imgui_widgets.cpp ${IMGUI}/backends/imgui_impl_sdl2.cpp ${IMGUI}/backends/imgui_impl_sdlrenderer2.cpp
  PROPERTIES COMPILE_OPTIONS "-fno-exceptions;-fno-rtti")
target_include_directories(ct_field PRIVATE ${ROOT}/src ${ROOT}/snes ${DEVKITPRO}/portlibs/switch/include/SDL2)
target_compile_options(ct_field PRIVATE -O2 -march=armv8-a+crc+crypto -mtune=cortex-a57 -fno-strict-aliasing -w)
target_link_libraries(ct_field PRIVATE SDL2 EGL glapi drm_nouveau stdc++ m)
nx_generate_nacp(ct_field.nacp NAME "Chrono Trigger (C)" AUTHOR "ChronoTrigger-NX" VERSION "0.1")
nx_create_nro(ct_field NACP ct_field.nacp)
