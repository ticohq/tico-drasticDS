#include <switch.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "drastic_config.h"
#include "drastic_rotation.h"
#include "prefs.h"

static int clamp_int(int value, int minimum, int maximum) {
  if (value < minimum) return minimum;
  if (value > maximum) return maximum;
  return value;
}

static unsigned system_firmware_language(void) {
  unsigned firmware_language = 1; /* English fallback. */
  if (R_FAILED(setInitialize())) return firmware_language;

  u64 language_code = 0;
  SetLanguage language = SetLanguage_ENUS;
  const Result get_result = setGetSystemLanguage(&language_code);
  const Result convert_result = R_SUCCEEDED(get_result)
      ? setMakeLanguage(language_code, &language) : get_result;
  setExit();
  if (R_FAILED(get_result) || R_FAILED(convert_result))
    return firmware_language;

  switch (language) {
    case SetLanguage_JA: firmware_language = 0; break;
    case SetLanguage_ENUS:
    case SetLanguage_ENGB: firmware_language = 1; break;
    case SetLanguage_FR:
    case SetLanguage_FRCA: firmware_language = 2; break;
    case SetLanguage_DE: firmware_language = 3; break;
    case SetLanguage_IT: firmware_language = 4; break;
    case SetLanguage_ES:
    case SetLanguage_ES419: firmware_language = 5; break;
    case SetLanguage_KO: firmware_language = 6; break;
    default: break;
  }
  return firmware_language;
}

static uint64_t flag_if(bool enabled, unsigned bit) {
  return enabled ? (UINT64_C(1) << bit) : 0;
}

uint64_t drastic_config_build_core_config(void) {
  const int frameskip = clamp_int(prefs_get_int("Drastic/FrameskipValue", 0), 0, 9);
  const int frameskip_type = clamp_int(prefs_get_int("Drastic/FrameskipType", 0), 0, 3);
  const int audio_latency = clamp_int(prefs_get_int("Drastic/AudioLatency", 2), 0, 3);
  const int fast_forward = clamp_int(prefs_get_int("Drastic/FastForwardSpeed", 2), 0, 5);
  const int cpu_threads = clamp_int(prefs_get_int("Drastic/CpuThreads", 3), 1, 3);
  const int autofire = clamp_int(prefs_get_int("Drastic/AutoFireSpeed", 2), 0, 7);
  const int mic_level = clamp_int(prefs_get_int("Drastic/MicLevel", 1), 0, 3);
  const int slot2 = clamp_int(prefs_get_int("Drastic/Slot2Type", 1), 0, 5);

  uint64_t value = (uint64_t)frameskip;
  value |= (uint64_t)frameskip_type << 5;
  value |= (uint64_t)audio_latency << 8;
  value |= (uint64_t)fast_forward << 12;
  value |= (uint64_t)cpu_threads << 16;
  value |= (uint64_t)autofire << 32;
  value |= (uint64_t)mic_level << 37;
  value |= (uint64_t)slot2 << 43;

  value |= flag_if(prefs_get_bool("Drastic/SoundEnabled", true), 31);
  value |= flag_if(prefs_get_bool("Drastic/ShowFPS", false), 30);
  value |= flag_if(prefs_get_bool("Drastic/Threaded3D", true), 28);
  value |= flag_if(prefs_get_bool("Drastic/CheatsEnabled", true), 27);
  value |= flag_if(prefs_get_bool("Drastic/MicEnabled", true), 26);
  value |= flag_if(prefs_get_bool("Drastic/BackupInSavestates", true), 25);
  value |= flag_if(prefs_get_bool("Drastic/IgnoreGamecardLimit", false), 24);
  value |= flag_if(prefs_get_bool("Drastic/Use16BitColor", false), 23);
  value |= flag_if(prefs_get_bool("Drastic/AutoTrim", false), 36);
  value |= flag_if(prefs_get_bool("Drastic/FixMainEngineScreen", false), 35);
  value |= flag_if(prefs_get_bool("Drastic/RtcSystemTime", true), 39);
  value |= flag_if(prefs_get_bool("Drastic/DisableEdgeMarking", false), 40);
  value |= flag_if(prefs_get_bool("Drastic/Hires3D", false), 41);
  value |= flag_if(prefs_get_bool("Drastic/LuaEnabled", true), 42);
  value |= flag_if(prefs_get_bool("Drastic/FrameskipSafe", false), 47);
  value |= flag_if(prefs_get_bool("Drastic/PreloadRoms", true), 48);
  value |= flag_if(prefs_get_bool("Drastic/Blend", false), 49);
  value |= flag_if(prefs_get_bool("Drastic/RawSaveFormat", false), 50);
  return value;
}

