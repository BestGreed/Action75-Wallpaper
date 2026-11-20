#pragma once
constexpr char SpriteBurstShader[] = R"HLSL(
cbuffer Burst : register(b0) {
  float4 displayInfo; // width, height, diameter, 1/columns
  float4 atlasInfo;   // 1/rows, width, height, columns
  float4 effectTint;  // user tint, independent of the cached backdrop
  float4 effectGrade; // saturation, cos(hue), sin(hue), hue enabled
  float4 hits[24];    // normalized x, y, fractional frame, gain
  float4 bounds[24];  // sample-aware local quad crop
  float4 palettes[24]; // target display RGB and palette enabled
};
#if defined(SPRITE_ATLAS_2D)
Texture2D atlas : register(t0);
#else
Texture3D atlas : register(t0);
#endif
SamplerState linearClamp : register(s0);
struct Out {
  float4 clip : SV_POSITION;
  nointerpolation float4 palette : TEXCOORD3;
#if defined(SPRITE_ATLAS_2D)
  float2 uvA : TEXCOORD0;
  float2 uvB : TEXCOORD1;
  nointerpolation float2 fade : TEXCOORD2;
#else
  float3 uvw : TEXCOORD0;
  nointerpolation float gain : TEXCOORD1;
#endif
};
#if defined(SPRITE_ATLAS_2D)
float2 atlasUV(uint index, float2 local) {
  uint columns = (uint)atlasInfo.w;
  uint row = index / columns;
  uint column = index % columns;
  float2 cell = float2(displayInfo.w, atlasInfo.x);
  float2 guard = .5 / atlasInfo.yz;
  return float2(column,row)*cell + guard + local*(cell-2*guard);
}
#endif
Out vs(uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
  float2 uv = float2(vertex & 1u, (vertex >> 1u) & 1u);
  float4 bound = bounds[instance];
  uv = lerp(bound.xy, bound.zw, uv);
  float4 hit = hits[instance];
  float2 position = hit.xy + (uv-.5)*displayInfo.z / displayInfo.xy;
  Out o;
  o.clip = float4(position*float2(2,-2) + float2(-1,1), 0, 1);
  o.palette=palettes[instance];
#if defined(SPRITE_ATLAS_2D)
  uint frame = (uint)hit.z;
  uint maxFrame = (uint)round(atlasInfo.w / atlasInfo.x)-1u;
  o.uvA = atlasUV(frame,uv);
  o.uvB = atlasUV(min(maxFrame,frame+1u),uv);
  o.fade = float2(frac(hit.z),hit.w);
#else
  float guard = .5/(atlasInfo.y*displayInfo.w);
  o.uvw = float3(guard+uv*(1-2*guard),(hit.z+.5)*(displayInfo.w*atlasInfo.x));
  o.gain = hit.w;
#endif
  return o;
}
float3 grade(float3 color, float4 palette) {
  // Neutral settings bypass the new arithmetic to preserve accepted pixels.
  if (palette.w<=.5 && effectGrade.x==1 && effectGrade.w==0) return color;
  if (palette.w>.5) {
    float low=min(color.x,min(color.y,color.z)), high=max(color.x,max(color.y,color.z));
    color=float3(low,low,low)+(high-low)*palette.rgb;
  }
  if (effectGrade.w!=0) {
    float luma=dot(color,float3(.2126,.7152,.0722));
    float mean=(color.x+color.y+color.z)/3;
    float3 crossColor=float3(color.z-color.y,color.x-color.z,color.y-color.x)*.577350269;
    color=color*effectGrade.y+crossColor*effectGrade.z+float3(mean,mean,mean)*(1-effectGrade.y);
    float correction=luma-dot(color,float3(.2126,.7152,.0722));
    color+=float3(correction,correction,correction);
  }
  if (effectGrade.x!=1) {
    float luma=dot(color,float3(.2126,.7152,.0722));
    color=float3(luma,luma,luma)+(color-float3(luma,luma,luma))*effectGrade.x;
  }
  return max(color,float3(0,0,0));
}
float4 ps(Out i) : SV_TARGET {
  float3 raw;
  float gain;
#if defined(SPRITE_ATLAS_2D)
  float3 a = atlas.Sample(linearClamp, i.uvA).rgb;
  float3 b = atlas.Sample(linearClamp, i.uvB).rgb;
  raw=lerp(a,b,i.fade.x);gain=i.fade.y;
#else
  raw=atlas.SampleLevel(linearClamp,i.uvw,0).rgb;gain=i.gain;
#endif
  float3 color;
  if(i.palette.w>.5) {
    const float density=max(raw.r,max(raw.g,raw.b));
    float3 desired=grade(i.palette.rgb,float4(1,1,1,0))*effectTint.rgb;
    const float targetPeak=max(desired.r,max(desired.g,desired.b));
    if(targetPeak>1) desired/=targetPeak;
    // Palette RGB describes the desired visible color, including on paper.
    color=effectTint.w>.5 ? saturate(density*gain)*(1-desired) : density*gain*desired;
  } else {
    color=grade(raw,i.palette)*gain*effectTint.rgb;
  }
  // Preserve chroma at high gain instead of independently clipping RGB to white.
  const float peak=max(color.r,max(color.g,color.b));
  if (peak>1) color/=peak;
  return float4(color,0);
}
)HLSL";
