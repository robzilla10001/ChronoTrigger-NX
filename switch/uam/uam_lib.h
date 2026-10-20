// uam_lib.h - devkitPro's uam (the deko3d shader compiler: GLSL -> a DKSH module) as a library, so the program can compile shaders on the console (src/slang -> the deko3d backend).
// Built by switch/get_uam.sh from github.com/devkitPro/uam (ISC / MIT, Mesa imported) into third_party/slang-switch/lib/libuam.a.
#ifndef CT_UAM_LIB_H
#define CT_UAM_LIB_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { CT_UAM_VERTEX = 0, CT_UAM_FRAGMENT = 1 };
// Compiles one stage of GLSL (version 430..460 core, bindings as `layout(binding = N)`). 0 = ok: *dksh is a malloc'd DKSH module of *size bytes (free it), for dkShaderInitialize.
// On failure *dksh is NULL and `log` holds a line.
int CtUamCompile(int stage, const char* glsl, void** dksh, size_t* size, char* log, size_t logN);
// The optimisation level of the compiles that follow (0..3, default 3).
void CtUamSetOptLevel(int level);
// Ends the compile thread (joins it). Call before the program exits and before a build that never compiles again; a later CtUamCompile starts a new one.
void CtUamShutdown(void);
#ifdef __cplusplus
}
#endif
#endif
