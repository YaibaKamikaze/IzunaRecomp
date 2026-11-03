#version 450
// [IzunaRecomp] The present pass: the native frame through the game's
// 256-entry gamma ramp (as the display hardware applies it: each 8-bit
// channel looks up a 10-bit value; entries are red << 20 | green << 10 | blue),
// with the player's anti-aliasing first (mode 1: FXAA, 2: FXAA extreme - the
// SDK's swap FXAA, from Xenia; luma from the frame itself).
layout(set = 0, binding = 0) uniform sampler2D frame;
layout(set = 0, binding = 1) readonly buffer Ramp { uint entries[256]; } ramp;
layout(push_constant) uniform Present { uint mode; } pc;
layout(location = 0) out vec4 color;

const vec3 kLumaWeights = vec3(0.299, 0.587, 0.114);

float Luma(vec2 uv) { return dot(textureLod(frame, uv, 0.0).rgb, kLumaWeights); }

vec3 Fxaa(vec2 uv, vec2 texel, bool extreme) {
  const float edge_threshold = extreme ? 0.063 : 0.166;
  const float edge_threshold_min = extreme ? 0.0312 : 0.0833;
  const float span_max = extreme ? 12.0 : 8.0;
  const float dir_reduce_mul = 0.125, dir_reduce_min = 1.0 / 128.0;
  const vec3 rgbm = textureLod(frame, uv, 0.0).rgb;
  const float lumaM = dot(rgbm, kLumaWeights);
  const float lumaNW = Luma(uv + vec2(-texel.x, -texel.y));
  const float lumaNE = Luma(uv + vec2(texel.x, -texel.y));
  const float lumaSW = Luma(uv + vec2(-texel.x, texel.y));
  const float lumaSE = Luma(uv + vec2(texel.x, texel.y));
  const float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
  const float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
  if (lumaMax - lumaMin < max(edge_threshold_min, lumaMax * edge_threshold)) return rgbm;
  vec2 dir = vec2(-((lumaNW + lumaNE) - (lumaSW + lumaSE)), (lumaNW + lumaSW) - (lumaNE + lumaSE));
  const float dir_reduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * dir_reduce_mul), dir_reduce_min);
  dir = clamp(dir / (min(abs(dir.x), abs(dir.y)) + dir_reduce), vec2(-span_max), vec2(span_max)) * texel;
  const vec3 rgbA = 0.5 * (textureLod(frame, uv + dir * (1.0 / 3.0 - 0.5), 0.0).rgb +
                           textureLod(frame, uv + dir * (2.0 / 3.0 - 0.5), 0.0).rgb);
  const vec3 rgbB = rgbA * 0.5 + 0.25 * (textureLod(frame, uv + dir * -0.5, 0.0).rgb +
                                         textureLod(frame, uv + dir * 0.5, 0.0).rgb);
  const float lumaB = dot(rgbB, kLumaWeights);
  return (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;
}

void main() {
  vec3 rgb;
  if (pc.mode == 0u) {
    rgb = texelFetch(frame, ivec2(gl_FragCoord.xy), 0).rgb;
  } else {
    const vec2 texel = 1.0 / vec2(textureSize(frame, 0));
    rgb = Fxaa(gl_FragCoord.xy * texel, texel, pc.mode == 2u);
  }
  const uvec3 c = uvec3(clamp(rgb, 0.0, 1.0) * 255.0 + 0.5);
  color = vec4(float((ramp.entries[c.r] >> 20) & 1023u), float((ramp.entries[c.g] >> 10) & 1023u),
               float(ramp.entries[c.b] & 1023u), 1023.0) / 1023.0;
}
