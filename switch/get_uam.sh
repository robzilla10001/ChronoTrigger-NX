#!/bin/sh
# Builds devkitPro's uam (the deko3d shader compiler) as a STATIC LIBRARY for the Switch: third_party/slang-switch/lib/libuam.a + include/uam_lib.h. uam is Mesa's GLSL front end and Nouveau's
# Maxwell code generator (~180 C/C++ files); the Mesa files that are generated (the GLSL lexer and parser, the built-in functions) are produced by building uam natively once (meson, flex, bison, python-mako in
# the Docker image), then every compile unit is cross-compiled with devkitA64. Needs the switch_dev_build container; run from the repository root. (docs/SLANG.md)
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"; cd "$HERE"
C=${CT_DOCKER:-switch_dev_build}
S=third_party/_slang_src
mkdir -p $S third_party/slang-switch/lib third_party/slang-switch/include
[ -d $S/uam ] || git clone -q --depth 1 https://github.com/devkitPro/uam.git $S/uam
docker exec $C mkdir -p /src/slang
docker exec $C sh -c 'apt-get install -y -qq meson flex bison python3-mako >/dev/null 2>&1 || true'
tar --exclude=.git -cf - -C $S uam | docker exec -i $C tar -xf - -C /src/slang
tar -cf - switch/uam | docker exec -i $C tar -xf - -C /src/slang
docker exec $C bash -c '
set -e; export DEVKITPRO=/opt/devkitpro; cd /src/slang/uam
python3 /src/slang/switch/uam/patch_uam.py /src/slang/uam
rm -rf bu; meson setup bu >/dev/null; ninja -C bu >/dev/null          # the native build: it generates the GLSL lexer / parser and the built-in functions
python3 /src/slang/switch/uam/gen_cmake.py /src/slang/uam /src/slang/switch/uam
rm -rf sw; mkdir sw; cp CMakeLists.cross.txt sw/CMakeLists.txt
/opt/devkitpro/portlibs/switch/bin/aarch64-none-elf-cmake -S sw -B sw/b -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS_RELEASE="-O2 -g0" -DCMAKE_CXX_FLAGS_RELEASE="-O2 -g0" >/dev/null
ninja -C sw/b -j16 2>&1 | grep -E "error|FAILED" | head -40
# 4. ONE relocatable object whose symbols are all PRIVATE: uam carries an old Mesa (ralloc, the GLSL front end, ir_* classes with their vtables, util_*) that must not meet the Mesa of the console'"'"'s libEGL.a
#    or the Mesa 26.2 of the unified build (which links with --allow-multiple-definition: the first copy of a name wins). Making the symbols local is NOT enough for C++: the inline functions and vtables are COMDAT
#    groups, merged by NAME across objects, and the linker kept uam'"'"'s copy and discarded the other Mesa'"'"'s. So every symbol uam defines (except CtUamCompile) is renamed with a prefix: nothing merges, nothing collides.
B=/opt/devkitpro/devkitA64/bin/aarch64-none-elf
$B-ld -r --whole-archive sw/b/libuam.a -o sw/uam_all.o
# the names to rename: every defined global symbol, AND every COMDAT group signature (GCC puts the constructors and destructors in groups named after symbols like _ZN3FooD5Ev that are not defined
# symbols at all; left alone, those groups merged with the other Mesa'"'"'s same-named ones, the linker kept the other copy, and our vtables'"'"' destructor slots came out NULL)
( $B-nm --defined-only -g sw/uam_all.o | awk '"'"'{print $3}'"'"'; $B-readelf -g sw/uam_all.o | grep "COMDAT group" | sed -E "s/.*\[(.*)\] contains.*/\1/" ) | grep -Ev "^(CtUamCompile|CtUamShutdown|CtUamSetOptLevel)$" | grep -v "^uampriv_" | sort -u | awk '"'"'{print $1 " uampriv_" $1}'"'"' > sw/redef.txt
$B-objcopy --redefine-syms=sw/redef.txt --strip-debug sw/uam_all.o
rm -f sw/b/libuam_iso.a; $B-ar rcs sw/b/libuam_iso.a sw/uam_all.o
echo "renamed $(wc -l < sw/redef.txt) names; COMDAT groups still unprefixed: $($B-readelf -g sw/uam_all.o | grep "COMDAT group" | grep -vc uampriv); exported unprefixed:"; $B-nm --defined-only -g sw/uam_all.o | grep -v " uampriv_" | head
ls -la sw/b/libuam.a sw/b/libuam_iso.a'
docker cp $C:/src/slang/uam/sw/b/libuam_iso.a third_party/slang-switch/lib/libuam.a
cp switch/uam/uam_lib.h third_party/slang-switch/include/uam_lib.h
echo "libuam.a: $(du -h third_party/slang-switch/lib/libuam.a | cut -f1)"
