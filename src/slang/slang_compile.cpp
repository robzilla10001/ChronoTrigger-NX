// slang_compile.cpp - see slang_compile.h.
#include "slang_compile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef CT_WITHOUT_SLANG
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <SPIRV/GlslangToSpv.h>
#include <spirv_cross/spirv_glsl.hpp>
#include <mutex>
#include <string>
#include <vector>

static std::once_flag g_once;
static void initOnce() { std::call_once(g_once, [] { glslang::InitializeProcess(); }); }

static void setErr(char* err, size_t n, const std::string& s) { if (err && n) snprintf(err, n, "%s", s.c_str()); }

static bool toSpv(const char* src, EShLanguage stage, std::vector<unsigned>& spv, std::string& log, glslang::TProgram** keep) {
  (void)keep;
  initOnce();
  glslang::TShader sh(stage);
  sh.setStrings(&src, 1);
  sh.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
  sh.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
  sh.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
  const TBuiltInResource* res = GetDefaultResources();
  EShMessages msgs = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);
  if (!sh.parse(res, 100, false, msgs)) { log = std::string(stage == EShLangVertex ? "vertex: " : "fragment: ") + sh.getInfoLog() + sh.getInfoDebugLog(); return false; }
  glslang::TProgram prog;
  prog.addShader(&sh);
  if (!prog.link(msgs)) { log = std::string("link: ") + prog.getInfoLog(); return false; }
  glslang::SpvOptions so; so.disableOptimizer = true; so.validate = false; so.stripDebugInfo = false;
  spv::SpvBuildLogger logger;
  glslang::GlslangToSpv(*prog.getIntermediate(stage), spv, &logger, &so);
  if (spv.empty()) { log = "no SPIR-V produced: " + logger.getAllMessages(); return false; }
  return true;
}

static void member(CtSlBlock* b, spirv_cross::Compiler& c, const spirv_cross::SPIRType& blockTy, uint32_t i, int stageBit) {
  std::string nm = c.get_member_name(blockTy.self, i);
  uint32_t off = c.type_struct_member_offset(blockTy, i);
  const spirv_cross::SPIRType& mt = c.get_type(blockTy.member_types[i]);
  CtSlMember* m = NULL;
  for (int k = 0; k < b->nMembers; k++) if (!strcmp(b->m[k].name, nm.c_str())) { m = &b->m[k]; break; }
  if (m) { m->stages |= stageBit; return; }                                  // (the same member in the other stage: the offsets are required to be equal by the spec)
  if (b->nMembers >= CT_SL_MAX_MEMBERS) return;
  m = &b->m[b->nMembers++];
  snprintf(m->name, sizeof m->name, "%s", nm.c_str());
  m->offset = off; m->size = (uint32_t)c.get_declared_struct_member_size(blockTy, i);
  m->vec = (int)mt.vecsize; m->cols = (int)mt.columns;
  m->base = mt.basetype == spirv_cross::SPIRType::Float ? CT_SL_T_FLOAT : mt.basetype == spirv_cross::SPIRType::UInt ? CT_SL_T_UINT : mt.basetype == spirv_cross::SPIRType::Int ? CT_SL_T_INT : CT_SL_T_OTHER;
  m->arrayN = mt.array.empty() ? 0 : (int)mt.array[0];
  m->stages = stageBit;
}

static void reflect(CtSlProgram* p, int s, std::string& log) {
  spirv_cross::Compiler c(p->spv[s], p->spvWords[s]);
  spirv_cross::ShaderResources r = c.get_shader_resources();
  int bit = 1 << s;
  for (auto& u : r.uniform_buffers) {
    const spirv_cross::SPIRType& t = c.get_type(u.base_type_id);
    p->ubo.present = 1; p->ubo.binding = c.get_decoration(u.id, spv::DecorationBinding); p->ubo.stages |= bit;
    uint32_t sz = (uint32_t)c.get_declared_struct_size(t); if (sz > p->ubo.size) p->ubo.size = sz;
    for (uint32_t i = 0; i < t.member_types.size(); i++) member(&p->ubo, c, t, i, bit);
  }
  for (auto& u : r.push_constant_buffers) {
    const spirv_cross::SPIRType& t = c.get_type(u.base_type_id);
    p->push.present = 1; p->push.stages |= bit;
    uint32_t sz = (uint32_t)c.get_declared_struct_size(t); if (sz > p->push.size) p->push.size = sz;
    for (uint32_t i = 0; i < t.member_types.size(); i++) member(&p->push, c, t, i, bit);
  }
  for (auto& u : r.sampled_images) {
    std::string nm = c.get_name(u.id);
    int dup = 0; for (int k = 0; k < p->nTex; k++) if (!strcmp(p->tex[k].name, nm.c_str())) dup = 1;
    if (dup || p->nTex >= CT_SL_MAX_TEX) continue;
    snprintf(p->tex[p->nTex].name, CT_SL_NAME, "%s", nm.c_str()); p->tex[p->nTex].binding = c.get_decoration(u.id, spv::DecorationBinding); p->nTex++;
  }
  if (!r.storage_buffers.empty() || !r.storage_images.empty() || !r.atomic_counters.empty()) log += "storage buffers, images and atomic counters are not allowed by the spec; ";
}

