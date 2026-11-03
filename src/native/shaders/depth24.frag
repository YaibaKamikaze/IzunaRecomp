#version 450
// [native renderer] Depth-only draws on a 20e4 (float 24) depth target: no
// pixel shader of the game's, so this one takes the fragment's depth to the
// 20e4 precision the hardware stores, truncated (ng2DepthTo24 in the
// XenosRecomp fork, the SDK's Float32To20e4 / Float20e4To32 round trip) - a
// depth pre-pass and the colour pass after it then compare as on the Xbox.
layout(depth_less) out float gl_FragDepth;
void main() {
  float z = clamp(gl_FragCoord.z, 0.0, 1.0);
  uint u = floatBitsToUint(z);
  gl_FragDepth = u >= 0x38800000u ? uintBitsToFloat(u & ~7u) : floor(z * 17179869184.0) * (1.0 / 17179869184.0);
}
