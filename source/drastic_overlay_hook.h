/* Lets a frontend draw its own UI on top of each presented frame, after the
 * DS screens and the host's overlay image, with whichever renderer is active.
 * Both hooks run on the presenting thread. */
#ifndef DRASTIC_NX_OVERLAY_HOOK_H
#define DRASTIC_NX_OVERLAY_HOOK_H

#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  VkInstance instance;
  VkPhysicalDevice physical_device;
  VkDevice device;
  VkQueue queue;
  uint32_t queue_family;
  /* the final render pass, open on command_buffer when the hook runs */
  VkRenderPass render_pass;
  VkCommandBuffer command_buffer;
  VkFormat format;
  uint32_t image_count;
  uint32_t width;
  uint32_t height;
} DrasticVkOverlayContext;

typedef void (*DrasticVkOverlayHook)(const DrasticVkOverlayContext *context);
/* called with the window framebuffer bound, before the buffer swap */
typedef void (*DrasticGlOverlayHook)(int width, int height);

extern DrasticVkOverlayHook drastic_vk_overlay_hook;
extern DrasticGlOverlayHook drastic_gl_overlay_hook;

#ifdef __cplusplus
}
#endif

#endif
