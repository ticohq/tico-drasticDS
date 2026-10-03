#include "drastic_renderer.h"
#include "drastic_overlay_hook.h"

#include <string.h>

/* The implementations intentionally keep independent state.  This small
 * dispatcher is the only public renderer ABI used by the rest of the host. */
bool drastic_gl_renderer_init(const DrasticRuntimeConfig *config);
void drastic_gl_renderer_present(const DrasticRuntimeConfig *config,
                                 DrasticCoreRenderFrame core_render,
                                 void *env, void *clazz,
                                 const DrasticOverlayFrame *overlay,
                                 bool consume_core_frame);
void drastic_gl_renderer_suspend(void);
void drastic_gl_renderer_resume(void);
void drastic_gl_renderer_shutdown(void);
unsigned drastic_gl_renderer_frame_count(void);
bool drastic_gl_renderer_lsfg_available(void);
bool drastic_gl_renderer_lsfg_enabled(void);
bool drastic_gl_renderer_lsfg_request_enabled(bool enabled);
bool drastic_gl_renderer_set_custom_shader(const char *relative_path,
                                           char *error, size_t error_size);
const char *drastic_gl_renderer_last_error(void);

bool drastic_vk_renderer_init(const DrasticRuntimeConfig *config);
void drastic_vk_renderer_present(const DrasticRuntimeConfig *config,
                                 DrasticCoreRenderFrame core_render,
                                 void *env, void *clazz,
                                 const DrasticOverlayFrame *overlay,
                                 bool consume_core_frame);
void drastic_vk_renderer_suspend(void);
void drastic_vk_renderer_resume(void);
void drastic_vk_renderer_shutdown(void);
unsigned drastic_vk_renderer_frame_count(void);
bool drastic_vk_renderer_lsfg_available(void);
bool drastic_vk_renderer_lsfg_enabled(void);
bool drastic_vk_renderer_lsfg_request_enabled(bool enabled);
bool drastic_vk_renderer_set_custom_shader(const char *relative_path,
                                           char *error, size_t error_size);
const char *drastic_vk_renderer_last_error(void);

DrasticVkOverlayHook drastic_vk_overlay_hook;
DrasticGlOverlayHook drastic_gl_overlay_hook;

static DrasticRendererBackend g_backend = DRASTIC_RENDERER_BACKEND_VULKAN;

void drastic_renderer_select(const char *name) {
  g_backend = name && (!strcmp(name, "gl") || !strcmp(name, "zink"))
                  ? DRASTIC_RENDERER_BACKEND_GL
                  : DRASTIC_RENDERER_BACKEND_VULKAN;
}

DrasticRendererBackend drastic_renderer_backend(void) { return g_backend; }

bool drastic_renderer_is_vulkan(void) {
  return g_backend == DRASTIC_RENDERER_BACKEND_VULKAN;
}

const char *drastic_renderer_backend_name(void) {
  return drastic_renderer_is_vulkan() ? "Vulkan" : "OpenGL";
}

bool drastic_renderer_init(const DrasticRuntimeConfig *config) {
  return drastic_renderer_is_vulkan() ? drastic_vk_renderer_init(config)
                                      : drastic_gl_renderer_init(config);
}

void drastic_renderer_present(const DrasticRuntimeConfig *config,
                              DrasticCoreRenderFrame core_render,
                              void *env, void *clazz,
                              const DrasticOverlayFrame *overlay,
                              bool consume_core_frame) {
  if (drastic_renderer_is_vulkan())
    drastic_vk_renderer_present(config, core_render, env, clazz, overlay,
                                consume_core_frame);
  else
    drastic_gl_renderer_present(config, core_render, env, clazz, overlay,
                                consume_core_frame);
}

void drastic_renderer_suspend(void) {
  if (drastic_renderer_is_vulkan()) drastic_vk_renderer_suspend();
  else drastic_gl_renderer_suspend();
}

void drastic_renderer_resume(void) {
  if (drastic_renderer_is_vulkan()) drastic_vk_renderer_resume();
  else drastic_gl_renderer_resume();
}

void drastic_renderer_shutdown(void) {
  if (drastic_renderer_is_vulkan()) drastic_vk_renderer_shutdown();
  else drastic_gl_renderer_shutdown();
}

unsigned drastic_renderer_frame_count(void) {
  return drastic_renderer_is_vulkan() ? drastic_vk_renderer_frame_count()
                                      : drastic_gl_renderer_frame_count();
}

bool drastic_renderer_lsfg_available(void) {
  return drastic_renderer_is_vulkan() &&
         drastic_vk_renderer_lsfg_available();
}

bool drastic_renderer_lsfg_enabled(void) {
  return drastic_renderer_is_vulkan() && drastic_vk_renderer_lsfg_enabled();
}

bool drastic_renderer_lsfg_request_enabled(bool enabled) {
  return drastic_renderer_is_vulkan()
             ? drastic_vk_renderer_lsfg_request_enabled(enabled)
             : drastic_gl_renderer_lsfg_request_enabled(enabled);
}

bool drastic_renderer_set_custom_shader(const char *relative_path,
                                        char *error, size_t error_size) {
  return drastic_renderer_is_vulkan()
             ? drastic_vk_renderer_set_custom_shader(relative_path, error,
                                                     error_size)
             : drastic_gl_renderer_set_custom_shader(relative_path, error,
                                                     error_size);
}

const char *drastic_renderer_last_error(void) {
  return drastic_renderer_is_vulkan() ? drastic_vk_renderer_last_error()
                                      : drastic_gl_renderer_last_error();
}
