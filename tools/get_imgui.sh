#!/bin/sh
# Download Dear ImGui (MIT, github.com/ocornut/imgui) into third_party/imgui for the mods menu (L3+R3 on the Switch).
# The POSIX twin of tools/get_imgui.ps1: same version, same files. Git-ignored. Without it the NRO still builds, with the menu compiled out.
# Run once, from anywhere:   sh tools/get_imgui.sh
set -eu
ver=1.91.5
root=$(cd "$(dirname "$0")/.." && pwd)
dest="$root/third_party/imgui"
if [ -f "$dest/imgui.h" ]; then echo "Dear ImGui already present at $dest"; exit 0; fi
work=$(mktemp -d)                                  # a fresh, empty directory for the untrusted archive
trap 'rm -rf "$work"' EXIT
url="https://github.com/ocornut/imgui/archive/refs/tags/v$ver.zip"
echo "Downloading $url"
curl -sL "$url" -o "$work/imgui.zip"
(cd "$work" && unzip -q imgui.zip)
src="$work/imgui-$ver"
[ -f "$src/imgui.h" ] || { echo "unexpected archive layout in $work" >&2; exit 1; }
mkdir -p "$dest/backends"
for f in imgui.cpp imgui.h imgui_demo.cpp imgui_draw.cpp imgui_internal.h imgui_tables.cpp imgui_widgets.cpp imconfig.h \
         imstb_rectpack.h imstb_textedit.h imstb_truetype.h LICENSE.txt; do cp "$src/$f" "$dest/$f"; done
for f in imgui_impl_sdl2.cpp imgui_impl_sdl2.h imgui_impl_sdlrenderer2.cpp imgui_impl_sdlrenderer2.h imgui_impl_opengl3.cpp \
         imgui_impl_opengl3.h imgui_impl_opengl3_loader.h imgui_impl_vulkan.cpp imgui_impl_vulkan.h; do cp "$src/backends/$f" "$dest/backends/$f"; done
echo "Dear ImGui $ver installed to $dest"