extern "C" int CtSlCompilerBuilt(void) { return 1; }

extern "C" int CtSlCompile(const CtSlSource* src, CtSlProgram* out, char* err, size_t errN) {
  memset(out, 0, sizeof *out);
  if (!src->hasVertex || !src->hasFragment) { setErr(err, errN, "the shader needs both '#pragma stage vertex' and '#pragma stage fragment'"); return 1; }
  std::vector<unsigned> v, f; std::string log;
  if (!toSpv(src->vs, EShLangVertex, v, log, NULL)) { setErr(err, errN, log); return 1; }
  if (!toSpv(src->fs, EShLangFragment, f, log, NULL)) { setErr(err, errN, log); return 1; }
  out->spvWords[0] = v.size(); out->spv[0] = (uint32_t*)malloc(v.size() * 4); memcpy(out->spv[0], v.data(), v.size() * 4);
  out->spvWords[1] = f.size(); out->spv[1] = (uint32_t*)malloc(f.size() * 4); memcpy(out->spv[1], f.data(), f.size() * 4);
  try { reflect(out, 0, log); reflect(out, 1, log); }
  catch (const std::exception& e) { setErr(err, errN, std::string("reflection: ") + e.what()); CtSlProgramFree(out); return 1; }
  if (!log.empty()) { setErr(err, errN, log); CtSlProgramFree(out); return 1; }
  return 0;
}

extern "C" int CtSlTranslate(CtSlProgram* p, int version, int es, char* err, size_t errN) {
  try {
    // the push block becomes a UBO right after the real one (the GL-style backends have no push constants). Uniform buffer binding points are their own namespace, apart from the texture units:
    // the next one up, never past the 16 that deko3d has however many samplers the shader reads.
    p->pushBinding = p->ubo.present ? p->ubo.binding + 1 : 0;
    for (int s = 0; s < 2; s++) {
      spirv_cross::CompilerGLSL g(p->spv[s], p->spvWords[s]);
      spirv_cross::CompilerGLSL::Options o; o.version = (uint32_t)version; o.es = es != 0; o.vertex.fixup_clipspace = false; o.vertex.flip_vert_y = false;
      o.enable_420pack_extension = true; o.separate_shader_objects = false; o.emit_push_constant_as_uniform_buffer = true; o.emit_uniform_buffer_as_plain_uniforms = false;
      g.set_common_options(o);
      spirv_cross::ShaderResources r = g.get_shader_resources();
      for (auto& u : r.push_constant_buffers) { g.set_decoration(u.id, spv::DecorationDescriptorSet, 0); g.set_decoration(u.id, spv::DecorationBinding, p->pushBinding); }
      std::string text = g.compile();
      free(p->glsl[s]); p->glsl[s] = strdup(text.c_str());
    }
  } catch (const std::exception& e) { setErr(err, errN, std::string("SPIR-V to GLSL: ") + e.what()); return 1; }
  return 0;
}
#else   // CT_WITHOUT_SLANG
extern "C" int CtSlCompilerBuilt(void) { return 0; }
extern "C" int CtSlCompile(const CtSlSource*, CtSlProgram* out, char* err, size_t errN) { memset(out, 0, sizeof *out); if (err && errN) snprintf(err, errN, "slang shaders are not built into this program"); return 1; }
extern "C" int CtSlTranslate(CtSlProgram*, int, int, char* err, size_t errN) { if (err && errN) snprintf(err, errN, "slang shaders are not built into this program"); return 1; }
#endif

extern "C" void CtSlProgramFree(CtSlProgram* p) {
  if (!p) return;
  for (int s = 0; s < 2; s++) { free(p->spv[s]); p->spv[s] = NULL; free(p->glsl[s]); p->glsl[s] = NULL; }
}