static const char *const layout_names[DRASTIC_LAYOUT_COUNT] = {
  "vertical", "horizontal", "top", "bottom", "hybrid_top", "hybrid_bottom",
  "custom", "large", "overlay",
};

const char *drastic_config_layout_name(DrasticLayoutMode layout) {
  return (unsigned)layout < DRASTIC_LAYOUT_COUNT ? layout_names[layout]
                                                 : layout_names[1];
}

DrasticLayoutMode drastic_config_parse_layout(const char *name) {
  for (int layout = 0; name && layout < DRASTIC_LAYOUT_COUNT; layout++)
    if (!strcmp(name, layout_names[layout])) return (DrasticLayoutMode)layout;
  return DRASTIC_LAYOUT_HORIZONTAL;
}

static DrasticLayoutMode read_layout(void) {
  return drastic_config_parse_layout(
      prefs_get_string("Wrapper/Layout", "horizontal"));
}

static DrasticSmallScreenPosition read_small_position(const char *key,
                                                      const char *fallback) {
  static const char *const names[] = {
    "top_right", "middle_right", "bottom_right", "top_left", "middle_left",
    "bottom_left", "above", "below",
  };
  const char *value = prefs_get_string(key, fallback);
  for (unsigned index = 0; index < sizeof(names) / sizeof(*names); index++)
    if (!strcmp(value, names[index])) return (DrasticSmallScreenPosition)index;
  return DRASTIC_SMALL_BOTTOM_RIGHT;
}

static DrasticVideoFilter read_filter(void) {
  const char *filter = prefs_get_string(
      "Wrapper/VideoFilter",
      prefs_get_string("Wrapper/TextureFilter", "nearest"));
  if (!strcmp(filter, "linear")) return DRASTIC_FILTER_LINEAR;
  if (!strcmp(filter, "quilez")) return DRASTIC_FILTER_QUILEZ;
  if (!strcmp(filter, "scanline")) return DRASTIC_FILTER_SCANLINE;
  if (!strcmp(filter, "scale2x")) return DRASTIC_FILTER_SCALE2X;
  if (!strcmp(filter, "hq2x")) return DRASTIC_FILTER_HQ2X;
  if (!strcmp(filter, "fxaa")) return DRASTIC_FILTER_FXAA;
  if (!strcmp(filter, "fxaa_hq")) return DRASTIC_FILTER_FXAA_HQ;
  if (!strcmp(filter, "smaa")) return DRASTIC_FILTER_SMAA;
  if (!strcmp(filter, "fsr")) return DRASTIC_FILTER_FSR;
  if (!strcmp(filter, "custom")) return DRASTIC_FILTER_CUSTOM;
  return DRASTIC_FILTER_NEAREST;
}

static DrasticMicrophoneSource read_microphone_source(void) {
  const char *source = prefs_get_string("Wrapper/MicrophoneSource", "noise");
  return !strcmp(source, "external") ? DRASTIC_MICROPHONE_EXTERNAL
                                     : DRASTIC_MICROPHONE_SIMULATED;
}

static DrasticStylusMode read_stylus_mode(void) {
  const char *mode = prefs_get_string("Wrapper/StylusMode", "stick");
  if (!strcmp(mode, "off")) return DRASTIC_STYLUS_OFF;
  if (!strcmp(mode, "motion")) return DRASTIC_STYLUS_MOTION;
  return DRASTIC_STYLUS_STICK;
}

const char *drastic_config_filter_name(DrasticVideoFilter filter) {
  static const char *names[DRASTIC_FILTER_COUNT] = {
    "nearest", "linear", "quilez", "scanline", "scale2x", "hq2x", "fxaa",
    "fxaa_hq", "smaa", "fsr", "custom"
  };
  if ((unsigned)filter >= DRASTIC_FILTER_COUNT) return names[0];
  return names[filter];
}

static float clamp_float(float value, float minimum, float maximum) {
  if (value < minimum) return minimum;
  if (value > maximum) return maximum;
  return value;
}

