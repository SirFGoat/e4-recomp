// Bloom post-process for the swap chain output (E4 recomp addition).
// Entry points (compile one variant each):
//   bloom_down_main      box-downsample source -> dest (BLOOM_BRIGHT=1 adds a soft-knee bright pass)
//   bloom_blur_main      9-fetch / 17-texel separable gaussian along xe_bloom_direction
//   bloom_composite_main screen-blend source + level1 * i1 + level2 * i2 into dest

cbuffer XeBloomConstants : register(b0) {
  uint2 xe_bloom_dest_size;
  float2 xe_bloom_src_texel;  // 1 / source size
  float2 xe_bloom_direction;  // blur step in source texels
  float xe_bloom_threshold;
  float xe_bloom_knee;
  float xe_bloom_intensity1;
  float xe_bloom_intensity2;
  uint xe_bloom_taps;  // downsample samples per axis
};

SamplerState xe_bloom_sampler : register(s0);
Texture2D<float4> xe_bloom_source : register(t0);
Texture2D<float4> xe_bloom_source1 : register(t1);
Texture2D<float4> xe_bloom_source2 : register(t2);
RWTexture2D<float4> xe_bloom_dest : register(u0);

#ifndef BLOOM_BRIGHT
#define BLOOM_BRIGHT 0
#endif

float3 XeBloomBrightPass(float3 c) {
  float l = max(c.r, max(c.g, c.b));
  float knee = max(xe_bloom_knee, 1e-4);
  float soft = clamp(l - xe_bloom_threshold + knee, 0.0, 2.0 * knee);
  soft = soft * soft / (4.0 * knee);
  float contrib = max(soft, l - xe_bloom_threshold) / max(l, 1e-4);
  return c * contrib;
}

[numthreads(8, 8, 1)]
void bloom_down_main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= xe_bloom_dest_size)) {
    return;
  }
  float2 uv0 = float2(id.xy) / float2(xe_bloom_dest_size);
  float2 uv1 = float2(id.xy + 1) / float2(xe_bloom_dest_size);
  uint n = max(xe_bloom_taps, 1u);
  float3 sum = 0.0;
  for (uint y = 0; y < n; ++y) {
    for (uint x = 0; x < n; ++x) {
      float2 uv = lerp(uv0, uv1, (float2(x, y) + 0.5) / float(n));
      sum += xe_bloom_source.SampleLevel(xe_bloom_sampler, uv, 0.0).rgb;
    }
  }
  sum /= float(n * n);
#if BLOOM_BRIGHT
  sum = XeBloomBrightPass(sum);
#endif
  xe_bloom_dest[id.xy] = float4(sum, 1.0);
}

[numthreads(8, 8, 1)]
void bloom_blur_main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= xe_bloom_dest_size)) {
    return;
  }
  static const float kOffsets[5] = {0.0, 1.41176471, 3.29411765, 5.17647059, 7.05882353};
  static const float kWeights[5] = {0.19648255, 0.29690696, 0.09447040, 0.01038136, 0.00050691};
  float2 uv = (float2(id.xy) + 0.5) / float2(xe_bloom_dest_size);
  float2 step = xe_bloom_direction * xe_bloom_src_texel;
  float3 sum = xe_bloom_source.SampleLevel(xe_bloom_sampler, uv, 0.0).rgb * kWeights[0];
  [unroll] for (int i = 1; i < 5; ++i) {
    float2 d = step * kOffsets[i];
    sum += (xe_bloom_source.SampleLevel(xe_bloom_sampler, uv + d, 0.0).rgb +
            xe_bloom_source.SampleLevel(xe_bloom_sampler, uv - d, 0.0).rgb) *
           kWeights[i];
  }
  xe_bloom_dest[id.xy] = float4(sum, 1.0);
}

[numthreads(8, 8, 1)]
void bloom_composite_main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= xe_bloom_dest_size)) {
    return;
  }
  float2 uv = (float2(id.xy) + 0.5) / float2(xe_bloom_dest_size);
  float4 base = xe_bloom_source.Load(int3(id.xy, 0));
  float3 glow = xe_bloom_source1.SampleLevel(xe_bloom_sampler, uv, 0.0).rgb * xe_bloom_intensity1 +
                xe_bloom_source2.SampleLevel(xe_bloom_sampler, uv, 0.0).rgb * xe_bloom_intensity2;
  // Screen blend: bright areas bleed light without clipping everything to white.
  float3 c = 1.0 - (1.0 - saturate(base.rgb)) * (1.0 - saturate(glow));
  xe_bloom_dest[id.xy] = float4(c, base.a);
}
