#include "common.hlsli"

Texture2D<float4> cachedSky : register(t1);
SamplerState linearClamp : register(s0);

struct ScreenVertex {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

ScreenVertex ScreenVS(uint vertex : SV_VertexID) {
    ScreenVertex output;
    output.uv = float2((vertex << 1u) & 2u, vertex & 2u);
    output.position = clipPosition(output.uv);
    return output;
}

float4 CopyPS(ScreenVertex input) : SV_Target {
    return cachedSky.Sample(linearClamp, input.uv);
}

float noise(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(hash21(i), hash21(i + float2(1, 0)), f.x),
                lerp(hash21(i + float2(0, 1)), hash21(i + 1.0), f.x), f.y);
}

float turbulence(float2 p) {
    const float2x2 rotate = float2x2(0.80, -0.60, 0.60, 0.80);
    float a = noise(p) * 0.55;
    p = mul(rotate, p) * 2.04 + 19.3;
    a += noise(p) * 0.26;
    p = mul(rotate, p) * 2.07 + 31.8;
    a += noise(p) * 0.13;
    p = mul(rotate, p) * 2.13 + 12.7;
    return a + noise(p) * 0.06;
}

float3 stars(float2 p, float scale, float threshold) {
    float2 cell = floor(p * scale);
    float id = hash21(cell);
    float2 offset = 0.24 * (float2(hash21(cell + 17.6), hash21(cell - 3.2)) - 0.5);
    float2 q = frac(p * scale) - 0.5 - offset;
    float core = falloff(0.015, 0.205, length(q));
    float halo = falloff(0.045, 0.42, length(q));
    float intensity = smoothstep(threshold, 1.0, id);
    float3 hue = lerp(float3(0.40, 0.58, 1.0), float3(1.0, 0.72, 0.53), hash21(cell + 51.9));
    return hue * intensity * (core * 0.78 + halo * 0.14);
}

float3 nebula(float2 p) {
    float2 lane = float2(0.86 * p.x - 0.51 * p.y, 0.51 * p.x + 0.86 * p.y + 0.08);
    float2 domain = lane * float2(3.2, 7.4) + float2(8.3, 12.1);
    float2 warp = float2(turbulence(domain * 0.67), turbulence(domain * 0.69 + 14.3)) - 0.5;
    float structure = turbulence(domain + warp * 3.8);
    float opened = lane.y + (structure - 0.52) * 0.39;
    float ribbon = falloff(0.018, 0.30, abs(opened));
    float fine = turbulence(domain * 3.1 + warp * 5.0);
    float filament = falloff(0.004, 0.069, abs(opened + 0.052 + (fine - 0.5) * 0.12));
    float dust = smoothstep(0.28, 0.75, structure * 0.81 + fine * 0.19);
    float3 violet = float3(0.22, 0.165, 0.43);
    float3 ember = float3(0.51, 0.195, 0.185);
    float3 cloud = lerp(violet, ember, smoothstep(-0.53, 0.38, lane.x + 0.22 * warp.y));
    float3 mist = float3(0.085, 0.125, 0.24) * falloff(0.08, 0.55, abs(opened - 0.12));
    return cloud * ribbon * dust * 0.90 + filament * dust * cloud * 0.38 + mist * (0.20 + 0.35 * fine);
}

float3 grade(float3 color) {
    float luma = dot(color, float3(0.2126, 0.7152, 0.0722));
    float3 axis = 0.57735027;
    float3 hue = color * skyTintCos.w + cross(axis, color) * light.x
               + dot(axis, color) * axis * (1.0 - skyTintCos.w);
    return max(0.0, lerp(luma.xxx, hue, sky.w)) * skyTintCos.xyz * sky.z;
}

float3 accretion(float2 p, float r) {
    // The near-side disk crosses the silhouette only at the bottom.
    float2 d = float2(p.x + 0.21 * p.y, p.y - 0.19 * p.x);
    float radius = length(d * float2(1.0, 5.2));
    float diskMask = step(0.0, d.y) + (1.0 - step(0.0, d.y)) * smoothstep(0.120, 0.131, r);
    float ring = smoothstep(0.128, 0.160, radius) * falloff(0.204, 0.616, radius);
    float striation = 0.71 + 0.14 * sin(111.0 * radius + 4.0 * noise(d * 37.0))
                    + 0.10 * sin(226.0 * radius);
    float doppler = lerp(0.66, 1.46, smoothstep(-0.35, 0.23, -d.x));
    float material = ring * striation * diskMask * doppler;
    float3 copper = lerp(float3(0.43, 0.112, 0.038), float3(1.0, 0.72, 0.39),
                         falloff(0.151, 0.293, radius));

    float nearRim = abs(r - 0.121 + p.x * 0.009);
    float upper = 0.22 + 0.78 * falloff(-0.085, 0.052, d.y);
    float photon = upper * (0.89 * falloff(0.0005, 0.0034, nearRim)
                            + 0.32 * falloff(0.002, 0.015, nearRim));

    float lensed = length(float2(d.x * 0.93, (d.y + 0.046) * 1.19));
    float lensMask = smoothstep(0.115, 0.164, r) * falloff(-0.008, 0.051, d.y);
    float arc = (0.33 * falloff(0.001, 0.0065, abs(lensed - 0.146))
                  + 0.31 * falloff(0.004, 0.043, abs(lensed - 0.154))) * lensMask;
    float aura = falloff(0.105, 0.296, r) * smoothstep(0.116, 0.15, r) * 0.12;
    return copper * material + float3(1.0, 0.60, 0.30) * photon * doppler
         + float3(0.93, 0.34, 0.10) * arc * doppler + float3(0.42, 0.145, 0.078) * aura;
}

float4 SkyPS(ScreenVertex input) : SV_Target {
    if (all(layerOpacity.xy == 0)) {
        return 0;
    }
    float2 p = (input.uv - float2(0.555, 0.415)) * float2(sky.x, 1.0);
    float r = length(p);
    float veil = falloff(0.36, max(0.80, sky.x * 0.77), length(p * float2(0.86, 1.21)));
    float3 skyColor = float3(0.010, 0.013, 0.027);
    float3 starlight = stars(p + 3.7, 84.0, 0.967) + stars(p - 4.9, 169.0, 0.981) * 0.47;
    if (sky.y < 0.5) {
        skyColor += nebula(p) * veil + starlight * (0.46 + veil * 0.50);
    } else {
        skyColor *= 0.45;
        skyColor += starlight * 0.13;
    }
    float shadow = smoothstep(0.116, 0.121, r);
    float3 disk = layerOpacity.y > 0 ? accretion(p, r) * light.y : 0;
    float dither = (hash21(input.position.xy) - 0.5) * (1.0 / 255.0);
    if (all(layerOpacity.xy == 1)) {
        return float4(max(0.0, grade(skyColor) * shadow + disk + dither * shadow), 1.0);
    }
    float occlusion = (1.0 - shadow) * layerOpacity.y;
    float3 base = saturate(grade(skyColor) + dither) * layerOpacity.x * (1.0 - occlusion);
    float3 emission = disk * layerOpacity.y;
    float diskCoverage = saturate(1.0 - shadow + max(disk.r, max(disk.g, disk.b))) * layerOpacity.y;
    float alpha = layerOpacity.x + (1.0 - layerOpacity.x) * diskCoverage;
    return float4(min(base + emission, alpha.xxx), alpha);
}