void drastic_config_load(DrasticRuntimeConfig *config) {
  memset(config, 0, sizeof(*config));
  snprintf(config->rom_path, sizeof(config->rom_path), "%s",
           prefs_get_string("Drastic/RomPath", DEFAULT_ROM_PATH));
  snprintf(config->core_path, sizeof(config->core_path), "%s",
           prefs_get_string("Wrapper/CoreSo", SO_NAME));
  snprintf(config->launcher_path, sizeof(config->launcher_path), "%s",
           prefs_get_string("Wrapper/LauncherPath", ""));
  snprintf(config->firmware_nickname, sizeof(config->firmware_nickname), "%s",
           prefs_get_string("Drastic/FirmwareNickname", "Switch"));
  const int configured_language =
      prefs_get_int("Drastic/FirmwareLanguage", -1);
  const unsigned firmware_language = configured_language >= 0 &&
      configured_language <= 6 ? (unsigned)configured_language
                               : system_firmware_language();
  const unsigned firmware_color = (unsigned)clamp_int(
      prefs_get_int("Drastic/FirmwareColor", 0), 0, 15);
  const unsigned firmware_month = (unsigned)clamp_int(
      prefs_get_int("Drastic/FirmwareBirthdayMonth", 6), 1, 12);
  const unsigned firmware_day = (unsigned)clamp_int(
      prefs_get_int("Drastic/FirmwareBirthdayDay", 6), 1, 31);
  config->firmware_userdata = firmware_language | (firmware_color << 8) |
                              (firmware_month << 16) | (firmware_day << 24);
  config->layout = read_layout();
  config->swap_screens = prefs_get_bool("Wrapper/SwapScreens", false);
  config->rotation = clamp_int(prefs_get_int("Wrapper/Rotation", 0), 0, 3);
  config->screen_gap = clamp_int(prefs_get_int("Wrapper/ScreenGap", 8), 0, 200);
  config->integer_scale = prefs_get_bool("Wrapper/IntegerScale", false);
  config->large_proportion = clamp_float(
      prefs_get_float("Wrapper/LargeScreenProportion", 4.0f), 1.0f, 16.0f);
  config->small_position =
      read_small_position("Wrapper/SmallScreenPosition", "middle_right");
  config->overlay_position =
      read_small_position("Wrapper/OverlayScreenPosition", "bottom_right");
  config->overlay_size =
      clamp_int(prefs_get_int("Wrapper/OverlayScreenSize", 25), 10, 60);
  config->overlay_opacity =
      clamp_int(prefs_get_int("Wrapper/OverlayScreenOpacity", 100), 10, 100);
  config->stretch_single = prefs_get_bool("Wrapper/StretchSingleScreen", false);
  config->padding[0][0] = clamp_int(prefs_get_int("Wrapper/TopPaddingX", 0), 0, 200);
  config->padding[0][1] = clamp_int(prefs_get_int("Wrapper/TopPaddingY", 0), 0, 200);
  config->padding[1][0] = clamp_int(prefs_get_int("Wrapper/BottomPaddingX", 0), 0, 200);
  config->padding[1][1] = clamp_int(prefs_get_int("Wrapper/BottomPaddingY", 0), 0, 200);
  config->background = (uint32_t)strtoul(
      prefs_get_string("Wrapper/BackgroundColor", "000000"), NULL, 16) & 0xffffffu;
  config->custom_aspect_lock =
      prefs_get_bool("Wrapper/CustomAspectLock", true);
  config->vulkan_low_latency =
      prefs_get_bool("Wrapper/VulkanLowLatency", false);
  config->video_filter = read_filter();
  config->fsr_sharpness =
      clamp_int(prefs_get_int("Wrapper/FsrSharpness", 40), 0, 100);
  snprintf(config->custom_shader, sizeof(config->custom_shader), "%s",
           prefs_get_string("Wrapper/CustomShader", ""));
  config->show_fps = prefs_get_bool("Drastic/ShowFPS", false);
  config->volume = clamp_int(prefs_get_int("Wrapper/Volume", 100), 0, 100);
  config->microphone_enabled =
      prefs_get_bool("Drastic/MicEnabled", true);
  config->microphone_source = read_microphone_source();
  config->autosave_seconds = clamp_int(
      prefs_get_int("Drastic/AutosaveInterval", 300), 0, 3600);
  config->vibration = prefs_get_bool("Wrapper/Vibration", true);
  config->motion = prefs_get_bool("Wrapper/Motion", true);
  config->lua_enabled = prefs_get_bool("Drastic/LuaEnabled", true);
  config->stylus_mode = read_stylus_mode();
  config->mouse_stylus = prefs_get_bool("Wrapper/MouseStylus", true);
  config->motion_stylus_sensitivity = clamp_int(
      prefs_get_int("Wrapper/MotionStylusSensitivity", 10), 1, 20);
  config->stylus_x = 128;
  config->stylus_y = 96;
  config->core_config = drastic_config_build_core_config();
  static const float defaults[2][4] = {
    {0.03f, 0.20666667f, 0.44f, 0.58666667f},
    {0.53f, 0.20666667f, 0.44f, 0.58666667f},
  };
  static const char *keys[2][4] = {
    {"Wrapper/CustomTopX", "Wrapper/CustomTopY",
     "Wrapper/CustomTopW", "Wrapper/CustomTopH"},
    {"Wrapper/CustomBottomX", "Wrapper/CustomBottomY",
     "Wrapper/CustomBottomW", "Wrapper/CustomBottomH"},
  };
  for (int screen = 0; screen < 2; screen++) {
    config->custom_screens[screen].x = clamp_float(
        prefs_get_float(keys[screen][0], defaults[screen][0]), 0.0f, 0.95f);
    config->custom_screens[screen].y = clamp_float(
        prefs_get_float(keys[screen][1], defaults[screen][1]), 0.0f, 0.95f);
    config->custom_screens[screen].width = clamp_float(
        prefs_get_float(keys[screen][2], defaults[screen][2]), 0.05f, 1.0f);
    config->custom_screens[screen].height = clamp_float(
        prefs_get_float(keys[screen][3], defaults[screen][3]), 0.05f, 1.0f);
    if (config->custom_screens[screen].x +
        config->custom_screens[screen].width > 1.0f)
      config->custom_screens[screen].x =
          1.0f - config->custom_screens[screen].width;
    if (config->custom_screens[screen].y +
        config->custom_screens[screen].height > 1.0f)
      config->custom_screens[screen].y =
          1.0f - config->custom_screens[screen].height;
    config->custom_screens[screen].screen = screen;
    config->custom_screens[screen].touch_target = screen == 1;
    config->custom_screens[screen].opacity = 1.0f;
  }
}

