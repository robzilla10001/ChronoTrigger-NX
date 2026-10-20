// slang_vk.c - see slang_vk.h.
#include "slang_vk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FUNCS(X) \
  X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) \
  X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCreateRenderPass) X(vkDestroyRenderPass) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
  X(vkMapMemory) X(vkUnmapMemory) X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) \
  X(vkDestroyPipelineLayout) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCreateSampler) X(vkDestroySampler) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) \
  X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) X(vkCreateCommandPool) X(vkDestroyCommandPool) \
  X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkQueueSubmit) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) X(vkDeviceWaitIdle) \
  X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkCmdCopyImage) X(vkCmdBlitImage) X(vkCmdClearColorImage) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) \
  X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdBindVertexBuffers) X(vkCmdDraw) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdCopyImageToBuffer)

typedef struct { uint32_t levels; int w, h, fmt, owned; VkImage img; VkDeviceMemory mem; VkImageView view, fbView; VkFramebuffer fb; } Tex;
typedef struct {
  VkShaderModule vs, fs; VkDescriptorSetLayout dsl; VkPipelineLayout pl; VkPipeline pipe[2]; VkFormat fmt;      // pipe[0] against the offscreen pass of `fmt`, pipe[1] against the host's final pass
  int hasUbo; uint32_t uboBinding; VkShaderStageFlags uboStages; uint32_t pushSize; VkShaderStageFlags pushStages; int nTex; uint32_t texBinding[CT_SL_MAX_TEX];
  int fmtId;
} Prog;
typedef struct { Tex* tex; Prog* prog; } Dead;

typedef struct Dev {
  CtSlVkEnv env;
#define DECL(f) PFN_##f f;
  FUNCS(DECL)
#undef DECL
  PFN_vkGetPhysicalDeviceMemoryProperties getMem;
  VkPhysicalDeviceMemoryProperties mp;
  VkRenderPass rp[CT_SL_FMT_COUNT];
  VkSampler samp[16];
  VkDescriptorPool pool;
  VkBuffer vbo; VkDeviceMemory vboMem;
  VkBuffer ring; VkDeviceMemory ringMem; uint8_t* ringMap; size_t ringSize, ringOff; uint32_t uboAlign;
  VkCommandPool cpool; VkCommandBuffer tcmd; VkFence tfence;
  VkCommandBuffer cmd;                  // the one the passes record into: the host's this frame, or ccmd during a capture
  VkCommandBuffer ccmd, hostCmd;        // the capture's private one; the host's, kept while it is in use
  int finalOpen;                        // the last pass of this frame began the host's render pass
  Dead dead[512]; int nDead;
} Dev;

static const VkFormat kVkFmt[CT_SL_FMT_COUNT] = {
  [CT_SL_FMT_RGBA8] = VK_FORMAT_R8G8B8A8_UNORM, [CT_SL_FMT_RGBA8_SRGB] = VK_FORMAT_R8G8B8A8_SRGB, [CT_SL_FMT_RGBA16F] = VK_FORMAT_R16G16B16A16_SFLOAT,
  [CT_SL_FMT_RGBA32F] = VK_FORMAT_R32G32B32A32_SFLOAT, [CT_SL_FMT_R8] = VK_FORMAT_R8_UNORM, [CT_SL_FMT_RG8] = VK_FORMAT_R8G8_UNORM, [CT_SL_FMT_R16F] = VK_FORMAT_R16_SFLOAT,
  [CT_SL_FMT_RG16F] = VK_FORMAT_R16G16_SFLOAT, [CT_SL_FMT_R32F] = VK_FORMAT_R32_SFLOAT, [CT_SL_FMT_RG32F] = VK_FORMAT_R32G32_SFLOAT,
  [CT_SL_FMT_A2B10G10R10] = VK_FORMAT_A2B10G10R10_UNORM_PACK32, [CT_SL_FMT_RGBA8_UINT] = VK_FORMAT_R8G8B8A8_UINT, [CT_SL_FMT_RGBA8_SINT] = VK_FORMAT_R8G8B8A8_SINT,
  [CT_SL_FMT_R8_UINT] = VK_FORMAT_R8_UINT, [CT_SL_FMT_RG8_UINT] = VK_FORMAT_R8G8_UINT, [CT_SL_FMT_R16_UINT] = VK_FORMAT_R16_UINT, [CT_SL_FMT_RG16_UINT] = VK_FORMAT_R16G16_UINT,
  [CT_SL_FMT_RGBA16_UINT] = VK_FORMAT_R16G16B16A16_UINT, [CT_SL_FMT_R32_UINT] = VK_FORMAT_R32_UINT, [CT_SL_FMT_RG32_UINT] = VK_FORMAT_R32G32_UINT,
  [CT_SL_FMT_RGBA32_UINT] = VK_FORMAT_R32G32B32A32_UINT,
};

