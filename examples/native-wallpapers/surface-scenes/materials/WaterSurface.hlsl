#include "LayerColor.hlsli"
cbuffer Frame : register(b0) {
    float4 Surface;             // water-space extent and its reciprocal
    float4 Grid;                // cell pitch and its reciprocal
    float4 Wave;                // time step, speed squared, damping, visibility
    float4 Style;               // ambient phase, exposure, crest brightness, backdrop
    float4 Tint;                // linear RGB, saturation
    float4 Backdrop;            // floor texture, caustics, ripple opacity, background opacity
    float4 WaterHue[3], CrestColor[3];
};
struct ImpactStamp {
    float4 geometry;            // UV, footprint radius in water space, roundness
    float4 impulse;             // depression, downward impulse, rebound impulse
};
cbuffer Impacts : register(b1) {
    ImpactStamp Hits[128];
};

Texture2D<float2> HeightVelocity : register(t0);
Texture2D<float2> Pressure : register(t1);
Texture2D<float4> SurfaceSlope : register(t2);
Texture2D<float4> Optics : register(t3);
RWTexture2D<float2> NextWave : register(u0);
RWTexture2D<float4> NextSlope : register(u1);
RWTexture2D<float4> OpticsOutput : register(u2);
SamplerState LinearClamp : register(s0);
SamplerState LinearWrap : register(s1);

struct Quad {
    float4 position : SV_Position;
    float2 coordinate : TEXCOORD0;
};

Quad ScreenVertex(uint vertex : SV_VertexID) {
    Quad result;
    result.coordinate = float2((vertex << 1) & 2, vertex & 2);
    result.position = float4(result.coordinate * float2(2, -2) + float2(-1, 1), 0, 1);
    return result;
}

struct ImpactQuad {
    float4 position : SV_Position;
    float2 local : TEXCOORD0;
    nointerpolation float4 load : TEXCOORD1;
};

ImpactQuad ImpactVertex(uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
    ImpactStamp hit = Hits[instance];
    ImpactQuad result;
    result.local = float2(vertex & 1, vertex >> 1) * 2 - 1;
    float2 uv = hit.geometry.xy + result.local * hit.geometry.z * Surface.zw;
    result.position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    result.load = float4(hit.geometry.w, hit.impulse.xyz);
    return result;
}

float CavityAndRim(float q) {
    // The depressed center is balanced by a raised rim in the radial integral.
    return (1 - q) * exp(-q);
}

float2 ImpactPixel(ImpactQuad input) : SV_Target {
    float r2 = dot(input.local, input.local);
    float q = r2 * lerp(17.5, 8.6, input.load.x);
    float taper = 1 - smoothstep(0.79, 1, r2);
    float height = -input.load.y * CavityAndRim(q);
    float momentum = -input.load.z * CavityAndRim(0.78 * q)
                     + input.load.w * CavityAndRim(1.63 * q);
    return float2(height, momentum) * taper;
}

float2 WaterAt(int2 cell, int2 extent) {
    return HeightVelocity.Load(int3(clamp(cell, int2(0, 0), extent - 1), 0));
}

float HeightAt(int2 cell, int2 extent) {
    return WaterAt(cell, extent).x;
}