static void fit_size(float available_width, float available_height,
                     int integer_scale, int rotation,
                     float *width, float *height) {
  const float native_width = (rotation & 1) ? 192.0f : 256.0f;
  const float native_height = (rotation & 1) ? 256.0f : 192.0f;
  float scale = fminf(available_width / native_width,
                      available_height / native_height);
  if (integer_scale && scale >= 1.0f) scale = floorf(scale);
  if (scale <= 0.0f) scale = 1.0f;
  *width = native_width * scale;
  *height = native_height * scale;
}

static int remap_screen(const DrasticRuntimeConfig *config, int screen) {
  return config->swap_screens ? 1 - screen : screen;
}

static void enforce_custom_native_aspect(DrasticScreenRect *rect,
                                         int rotation,
                                         int canvas_width,
                                         int canvas_height) {
  if (!rect || canvas_width <= 0 || canvas_height <= 0) return;
  const float physical_aspect = (rotation & 1) ? 3.0f / 4.0f : 4.0f / 3.0f;
  const float normalized_aspect =
      physical_aspect * (float)canvas_height / (float)canvas_width;
  if (normalized_aspect <= 0.0f) return;

  const float center_x = rect->x + rect->width * 0.5f;
  const float center_y = rect->y + rect->height * 0.5f;
  const float height_from_width = rect->width / normalized_aspect;
  const float width_from_height = rect->height * normalized_aspect;
  const float width_cost = fabsf(height_from_width - rect->height) * canvas_height;
  const float height_cost = fabsf(width_from_height - rect->width) * canvas_width;
  float height = width_cost <= height_cost ? height_from_width : rect->height;
  const float minimum_height = fmaxf(0.08f, 0.08f / normalized_aspect);
  const float maximum_height = fminf(1.0f, 1.0f / normalized_aspect);
  height = clamp_float(height, minimum_height, maximum_height);
  rect->height = height;
  rect->width = height * normalized_aspect;
  rect->x = clamp_float(center_x - rect->width * 0.5f,
                        0.0f, 1.0f - rect->width);
  rect->y = clamp_float(center_y - rect->height * 0.5f,
                        0.0f, 1.0f - rect->height);
}