static uint32_t memType(Dev* d, uint32_t bits, VkMemoryPropertyFlags want) {
  for (uint32_t i = 0; i < d->mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (d->mp.memoryTypes[i].propertyFlags & want) == want) return i;
  return UINT32_MAX;
}
static int mkBuffer(Dev* d, VkDeviceSize size, VkBufferUsageFlags use, VkBuffer* b, VkDeviceMemory* m, void** map) {
  VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO }; bi.size = size; bi.usage = use; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (d->vkCreateBuffer(d->env.device, &bi, NULL, b) != VK_SUCCESS) return 0;
  VkMemoryRequirements mr; d->vkGetBufferMemoryRequirements(d->env.device, *b, &mr);
  VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO }; ai.allocationSize = mr.size;
  ai.memoryTypeIndex = memType(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (ai.memoryTypeIndex == UINT32_MAX || d->vkAllocateMemory(d->env.device, &ai, NULL, m) != VK_SUCCESS) return 0;
  d->vkBindBufferMemory(d->env.device, *b, *m, 0);
  return d->vkMapMemory(d->env.device, *m, 0, size, 0, map) == VK_SUCCESS;
}
static void barrierL(Dev* d, VkCommandBuffer c, VkImage im, uint32_t base, uint32_t n, VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
  VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
  b.oldLayout = from; b.newLayout = to; b.srcAccessMask = sa; b.dstAccessMask = da; b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = im;
  b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.subresourceRange.baseMipLevel = base; b.subresourceRange.levelCount = n; b.subresourceRange.layerCount = 1;
  d->vkCmdPipelineBarrier(c, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
}
#define ALLCMD VK_PIPELINE_STAGE_ALL_COMMANDS_BIT
#define MEMRW (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT)

// A one-time command buffer of our own for the work a texture's creation needs (clears, uploads, mips): recorded, submitted, waited for.
static VkCommandBuffer tBegin(Dev* d) {
  d->vkResetCommandBuffer(d->tcmd, 0);
  VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  d->vkBeginCommandBuffer(d->tcmd, &bi);
  return d->tcmd;
}
static void tEnd(Dev* d) {
  d->vkEndCommandBuffer(d->tcmd);
  VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &d->tcmd;
  d->vkResetFences(d->env.device, 1, &d->tfence);
  d->vkQueueSubmit(d->env.queue, 1, &si, d->tfence);
  d->vkWaitForFences(d->env.device, 1, &d->tfence, VK_TRUE, UINT64_MAX);
}

static VkRenderPass getRp(Dev* d, int fmt) {
  if (d->rp[fmt]) return d->rp[fmt];
  VkAttachmentDescription a = { 0 }; a.format = kVkFmt[fmt]; a.samples = VK_SAMPLE_COUNT_1_BIT; a.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; a.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkAttachmentReference r = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
  VkSubpassDescription sp = { 0 }; sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sp.colorAttachmentCount = 1; sp.pColorAttachments = &r;
  VkSubpassDependency dep[2] = { { 0 }, { 0 } };
  dep[0].srcSubpass = VK_SUBPASS_EXTERNAL; dep[0].dstSubpass = 0; dep[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; dep[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dep[1].srcSubpass = 0; dep[1].dstSubpass = VK_SUBPASS_EXTERNAL; dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT; dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
  VkRenderPassCreateInfo ri = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO }; ri.attachmentCount = 1; ri.pAttachments = &a; ri.subpassCount = 1; ri.pSubpasses = &sp; ri.dependencyCount = 2; ri.pDependencies = dep;
  if (d->vkCreateRenderPass(d->env.device, &ri, NULL, &d->rp[fmt]) != VK_SUCCESS) d->rp[fmt] = VK_NULL_HANDLE;
  return d->rp[fmt];
}

static uint32_t levelsFor(int w, int h) { uint32_t n = 1; while (w > 1 || h > 1) { w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; n++; } return n; }

static Tex* newImage(Dev* d, int w, int h, int fmt, int mip, VkImageUsageFlags use) {
  Tex* t = (Tex*)calloc(1, sizeof *t); if (!t) return NULL;
  t->w = w; t->h = h; t->fmt = fmt; t->owned = 1; t->levels = mip ? levelsFor(w, h) : 1;
  VkImageCreateInfo ic = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO }; ic.imageType = VK_IMAGE_TYPE_2D; ic.format = kVkFmt[fmt]; ic.extent.width = (uint32_t)w; ic.extent.height = (uint32_t)h; ic.extent.depth = 1;
  ic.mipLevels = t->levels; ic.arrayLayers = 1; ic.samples = VK_SAMPLE_COUNT_1_BIT; ic.tiling = VK_IMAGE_TILING_OPTIMAL; ic.usage = use; ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (d->vkCreateImage(d->env.device, &ic, NULL, &t->img) != VK_SUCCESS) { free(t); return NULL; }
  VkMemoryRequirements mr; d->vkGetImageMemoryRequirements(d->env.device, t->img, &mr);
  VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO }; ai.allocationSize = mr.size; ai.memoryTypeIndex = memType(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (ai.memoryTypeIndex == UINT32_MAX || d->vkAllocateMemory(d->env.device, &ai, NULL, &t->mem) != VK_SUCCESS) { d->vkDestroyImage(d->env.device, t->img, NULL); free(t); return NULL; }
  d->vkBindImageMemory(d->env.device, t->img, t->mem, 0);
  VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO }; vi.image = t->img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = kVkFmt[fmt];
  vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; vi.subresourceRange.levelCount = t->levels; vi.subresourceRange.layerCount = 1;
  d->vkCreateImageView(d->env.device, &vi, NULL, &t->view);
  vi.subresourceRange.levelCount = 1; d->vkCreateImageView(d->env.device, &vi, NULL, &t->fbView);
  return t;
}
static void freeTex(Dev* d, Tex* t) {
  if (!t) return;
  if (t->owned) {
    if (t->fb) d->vkDestroyFramebuffer(d->env.device, t->fb, NULL);
    if (t->fbView) d->vkDestroyImageView(d->env.device, t->fbView, NULL);
    if (t->view) d->vkDestroyImageView(d->env.device, t->view, NULL);
    if (t->img) d->vkDestroyImage(d->env.device, t->img, NULL);
    if (t->mem) d->vkFreeMemory(d->env.device, t->mem, NULL);
  }
  free(t);
}
static void freeProg(Dev* d, Prog* p) {
  if (!p) return;
  for (int i = 0; i < 2; i++) if (p->pipe[i]) d->vkDestroyPipeline(d->env.device, p->pipe[i], NULL);
  if (p->pl) d->vkDestroyPipelineLayout(d->env.device, p->pl, NULL);
  if (p->dsl) d->vkDestroyDescriptorSetLayout(d->env.device, p->dsl, NULL);
  if (p->vs) d->vkDestroyShaderModule(d->env.device, p->vs, NULL);
  if (p->fs) d->vkDestroyShaderModule(d->env.device, p->fs, NULL);
  free(p);
}
static void flushDead(Dev* d) { for (int i = 0; i < d->nDead; i++) { freeTex(d, d->dead[i].tex); freeProg(d, d->dead[i].prog); } d->nDead = 0; }