[numthreads(16, 8, 1)]
void StepWave(uint2 id : SV_DispatchThreadID) {
    uint2 extent;
    HeightVelocity.GetDimensions(extent.x, extent.y);
    if (any(id >= extent)) return;

    int2 p = int2(id);
    float2 state = HeightVelocity.Load(int3(p, 0));
    float2 x = WaterAt(p + int2(1, 0), extent) + WaterAt(p - int2(1, 0), extent);
    float2 y = WaterAt(p + int2(0, 1), extent) + WaterAt(p - int2(0, 1), extent);
    float diagonals = HeightAt(p + int2(1, 1), extent) + HeightAt(p - int2(1, 1), extent)
                    + HeightAt(p + int2(-1, 1), extent) + HeightAt(p + int2(1, -1), extent);
    float2 inversePitchSquared = Grid.zw * Grid.zw;
    float2 second = float2(x.x, y.x) - 2 * state.x;
    // The diagonal correction reduces directional bias on short waves.
    float laplacian = dot(second, inversePitchSquared)
                    + (diagonals - 2 * (x.x + y.x) + 4 * state.x) * (Grid.z * Grid.w / 6);
    // Diffuse vertical velocity so short wavelets dissipate before the broad swell.
    float diffusion = 0.00012 * dot(float2(x.y, y.y) - 2 * state.y, inversePitchSquared);
    float2 wallDistance = min(id, extent - 1 - id) * Grid.xy;
    float wall = saturate(1 - min(wallDistance.x, wallDistance.y) / 0.089);
    wall *= wall;
    // Pressure is bound only for the first substep that follows an input.
    float2 impulse = clamp(Pressure.Load(int3(p, 0)), float2(-0.063, -1.55), float2(0.036, 1.55));
    float velocity = (state.y + (laplacian * Wave.y + diffusion) * Wave.x)
                   * (1 - Wave.x * (Wave.z + 20 * wall));
    velocity = clamp(velocity + impulse.y, -2.05, 2.05);
    float height = (state.x + velocity * Wave.x + impulse.x) * (1 - Wave.x * (0.18 + 8 * wall));
    NextWave[id] = float2(clamp(height, -0.075, 0.075), velocity);
}

[numthreads(16, 8, 1)]
void BuildSlope(uint2 id : SV_DispatchThreadID) {
    uint2 extent;
    HeightVelocity.GetDimensions(extent.x, extent.y);
    if (any(id >= extent)) return;

    int2 p = int2(id);
    float h = HeightAt(p, extent);
    float w = HeightAt(p - int2(1, 0), extent), e = HeightAt(p + int2(1, 0), extent);
    float n = HeightAt(p - int2(0, 1), extent), s = HeightAt(p + int2(0, 1), extent);
    float2 gradient = float2(e - w, s - n) * (0.5 * Grid.zw);
    float curvature = dot(float2(e + w, s + n) - 2 * h, Grid.zw * Grid.zw);
    NextSlope[id] = float4(gradient, clamp(curvature * 0.026, -1, 1), h);
}

float RandomCell(uint2 cell, uint seed) {
    uint bits = cell.x * 1597334677u ^ cell.y * 3812015801u ^ seed * 2798796415u;
    bits = (bits ^ (bits >> 16)) * 0x7feb352du;
    bits = (bits ^ (bits >> 15)) * 0x846ca68bu;
    return ((bits ^ (bits >> 16)) >> 8) * (1.0 / 16777216.0);
}

float3 NoiseGradient(float2 uv, int frequency, uint seed) {
    float2 p = uv * frequency;
    int2 i = int2(floor(p));
    int mask = frequency - 1;
    float2 f = frac(p);
    float2 u = f * f * f * (f * (6 * f - 15) + 10);
    float2 du = 30 * f * f * (f - 1) * (f - 1);
    float a = RandomCell(i & mask, seed);
    float b = RandomCell((i + int2(1, 0)) & mask, seed);
    float c = RandomCell((i + int2(0, 1)) & mask, seed);
    float d = RandomCell((i + int2(1, 1)) & mask, seed);
    return float3(lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y),
                  du * float2(lerp(b - a, d - c, u.y), lerp(c - a, d - b, u.x)));
}

float CausticEdges(float2 uv) {
    const int frequency = 8;
    float2 p = uv * frequency;
    int2 tile = int2(floor(p));
    float2 f = frac(p);
    float first = 9, second = 9;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            int2 offset = int2(x, y);
            uint2 cell = (tile + offset) & (frequency - 1);
            float2 site = 0.11 + 0.78 * float2(RandomCell(cell, 13), RandomCell(cell, 27));
            float2 delta = offset + site - f;
            float distanceSquared = dot(delta, delta);
            second = min(second, max(first, distanceSquared));
            first = min(first, distanceSquared);
        }
    }
    float distanceToEdge = max(sqrt(second) - sqrt(first), 0);
    return 0.63 * exp2(-190 * distanceToEdge * distanceToEdge) + 0.37 * exp2(-20 * distanceToEdge);
}

