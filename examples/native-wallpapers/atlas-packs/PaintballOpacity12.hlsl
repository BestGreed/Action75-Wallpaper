// To regenerate the checked-in header: compile_atlas_shaders PaintballOpacity12.hlsl PaintballOpacityShaders12.h PaintballOpacity
Texture2D<float4> foreground : register(t0);

float4 vs(uint vertex : SV_VertexID) : SV_POSITION {
  const float2 corner = float2((vertex << 1) & 2, vertex & 2);
  return float4(corner * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 ps(float4 position : SV_POSITION) : SV_TARGET {
  return foreground.Load(int3(int2(position.xy), 0));
}