// ---- the interface -----------------------------------------------------------------------------------------------------------------------------------------------------------------------

static void* texCreate(void* dv, int w, int h, CtSlFormat fmt, int mip) {
  Dev* d = (Dev*)dv;
  if ((unsigned)fmt >= CT_SL_FMT_COUNT || !kVkFmt[fmt]) fmt = CT_SL_FMT_RGBA8;
  Tex* t = newImage(d, w, h, fmt, mip, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
  if (!t) return NULL;
  VkRenderPass rp = getRp(d, fmt);
  VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO }; fi.renderPass = rp; fi.attachmentCount = 1; fi.pAttachments = &t->fbView; fi.width = (uint32_t)w; fi.height = (uint32_t)h; fi.layers = 1;
  if (!rp || d->vkCreateFramebuffer(d->env.device, &fi, NULL, &t->fb) != VK_SUCCESS) { freeTex(d, t); return NULL; }
  VkCommandBuffer c = tBegin(d);                                   // transparent black to begin with (the spec: frames before the first are 0)
  barrierL(d, c, t->img, 0, t->levels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, ALLCMD, ALLCMD);
  VkClearColorValue z; memset(&z, 0, sizeof z); VkImageSubresourceRange rr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, t->levels, 0, 1 };
  d->vkCmdClearColorImage(c, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &rr);
  barrierL(d, c, t->img, 0, t->levels, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, MEMRW, ALLCMD, ALLCMD);
  tEnd(d);
  return t;
}

