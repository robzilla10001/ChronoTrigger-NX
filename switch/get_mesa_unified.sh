#!/bin/sh
# The SECOND configuration: everything on mesa-switch's Mesa 26.2 -- its EGL / OpenGL / GLES (what SDL_Renderer and the OpenGL backend draw through) AND its
# NVK Vulkan -- instead of the console's older Mesa. Stages third_party/mesa-switch-unified/{include,lib,rust}; then
#   cmake -S switch -B build-switch-mesa -DCT_MESA_UNIFIED=ON -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake   ->  ct_field_mesa.nro
# (switch/build_both.sh does the whole thing). Same requirements and Rust fixes as switch/get_mesa.sh, which it runs first when the checkout is not built yet.
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${1:-$HERE/third_party/_mesa-switch-src}"
OUT="$HERE/third_party/mesa-switch-unified"
C=mesa-switch-build
[ -d "$SRC/builddir-native" ] || sh "$HERE/switch/get_mesa.sh" "$SRC"
docker start $C >/dev/null
docker exec -e MESA_SWITCH_RUST_TARGET=aarch64-unknown-linux-gnu $C bash -c '
set -e
export PATH="/usr/local/libexec:/project/builddir-native/src/compiler/clc:/project/builddir-native/src/compiler/spirv:$PATH"
cd /project
git config --global --add safe.directory /project
meson setup builddir-unified --wipe --cross-file switch_cross_file.txt --default-library=static \
    --prefix=/opt/devkitpro/portlibs/switch --libdir=lib \
    --buildtype=release -Doptimization=2 -Db_lto=false -Db_ndebug=true \
    -Dvulkan-drivers=nouveau -Dgallium-drivers=nouveau,zink -Dgallium-rusticl=false \
    -Dplatforms=switch -Degl-native-platform=switch \
    -Dglx=disabled -Degl=enabled -Dopengl=true -Dgles1=enabled -Dgles2=enabled \
    -Dvideo-codecs= -Dshader-cache=enabled -Dxmlconfig=enabled -Dexpat=enabled -Dtools=[] \
    -Dllvm=disabled -Dshared-glapi=disabled -Dshared-llvm=disabled \
    -Dmesa-clc=system -Dprecomp-compiler=system -Dcpp_rtti=false -Dbuild-tests=false \
    -Dnvk-build-id="$(git rev-parse HEAD)"
ninja -C builddir-unified
rm -rf /project/mesa-unified-install
meson install -C builddir-unified --destdir /project/mesa-unified-install'
S="$SRC/mesa-unified-install/opt/devkitpro/portlibs/switch"
rm -rf "$OUT"; mkdir -p "$OUT/lib" "$OUT/rust"
cp "$S"/lib/libEGL.a "$S"/lib/libGLESv2.a "$S"/lib/libGLESv1_CM.a "$S"/lib/libglapi.a "$S"/lib/libvulkan.a "$OUT/lib/"
cp -r "$S/include" "$OUT/include"
# the Rust runtime the shader compiler needs (the cross standard library of the container's nightly)
docker exec $C sh -c 'RL=$(ls -d /root/.rustup/toolchains/*/lib/rustlib/aarch64-unknown-linux-gnu/lib); for n in std core alloc compiler_builtins hashbrown addr2line gimli miniz_oxide adler2 memchr object rustc_demangle cfg_if libc panic_abort panic_unwind unwind std_detect; do ls $RL/lib$n-*.rlib; done; ls $RL/librustc_std_workspace_*.rlib' |
  while read f; do docker cp "$C:$f" "$OUT/rust/"; done
ls "$OUT/lib" "$OUT/rust" | head -40
