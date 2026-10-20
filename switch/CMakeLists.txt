# The Nintendo Switch build of ct_field (devkitA64 + libnx + switch-sdl2). A standalone project so the PC build's
# CMakeLists.txt is untouched:
#   cmake -S switch -B build-switch -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake && cmake --build build-switch
# Needs the generated sources (src/data/*.h, src/xlat/*.c: python tools/setup.py --rom <your ROM>). Output:
# build-switch/ct_field.nro -> sdmc:/switch/ct/, next to ct_assets.dat and Chrono_Trigger_USA_SNES.sfc (docs/SWITCH.md).
# Video backends (mods menu, chosen at the next start): SDL_Renderer (default), OpenGL 3.3 through Mesa (glad as the loader), and
# deko3d (src/present_dk.cpp; its shaders are pre-compiled in src/dk/dk_shaders.inc, rebuild with src/dk/build_shaders.sh), and Vulkan
# through mesa-switch's NVK, linked statically and loaderless: sh switch/get_mesa.sh builds it into third_party/mesa-switch, and this
# file then compiles the Vulkan backend (src/present_vk.c) in. Without that folder the build has no working Vulkan. The mods menu (the F1 overlay on PC)
# is Dear ImGui 1.91.5 from third_party/imgui (python/ps1 tools/get_imgui.ps1), opened on a controller with L3 + R3.
cmake_minimum_required(VERSION 3.15)
project(chrono_trigger_switch C CXX)
set(CMAKE_C_STANDARD 11)
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
file(GLOB SNES_SRC CONFIGURE_DEPENDS ${ROOT}/snes/*.c)
file(GLOB XLAT_SRC CONFIGURE_DEPENDS ${ROOT}/src/xlat/*.c)
set(CT_SRC field scene lz assets ct_vm collision player world battle objslot tileanim menu menu_cost field_cost oam
    settings present bench hoststats hostpool motion fieldnative move audio title_intro seq_ppu seq_xlat ct_cpu_infra rom_verify ct_lake_state
    switch_platform savestate savestate_engine)
list(TRANSFORM CT_SRC PREPEND ${ROOT}/src/)
list(TRANSFORM CT_SRC APPEND .c)
set(IMGUI ${ROOT}/third_party/imgui)
if(NOT EXISTS ${IMGUI}/imgui.h)
  message(FATAL_ERROR "third_party/imgui is missing: Dear ImGui 1.91.5 (tools/get_imgui.ps1) -- the mods menu needs it")
endif()
add_executable(ct_field ${CT_SRC} ${XLAT_SRC} ${SNES_SRC} ${ROOT}/src/overlay.cpp
  ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
  ${ROOT}/src/present_dk.cpp
  ${IMGUI}/backends/imgui_impl_sdl2.cpp ${IMGUI}/backends/imgui_impl_sdlrenderer2.cpp ${IMGUI}/backends/imgui_impl_opengl3.cpp)
# SLANG SHADERS (src/slang, docs/SLANG.md): RetroArch .slangp presets run over the picture on the GPU. glslang and SPIRV-Cross come from third_party/slang-switch
# (sh switch/get_slang.sh); without that folder, or with -DCT_WITHOUT_SLANG=ON, the program is built without shader support and everything else is unchanged.
option(CT_WITHOUT_SLANG "build without the slang shader support" OFF)
set(SL ${ROOT}/third_party/slang-switch)
if(NOT CT_WITHOUT_SLANG AND EXISTS ${SL}/lib/libglslang.a)
  message(STATUS "Shaders: slang support from ${SL}")
  target_sources(ct_field PRIVATE ${ROOT}/src/slang/slang_preset.c ${ROOT}/src/slang/slang_source.c ${ROOT}/src/slang/slang_chain.c ${ROOT}/src/slang/slang_image.c
    ${ROOT}/src/slang/slang_gl.c ${ROOT}/src/slang/slang_vk.c ${ROOT}/src/slang/slang_compile.cpp)
  if(NOT CT_WITHOUT_DEKO3D AND EXISTS ${SL}/lib/libuam.a)
    target_sources(ct_field PRIVATE ${ROOT}/src/slang/slang_dk.cpp)
    set_source_files_properties(${ROOT}/src/slang/slang_dk.cpp PROPERTIES COMPILE_OPTIONS "-fno-exceptions;-fno-rtti")
    target_compile_definitions(ct_field PRIVATE CT_HAVE_SLANG_DK)
    set(CT_UAM_LIB ${SL}/lib/libuam.a)
  else()
    message(STATUS "Shaders: deko3d runs without them (no libuam.a: sh switch/get_uam.sh)")
  endif()
  set_source_files_properties(${ROOT}/src/slang/slang_compile.cpp PROPERTIES COMPILE_OPTIONS "-std=gnu++17")
  target_compile_definitions(ct_field PRIVATE CT_HAVE_SLANG CT_HAVE_PNG)
  target_include_directories(ct_field PRIVATE ${ROOT}/src/slang ${SL}/include ${SL}/include/glslang ${SL}/include/spirv_cross)
  set(CT_SLANG_LIBS -Wl,--start-group ${SL}/lib/libspirv-cross-glsl.a ${SL}/lib/libspirv-cross-core.a ${SL}/lib/libglslang-default-resource-limits.a
    ${SL}/lib/libglslang.a ${SL}/lib/libSPIRV.a ${SL}/lib/libMachineIndependent.a ${SL}/lib/libGenericCodeGen.a ${SL}/lib/libOSDependent.a -Wl,--end-group ${CT_UAM_LIB} png16 z)
else()
  set(CT_SLANG_LIBS "")
  message(STATUS "Shaders: third_party/slang-switch not found (sh switch/get_slang.sh) -- built without slang shader support")
endif()
# BISECTING THE EXIT DIALOG (docs/SWITCH.md): CT_WITHOUT_DEKO3D / CT_WITHOUT_VULKAN leave those backends, and their libraries, out of a build.
option(CT_WITHOUT_DEKO3D "build without the deko3d backend" OFF)
option(CT_WITHOUT_VULKAN "build without the Vulkan backend" OFF)
if(CT_WITHOUT_DEKO3D)
  target_compile_definitions(ct_field PRIVATE CT_HAVE_IMGUI IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)
  set(CT_DK_LIB "")
  get_target_property(_srcs ct_field SOURCES)
  list(REMOVE_ITEM _srcs ${ROOT}/src/present_dk.cpp)
  set_target_properties(ct_field PROPERTIES SOURCES "${_srcs}")
else()
  target_compile_definitions(ct_field PRIVATE CT_HAVE_IMGUI CT_HAVE_DEKO3D IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)
  set(CT_DK_LIB deko3d)
endif()
# ImGui's GL backend with glad as its loader (the stock one dlopens libGL, which the console does not have)
set_source_files_properties(${IMGUI}/backends/imgui_impl_opengl3.cpp PROPERTIES COMPILE_DEFINITIONS IMGUI_IMPL_OPENGL_LOADER_CUSTOM
  COMPILE_OPTIONS "-include;glad/glad.h;-fno-exceptions;-fno-rtti")
target_include_directories(ct_field PRIVATE ${IMGUI})
set_source_files_properties(${ROOT}/src/overlay.cpp ${ROOT}/src/present_dk.cpp ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp
  ${IMGUI}/imgui_widgets.cpp ${IMGUI}/backends/imgui_impl_sdl2.cpp ${IMGUI}/backends/imgui_impl_sdlrenderer2.cpp
  PROPERTIES COMPILE_OPTIONS "-fno-exceptions;-fno-rtti")
target_include_directories(ct_field PRIVATE ${ROOT}/src ${ROOT}/snes ${DEVKITPRO}/portlibs/switch/include/SDL2 ${DEVKITPRO}/portlibs/switch/include)
target_compile_options(ct_field PRIVATE -O2 -march=armv8-a+crc+crypto -mtune=cortex-a57 -fno-strict-aliasing -w)
# chain_probe.nro: the smallest self-relaunching program (switch/chain_probe.c), to tell the loader's own limit on chained launches from anything the game leaves open.
add_executable(chain_probe ${CMAKE_CURRENT_SOURCE_DIR}/chain_probe.c)
target_compile_options(chain_probe PRIVATE -O2 -w)
nx_generate_nacp(chain_probe.nacp NAME "chain_probe" AUTHOR "ChronoTrigger-NX" VERSION "0.1")
nx_create_nro(chain_probe NACP chain_probe.nacp)
add_executable(chain_probe_big ${CMAKE_CURRENT_SOURCE_DIR}/chain_probe.c)                            # the same with a 28 MB array linked in: the size of the game's NRO
target_compile_options(chain_probe_big PRIVATE -O2 -w)
target_compile_definitions(chain_probe_big PRIVATE BIG_PAD)
nx_generate_nacp(chain_probe_big.nacp NAME "chain_probe_big" AUTHOR "ChronoTrigger-NX" VERSION "0.1")
nx_create_nro(chain_probe_big NACP chain_probe_big.nacp)

# TWO CONFIGURATIONS, built side by side (switch/build_both.sh):
#   default  (no option)           the console's own Mesa (portlibs libEGL, libdrm_nouveau) for SDL_Renderer and OpenGL, plus NVK for Vulkan as one
#                                  isolated object (switch/get_mesa.sh). The known-good one.
#   -DCT_MESA_UNIFIED=ON           EVERYTHING on mesa-switch's Mesa 26.2: its EGL / OpenGL / GLES (for SDL and the OpenGL backend) and its NVK Vulkan, one
#                                  stack (switch/get_mesa_unified.sh -> third_party/mesa-switch-unified). Output ct_field_mesa.nro.
option(CT_MESA_UNIFIED "link the unified mesa-switch stack instead of the console's Mesa" OFF)
set(MESA ${ROOT}/third_party/mesa-switch)
if(CT_MESA_UNIFIED)
  set(MU ${ROOT}/third_party/mesa-switch-unified)
  if(NOT EXISTS ${MU}/lib/libEGL.a)
    message(FATAL_ERROR "third_party/mesa-switch-unified is missing (sh switch/get_mesa_unified.sh)")
  endif()
  message(STATUS "Mesa: the unified mesa-switch stack from ${MU}")
  target_compile_definitions(ct_field PRIVATE CT_HAVE_VULKAN CT_MESA_UNIFIED IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
  target_include_directories(ct_field BEFORE PRIVATE ${MU}/include)
  target_sources(ct_field PRIVATE ${ROOT}/src/present_vk.c ${IMGUI}/backends/imgui_impl_vulkan.cpp)
  set_source_files_properties(${IMGUI}/backends/imgui_impl_vulkan.cpp PROPERTIES COMPILE_OPTIONS "-fno-exceptions;-fno-rtti")
  file(GLOB MU_RUST ${MU}/rust/*.rlib)
  target_link_options(ct_field PRIVATE -Wl,--gc-sections)
  # the archives repeat shared helper code (each is a fat static library of the same Mesa): the copies are identical, first wins
  target_link_libraries(ct_field PRIVATE SDL2 glad -Wl,--allow-multiple-definition -Wl,--start-group
    ${MU}/lib/libEGL.a ${MU}/lib/libGLESv2.a ${MU}/lib/libGLESv1_CM.a ${MU}/lib/libglapi.a ${MU}/lib/libvulkan.a ${MU_RUST} -Wl,--end-group
    ${CT_SLANG_LIBS} zstd expat z deko3d stdc++ m)
  nx_generate_nacp(ct_field.nacp NAME "Chrono Trigger (C) - Mesa" AUTHOR "ChronoTrigger-NX" VERSION "0.6b")
  nx_create_nro(ct_field NACP ct_field.nacp)
  return()
endif()
if(NOT CT_WITHOUT_VULKAN AND EXISTS ${MESA}/nvk_vulkan.o AND EXISTS ${IMGUI}/backends/imgui_impl_vulkan.cpp)
  message(STATUS "Vulkan: mesa-switch NVK from ${MESA}")
  target_compile_definitions(ct_field PRIVATE CT_HAVE_VULKAN IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
  target_include_directories(ct_field PRIVATE ${MESA}/include)
  target_sources(ct_field PRIVATE ${ROOT}/src/present_vk.c ${IMGUI}/backends/imgui_impl_vulkan.cpp)
  set_source_files_properties(${IMGUI}/backends/imgui_impl_vulkan.cpp PROPERTIES COMPILE_OPTIONS "-fno-exceptions;-fno-rtti")
  # ONE relocatable object (switch/get_mesa.sh): NVK, its Rust compiler and the Rust runtime, with every symbol but the vk* API made local, so
  # it cannot collide with the console's older Mesa that SDL's EGL and the OpenGL backend link. Its unused parts go away with --gc-sections.
  set(CT_VULKAN_LIBS ${MESA}/nvk_vulkan.o zstd expat z)
  target_link_options(ct_field PRIVATE -Wl,--gc-sections)
else()
  message(STATUS "Vulkan: third_party/mesa-switch not found (sh switch/get_mesa.sh) -- built without it")
endif()
target_link_libraries(ct_field PRIVATE ${CT_VULKAN_LIBS} ${CT_SLANG_LIBS} SDL2 glad EGL glapi drm_nouveau ${CT_DK_LIB} stdc++ m)
nx_generate_nacp(ct_field.nacp NAME "Chrono Trigger (C)" AUTHOR "ChronoTrigger-NX" VERSION "0.6b")
nx_create_nro(ct_field NACP ct_field.nacp)
