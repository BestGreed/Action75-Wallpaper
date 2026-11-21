struct V { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
V vs(uint id:SV_VertexID) { V v; float2 uv=float2((id<<1)&2,id&2); v.position=float4(uv*float2(2,-2)+float2(-1,1),0,1); v.uv=uv; return v; }
Texture2D source:register(t0); SamplerState linearSampler:register(s0);
cbuffer BackgroundAppearance:register(b0) { float4 appearance; float4 backgroundTint; }
float3 gradeBackground(float3 color) {
  // Branching on a uniform keeps the neutral sample bit-for-bit unchanged.
  // These textures/targets use UNORM, so grade in visible sRGB code values.
  if (appearance.y!=1.0 || appearance.z!=0.0) {
    float highest=max(color.r,max(color.g,color.b));
    float lowest=min(color.r,min(color.g,color.b));
    float chroma=highest-lowest;
    if (chroma>0.0) {
      float hue;
      if (highest==color.r) hue=(color.g-color.b)/chroma;
      else if (highest==color.g) hue=(color.b-color.r)/chroma+2.0;
      else hue=(color.r-color.g)/chroma+4.0;
      hue=frac(hue/6.0+appearance.z);
      float saturation=saturate((chroma/highest)*appearance.y);
      float3 pure=saturate(abs(frac(hue+float3(0.0,0.666666667,0.333333333))*6.0-3.0)-1.0);
      color=highest*lerp(float3(1.0,1.0,1.0),pure,saturation);
    }
  }
  if (appearance.x<=1.0) return color*appearance.x;
  // A bounded shoulder brightens shadows without clipping color channels.
  // White remains white; a black input stays black even at maximum gain.
  float peak=max(color.r,max(color.g,color.b));
  return color*(appearance.x/(1.0+(appearance.x-1.0)*peak));
}
float4 ps(V v):SV_TARGET {
  float3 color=source.Sample(linearSampler,v.uv).rgb;
  if (appearance.w!=0.0) color=gradeBackground(color*backgroundTint.rgb);
  return float4(color * backgroundTint.w,backgroundTint.w);
}
