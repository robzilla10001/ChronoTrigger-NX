// slang_vk.h - the Vulkan implementation of the shader chain's GPU interface (slang_gpu.h). Self-contained: it loads the Vulkan functions it needs itself, from the handles of the
// device it is given (present_vk.c's on the console, a software Vulkan's in tools/slangtool), and records into the command buffer the host is recording.
//
// The host (the backend) owns the frame: it acquires the swapchain image, records into one command buffer, and presents. Around the chain it does:
//     CtSlVkBeginFrame(dev, cmd);   // after the frame's fence wait: the previous frame is over, what died with it is freed
//     ... upload the picture, clear the backbuffer ...
//     CtSlChainRun(chain, &frame);  // the offscreen passes record their own render passes; the LAST pass calls env.beginFinal, which begins the host's render pass on the backbuffer
// and goes on (the overlay) in the render pass beginFinal opened, then ends it. Nothing here submits except the one-time clears and uploads of textures (a private command buffer, waited for).
#ifndef CT_SLANG_VK_H
#define CT_SLANG_VK_H
#include <vulkan/vulkan.h>
#include "slang_gpu.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct CtSlVkEnv {
  PFN_vkGetInstanceProcAddr getInstanceProcAddr;
  VkInstance instance; VkPhysicalDevice phys; VkDevice device; VkQueue queue; uint32_t queueFamily;
  VkRenderPass finalPass;              // the pass the last chain pass draws in (the backbuffer's format, loadOp LOAD: the host cleared it)
  // Called by the LAST pass: transition the backbuffer for drawing, begin finalPass on it (render area: the whole backbuffer) and set the viewport and the scissor to `vp` -- the chain draws
  // into that rectangle. The host ends the render pass after its overlay. 0 = ok.
  int (*beginFinal)(void* user, const CtSlRect* vp);
  void* user;
} CtSlVkEnv;

int  CtSlVkCreate(const CtSlVkEnv* env, CtSlGpuOps* ops, void** dev, char* err, size_t errN);
void CtSlVkDestroy(void* dev);                                  // waits for the device to be idle first
void CtSlVkBeginFrame(void* dev, VkCommandBuffer cmd);
// The game's picture as a chain texture: an image the host keeps in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL while the chain runs. Not freed by the chain. Update on a size change.
void* CtSlVkWrap(void* dev, VkImage image, VkImageView view, int w, int h);
void  CtSlVkWrapSet(void* wrapped, VkImage image, VkImageView view, int w, int h);
void  CtSlVkWrapFree(void* wrapped);
// The texture's image / format for the host's own use (tests: reading the result back).
VkImage CtSlVkImageOf(void* tex);

#ifdef __cplusplus
}
#endif
#endif
