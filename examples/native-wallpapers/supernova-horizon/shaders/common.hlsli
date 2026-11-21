cbuffer Settings : register(b0) {
    float4 sky;            // Aspect, background mode, brightness, saturation.
    float4 skyTintCos;     // RGB filter and cosine of hue rotation.
    float4 light;          // Sine of hue rotation, disk brightness, nova brightness, nova radius.
    float4 novaTint;       // RGB filter and supernova opacity.
    float4 layerOpacity;   // Sky and accretion opacity.
};

float falloff(float inner, float outer, float value) {
    return 1.0 - smoothstep(inner, outer, value);
}

float hash21(float2 p) {
    float3 q = frac(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973));
    q += dot(q, q.yzx + 33.33);
    return frac((q.x + q.y) * q.z);
}

float2 quadCorner(uint vertex) {
    return float2((vertex & 1u) != 0u ? 1.0 : -1.0,
                  (vertex & 2u) != 0u ? 1.0 : -1.0);
}

float4 clipPosition(float2 uv) {
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