// all levels are SHADER_READ_ONLY on entry (level 0 holds the picture, the rest is stale); on exit the whole chain is.
static void genMips(Dev* d, VkCommandBuffer c, Tex* t) {
  if (t->levels < 2) return;
  barrierL(d, c, t->img, 0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, MEMRW, VK_ACCESS_TRANSFER_READ_BIT, ALLCMD, ALLCMD);
  barrierL(d, c, t->img, 1, t->levels - 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, ALLCMD, ALLCMD);
  int lw = t->w, lh = t->h;
  for (uint32_t l = 1; l < t->levels; l++) {
    int nw = lw > 1 ? lw / 2 : 1, nh = lh > 1 ? lh / 2 : 1;
    VkImageBlit b = { 0 }; b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.srcSubresource.mipLevel = l - 1; b.srcSubresource.layerCount = 1;
    b.srcOffsets[1].x = lw; b.srcOffsets[1].y = lh; b.srcOffsets[1].z = 1; b.dstSubresource = b.srcSubresource; b.dstSubresource.mipLevel = l;
    b.dstOffsets[1].x = nw; b.dstOffsets[1].y = nh; b.dstOffsets[1].z = 1;
    d->vkCmdBlitImage(c, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
    if (l + 1 < t->levels) barrierL(d, c, t->img, l, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, ALLCMD, ALLCMD);
    lw = nw; lh = nh;
  }
  barrierL(d, c, t->img, 0, t->levels - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, MEMRW, ALLCMD, ALLCMD);
  barrierL(d, c, t->img, t->levels - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, MEMRW, ALLCMD, ALLCMD);
}

static void* texFromRgba(void* dv, const uint8_t* rgba, int w, int h, int mip) {
  Dev* d = (Dev*)dv;
  Tex* t = newImage(d, w, h, CT_SL_FMT_RGBA8, mip, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
  if (!t) return NULL;
  VkBuffer sb; VkDeviceMemory sm; void* map = NULL; size_t n = (size_t)w * h * 4;
  if (!mkBuffer(d, n, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &sb, &sm, &map)) { freeTex(d, t); return NULL; }
  memcpy(map, rgba, n);
  VkCommandBuffer c = tBegin(d);
  barrierL(d, c, t->img, 0, t->levels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, ALLCMD, ALLCMD);
  VkBufferImageCopy bc = { 0 }; bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; bc.imageSubresource.layerCount = 1; bc.imageExtent.width = (uint32_t)w; bc.imageExtent.height = (uint32_t)h; bc.imageExtent.depth = 1;
  d->vkCmdCopyBufferToImage(c, sb, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
  barrierL(d, c, t->img, 0, t->levels, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, MEMRW, ALLCMD, ALLCMD);
  genMips(d, c, t);
  tEnd(d);
  d->vkUnmapMemory(d->env.device, sm); d->vkDestroyBuffer(d->env.device, sb, NULL); d->vkFreeMemory(d->env.device, sm, NULL);
  return t;
}
static void texDestroy(void* dv, void* tex) {                      // not now: the frame being recorded may still name it. After the next frame's fence wait (CtSlVkBeginFrame).
  Dev* d = (Dev*)dv; Tex* t = (Tex*)tex; if (!t) return;
  if (!t->owned) { free(t); return; }
  if (d->nDead < 512) { d->dead[d->nDead].tex = t; d->dead[d->nDead].prog = NULL; d->nDead++; } else { d->vkDeviceWaitIdle(d->env.device); freeTex(d, t); }
}
static void texCopy(void* dv, void* dst, void* src, int w, int h) {   // outside any render pass (the chain copies the history first thing in the frame)
  Dev* d = (Dev*)dv; Tex* a = (Tex*)dst; Tex* b = (Tex*)src; VkCommandBuffer c = d->cmd;
  barrierL(d, c, b->img, 0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, MEMRW, VK_ACCESS_TRANSFER_READ_BIT, ALLCMD, ALLCMD);
  barrierL(d, c, a->img, 0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, MEMRW, VK_ACCESS_TRANSFER_WRITE_BIT, ALLCMD, ALLCMD);
  VkImageCopy cp = { 0 }; cp.srcSubresource.aspectMask = cp.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; cp.srcSubresource.layerCount = cp.dstSubresource.layerCount = 1;
  cp.extent.width = (uint32_t)w; cp.extent.height = (uint32_t)h; cp.extent.depth = 1;
  d->vkCmdCopyImage(c, b->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, a->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
  barrierL(d, c, b->img, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, MEMRW, ALLCMD, ALLCMD);
  barrierL(d, c, a->img, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, MEMRW, ALLCMD, ALLCMD);
}
static void texGenMips(void* dv, void* tex) { Dev* d = (Dev*)dv; Tex* t = (Tex*)tex; if (t && t->levels > 1) genMips(d, d->cmd, t); }

// A private frame for the capture: the passes record into ccmd, which is submitted and waited for at frameEnd.
static void frameBegin(void* dv) {
  Dev* d = (Dev*)dv; d->hostCmd = d->cmd; d->cmd = d->ccmd; d->finalOpen = 0; d->ringOff = 0;
  d->vkResetDescriptorPool(d->env.device, d->pool, 0);
  d->vkResetCommandBuffer(d->ccmd, 0);
  VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  d->vkBeginCommandBuffer(d->ccmd, &bi);
}
static void frameEnd(void* dv) {
  Dev* d = (Dev*)dv;
  d->vkEndCommandBuffer(d->ccmd);
  VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &d->ccmd;
  d->vkResetFences(d->env.device, 1, &d->tfence);
  d->vkQueueSubmit(d->env.queue, 1, &si, d->tfence);
  d->vkWaitForFences(d->env.device, 1, &d->tfence, VK_TRUE, UINT64_MAX);
  d->cmd = d->hostCmd; d->ringOff = 0;
  d->vkResetDescriptorPool(d->env.device, d->pool, 0);
}
static uint8_t* texReadback(void* dv, void* tex, int* w, int* h) {
  Dev* d = (Dev*)dv; Tex* t = (Tex*)tex; if (!t || !t->img) return NULL;
  size_t n = (size_t)t->w * t->h * 4; VkBuffer b; VkDeviceMemory m; void* map = NULL;
  if (!mkBuffer(d, n, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &b, &m, &map)) return NULL;
  VkCommandBuffer c = tBegin(d);
  barrierL(d, c, t->img, 0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, MEMRW, VK_ACCESS_TRANSFER_READ_BIT, ALLCMD, ALLCMD);
  VkBufferImageCopy bc = { 0 }; bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; bc.imageSubresource.layerCount = 1; bc.imageExtent.width = (uint32_t)t->w; bc.imageExtent.height = (uint32_t)t->h; bc.imageExtent.depth = 1;
  d->vkCmdCopyImageToBuffer(c, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b, 1, &bc);
  barrierL(d, c, t->img, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, MEMRW, ALLCMD, ALLCMD);
  tEnd(d);
  uint8_t* out = (uint8_t*)malloc(n);
  if (out) { memcpy(out, map, n); *w = t->w; *h = t->h; }
  d->vkUnmapMemory(d->env.device, m); d->vkDestroyBuffer(d->env.device, b, NULL); d->vkFreeMemory(d->env.device, m, NULL);
  return out;
}

static VkShaderStageFlags stagesOf(int bits) { return (bits & 1 ? VK_SHADER_STAGE_VERTEX_BIT : 0) | (bits & 2 ? VK_SHADER_STAGE_FRAGMENT_BIT : 0); }

static void* progCreate(void* dv, const CtSlProgram* sp, CtSlFormat fmt, char* err, size_t errN) {
  Dev* d = (Dev*)dv; VkDevice dev = d->env.device;
  if ((unsigned)fmt >= CT_SL_FMT_COUNT || !kVkFmt[fmt]) fmt = CT_SL_FMT_RGBA8;
  Prog* p = (Prog*)calloc(1, sizeof *p); if (!p) return NULL;
  p->fmt = kVkFmt[fmt]; p->fmtId = fmt;
  VkShaderModuleCreateInfo mi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
  mi.codeSize = sp->spvWords[0] * 4; mi.pCode = sp->spv[0];
  if (d->vkCreateShaderModule(dev, &mi, NULL, &p->vs) != VK_SUCCESS) { snprintf(err, errN, "vkCreateShaderModule (vertex)"); freeProg(d, p); return NULL; }
  mi.codeSize = sp->spvWords[1] * 4; mi.pCode = sp->spv[1];
  if (d->vkCreateShaderModule(dev, &mi, NULL, &p->fs) != VK_SUCCESS) { snprintf(err, errN, "vkCreateShaderModule (fragment)"); freeProg(d, p); return NULL; }
  VkDescriptorSetLayoutBinding lb[CT_SL_MAX_TEX + 1]; int nb = 0;
  if (sp->ubo.present) { lb[nb].binding = sp->ubo.binding; lb[nb].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; lb[nb].descriptorCount = 1; lb[nb].stageFlags = stagesOf(sp->ubo.stages) ? stagesOf(sp->ubo.stages) : VK_SHADER_STAGE_ALL_GRAPHICS; lb[nb].pImmutableSamplers = NULL; nb++;
    p->hasUbo = 1; p->uboBinding = sp->ubo.binding; }
  for (int i = 0; i < sp->nTex; i++) { lb[nb].binding = sp->tex[i].binding; lb[nb].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; lb[nb].descriptorCount = 1; lb[nb].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; lb[nb].pImmutableSamplers = NULL; nb++;
    p->texBinding[p->nTex++] = sp->tex[i].binding; }
  VkDescriptorSetLayoutCreateInfo di = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO }; di.bindingCount = (uint32_t)nb; di.pBindings = lb;
  if (d->vkCreateDescriptorSetLayout(dev, &di, NULL, &p->dsl) != VK_SUCCESS) { snprintf(err, errN, "vkCreateDescriptorSetLayout"); freeProg(d, p); return NULL; }
  VkPushConstantRange pr = { 0 };
  VkPipelineLayoutCreateInfo li = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO }; li.setLayoutCount = 1; li.pSetLayouts = &p->dsl;
  if (sp->push.present && sp->push.size) { p->pushSize = (sp->push.size + 3) & ~3u; p->pushStages = stagesOf(sp->push.stages) ? stagesOf(sp->push.stages) : VK_SHADER_STAGE_ALL_GRAPHICS;
    pr.stageFlags = p->pushStages; pr.offset = 0; pr.size = p->pushSize; li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pr; }
  if (d->vkCreatePipelineLayout(dev, &li, NULL, &p->pl) != VK_SUCCESS) { snprintf(err, errN, "vkCreatePipelineLayout"); freeProg(d, p); return NULL; }
  return p;
}
static void progDestroy(void* dv, void* prog) {
  Dev* d = (Dev*)dv; Prog* p = (Prog*)prog; if (!p) return;
  if (d->nDead < 512) { d->dead[d->nDead].prog = p; d->dead[d->nDead].tex = NULL; d->nDead++; } else { d->vkDeviceWaitIdle(d->env.device); freeProg(d, p); }
}

static VkPipeline makePipeline(Dev* d, Prog* p, VkRenderPass rp) {
  VkPipelineShaderStageCreateInfo st[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO }, { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
  st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = p->vs; st[0].pName = "main"; st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = p->fs; st[1].pName = "main";
  VkVertexInputBindingDescription vb = { 0, 6 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX };
  VkVertexInputAttributeDescription va[2] = { { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 }, { 1, 0, VK_FORMAT_R32G32_SFLOAT, 4 * sizeof(float) } };
  VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO }; vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb; vi.vertexAttributeDescriptionCount = 2; vi.pVertexAttributeDescriptions = va;
  VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO }; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
  VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO }; vp.viewportCount = 1; vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO }; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO }; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState ba = { 0 }; ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO }; cb.attachmentCount = 1; cb.pAttachments = &ba;
  VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
  VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO }; ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
  VkGraphicsPipelineCreateInfo gi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO }; gi.stageCount = 2; gi.pStages = st; gi.pVertexInputState = &vi; gi.pInputAssemblyState = &ia; gi.pViewportState = &vp;
  gi.pRasterizationState = &rs; gi.pMultisampleState = &ms; gi.pColorBlendState = &cb; gi.pDynamicState = &ds; gi.layout = p->pl; gi.renderPass = rp; gi.subpass = 0;
  VkPipeline pipe = VK_NULL_HANDLE;
  if (d->vkCreateGraphicsPipelines(d->env.device, VK_NULL_HANDLE, 1, &gi, NULL, &pipe) != VK_SUCCESS) return VK_NULL_HANDLE;
  return pipe;
}

