#ifndef DRASTIC_NX_DFX_H
#define DRASTIC_NX_DFX_H

#include <stdint.h>

#include "drastic_config.h"

typedef enum {
  DRASTIC_DFX_COPY,
  DRASTIC_DFX_QUILEZ,
  DRASTIC_DFX_SCANLINE,
  DRASTIC_DFX_SCALE2X,
  DRASTIC_DFX_HQ2X,
  DRASTIC_DFX_FXAA,
  DRASTIC_DFX_FXAA_LUMA,
  DRASTIC_DFX_FXAA_HQ,
  DRASTIC_DFX_SMAA_EDGE,
  DRASTIC_DFX_SMAA_WEIGHT,
  DRASTIC_DFX_SMAA_BLEND,
  /* FSR 1.0 spatial upscaling, drawn straight from the screen texture */
  DRASTIC_DFX_FSR,
  DRASTIC_DFX_SHADER_COUNT,
} DrasticDfxShader;

typedef enum {
  DRASTIC_DFX_SOURCE,
  DRASTIC_DFX_WORK_A,
  DRASTIC_DFX_WORK_B,
  DRASTIC_DFX_WORK_2X,
  DRASTIC_DFX_AREA,
  DRASTIC_DFX_SEARCH,
} DrasticDfxTextureRole;

typedef enum {
  DRASTIC_DFX_NEAREST,
  DRASTIC_DFX_LINEAR,
} DrasticDfxSampler;

typedef struct {
  DrasticDfxTextureRole texture;
  DrasticDfxSampler sampler;
} DrasticDfxInput;

typedef struct {
  DrasticDfxShader shader;
  uint8_t input_count;
  DrasticDfxInput inputs[3];
  DrasticDfxTextureRole output;
  uint8_t output_scale;
} DrasticDfxPass;

typedef struct {
  uint8_t pass_count;
  DrasticDfxPass passes[3];
  DrasticDfxShader final_shader;
  DrasticDfxTextureRole final_texture;
  DrasticDfxSampler final_sampler;
} DrasticDfxChain;

#define DFX_INPUT(role, filtering) { (role), (filtering) }
#define DFX_PASS(shader_id, count, first, second, third, target, scale) \
  { (shader_id), (count), { first, second, third }, (target), (scale) }