static void set_rect(DrasticRuntimeConfig *config, int index, int screen,
                     float x, float y, float width, float height) {
  config->screens[index].x = x;
  config->screens[index].y = y;
  config->screens[index].width = width;
  config->screens[index].height = height;
  config->screens[index].screen = remap_screen(config, screen);
  config->screens[index].touch_target =
      config->screens[index].screen == 1;
  config->screens[index].opacity = 1.0f;
}

/* Large screen: the large one fits beside (or above/below) its small copy,
 * proportion times smaller, and the small one lines up with its edge. */
static void layout_large(DrasticRuntimeConfig *config, int width, int height,
                         float gap) {
  const float native_width = (config->rotation & 1) ? 192.0f : 256.0f;
  const float native_height = (config->rotation & 1) ? 256.0f : 192.0f;
  const float small = 1.0f / config->large_proportion;
  const DrasticSmallScreenPosition position = config->small_position;
  const int stacked = position == DRASTIC_SMALL_ABOVE ||
                      position == DRASTIC_SMALL_BELOW;
  float scale = stacked
      ? fminf((float)width / native_width,
              ((float)height - gap) / (native_height * (1.0f + small)))
      : fminf(((float)width - gap) / (native_width * (1.0f + small)),
              (float)height / native_height);
  if (config->integer_scale && scale >= 1.0f) scale = floorf(scale);
  if (scale <= 0.0f) scale = 1.0f;
  const float large_w = native_width * scale, large_h = native_height * scale;
  const float small_w = large_w * small, small_h = large_h * small;
  float large_x, large_y, small_x, small_y;
  if (stacked) {
    const float total_h = large_h + gap + small_h;
    const float top = ((float)height - total_h) * 0.5f;
    large_x = ((float)width - large_w) * 0.5f;
    small_x = ((float)width - small_w) * 0.5f;
    if (position == DRASTIC_SMALL_ABOVE) {
      small_y = top;
      large_y = top + small_h + gap;
    } else {
      large_y = top;
      small_y = top + large_h + gap;
    }
  } else {
    const float total_w = large_w + gap + small_w;
    const float left = ((float)width - total_w) * 0.5f;
    const int on_left = position == DRASTIC_SMALL_TOP_LEFT ||
                        position == DRASTIC_SMALL_MIDDLE_LEFT ||
                        position == DRASTIC_SMALL_BOTTOM_LEFT;
    large_x = on_left ? left + small_w + gap : left;
    small_x = on_left ? left : left + large_w + gap;
    large_y = ((float)height - large_h) * 0.5f;
    if (position == DRASTIC_SMALL_TOP_RIGHT || position == DRASTIC_SMALL_TOP_LEFT)
      small_y = large_y;
    else if (position == DRASTIC_SMALL_BOTTOM_RIGHT ||
             position == DRASTIC_SMALL_BOTTOM_LEFT)
      small_y = large_y + large_h - small_h;
    else
      small_y = large_y + (large_h - small_h) * 0.5f;
  }
  set_rect(config, 0, 0, large_x, large_y, large_w, large_h);
  set_rect(config, 1, 1, small_x, small_y, small_w, small_h);
  config->screen_count = 2;
}

/* Screen overlay: the large screen fills the display and the small one is
 * drawn over its corner (or top/bottom centre), gap pixels in from its edge. */
