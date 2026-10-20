// slang_compile.h - a slang shader (slang_source.h) to the forms the GPU backends take, and what the shader asks for (reflection).
//
//   slang text --glslang--> SPIR-V (vertex, fragment)      the Vulkan backend takes these as they are
//        |                         |
//        |                         +--SPIRV-Cross--> reflection: the UBO / push-constant members and the samplers by NAME (the spec's semantics)
//        |                         +--SPIRV-Cross--> GLSL   the OpenGL backend compiles this with the driver; the deko3d backend compiles it with uam (DKSH)
//
// Needs glslang and SPIRV-Cross (third_party/_slang_src, built by switch/get_slang.sh for the console, by tools/build_slangtool.sh for the PC). Built without them (CT_WITHOUT_SLANG)
// every call here reports "not built in" and the rest of the program is unchanged.
#ifndef CT_SLANG_COMPILE_H
#define CT_SLANG_COMPILE_H
#include "slang_source.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum { CT_SL_T_OTHER = 0, CT_SL_T_FLOAT, CT_SL_T_UINT, CT_SL_T_INT } CtSlBase;

typedef struct CtSlMember {
  char name[CT_SL_NAME];
  uint32_t offset, size;               // bytes, inside the block (std140 for a UBO, the SPIR-V's own offsets for push constants)
  int base, vec, cols;                 // CtSlBase; components (1..4); columns (1 = not a matrix)
  int arrayN;                          // 0 = not an array
  int stages;                          // bit 0 = the vertex stage names it, bit 1 = the fragment stage
} CtSlMember;

#define CT_SL_MAX_MEMBERS 64
#define CT_SL_MAX_TEX 16

typedef struct CtSlBlock {
  int present;
  uint32_t binding, size;              // a UBO's binding (a push block has none); the block's size in bytes
  int stages;
  int nMembers; CtSlMember m[CT_SL_MAX_MEMBERS];
} CtSlBlock;

typedef struct CtSlTex { char name[CT_SL_NAME]; uint32_t binding; } CtSlTex;       // `uniform sampler2D Source;` -- fragment only

typedef struct CtSlProgram {
  uint32_t* spv[2]; size_t spvWords[2];      // [0] vertex, [1] fragment
  CtSlBlock ubo, push;
  int nTex; CtSlTex tex[CT_SL_MAX_TEX];
  char* glsl[2];                             // after CtSlTranslate: desktop GLSL (push constants as a second UBO at pushBinding)
  uint32_t pushBinding;                      // the binding the translated GLSL gives the push block
} CtSlProgram;

int  CtSlCompilerBuilt(void);                                        // 1 when glslang + SPIRV-Cross are linked in
// slang_source -> SPIR-V + reflection. 0 = ok; else `err` holds the compiler's log (with the file's line numbers).
int  CtSlCompile(const CtSlSource* src, CtSlProgram* out, char* err, size_t errN);
// SPIR-V -> GLSL for the GL-style backends. `version` e.g. 430 (desktop) or 320 with es = 1. 0 = ok.
int  CtSlTranslate(CtSlProgram* p, int version, int es, char* err, size_t errN);
void CtSlProgramFree(CtSlProgram* p);

#ifdef __cplusplus
}
#endif
#endif