static inline const DrasticDfxChain *drastic_dfx_chain(
    DrasticVideoFilter filter) {
  static const DrasticDfxChain chains[DRASTIC_FILTER_COUNT] = {
    [DRASTIC_FILTER_NEAREST] = {
      .final_shader = DRASTIC_DFX_COPY,
      .final_texture = DRASTIC_DFX_SOURCE,
      .final_sampler = DRASTIC_DFX_NEAREST,
    },
    [DRASTIC_FILTER_LINEAR] = {
      .final_shader = DRASTIC_DFX_COPY,
      .final_texture = DRASTIC_DFX_SOURCE,
      .final_sampler = DRASTIC_DFX_LINEAR,
    },
    [DRASTIC_FILTER_QUILEZ] = {
      .final_shader = DRASTIC_DFX_QUILEZ,
      .final_texture = DRASTIC_DFX_SOURCE,
      .final_sampler = DRASTIC_DFX_LINEAR,
    },
    [DRASTIC_FILTER_SCANLINE] = {
      .final_shader = DRASTIC_DFX_SCANLINE,
      .final_texture = DRASTIC_DFX_SOURCE,
      .final_sampler = DRASTIC_DFX_LINEAR,
    },
    [DRASTIC_FILTER_SCALE2X] = {
      .pass_count = 1,
      .passes = { DFX_PASS(
          DRASTIC_DFX_SCALE2X, 1,
          DFX_INPUT(DRASTIC_DFX_SOURCE, DRASTIC_DFX_NEAREST),
          DFX_INPUT(DRASTIC_DFX_SOURCE, DRASTIC_DFX_NEAREST),
          DFX_INPUT(DRASTIC_DFX_SOURCE, DRASTIC_DFX_NEAREST),
          DRASTIC_DFX_WORK_2X, 2) },
      .final_shader = DRASTIC_DFX_COPY,
      .final_texture = DRASTIC_DFX_WORK_2X,
      .final_sampler = DRASTIC_DFX_LINEAR,
    },
    [DRASTIC_FILTER_HQ2X] = {
      .pass_count = 1,
      .passes = { DFX_PASS(
          DRASTIC_DFX_HQ2X, 1,
          DFX_INPUT(DRASTIC_DFX_SOURCE, DRASTIC_DFX_NEAREST),
          DFX_INPUT(DRASTIC_DFX_SOURCE, DRASTIC_DFX_NEAREST),
          DFX_INPUT(DRASTIC_DFX_SOURCE, DRASTIC_DFX_NEAREST),
          DRASTIC_DFX_WORK_2X, 2) },
      .final_shader = DRASTIC_DFX_COPY,
      .final_texture = DRASTIC_DFX_WORK_2X,
      .final_sampler = DRASTIC_DFX_LINEAR,
    },
    [DRASTIC_FILTER_FXAA] = {
      .pass_count = 1,
      .passes = { DFX_PASS(
          DRASTIC_DFX_FXAA, 1,
          DFX_INPUT(DRASTIC_DFX_SOURCE, DRASTIC_DFX_LINEAR),
          DFX_INPUT(DRASTIC_DFX_SOURCE, DRASTIC_DFX_LINEAR),
          DFX_INPUT(DRASTIC_DFX_SOURCE, DRASTIC_DFX_LINEAR),
          DRASTIC_DFX_WORK_A, 1) },
      .final_shader = DRASTIC_DFX_QUILEZ,
      .final_texture = DRASTIC_DFX_WORK_A,
      .final_sampler = DRASTIC_DFX_LINEAR,
    },
    [DRASTIC_FILTER_FXAA_HQ] = {
      .pass_count = 2,
      .passes = {
        DFX_PASS(
            DRASTIC_DFX_FXAA_LUMA, 1,
            DFX_INPUT(DRASTIC_DFX_SOURCE, DRASTIC_DFX_LINEAR),
            DFX_INPUT(DRASTIC_DFX_SOURCE, DRASTIC_DFX_LINEAR),
            DFX_INPUT(DRASTIC_DFX_SOURCE, DRASTIC_DFX_LINEAR),
            DRASTIC_DFX_WORK_A, 1),
        DFX_PASS(
            DRASTIC_DFX_FXAA_HQ, 1,
            DFX_INPUT(DRASTIC_DFX_WORK_A, DRASTIC_DFX_LINEAR),
            DFX_INPUT(DRASTIC_DFX_WORK_A, DRASTIC_DFX_LINEAR),
            DFX_INPUT(DRASTIC_DFX_WORK_A, DRASTIC_DFX_LINEAR),
            DRASTIC_DFX_WORK_B, 1),
      },
      .final_shader = DRASTIC_DFX_QUILEZ,
      .final_texture = DRASTIC_DFX_WORK_B,
      .final_sampler = DRASTIC_DFX_LINEAR,
    },
    [DRASTIC_FILTER_SMAA] = {
      .pass_count = 3,
      .passes = {
        DFX_PASS(
            DRASTIC_DFX_SMAA_EDGE, 1,
            DFX_INPUT(DRASTIC_DFX_SOURCE, DRASTIC_DFX_LINEAR),
            DFX_INPUT(DRASTIC_DFX_SOURCE, DRASTIC_DFX_LINEAR),
            DFX_INPUT(DRASTIC_DFX_SOURCE, DRASTIC_DFX_LINEAR),
            DRASTIC_DFX_WORK_A, 1),
        DFX_PASS(
            DRASTIC_DFX_SMAA_WEIGHT, 3,
            DFX_INPUT(DRASTIC_DFX_WORK_A, DRASTIC_DFX_LINEAR),
            DFX_INPUT(DRASTIC_DFX_AREA, DRASTIC_DFX_LINEAR),
            DFX_INPUT(DRASTIC_DFX_SEARCH, DRASTIC_DFX_LINEAR),
            DRASTIC_DFX_WORK_B, 1),
        DFX_PASS(
            DRASTIC_DFX_SMAA_BLEND, 2,
            DFX_INPUT(DRASTIC_DFX_SOURCE, DRASTIC_DFX_LINEAR),
            DFX_INPUT(DRASTIC_DFX_WORK_B, DRASTIC_DFX_LINEAR),
            DFX_INPUT(DRASTIC_DFX_SOURCE, DRASTIC_DFX_LINEAR),
            DRASTIC_DFX_WORK_A, 1),
      },
      .final_shader = DRASTIC_DFX_QUILEZ,
      .final_texture = DRASTIC_DFX_WORK_A,
      .final_sampler = DRASTIC_DFX_LINEAR,
    },
    [DRASTIC_FILTER_FSR] = {
      .final_shader = DRASTIC_DFX_FSR,
      .final_texture = DRASTIC_DFX_SOURCE,
      .final_sampler = DRASTIC_DFX_NEAREST,
    },
  };
  if ((unsigned)filter >= DRASTIC_FILTER_COUNT)
    filter = DRASTIC_FILTER_NEAREST;
  return &chains[filter];
}

#undef DFX_INPUT
#undef DFX_PASS

#endif