[numthreads(8, 8, 1)]
void BakeOptics(uint2 id : SV_DispatchThreadID) {
    uint2 extent;
    OpticsOutput.GetDimensions(extent.x, extent.y);
    float2 uv = (id + 0.5) / extent;
    float3 broad = NoiseGradient(uv, 8, 4), swell = NoiseGradient(uv, 16, 37);
    float3 detail = NoiseGradient(uv, 32, 20), grains = NoiseGradient(uv, 64, 61);
    float2 slope = 0.37 * broad.yz + 0.24 * swell.yz + 0.105 * detail.yz;
    float bed = 0.24 * broad.x + 0.36 * swell.x + 0.26 * detail.x + 0.14 * grains.x;
    float2 bend = float2(swell.x - 0.5, broad.x - 0.5) * 0.074;
    OpticsOutput[id] = float4(saturate(0.5 + 0.5 * slope), bed, CausticEdges(uv + bend));
}

[numthreads(8, 8, 1)]
void DownsampleOptics(uint2 id : SV_DispatchThreadID) {
    uint2 extent;
    OpticsOutput.GetDimensions(extent.x, extent.y);
    if (any(id >= extent)) return;
    OpticsOutput[id] = Optics.SampleLevel(LinearClamp, (id + 0.5) / extent, 0);
}

float3 SkyRadiance(float3 ray) {
    float2 sky = ray.xy / (0.38 + max(ray.z, 0));
    float2 delta = sky - float2(-0.142, 0.479);
    float u = dot(delta, float2(0.83, 0.56));
    float v = dot(delta, float2(-0.56, 0.83));
    float overcast = exp2(-2.8 * u * u - 1.8 * v * v);
    float window = exp2(-50 * (u + 0.072) * (u + 0.072) - 6 * (v - 0.06) * (v - 0.06));
    float rim = exp2(-235 * (u - 0.064) * (u - 0.064) - 8 * (v + 0.07) * (v + 0.07));
    float glint = exp2(-64 * (u + 0.36) * (u + 0.36) - 17 * (v - 0.16) * (v - 0.16));
    return float3(0.021, 0.052, 0.080) + float3(0.13, 0.32, 0.43) * overcast
         + float3(1.02, 1.58, 1.61) * window + float3(2.14, 2.71, 2.77) * rim
         + float3(0.58, 0.87, 1.19) * glint;
}

float3 LinearToSrgb(float3 color) {
    float3 useGamma = step(0.0031308, color);
    return lerp(12.92 * color, 1.055 * pow(max(color, 1e-5), 1.0 / 2.4) - 0.055, useGamma);
}

static const uint Lagoon = 1, Dusk = 2, Obsidian = 3, Custom = 4;

// Floor pigment and depth in water space. Each basin has its own light distribution.
float4 Basin(float2 p, uint look) {
    if (look == Lagoon) {
        float coast = p.y + 0.25 * p.x + 0.12 * sin(2.9 * p.x) + 0.06;
        float bank = smoothstep(-0.42, 0.48, coast);
        return float4(lerp(float3(0.026, 0.25, 0.24), float3(0.52, 0.63, 0.39), bank),
                      lerp(0.37, 0.068, bank));
    }
    if (look == Dusk) {
        float streak = p.y + 0.19 * p.x - 0.20 + 0.04 * sin(2.3 * p.x);
        float glow = exp2(-10 * streak * streak - 0.68 * (p.x + 0.24) * (p.x + 0.24));
        return float4(lerp(float3(0.025, 0.018, 0.072), float3(0.35, 0.11, 0.061), glow),
                      lerp(0.38, 0.105, glow));
    }
    if (look == Obsidian) {
        float2 pool = mul(float2x2(0.90, 0.43, -0.43, 0.90), p + float2(0.52, 0.25));
        float light = exp2(-dot(pool * pool, float2(0.93, 3.5)));
        return float4(lerp(float3(0.009, 0.010, 0.011), float3(0.063, 0.063, 0.065), light),
                      0.26);
    }
    if (look == Custom)
        return float4(Tint.rgb, 0.19);

    float diagonal = p.y + 0.31 * p.x + 0.15;
    float pool = exp2(-5.9 * diagonal * diagonal - 0.61 * (p.x + 0.33) * (p.x + 0.33));
    return float4(lerp(float3(0.005, 0.016, 0.032), float3(0.022, 0.078, 0.103), pool),
                  lerp(0.52, 0.205, pool));
}

