#!/bin/sh
# Builds the libraries behind the slang shader support (src/slang) for the Switch: glslang (Vulkan GLSL -> SPIR-V) and SPIRV-Cross (reflection, SPIR-V -> GLSL), static, into
# third_party/slang-switch/{include,lib}. Sources are cloned (shallow) into third_party/_slang_src (git-ignored). Needs the switch_dev_build container; run from the repository root.
# Without third_party/slang-switch the Switch build compiles the program without shader support (CT_WITHOUT_SLANG), and everything else is unchanged.
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"; cd "$HERE"
C=${CT_DOCKER:-switch_dev_build}
S=third_party/_slang_src
mkdir -p $S
[ -d $S/glslang ] || git clone -q --depth 1 https://github.com/KhronosGroup/glslang.git $S/glslang
[ -d $S/SPIRV-Cross ] || git clone -q --depth 1 https://github.com/KhronosGroup/SPIRV-Cross.git $S/SPIRV-Cross
docker exec $C mkdir -p /src/slang
tar --exclude=.git -cf - $S/glslang $S/SPIRV-Cross | docker exec -i $C tar -xf - -C /src/slang
docker exec $C bash -c '
set -e; export DEVKITPRO=/opt/devkitpro; cd /src/slang; rm -rf sw-prefix sw-gs sw-sc
CM=/opt/devkitpro/portlibs/switch/bin/aarch64-none-elf-cmake
# NOT -DCMAKE_CXX_FLAGS: the toolchain file puts -mtp=soft (the software thread pointer libnx needs for thread_local) and the arch flags there; only the RELEASE flags are ours
FLAGS="-O2 -g0 -DNDEBUG"
$CM -S third_party/_slang_src/glslang -B sw-gs -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/src/slang/sw-prefix -DENABLE_OPT=OFF -DENABLE_HLSL=OFF -DENABLE_GLSLANG_BINARIES=OFF -DBUILD_EXTERNAL=OFF -DGLSLANG_TESTS=OFF -DUNIX=ON -DCMAKE_CXX_FLAGS_RELEASE="$FLAGS" -DCMAKE_C_FLAGS_RELEASE="$FLAGS" >/dev/null
ninja -C sw-gs install >/dev/null
$CM -S third_party/_slang_src/SPIRV-Cross -B sw-sc -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/src/slang/sw-prefix -DSPIRV_CROSS_CLI=OFF -DSPIRV_CROSS_ENABLE_TESTS=OFF -DSPIRV_CROSS_SHARED=OFF -DSPIRV_CROSS_STATIC=ON -DSPIRV_CROSS_ENABLE_HLSL=OFF -DSPIRV_CROSS_ENABLE_MSL=OFF -DSPIRV_CROSS_ENABLE_CPP=OFF -DSPIRV_CROSS_ENABLE_C_API=OFF -DSPIRV_CROSS_ENABLE_UTIL=OFF -DCMAKE_CXX_FLAGS_RELEASE="$FLAGS" >/dev/null
ninja -C sw-sc install >/dev/null
/opt/devkitpro/devkitA64/bin/aarch64-none-elf-strip --strip-debug sw-prefix/lib/*.a
ls -la sw-prefix/lib/*.a'
mkdir -p third_party/slang-switch
rm -rf third_party/slang-switch/include third_party/slang-switch/lib
docker cp $C:/src/slang/sw-prefix/include third_party/slang-switch/include
docker cp $C:/src/slang/sw-prefix/lib third_party/slang-switch/lib
rm -rf third_party/slang-switch/lib/cmake third_party/slang-switch/lib/pkgconfig
echo "third_party/slang-switch: $(du -sh third_party/slang-switch | cut -f1)"
