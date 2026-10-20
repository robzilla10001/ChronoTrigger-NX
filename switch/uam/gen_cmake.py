# gen_cmake.py <uam root> <our wrapper dir>: a cross-compile CMake project for libuam.a from the compile commands of the native meson build (bu/), which also produced the generated sources.
import json, os, re, sys
root, wrap = sys.argv[1], sys.argv[2]
bu = root + "/bu"
cc = json.load(open(bu + "/compile_commands.json"))
srcs = []; incs = set()
for x in cc:
    f = os.path.normpath(os.path.join(bu, x["file"]))
    if f.endswith("/main.cpp"): continue
    srcs.append(f)
    for m in re.findall(r"-I(\S+)", x["command"]): incs.add(os.path.normpath(os.path.join(bu, m)))
incs.add(root + "/source"); incs.add(wrap)
out = ["cmake_minimum_required(VERSION 3.15)", "project(uamlib C CXX)", "set(CMAKE_C_STANDARD 99)", "set(CMAKE_CXX_STANDARD 11)", "set(CMAKE_C_EXTENSIONS ON)", "set(CMAKE_CXX_EXTENSIONS ON)",
       "add_library(uam STATIC " + " ".join(srcs) + " " + wrap + "/uam_lib.cpp)",
       "target_include_directories(uam PRIVATE " + " ".join(sorted(incs)) + ")",
       "target_compile_definitions(uam PRIVATE DESKTOP _GNU_SOURCE HAVE_POSIX_MEMALIGN NDEBUG _USE_MATH_DEFINES _FILE_OFFSET_BITS=64 \"PACKAGE_STRING=\\\"uam 1.1.0\\\"\")",
       "target_compile_options(uam PRIVATE -w -ffunction-sections -fdata-sections)"]
open(root + "/CMakeLists.cross.txt", "w").write("\n".join(out) + "\n")
print(len(srcs), "sources")
