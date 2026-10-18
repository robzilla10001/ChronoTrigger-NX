#!/bin/sh
# Builds mesa-switch (https://github.com/danfromtico/mesa-switch, Mesa 26.2.4 for Horizon: the NVK Vulkan driver) and stages ONE object the
# game links into third_party/mesa-switch/{include,nvk_vulkan.o}. Needs Docker; the project's own Docker.rust image (devkitA64 + SPIRV-Tools + a Rust
# nightly) is built on the first run, which takes a while and ~10 GB of disk. The Vulkan video backend is compiled in when third_party/mesa-switch
# exists (switch/CMakeLists.txt); without it the Switch build has no working Vulkan entry.
#   sh switch/get_mesa.sh [path to a mesa-switch checkout]        SKIP_BUILD=1: only stage an existing build
#
# Two things the project's Docker build does not do on its own, done here:
#  1. Rust must be cross-compiled for aarch64 (its wrapper only does so when MESA_SWITCH_RUST_TARGET is set, else it silently emits x86-64 objects),
#     and the build-machine crates (proc macros and what they use) must stay native: the wrapper learns that from the plain `cc` linker Meson gives them.
#  2. The console's own older Mesa (libEGL, for SDL_Renderer and the OpenGL backend) defines the same util_* / ralloc_* symbols. So NVK, its Rust compiler
#     and the Rust runtime are combined into one relocatable object in which only the vk* API stays global; --gc-sections drops what is unused.
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${1:-$HERE/third_party/_mesa-switch-src}"
OUT="$HERE/third_party/mesa-switch"
REV="${MESA_SWITCH_REV:-main}"
C=mesa-switch-build
if [ ! -d "$SRC/.git" ]; then git clone https://github.com/danfromtico/mesa-switch "$SRC"; git -C "$SRC" checkout "$REV"; fi
if [ -z "$SKIP_BUILD" ]; then
  cd "$SRC"
  python3 - <<'PY'
import re
p = 'rustc-switch-wrapper.sh'; s = open(p).read()
if 'linker=cc' not in s:
    s = s.replace("*sanity_check_for_rust.rs*|*sanitycheckrs.rs*) RUST_TARGET= ;;",
                  "*sanity_check_for_rust.rs*|*sanitycheckrs.rs*) RUST_TARGET= ;;\n        proc-macro|linker=cc) RUST_TARGET= ;;      # build-machine crates run on the host", 1)
    open(p, 'w').write(s)
p = 'build-switch.sh'; s = open(p).read()
if 'MESA_SWITCH_RUST_TARGET' not in s:
    s = s.replace('run() { docker exec "$CONTAINER_NAME" bash -c "$1"; }',
                  'run() { docker exec -e MESA_SWITCH_RUST_TARGET=aarch64-unknown-linux-gnu "$CONTAINER_NAME" bash -c "$1"; }\nrun "rustup target add aarch64-unknown-linux-gnu"', 1)
    open(p, 'w').write(s)
PY
  ./build-switch.sh
fi
docker start $C >/dev/null
rm -rf "$OUT"; mkdir -p "$OUT"
docker exec $C bash -c '
set -e
cd /project/builddir-switch
B=/opt/devkitpro/devkitA64/bin/aarch64-none-elf
$B-nm -g --defined-only src/nouveau/vulkan/libvulkan.a 2>/dev/null | awk "\$2==\"T\" && \$3 ~ /^vk[A-Z]/ {print \$3}" | sort -u > /tmp/keep.txt
RL=$(ls -d /root/.rustup/toolchains/*/lib/rustlib/aarch64-unknown-linux-gnu/lib)
STD=""; for n in std core alloc compiler_builtins hashbrown addr2line gimli miniz_oxide adler2 memchr object rustc_demangle cfg_if libc panic_abort panic_unwind unwind std_detect; do STD="$STD $(ls $RL/lib$n-*.rlib)"; done
$B-ld -r -o /tmp/nvk_all.o $(sed "s/^/-u /" /tmp/keep.txt | tr "\n" " ") --start-group src/nouveau/vulkan/libvulkan.a src/nouveau/compiler/libnak_rs.a $STD $RL/librustc_std_workspace_*.rlib --end-group 2>&1 | grep -v "bfd plugin" || true
$B-objcopy --keep-global-symbols=/tmp/keep.txt /tmp/nvk_all.o /tmp/nvk_vulkan.o'
docker cp $C:/tmp/nvk_vulkan.o "$OUT/nvk_vulkan.o"
mkdir -p "$OUT/include"; cp -r "$SRC/include/vulkan" "$SRC/include/vk_video" "$OUT/include/"
ls -la "$OUT/nvk_vulkan.o"