static void layout_overlay(DrasticRuntimeConfig *config, int width,
                           int height, float gap) {
  float large_w, large_h;
  fit_size((float)width, (float)height, config->integer_scale,
           config->rotation, &large_w, &large_h);
  const float large_x = ((float)width - large_w) * 0.5f;
  const float large_y = ((float)height - large_h) * 0.5f;
  const float small_w = large_w * (float)config->overlay_size / 100.0f;
  const float small_h = small_w * large_h / large_w;
  const float left = large_x + gap;
  const float right = large_x + large_w - small_w - gap;
  const float centre_x = large_x + (large_w - small_w) * 0.5f;
  const float top = large_y + gap;
  const float bottom = large_y + large_h - small_h - gap;
  const float middle = large_y + (large_h - small_h) * 0.5f;
  float x = right, y = bottom;
  switch (config->overlay_position) {
    case DRASTIC_SMALL_TOP_RIGHT: x = right; y = top; break;
    case DRASTIC_SMALL_MIDDLE_RIGHT: x = right; y = middle; break;
    case DRASTIC_SMALL_BOTTOM_RIGHT: x = right; y = bottom; break;
    case DRASTIC_SMALL_TOP_LEFT: x = left; y = top; break;
    case DRASTIC_SMALL_MIDDLE_LEFT: x = left; y = middle; break;
    case DRASTIC_SMALL_BOTTOM_LEFT: x = left; y = bottom; break;
    case DRASTIC_SMALL_ABOVE: x = centre_x; y = top; break;
    case DRASTIC_SMALL_BELOW: x = centre_x; y = bottom; break;
  }
  set_rect(config, 0, 0, large_x, large_y, large_w, large_h);
  /* drawn second, so it lands on top */
  set_rect(config, 1, 1, x, y, small_w, small_h);
  config->screens[1].opacity = (float)config->overlay_opacity / 100.0f;
  config->screen_count = 2;
}

/* Shrinks each screen by its padding, keeping its aspect ratio (unless it
 * was stretched) and its centre. */
static void apply_padding(DrasticRuntimeConfig *config, int height,
                          int keep_aspect) {
  const float unit = (float)height / 720.0f;
  for (int index = 0; index < config->screen_count; index++) {
    DrasticScreenRect *rect = &config->screens[index];
    const int *padding = config->padding[rect->screen ? 1 : 0];
    const float pad_x = (float)padding[0] * unit;
    const float pad_y = (float)padding[1] * unit;
    if (pad_x <= 0.0f && pad_y <= 0.0f) continue;
    const float available_w = fmaxf(rect->width - pad_x * 2.0f, 8.0f);
    const float available_h = fmaxf(rect->height - pad_y * 2.0f, 8.0f);
    float w = available_w, h = available_h;
    if (keep_aspect) {
      const float scale = fminf(available_w / rect->width,
                                available_h / rect->height);
      w = rect->width * scale;
      h = rect->height * scale;
    }
    rect->x += (rect->width - w) * 0.5f;
    rect->y += (rect->height - h) * 0.5f;
    rect->width = w;
    rect->height = h;
  }
}

void drastic_config_calculate_layout(DrasticRuntimeConfig *config,
                                     int width, int height) {
  const float gap = (float)config->screen_gap;
  config->screen_count = 0;

  if (config->layout == DRASTIC_LAYOUT_CUSTOM) {
    for (int screen = 0; screen < 2; screen++) {
      DrasticScreenRect *custom = &config->custom_screens[screen];
      if (config->custom_aspect_lock)
        enforce_custom_native_aspect(custom, config->rotation, width, height);
      set_rect(config, screen, screen,
               custom->x * width, custom->y * height,
               custom->width * width, custom->height * height);
    }
    config->screen_count = 2;
    return;
  }

  if (config->layout == DRASTIC_LAYOUT_VERTICAL) {
    float w, h;
    fit_size((float)width, ((float)height - gap) * 0.5f,
             config->integer_scale, config->rotation, &w, &h);
    const float x = ((float)width - w) * 0.5f;
    const float y = ((float)height - (h * 2.0f + gap)) * 0.5f;
    set_rect(config, 0, 0, x, y, w, h);
    set_rect(config, 1, 1, x, y + h + gap, w, h);
    config->screen_count = 2;
  } else if (config->layout == DRASTIC_LAYOUT_HORIZONTAL) {
    float w, h;
    fit_size(((float)width - gap) * 0.5f, (float)height,
             config->integer_scale, config->rotation, &w, &h);
    const float x = ((float)width - (w * 2.0f + gap)) * 0.5f;
    const float y = ((float)height - h) * 0.5f;
    set_rect(config, 0, 0, x, y, w, h);
    set_rect(config, 1, 1, x + w + gap, y, w, h);
    config->screen_count = 2;
  } else if (config->layout == DRASTIC_LAYOUT_LARGE) {
    layout_large(config, width, height, gap);
  } else if (config->layout == DRASTIC_LAYOUT_OVERLAY) {
    layout_overlay(config, width, height, gap);
  } else if (config->layout == DRASTIC_LAYOUT_TOP_ONLY ||
             config->layout == DRASTIC_LAYOUT_BOTTOM_ONLY) {
    float w, h;
    fit_size((float)width, (float)height, config->integer_scale,
             config->rotation, &w, &h);
    if (config->stretch_single) {
      w = (float)width;
      h = (float)height;
    }
    const int screen = config->layout == DRASTIC_LAYOUT_TOP_ONLY ? 0 : 1;
    set_rect(config, 0, screen, ((float)width - w) * 0.5f,
             ((float)height - h) * 0.5f, w, h);
    config->screen_count = 1;
  } else {
    const int primary = config->layout == DRASTIC_LAYOUT_HYBRID_TOP ? 0 : 1;
    const float side_width = fminf((float)width * 0.27f, 384.0f);
    float small_w, small_h;
    fit_size(side_width, ((float)height - gap) * 0.5f,
             config->integer_scale, config->rotation, &small_w, &small_h);
    float large_w, large_h;
    fit_size((float)width - small_w - gap, (float)height,
             config->integer_scale, config->rotation, &large_w, &large_h);
    const float total_w = large_w + gap + small_w;
    const float x = ((float)width - total_w) * 0.5f;
    set_rect(config, 0, primary, x, ((float)height - large_h) * 0.5f,
             large_w, large_h);
    const float small_y = ((float)height - (small_h * 2.0f + gap)) * 0.5f;
    set_rect(config, 1, 0, x + large_w + gap, small_y, small_w, small_h);
    set_rect(config, 2, 1, x + large_w + gap, small_y + small_h + gap,
             small_w, small_h);
    config->screen_count = 3;
  }
  apply_padding(config, height,
                !(config->stretch_single &&
                  (config->layout == DRASTIC_LAYOUT_TOP_ONLY ||
                   config->layout == DRASTIC_LAYOUT_BOTTOM_ONLY)));
}