float3 BackgroundLight(float2 p, float2 uv, uint look, float4 waves, float2 wake) {
    if (look == Custom && Backdrop.x + Backdrop.y == 0) {
        float value = dot(Tint.rgb, float3(0.2126, 0.7152, 0.0722));
        return saturate(lerp(value.xxx, Tint.rgb, Tint.w) * Style.y);
    }
    float t = Style.x;
    const float2x2 turn = float2x2(0.74, -0.67, 0.67, 0.74);
    const float2x2 windTurn = float2x2(0.93, 0.37, -0.37, 0.93);
    float2 wind = Optics.Sample(LinearWrap, mul(windTurn, p) * float2(0.39, 0.57)
                                           + float2(0.028, -0.011) * t).rg * 2 - 1;
    float2 crossSwell = Optics.Sample(LinearWrap, mul(turn, p) * 1.09 + float2(-0.044, 0.034) * t).rg * 2 - 1;
    float longSwell = sin(dot(p, float2(10, 20)) + (6.283185307 * 202 / 1000) * t + 1.8 * wind.x);
    float2 slope = mul(wind * float2(0.084, 0.151), windTurn)
                 + mul(crossSwell, turn) * 0.084 + longSwell * float2(0.011, 0.022);
    slope = slope * min(Backdrop.x * 1.12, 1.45) + wake;

    float3 normal = normalize(float3(-slope, 1));
    float3 view = normalize(float3(-0.22 * p.x, -0.82 + 0.43 * p.y, 1));
    float3 reflected = reflect(-view, normal);
    // Air to water. The same normal drives the reflected light and the position
    // where the transmitted ray strikes the bottom.
    float3 transmitted = refract(-view, normal, 1.0 / 1.333);
    float grazing = 1 - saturate(dot(normal, view));
    float grazingSquared = grazing * grazing;
    float fresnel = 0.02037 + 0.97963 * grazingSquared * grazingSquared * grazing;

    float4 basin = Basin(p, look);
    float travel = max(0.012, basin.w + waves.w) / max(-transmitted.z, 0.35);
    float2 bottom = p + travel * transmitted.xy;
    float frequency = look == Lagoon ? 1.15 : 0.77;
    float2 causticUv = bottom * frequency + wind * 0.126 + float2(0.015, 0.024) * t;
    float4 bed = Optics.Sample(LinearWrap, causticUv);
    float4 secondary = Optics.Sample(LinearWrap, mul(turn, bottom) * 0.56 - crossSwell * 0.11
                                               + float2(-0.018, 0.013) * t);
    float caustic = bed.a * (0.57 + 0.73 * secondary.a) + 0.22 * secondary.a;
    float focus = clamp(1 - 1.08 * waves.z, 0.35, 1.87);
    float causticLight = caustic * caustic * focus * Backdrop.y;
    float grain = bed.b - 0.5;
    if (look == Obsidian) {
        float vein = grain + 0.48 * (secondary.b - 0.5);
        grain = 1.60 * grain + 0.36 * exp2(-94 * vein * vein) - 0.15;
    }
    float bedDetail = 1 + grain * Backdrop.x * 0.55;
    float3 body = basin.rgb * bedDetail;
    if (look == Custom) {
        body *= 1 + causticLight * 0.38;
    } else {
        float3 extinction = look == Dusk ? float3(3.5, 4.3, 3.1)
                          : look == Obsidian ? float3(4.2, 4.2, 4.0)
                          : float3(8.6, 4.0, 3.25);
        float3 transmission = exp2(-travel * extinction);
        float3 scattering = look == Dusk ? float3(0.013, 0.007, 0.020)
                         : look == Obsidian ? float3(0.006, 0.006, 0.007)
                                         : float3(0.004, 0.010, 0.016);
        float3 light = look == Lagoon ? float3(0.070, 0.125, 0.102)
                     : look == Dusk ? float3(0.106, 0.045, 0.023)
                     : look == Obsidian ? float3(0.012, 0.012, 0.013)
                     : float3(0.008, 0.026, 0.036);
        body = lerp(scattering, body, transmission) + causticLight * light;
        float3 sky = SkyRadiance(reflected);
        if (look == Dusk) sky *= float3(1.50, 0.73, 0.51);
        if (look == Obsidian) sky = dot(sky, float3(0.2126, 0.7152, 0.0722)) * 0.58;
        body = lerp(body, sky, fresnel * (look == Dusk ? 0.36 : 0.22));
        float2 falloff = abs(uv - 0.5) * 1.4;
        body *= (1 - 0.28 * dot(falloff, falloff)) * sqrt(Tint.rgb);
    }

    float luma = dot(body, float3(0.2126, 0.7152, 0.0722));
    float3 graded = max(lerp(luma.xxx, body, Tint.w) * Style.y, 0);
    return look == Custom ? saturate(graded) : graded / (1 + 0.55 * graded);
}

