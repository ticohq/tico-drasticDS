#version 450

layout(set = 0, binding = 0) uniform sampler2D source_texture;

layout(push_constant) uniform DrawParameters {
    int effect;
    int mode;
    vec2 texture_size;
    vec2 target_size;
    vec2 padding;
    vec4 color;
} parameters;

layout(location = 0) in vec2 texcoord;
layout(location = 0) out vec4 output_color;

ivec2 clamped_coordinate(ivec2 coordinate) {
    return clamp(coordinate, ivec2(0), ivec2(parameters.texture_size) - 1);
}

vec4 fetch_pixel(ivec2 coordinate) {
    return texelFetch(source_texture, clamped_coordinate(coordinate), 0);
}

vec4 nearest_pixel(vec2 coordinate) {
    return fetch_pixel(ivec2(floor(coordinate * parameters.texture_size)));
}

vec4 quilez_pixel(vec2 coordinate) {
    vec2 position = coordinate * parameters.texture_size + 0.5;
    vec2 base = floor(position);
    vec2 fraction = fract(position);
    fraction = fraction * fraction * fraction *
               (fraction * (fraction * 6.0 - 15.0) + 10.0);
    return texture(source_texture,
                   (base + fraction - 0.5) / parameters.texture_size);
}

vec2 scanline_resolution;

float to_linear_component(float component) {
    return component <= 0.04045 ? component / 12.92
        : pow((component + 0.055) / 1.055, 2.4);
}

vec3 to_linear(vec3 color) {
    return vec3(to_linear_component(color.r), to_linear_component(color.g),
                to_linear_component(color.b));
}

float to_srgb_component(float component) {
    return component < 0.0031308 ? component * 12.92
        : 1.055 * pow(component, 0.41666) - 0.055;
}

vec3 to_srgb(vec3 color) {
    return vec3(to_srgb_component(color.r), to_srgb_component(color.g),
                to_srgb_component(color.b));
}

vec3 scanline_fetch(vec2 position, vec2 offset) {
    position = floor(position * scanline_resolution + offset) /
               scanline_resolution;
    if (max(abs(position.x - 0.5), abs(position.y - 0.5)) > 0.5)
        return vec3(0.0);
    return to_linear(texture(source_texture, position, -16.0).rgb);
}

vec2 scanline_distance(vec2 position) {
    position *= scanline_resolution;
    return -((position - floor(position)) - vec2(0.5));
}

float gaussian(float position, float scale) {
    return exp2(scale * position * position);
}

vec3 horizontal_three(vec2 position, float offset) {
    vec3 b = scanline_fetch(position, vec2(-1.0, offset));
    vec3 c = scanline_fetch(position, vec2( 0.0, offset));
    vec3 d = scanline_fetch(position, vec2( 1.0, offset));
    float distance = scanline_distance(position).x;
    float wb = gaussian(distance - 1.0, -3.0);
    float wc = gaussian(distance,       -3.0);
    float wd = gaussian(distance + 1.0, -3.0);
    return (b * wb + c * wc + d * wd) / (wb + wc + wd);
}

vec3 horizontal_five(vec2 position, float offset) {
    vec3 a = scanline_fetch(position, vec2(-2.0, offset));
    vec3 b = scanline_fetch(position, vec2(-1.0, offset));
    vec3 c = scanline_fetch(position, vec2( 0.0, offset));
    vec3 d = scanline_fetch(position, vec2( 1.0, offset));
    vec3 e = scanline_fetch(position, vec2( 2.0, offset));
    float distance = scanline_distance(position).x;
    float wa = gaussian(distance - 2.0, -3.0);
    float wb = gaussian(distance - 1.0, -3.0);
    float wc = gaussian(distance,       -3.0);
    float wd = gaussian(distance + 1.0, -3.0);
    float we = gaussian(distance + 2.0, -3.0);
    return (a * wa + b * wb + c * wc + d * wd + e * we) /
           (wa + wb + wc + wd + we);
}

float scan_weight(vec2 position, float offset) {
    return gaussian(scanline_distance(position).y + offset, -8.0);
}

vec3 scanline_tri(vec2 position) {
    vec3 a = horizontal_three(position, -1.0);
    vec3 b = horizontal_five(position, 0.0);
    vec3 c = horizontal_three(position, 1.0);
    return a * scan_weight(position, -1.0) +
           b * scan_weight(position,  0.0) +
           c * scan_weight(position,  1.0);
}