bool drastic_config_map_touch_rects(const DrasticScreenRect *screens,
                                    int screen_count, int rotation,
                                    float panel_x, float panel_y,
                                    int *ds_x, int *ds_y) {
  if (!screens || screen_count <= 0) return false;
  /* Prefer the largest bottom-screen rectangle in hybrid modes. */
  const DrasticScreenRect *target = NULL;
  for (int index = 0; index < screen_count; index++) {
    const DrasticScreenRect *rect = &screens[index];
    if (!rect->touch_target || rect->width <= 0.0f ||
        rect->height <= 0.0f || panel_x < rect->x || panel_y < rect->y ||
        panel_x >= rect->x + rect->width ||
        panel_y >= rect->y + rect->height)
      continue;
    if (!target || rect->width * rect->height > target->width * target->height)
      target = rect;
  }
  if (!target) return false;
  const float display_u = (panel_x - target->x) / target->width;
  const float display_v = (panel_y - target->y) / target->height;
  float source_u, source_v;
  drastic_rotation_display_to_source(rotation, display_u, display_v,
                                     &source_u, &source_v);
  if (ds_x) *ds_x = clamp_int((int)(source_u * 256.0f), 0, 255);
  if (ds_y) *ds_y = clamp_int((int)(source_v * 192.0f), 0, 191);
  return true;
}

bool drastic_config_map_touch(const DrasticRuntimeConfig *config,
                              float panel_x, float panel_y,
                              int *ds_x, int *ds_y) {
  if (!config) return false;
  return drastic_config_map_touch_rects(
      config->screens, config->screen_count, config->rotation,
      panel_x, panel_y, ds_x, ds_y);
}

bool drastic_config_map_stylus(const DrasticRuntimeConfig *config,
                               int ds_x, int ds_y,
                               float *panel_x, float *panel_y) {
  const DrasticScreenRect *target = NULL;
  for (int index = 0; index < config->screen_count; index++) {
    const DrasticScreenRect *rect = &config->screens[index];
    if (!rect->touch_target) continue;
    if (!target || rect->width * rect->height > target->width * target->height)
      target = rect;
  }
  if (!target) return false;
  float source_u = (float)clamp_int(ds_x, 0, 255) / 255.0f;
  float source_v = (float)clamp_int(ds_y, 0, 191) / 191.0f;
  float display_u, display_v;
  drastic_rotation_source_to_display(config->rotation, source_u, source_v,
                                     &display_u, &display_v);
  if (panel_x) *panel_x = target->x + display_u * target->width;
  if (panel_y) *panel_y = target->y + display_v * target->height;
  return true;
}