float4 WaterLayer(Quad input, float2 opacity) {
    float2 uv = input.coordinate;
    float2 p = (uv - 0.5) * Surface.xy;
    const uint look = (uint) Style.w;
    float4 waves = 0;
    [branch] if (Wave.w > 0)
        waves = SurfaceSlope.SampleLevel(LinearClamp, uv, 0) * Wave.w;
    float2 wake = 1.51 * waves.xy;
    wake /= 1 + 0.72 * length(wake);

    float3 color = 0;
    [branch] if (Style.y > 0 && opacity.y > 0)
        color = gradeLayer(BackgroundLight(p, uv, look, waves, wake), WaterHue);

    float4 ripple = 0;
    [branch] if (Wave.w > 0) {
        // The light-facing flank and its adjacent trough separate the ripple
        // from the floor even when the floor is completely black or white.
        float facing = dot(wake, float2(-0.56, -0.8285));
        float trough = 7.4 * max(-facing, 0) + 0.34 * max(waves.z, 0);
        float falloff = 0.65 * trough / (1 + trough);
        float steepness = length(wake);
        float drift = smoothstep(0.003, 0.025, steepness);
        float crest = 13.0 * max(facing, 0) + 0.68 * steepness + 0.13 * max(-waves.z, 0) * drift;
        float reflected = crest * Style.z;
        float glint = (0.19 + 0.56 * drift) * reflected / (1 + reflected);
        float3 light = look == Dusk ? float3(0.63, 0.43, 0.33)
                     : look == Obsidian ? float3(0.58, 0.61, 0.67)
                     : float3(0.38, 0.62, 0.72);
        color *= 1 - falloff;
        float3 crestLight = gradeLayer(light, CrestColor);
        color = lerp(color, max(color, crestLight), glint);
        ripple = float4(saturate(LinearToSrgb(crestLight)) * glint, saturate(glint + falloff));
    }
    color = LinearToSrgb(color);
    float dither = frac(52.9829189 * frac(dot(input.position.xy, float2(0.06711056, 0.00583715)))) - 0.5;
    float3 visible = saturate(color + dither / 255);
    if (opacity.x >= 1 && opacity.y >= 1)
        return float4(visible, 1);
    if (opacity.y <= 0)
        return ripple * opacity.x;
    if (opacity.x < 1 && Wave.w > 0) {
        float3 base = 0;
        if (Style.y > 0)
            base = LinearToSrgb(gradeLayer(BackgroundLight(p, uv, look, 0, 0), WaterHue));
        visible = lerp(saturate(base + dither / 255), visible, opacity.x);
    }
    return lerp(ripple * opacity.x, float4(visible, 1), opacity.y);
}

float4 WaterPixel(Quad input) : SV_Target {
    return WaterLayer(input, float2(1, 1));
}

float4 TranslucentWaterPixel(Quad input) : SV_Target {
    return WaterLayer(input, Backdrop.zw);
}