vec3 shadow_mask(vec2 position) {
    position.x += position.y * 3.0;
    vec3 mask = vec3(0.5);
    position.x = fract(position.x / 6.0);
    if (position.x < 0.333) mask.r = 1.5;
    else if (position.x < 0.666) mask.g = 1.5;
    else mask.b = 1.5;
    return mask;
}

vec4 scanline_pixel(vec2 coordinate) {
    scanline_resolution = parameters.target_size;
    vec2 screen_coordinate = coordinate * parameters.target_size;
    vec3 color = scanline_tri(coordinate) * shadow_mask(screen_coordinate);
    return vec4(to_srgb(color), 1.0);
}

// FSR 1.0 style spatial upscaling, after Autorun's blit_upscale.comp: an
// edge-adaptive Lanczos-like reconstruction of the four nearest texels, then
// contrast-adaptive sharpening clamped to the local range so it cannot ring.
// padding.x carries the sharpness, 0..1.
float fsr_luma(vec3 color) {
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

float fsr_kernel(float x) {
    x = clamp(abs(x), 0.0, 2.0);
    float x2 = x * x;
    return (1.0 - 0.4 * x2) * (x2 - 1.0) * (x2 - 1.0);
}

vec3 fsr_fetch(ivec2 coordinate) {
    return fetch_pixel(coordinate).rgb;
}

vec4 fsr_pixel(vec2 coordinate) {
    vec2 position = coordinate * parameters.texture_size - 0.5;
    ivec2 base = ivec2(floor(position));
    vec2 f = position - vec2(base);

    vec3 s00 = fsr_fetch(base);
    vec3 s10 = fsr_fetch(base + ivec2(1, 0));
    vec3 s01 = fsr_fetch(base + ivec2(0, 1));
    vec3 s11 = fsr_fetch(base + ivec2(1, 1));
    vec3 s_top = fsr_fetch(base + ivec2(0, -1));
    vec3 s_left = fsr_fetch(base + ivec2(-1, 0));
    vec3 s_right = fsr_fetch(base + ivec2(2, 0));
    vec3 s_bottom = fsr_fetch(base + ivec2(0, 2));

    float l00 = fsr_luma(s00);
    float l10 = fsr_luma(s10);
    float l01 = fsr_luma(s01);
    float l11 = fsr_luma(s11);
    vec2 direction = vec2((l10 - l00) + (l11 - l01), (l01 - l00) + (l11 - l10));
    float length_ = length(direction);
    vec2 edge = length_ > 0.001 ? direction / length_ : vec2(0.0);

    float wx0 = fsr_kernel(f.x);
    float wx1 = fsr_kernel(1.0 - f.x);
    float wy0 = fsr_kernel(f.y);
    float wy1 = fsr_kernel(1.0 - f.y);
    float stretch = clamp(length_ * 2.0, 0.0, 0.5);
    float w00 = max(wx0 * wy0 + stretch * dot(edge, vec2(-1.0, -1.0)), 0.0001);
    float w10 = max(wx1 * wy0 + stretch * dot(edge, vec2( 1.0, -1.0)), 0.0001);
    float w01 = max(wx0 * wy1 + stretch * dot(edge, vec2(-1.0,  1.0)), 0.0001);
    float w11 = max(wx1 * wy1 + stretch * dot(edge, vec2( 1.0,  1.0)), 0.0001);
    vec3 color = (s00 * w00 + s10 * w10 + s01 * w01 + s11 * w11) /
                 (w00 + w10 + w01 + w11);

    float sharpness = parameters.padding.x;
    if (sharpness > 0.01) {
        vec3 low = min(min(min(s00, s10), min(s01, s11)),
                       min(min(s_top, s_bottom), min(s_left, s_right)));
        vec3 high = max(max(max(s00, s10), max(s01, s11)),
                        max(max(s_top, s_bottom), max(s_left, s_right)));
        vec3 average = (s00 + s10 + s01 + s11) * 0.25;
        color = clamp(color + (color - average) * (sharpness * 1.5), low, high);
    }
    return vec4(color, 1.0);
}

void main() {
    if (parameters.mode == 1) {
        output_color = parameters.color;
        return;
    }

    vec4 color;
    if (parameters.effect == 1)
        color = texture(source_texture, texcoord);
    else if (parameters.effect == 2)
        color = quilez_pixel(texcoord);
    else if (parameters.effect == 3)
        color = scanline_pixel(texcoord);
    else if (parameters.effect == 4)
        color = fsr_pixel(texcoord);
    else
        color = nearest_pixel(texcoord);
    output_color = color * parameters.color;
    // screens are opaque, at the opacity the layout gives them
    if (parameters.mode == 2)
        output_color.a = parameters.color.a;
}
