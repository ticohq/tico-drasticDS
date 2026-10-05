#ifndef DRASTIC_NX_INPUT_SAMPLER_H
#define DRASTIC_NX_INPUT_SAMPLER_H

#include <stdbool.h>
#include <stdint.h>
#include <switch.h>

#include "drastic_config.h"

#define DRASTIC_INPUT_MAX_BINDINGS 16

typedef enum {
  DRASTIC_INPUT_HOTKEY_MENU,
  DRASTIC_INPUT_HOTKEY_FAST_FORWARD,
  DRASTIC_INPUT_HOTKEY_SWAP_SCREENS,
  DRASTIC_INPUT_HOTKEY_MICROPHONE,
  DRASTIC_INPUT_HOTKEY_MOTION_STYLUS_RECENTER,
  DRASTIC_INPUT_HOTKEY_AUTOFIRE,
  DRASTIC_INPUT_HOTKEY_LID,
  DRASTIC_INPUT_HOTKEY_SAVE_STATE,
  DRASTIC_INPUT_HOTKEY_LOAD_STATE,
  DRASTIC_INPUT_HOTKEY_NEXT_SLOT,
  DRASTIC_INPUT_HOTKEY_PREVIOUS_SLOT,
  DRASTIC_INPUT_HOTKEY_RESET,
  DRASTIC_INPUT_HOTKEY_QUIT,
  DRASTIC_INPUT_HOTKEY_CYCLE_LAYOUT,
  DRASTIC_INPUT_HOTKEY_COUNT,
} DrasticInputHotkey;

#define DRASTIC_INPUT_HOTKEY_BIT(hotkey) (UINT32_C(1) << (hotkey))

typedef struct {
  u64 switch_mask;
  int ds_mask;
} DrasticInputBinding;

typedef void (*DrasticInputUpdateCallback)(void *user, int buttons,
                                           int touch_position, int autofire);

typedef struct {
  DrasticInputUpdateCallback update;
  void *user;
  DrasticInputBinding bindings[DRASTIC_INPUT_MAX_BINDINGS];
  int binding_count;
  int analog_dpad;
  int analog_deadzone;
  u64 hotkeys[DRASTIC_INPUT_HOTKEY_COUNT];
  u64 analog_touch_button;
  int stylus_speed;
  int panel_width;
  int panel_height;
} DrasticInputSamplerConfig;

typedef struct {
  u64 buttons;
  u64 buttons_down;
  HidAnalogStickState left;
  HidAnalogStickState right;
  u32 style_set;
  u32 attributes;
  uint32_t hotkeys_pressed;
  uint64_t motion_sample;
  HidVector motion_acceleration;
  HidVector motion_angular_velocity;
  int motion_source;
  int motion_right_joycon;
  int stylus_x;
  int stylus_y;
  int stylus_visible;
} DrasticInputSnapshot;

typedef struct DrasticInputSampler DrasticInputSampler;

DrasticInputSampler *drastic_input_sampler_create(
    const DrasticInputSamplerConfig *config);
void drastic_input_sampler_update_runtime(
    DrasticInputSampler *sampler, const DrasticRuntimeConfig *config,
    bool gameplay_enabled);
void drastic_input_sampler_read(DrasticInputSampler *sampler,
                                DrasticInputSnapshot *snapshot);
void drastic_input_sampler_destroy(DrasticInputSampler *sampler);

#endif