static int passBegin(void* dv, void* target, const CtSlRect* vp) {
  Dev* d = (Dev*)dv; Tex* t = (Tex*)target; VkCommandBuffer c = d->cmd;
  if (!t) { if (d->env.beginFinal(d->env.user, vp)) return 1; d->finalOpen = 1; return 0; }
  VkRenderPassBeginInfo rb = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO }; rb.renderPass = getRp(d, t->fmt); rb.framebuffer = t->fb; rb.renderArea.extent.width = (uint32_t)t->w; rb.renderArea.extent.height = (uint32_t)t->h;
  d->vkCmdBeginRenderPass(c, &rb, VK_SUBPASS_CONTENTS_INLINE);
  VkViewport v = { (float)vp->x, (float)vp->y, (float)vp->w, (float)vp->h, 0.0f, 1.0f }; VkRect2D sc = { { vp->x, vp->y }, { (uint32_t)vp->w, (uint32_t)vp->h } };
  d->vkCmdSetViewport(c, 0, 1, &v); d->vkCmdSetScissor(c, 0, 1, &sc);
  return 0;
}
static void passDraw(void* dv, void* prog, const CtSlBind* binds, int nBinds, const void* ubo, size_t uboN, const void* push, size_t pushN) {
  Dev* d = (Dev*)dv; Prog* p = (Prog*)prog; VkCommandBuffer c = d->cmd; VkDevice dev = d->env.device;
  int fin = d->finalOpen && !p->pipe[1] ? 1 : d->finalOpen;                 // (the final pass is the one drawn after beginFinal opened the host's pass)
  VkPipeline* pp = &p->pipe[fin ? 1 : 0];
  if (!*pp) { *pp = makePipeline(d, p, fin ? d->env.finalPass : getRp(d, p->fmtId)); if (!*pp) return; }
  VkDescriptorSet ds; VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO }; ai.descriptorPool = d->pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &p->dsl;
  if (d->vkAllocateDescriptorSets(dev, &ai, &ds) != VK_SUCCESS) return;
  VkWriteDescriptorSet w[CT_SL_MAX_TEX + 1]; VkDescriptorImageInfo ii[CT_SL_MAX_TEX]; VkDescriptorBufferInfo bi; int nw = 0; uint32_t dynOff = 0;
  if (p->hasUbo) {
    size_t at = (d->ringOff + d->uboAlign - 1) & ~((size_t)d->uboAlign - 1);
    if (at + uboN > d->ringSize) return;                                       // (out of ring: skip the draw; 2 MB is far more than 64 passes need)
    memcpy(d->ringMap + at, ubo, uboN); d->ringOff = at + uboN; dynOff = (uint32_t)at;
    bi.buffer = d->ring; bi.offset = 0; bi.range = uboN ? uboN : 4;
    memset(&w[nw], 0, sizeof w[nw]); w[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[nw].dstSet = ds; w[nw].dstBinding = p->uboBinding; w[nw].descriptorCount = 1;
    w[nw].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; w[nw].pBufferInfo = &bi; nw++;
  }
  for (int i = 0; i < nBinds && i < CT_SL_MAX_TEX; i++) {
    const Tex* t = (const Tex*)binds[i].tex; if (!t) continue;
    int idx = (binds[i].filter ? 1 : 0) | (binds[i].wrap & 3) << 1 | ((binds[i].mip && t->levels > 1) ? 8 : 0);
    if (!d->samp[idx]) {
      VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO }; si.magFilter = binds[i].filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST; si.minFilter = si.magFilter;
      si.mipmapMode = binds[i].filter ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
      VkSamplerAddressMode am = binds[i].wrap == CT_SL_WRAP_EDGE ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : binds[i].wrap == CT_SL_WRAP_REPEAT ? VK_SAMPLER_ADDRESS_MODE_REPEAT :
                                binds[i].wrap == CT_SL_WRAP_MIRROR ? VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
      si.addressModeU = si.addressModeV = si.addressModeW = am; si.maxLod = (idx & 8) ? VK_LOD_CLAMP_NONE : 0.0f; si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
      if (d->vkCreateSampler(dev, &si, NULL, &d->samp[idx]) != VK_SUCCESS) return;
    }
    ii[i].sampler = d->samp[idx]; ii[i].imageView = t->view; ii[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    memset(&w[nw], 0, sizeof w[nw]); w[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[nw].dstSet = ds; w[nw].dstBinding = binds[i].binding; w[nw].descriptorCount = 1;
    w[nw].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[nw].pImageInfo = &ii[i]; nw++;
  }
  d->vkUpdateDescriptorSets(dev, (uint32_t)nw, w, 0, NULL);
  d->vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, *pp);
  d->vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pl, 0, 1, &ds, p->hasUbo ? 1 : 0, &dynOff);
  if (p->pushSize) { uint8_t pc[256] = { 0 }; size_t n = pushN < sizeof pc ? pushN : sizeof pc; if (push) memcpy(pc, push, n); d->vkCmdPushConstants(c, p->pl, p->pushStages, 0, p->pushSize < sizeof pc ? p->pushSize : (uint32_t)sizeof pc, pc); }
  VkDeviceSize zero = 0; d->vkCmdBindVertexBuffers(c, 0, 1, &d->vbo, &zero);
  d->vkCmdDraw(c, 4, 1, 0, 0);
}
static void passEnd(void* dv, void* target) { Dev* d = (Dev*)dv; if (target) d->vkCmdEndRenderPass(d->cmd); else (void)d; }

