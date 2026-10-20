// slang_source.h - a .slang file made ready for the compiler: #include resolved, the file split into its vertex and fragment sources, and the metadata its #pragma lines carry.
// (The spec: libretro/slang-shaders spec/SHADER_SPEC.md, "Initial preprocess of slang files".) No GPU here, no compiler: text in, text out.
#ifndef CT_SLANG_SOURCE_H
#define CT_SLANG_SOURCE_H
#include "slang_preset.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct CtSlParam {
  char id[CT_SL_NAME]; char desc[96];
  float init, min, max, step;
} CtSlParam;

typedef struct CtSlSource {
  char* vs; char* fs;                  // the two stages' GLSL (both begin with the file's #version line; the other stage's lines are blank, so a compiler's line numbers are the file's)
  char name[CT_SL_NAME];               // #pragma name ("" = none)
  int format;                          // #pragma format as a CtSlFormat; -1 = none (R8G8B8A8_UNORM unless the preset says float / sRGB)
  int nParams, capParams; CtSlParam* param;     // every #pragma parameter of the file and its includes (a growing array: Mega Bezel declares hundreds)
  int hasVertex, hasFragment;          // a stage that was never written is an error to the compiler, except that a missing vertex stage may take the stock one (CtSlStockVertex)
} CtSlSource;

// 0 = ok. `path` is the .slang file; includes are read relative to the file that names them (through `rd`).
int  CtSlSourceLoad(CtSlSource* s, const char* path, CtSlReadFn rd, void* rdCtx, char* err, size_t errN);
void CtSlSourceFree(CtSlSource* s);

#ifdef __cplusplus
}
#endif
#endif
