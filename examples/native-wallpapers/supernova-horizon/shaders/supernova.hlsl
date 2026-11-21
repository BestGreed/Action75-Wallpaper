#include "common.hlsli"

struct Burst {
    float2 center;
    float age;
    float seed;
};

StructuredBuffer<Burst> bursts : register(t0);
static const uint sparksPerBurst = 36;
static const float tau = 6.28318531;

float4 emission(float3 color) {
    color *= novaTint.w;
    return float4(color, max(color.r, max(color.g, color.b)));
}

struct FlashVertex {
    float4 position : SV_Position;
    float2 local : TEXCOORD0;
    nointerpolation float age : TEXCOORD1;
    nointerpolation float seed : TEXCOORD2;
};

FlashVertex NovaVS(uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
    Burst burst = bursts[instance];
    FlashVertex output;
    output.local = quadCorner(vertex);
    float bound = 0.143 * light.w * (0.33 + 0.67 * sqrt(burst.age));
    output.position = clipPosition(burst.center + output.local * bound * float2(rcp(sky.x), 1.0));
    output.age = burst.age;
    output.seed = burst.seed;
    return output;
}

float4 NovaPS(FlashVertex input) : SV_Target {
    float age = input.age;
    float2 p = input.local;
    float r = length(p);
    if (r >= 1.0) {
        return 0.0;
    }
    float angle = atan2(p.y, p.x + 0.000001);
    float fade = falloff(0.22, 1.0, age);
    float flash = falloff(0.027, 0.29, age);
    float core = falloff(0.007, 0.226 - 0.143 * age, r) * flash;
    float thermal = falloff(0.02, 0.53, r) * (0.15 * fade + 0.26 * flash);

    float spokes = 0.58 + 0.23 * sin(8.0 * angle + 21.0 * r + input.seed)
                 + 0.10 * sin(17.0 * angle - 17.0 * r + input.seed)
                 + 0.08 * sin(41.0 * angle + 5.0 * r);
    float front = 0.18 + 0.70 * (1.0 - exp2(-4.5 * age));
    float waveR = r + 0.026 * sin(angle * 9.0 + input.seed)
                + 0.010 * sin(angle * 22.0) + 0.008 * sin(angle * 33.0 - 19.0 * r);
    float edge = falloff(0.004, 0.027 + 0.012 * age, abs(waveR - front));
    float precursor = falloff(0.007, 0.071, abs(waveR - front * 0.96));
    float wake = falloff(0.019, 0.19, abs(waveR - front * 0.78));
    float ring = fade * (edge * (0.14 + 0.47 * spokes) + precursor * 0.12 + wake * 0.17 * spokes);
    float reverse = falloff(0.009, 0.052, abs(r - 0.63 * front)) * fade * spokes * 0.21;
    float clump = 0.5 + 0.5 * sin(angle * 13.0 - 19.0 * r + input.seed);
    float ejecta = clump * clump * fade * smoothstep(0.05, 0.18, age)
                 * falloff(0.020, 0.13, abs(r - front * (0.68 + 0.23 * spokes))) * 0.26;

    float2 ray = abs(p * float2(0.993, 1.0) + p.yx * float2(0.12, -0.12));
    float diffraction = (falloff(0.002, 0.033, ray.y) * falloff(0.038, 0.83, ray.x) * 0.55
                        + falloff(0.002, 0.028, ray.x) * falloff(0.029, 0.54, ray.y) * 0.30) * flash;
    float3 color = float3(0.83, 0.93, 1.0) * (core * 1.10 + diffraction)
                 + float3(0.48, 0.27, 1.0) * (thermal + reverse)
                 + float3(0.73, 0.45, 0.94) * ejecta
                 + float3(0.28, 0.67, 1.0) * ring;
    return emission(color * novaTint.xyz * light.z);
}

struct SparkVertex {
    float4 position : SV_Position;
    float2 local : TEXCOORD0;
    nointerpolation float3 glow : TEXCOORD1;
};

SparkVertex SparkVS(uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
    Burst burst = bursts[instance / sparksPerBurst];
    float id = float(instance % sparksPerBurst);
    float age = burst.age;
    float jitter = hash21(float2(id, burst.seed));
    float variety = hash21(float2(id + 1.3, burst.seed + 4.2));
    float angle = tau * (id + 0.79 * jitter) / float(sparksPerBurst) + burst.seed;
    float2 radial = float2(cos(angle), sin(angle));
    float2 tangent = float2(-radial.y, radial.x);
    float progress = 1.0 - exp2(-3.4 * age);
    float distance = (0.028 + (0.079 + 0.090 * variety) * progress) * light.w;
    float2 origin = radial * distance + float2(0.007 * sin(angle * 2.0), 0.014) * age * age;
    float tail = (0.004 + jitter * 0.009) * light.w * (0.75 + age);
    float width = (0.00058 + 0.0012 * variety) * light.w;
    float2 local = quadCorner(vertex);
    float2 offset = radial * (local.x * tail - tail * 0.68) + tangent * local.y * width;
    float life = smoothstep(0.012, 0.095, age) * falloff(0.13 + jitter * 0.24, 0.64 + jitter * 0.36, age);
    float3 spectrum = lerp(float3(1.0, 0.42, 0.16), float3(0.36, 0.75, 1.0), smoothstep(0.21, 0.64, variety));
    SparkVertex output;
    output.position = clipPosition(burst.center + (origin + offset) * float2(rcp(sky.x), 1.0));
    output.local = local;
    output.glow = spectrum * (0.59 + 0.35 * variety) * life * novaTint.xyz * light.z;
    return output;
}

float4 SparkPS(SparkVertex input) : SV_Target {
    float x = input.local.x;
    float y = abs(input.local.y);
    float trail = smoothstep(-1.0, 0.76, x) * falloff(0.65, 1.0, x) * falloff(0.06, 1.0, y);
    float tip = falloff(0.03, 0.41, length(float2((x - 0.65) * 2.2, y))) * 0.44;
    return emission(input.glow * (trail + tip));
}
