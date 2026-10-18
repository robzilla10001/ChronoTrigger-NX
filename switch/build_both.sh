#!/bin/sh
# Builds the Switch game in its TWO configurations side by side (needs the switch_dev_build container of the README's Quick start):
#   build-switch/ct_field.nro        the known-good one: the console's Mesa for SDL_Renderer / OpenGL, NVK Vulkan isolated beside it
#   build-switch/ct_field_mesa.nro   everything on mesa-switch's Mesa 26.2 (EGL, OpenGL, GLES, Vulkan)
# Set SKIP_MESA=1 to reuse what third_party/mesa-switch{,-unified} already hold.
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"; cd "$HERE"
if [ -z "$SKIP_MESA" ]; then sh switch/get_mesa.sh; sh switch/get_mesa_unified.sh; fi
tar -cf - src switch third_party/mesa-switch third_party/mesa-switch-unified third_party/imgui snes | docker exec -i switch_dev_build tar -xf - -C /src
docker exec switch_dev_build bash -c 'export DEVKITPRO=/opt/devkitpro; cd /src
  cmake -S switch -B bs -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake >/dev/null && cmake --build bs -j8
  cmake -S switch -B bs-mesa -DCT_MESA_UNIFIED=ON -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake >/dev/null && cmake --build bs-mesa -j8'
mkdir -p build-switch
docker cp switch_dev_build:/src/bs/ct_field.nro build-switch/ct_field.nro
docker cp switch_dev_build:/src/bs-mesa/ct_field.nro build-switch/ct_field_mesa.nro
ls -la build-switch/*.nro
