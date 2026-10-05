#ifndef DRASTIC_NX_RUNTIME_CONFIG_H
#define DRASTIC_NX_RUNTIME_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#include "drastic_custom_shader.h"

typedef enum {
  DRASTIC_LAYOUT_VERTICAL,
  DRASTIC_LAYOUT_HORIZONTAL,
  DRASTIC_LAYOUT_TOP_ONLY,
  DRASTIC_LAYOUT_BOTTOM_ONLY,
  DRASTIC_LAYOUT_HYBRID_TOP,
  DRASTIC_LAYOUT_HYBRID_BOTTOM,
  DRASTIC_LAYOUT_CUSTOM,
  /* one screen large, the other small beside, above or below it */
  DRASTIC_LAYOUT_LARGE,
  /* one screen filling the display, the other floating on top of it */
  DRASTIC_LAYOUT_OVERLAY,
  DRASTIC_LAYOUT_COUNT,
} DrasticLayoutMode;

/* Where the small screen sits: beside the large one (Large screen) or
 * inside it (Screen overlay, where above/below mean top/bottom centre). */
typedef enum {
  DRASTIC_SMALL_TOP_RIGHT,
  DRASTIC_SMALL_MIDDLE_RIGHT,
  DRASTIC_SMALL_BOTTOM_RIGHT,
  DRASTIC_SMALL_TOP_LEFT,
  DRASTIC_SMALL_MIDDLE_LEFT,
  DRASTIC_SMALL_BOTTOM_LEFT,
  DRASTIC_SMALL_ABOVE,
  DRASTIC_SMALL_BELOW,
} DrasticSmallScreenPosition;

typedef enum {
  DRASTIC_FILTER_NEAREST,
  DRASTIC_FILTER_LINEAR,
  DRASTIC_FILTER_QUILEZ,
  DRASTIC_FILTER_SCANLINE,
  DRASTIC_FILTER_SCALE2X,
  DRASTIC_FILTER_HQ2X,
  DRASTIC_FILTER_FXAA,
  DRASTIC_FILTER_FXAA_HQ,
  DRASTIC_FILTER_SMAA,
  DRASTIC_FILTER_FSR,
  DRASTIC_FILTER_CUSTOM,
  DRASTIC_FILTER_COUNT,
} DrasticVideoFilter;

typedef enum {
  DRASTIC_MICROPHONE_SIMULATED,
  DRASTIC_MICROPHONE_EXTERNAL,
} DrasticMicrophoneSource;

typedef enum {
  DRASTIC_STYLUS_OFF,
  DRASTIC_STYLUS_STICK,
  DRASTIC_STYLUS_MOTION,
} DrasticStylusMode;

typedef struct {
  float x;
  float y;
  float width;
  float height;
  int screen;       /* 0 = DS top, 1 = DS bottom */
  int touch_target; /* accepts touchscreen input */
  float opacity;    /* 1 = opaque; the overlaid screen may be see-through */
} DrasticScreenRect;

typedef struct {
  char rom_path[1024];
  char core_path[256];
  char launcher_path[1024];
  char firmware_nickname[32];
  DrasticLayoutMode layout;
  int swap_screens;
  int rotation;
  int screen_gap;
  int integer_scale;
  /* Large screen: how many times bigger the large screen is */
  float large_proportion;
  DrasticSmallScreenPosition small_position;
  /* Screen overlay: where the small screen sits, its width as a percentage
   * of the large one, and how solid it is drawn (percent) */
  DrasticSmallScreenPosition overlay_position;
  int overlay_size;
  int overlay_opacity;
  /* single-screen layouts fill the display, ignoring the DS aspect ratio */
  int stretch_single;
  /* blank space around each DS screen, in 720p pixels: x then y, top
   * screen then bottom screen */
  int padding[2][2];
  /* behind the screens, 0xRRGGBB */
  uint32_t background;
  int custom_aspect_lock;
  int vulkan_low_latency;
  DrasticVideoFilter video_filter;
  /* FSR 1.0 sharpening, 0..100 */
  int fsr_sharpness;
  char custom_shader[DRASTIC_CUSTOM_SHADER_PATH_MAX];
  int show_fps;
  int volume;
  int microphone_enabled;
  DrasticMicrophoneSource microphone_source;
  int autosave_seconds;
  int vibration;
  int motion;
  int lua_enabled;
  DrasticStylusMode stylus_mode;
  int mouse_stylus;
  int motion_stylus_sensitivity;
  int stylus_x;
  int stylus_y;
  int stylus_visible;
  int screen_count;
  uint32_t firmware_userdata;
  uint64_t core_config;
  DrasticScreenRect screens[3];
  /* Normalized custom rectangles. Index 0 is the DS top screen. */
  DrasticScreenRect custom_screens[2];
} DrasticRuntimeConfig;

void drastic_config_load(DrasticRuntimeConfig *config);
/* The Wrapper/Layout value of a layout, e.g. "vertical". */
const char *drastic_config_layout_name(DrasticLayoutMode layout);
DrasticLayoutMode drastic_config_parse_layout(const char *name);
uint64_t drastic_config_build_core_config(void);
const char *drastic_config_filter_name(DrasticVideoFilter filter);
void drastic_config_calculate_layout(DrasticRuntimeConfig *config,
                                     int width, int height);
bool drastic_config_map_touch(const DrasticRuntimeConfig *config,
                              float panel_x, float panel_y,
                              int *ds_x, int *ds_y);
bool drastic_config_map_touch_rects(const DrasticScreenRect *screens,
                                    int screen_count, int rotation,
                                    float panel_x, float panel_y,
                                    int *ds_x, int *ds_y);
bool drastic_config_map_stylus(const DrasticRuntimeConfig *config,
                               int ds_x, int ds_y,
                               float *panel_x, float *panel_y);

#endif
