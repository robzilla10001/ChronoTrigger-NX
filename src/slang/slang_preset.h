// slang_preset.h - a RetroArch .slangp preset: the list of shader passes (each with its filter, wrap, scale and format), the look-up textures and the parameter values.
//
// The format is the one RetroArch documents (libretro/slang-shaders, spec/SHADER_SPEC.md): `key = value` lines, `#reference "other.slangp"` to start from another preset, paths
// relative to the preset's own folder. This file only READS a preset; nothing here knows about a GPU (slang_chain.h turns the passes into a frame, slang_gpu.h is the GPU's side).
#ifndef CT_SLANG_PRESET_H
#define CT_SLANG_PRESET_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define CT_SL_MAX_PASSES 64
#define CT_SL_MAX_LUTS 64
#define CT_SL_MAX_PARAMS 256
#define CT_SL_PATH 512
#define CT_SL_NAME 64

typedef enum { CT_SL_WRAP_BORDER = 0, CT_SL_WRAP_EDGE, CT_SL_WRAP_REPEAT, CT_SL_WRAP_MIRROR } CtSlWrap;
typedef enum { CT_SL_SCALE_SOURCE = 0, CT_SL_SCALE_VIEWPORT, CT_SL_SCALE_ABSOLUTE } CtSlScaleType;
typedef enum { CT_SL_FILTER_UNSPEC = -1, CT_SL_FILTER_NEAREST = 0, CT_SL_FILTER_LINEAR = 1 } CtSlFilter;

// The render target formats of `#pragma format` (the spec's list). The GPU backends map the ones they can; an unsupported one falls back to CT_SL_FMT_RGBA8.
typedef enum {
  CT_SL_FMT_RGBA8 = 0, CT_SL_FMT_RGBA8_SRGB, CT_SL_FMT_RGBA16F, CT_SL_FMT_RGBA32F,
  CT_SL_FMT_R8, CT_SL_FMT_RG8, CT_SL_FMT_R16F, CT_SL_FMT_RG16F, CT_SL_FMT_R32F, CT_SL_FMT_RG32F,
  CT_SL_FMT_A2B10G10R10, CT_SL_FMT_RGBA8_UINT, CT_SL_FMT_RGBA8_SINT, CT_SL_FMT_R8_UINT, CT_SL_FMT_RG8_UINT, CT_SL_FMT_R16_UINT, CT_SL_FMT_RG16_UINT, CT_SL_FMT_RGBA16_UINT,
  CT_SL_FMT_R32_UINT, CT_SL_FMT_RG32_UINT, CT_SL_FMT_RGBA32_UINT, CT_SL_FMT_COUNT
} CtSlFormat;
CtSlFormat CtSlFormatFromName(const char* name);        // "R16G16B16A16_SFLOAT" -> CT_SL_FMT_RGBA16F; unknown -> -1
const char* CtSlFormatName(CtSlFormat f);

typedef struct CtSlPassCfg {
  char path[CT_SL_PATH];                 // the .slang file
  char alias[CT_SL_NAME];                // `aliasN`: the name later passes use for this pass's output ("" = none)
  int filter;                            // CtSlFilter: how THIS pass samples its inputs (RetroArch's filter_linearN); UNSPEC when the preset does not say
  int wrap;                              // CtSlWrap
  int mipmap;                            // mipmap_inputN
  int floatFb, srgbFb;                   // float_framebufferN, srgb_framebufferN
  int scaleTypeX, scaleTypeY;            // CtSlScaleType
  int hasScale;                          // any scale key was given (else: the last pass is viewport-sized, the others source-sized)
  float scaleX, scaleY;                  // the factor (or, absolute, the size in pixels)
  unsigned frameCountMod;                // 0 = none
} CtSlPassCfg;

typedef struct CtSlLutCfg {
  char name[CT_SL_NAME]; char path[CT_SL_PATH];
  int filter; int wrap; int mipmap;      // `<name>_linear`, `<name>_wrap_mode`, `<name>_mipmap`
} CtSlLutCfg;

typedef struct CtSlParamVal { char name[CT_SL_NAME]; float value; } CtSlParamVal;

typedef struct CtSlPreset {
  char path[CT_SL_PATH];                 // the preset file
  int nPasses; CtSlPassCfg pass[CT_SL_MAX_PASSES];
  int nLuts; CtSlLutCfg lut[CT_SL_MAX_LUTS];
  int nParams; CtSlParamVal param[CT_SL_MAX_PARAMS];     // the values the preset sets (a shader's own #pragma parameter supplies the rest)
} CtSlPreset;

// Reading goes through a function so the engine can read from the SD card, a test from memory: returns a malloc'd, NUL-terminated copy of the file (free it) or NULL.
typedef char* (*CtSlReadFn)(const char* path, size_t* len, void* ctx);
char* CtSlReadFile(const char* path, size_t* len, void* ctx);           // the default: stdio

// 0 = ok; else `err` says why (a pass without a shader, a missing #reference, ...).
int CtSlPresetLoad(CtSlPreset* p, const char* path, CtSlReadFn rd, void* rdCtx, char* err, size_t errN);
// A path relative to the folder of `base` (a file); an absolute path or one with a drive stays as is. out == the joined, normalised ("a/../b" -> "b") path.
void CtSlJoinPath(char* out, size_t n, const char* base, const char* rel);

#ifdef __cplusplus
}
#endif
#endif