// ---- the device ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------

int CtSlVkCreate(const CtSlVkEnv* env, CtSlGpuOps* ops, void** out, char* err, size_t errN) {
  Dev* d = (Dev*)calloc(1, sizeof *d); if (!d) { snprintf(err, errN, "out of memory"); return 1; }
  d->env = *env;
  PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)env->getInstanceProcAddr(env->instance, "vkGetDeviceProcAddr");
  d->getMem = (PFN_vkGetPhysicalDeviceMemoryProperties)env->getInstanceProcAddr(env->instance, "vkGetPhysicalDeviceMemoryProperties");
  PFN_vkGetPhysicalDeviceProperties gpdp = (PFN_vkGetPhysicalDeviceProperties)env->getInstanceProcAddr(env->instance, "vkGetPhysicalDeviceProperties");
  if (!gdpa || !d->getMem || !gpdp) { snprintf(err, errN, "Vulkan: no vkGetDeviceProcAddr"); free(d); return 1; }
#define LOAD(f) if (!(d->f = (PFN_##f)gdpa(env->device, #f))) { snprintf(err, errN, "Vulkan: no %s", #f); free(d); return 1; }
  FUNCS(LOAD)
#undef LOAD
  d->getMem(env->phys, &d->mp);
  VkPhysicalDeviceProperties pp; gpdp(env->phys, &pp);
  d->uboAlign = (uint32_t)pp.limits.minUniformBufferOffsetAlignment; if (d->uboAlign < 16) d->uboAlign = 16;
  VkDevice dev = env->device;
  static const float quad[4][6] = { { 0, 0, 0, 1, 0, 0 }, { 1, 0, 0, 1, 1, 0 }, { 0, 1, 0, 1, 0, 1 }, { 1, 1, 0, 1, 1, 1 } };      // Position (x, y, 0, 1), TexCoord (x, y); (0, 0) is the top left
  void* map = NULL;
  if (!mkBuffer(d, sizeof quad, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &d->vbo, &d->vboMem, &map)) { snprintf(err, errN, "Vulkan: no vertex buffer"); CtSlVkDestroy(d); return 1; }
  memcpy(map, quad, sizeof quad);
  d->ringSize = 2u << 20;
  if (!mkBuffer(d, d->ringSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &d->ring, &d->ringMem, (void**)&d->ringMap)) { snprintf(err, errN, "Vulkan: no uniform buffer"); CtSlVkDestroy(d); return 1; }
  VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 256 * CT_SL_MAX_TEX }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 256 } };
  VkDescriptorPoolCreateInfo di = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO }; di.maxSets = 256; di.poolSizeCount = 2; di.pPoolSizes = ps;
  if (d->vkCreateDescriptorPool(dev, &di, NULL, &d->pool) != VK_SUCCESS) { snprintf(err, errN, "Vulkan: no descriptor pool"); CtSlVkDestroy(d); return 1; }
  VkCommandPoolCreateInfo ci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO }; ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; ci.queueFamilyIndex = env->queueFamily;
  VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO }; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
  VkFenceCreateInfo fc = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
  if (d->vkCreateCommandPool(dev, &ci, NULL, &d->cpool) != VK_SUCCESS || (ai.commandPool = d->cpool, d->vkAllocateCommandBuffers(dev, &ai, &d->tcmd) != VK_SUCCESS) || d->vkAllocateCommandBuffers(dev, &ai, &d->ccmd) != VK_SUCCESS ||
      d->vkCreateFence(dev, &fc, NULL, &d->tfence) != VK_SUCCESS) { snprintf(err, errN, "Vulkan: no command buffer"); CtSlVkDestroy(d); return 1; }
  memset(ops, 0, sizeof *ops);
  ops->name = "Vulkan"; ops->flipBackbuffer = 0; ops->wantsGlsl = 0; ops->maxPushBytes = (int)(pp.limits.maxPushConstantsSize < 256 ? pp.limits.maxPushConstantsSize : 256);
  ops->texCreate = texCreate; ops->texFromRgba = texFromRgba; ops->texDestroy = texDestroy; ops->texCopy = texCopy; ops->texGenMips = texGenMips;
  ops->progCreate = progCreate; ops->progDestroy = progDestroy; ops->passBegin = passBegin; ops->passDraw = passDraw; ops->passEnd = passEnd;
  ops->frameBegin = frameBegin; ops->frameEnd = frameEnd; ops->texReadback = texReadback;
  *out = d; return 0;
}
void CtSlVkDestroy(void* dv) {
  Dev* d = (Dev*)dv; if (!d) return;
  VkDevice dev = d->env.device;
  if (d->vkDeviceWaitIdle) d->vkDeviceWaitIdle(dev);
  flushDead(d);
  for (int i = 0; i < 16; i++) if (d->samp[i]) d->vkDestroySampler(dev, d->samp[i], NULL);
  for (int i = 0; i < CT_SL_FMT_COUNT; i++) if (d->rp[i]) d->vkDestroyRenderPass(dev, d->rp[i], NULL);
  if (d->pool) d->vkDestroyDescriptorPool(dev, d->pool, NULL);
  if (d->tfence) d->vkDestroyFence(dev, d->tfence, NULL);
  if (d->cpool) d->vkDestroyCommandPool(dev, d->cpool, NULL);
  if (d->ring) { d->vkDestroyBuffer(dev, d->ring, NULL); d->vkFreeMemory(dev, d->ringMem, NULL); }
  if (d->vbo) { d->vkDestroyBuffer(dev, d->vbo, NULL); d->vkFreeMemory(dev, d->vboMem, NULL); }
  free(d);
}
void CtSlVkBeginFrame(void* dv, VkCommandBuffer cmd) {
  Dev* d = (Dev*)dv; d->cmd = cmd; d->finalOpen = 0; d->ringOff = 0;
  d->vkResetDescriptorPool(d->env.device, d->pool, 0);
  flushDead(d);                                                    // (the host waited for the last frame's fence before calling this)
}
void* CtSlVkWrap(void* dv, VkImage image, VkImageView view, int w, int h) { (void)dv; Tex* t = (Tex*)calloc(1, sizeof *t); if (t) { t->img = image; t->view = view; t->w = w; t->h = h; t->levels = 1; t->fmt = CT_SL_FMT_RGBA8; } return t; }
void CtSlVkWrapSet(void* wr, VkImage image, VkImageView view, int w, int h) { Tex* t = (Tex*)wr; if (t) { t->img = image; t->view = view; t->w = w; t->h = h; } }
void CtSlVkWrapFree(void* wr) { free(wr); }
VkImage CtSlVkImageOf(void* tex) { return tex ? ((Tex*)tex)->img : VK_NULL_HANDLE; }
