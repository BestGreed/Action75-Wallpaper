#include "LayerColor.hlsli"
static const uint TrailSegments = 6;
static const uint PatternSize = 384;
static const uint MaxBursts = 112;
static const float Ember = 1;
static const float Flash = 2;
static const float HeartContour = 3;
static const float HeartEmber = 4;

struct Seed
{
    float4 origin;       // origin relative to the press, ignition delay
    float4 direction;    // terminal displacement, gravity
    float4 burn;         // life, drag, tail duration, apparent width
    float4 light;        // energy, phase, particle kind, strobe
    float4 early;        // linear color, transition time
    float4 late;         // linear color, custom palette index
};

struct Burst
{
    float4 location;    // normalized x/y, elapsed scene time, radius scale
    float4 motion;      // trail, gravity, lifetime, wind
    float4 rotation;    // cos, sin, pattern, retained energy
    float4 palette;     // first color, color count
};

cbuffer Frame : register(b0)
{
    float4 screen;      // viewport, ripple clock, horizon
    float4 exposure;    // instances per burst, brightness, ember gain, shimmer
    float4 atmosphere;  // bloom, smoke, reflections
    float4 background;  // linear tint and brightness
    float4 skyGrade[3], sparkGrade[3];
    float4 palette[81]; // fixed palette and eight rotation slots, nine colors each
    Burst bursts[MaxBursts];
};

cbuffer PassConstants : register(b1)
{
    float4 passParameters; // Blur offset/downsample or foreground/background opacity.
};

StructuredBuffer<Seed> seeds : register(t3);
Texture2D<float4> directLight : register(t0);
Texture2D<float4> bloomLight : register(t1);
SamplerState linearClamp : register(s0);

struct SparkVertex
{
    float4 position : SV_Position;
    float2 field : TEXCOORD0;
    float3 emission : COLOR0;
    nointerpolation float kind : TEXCOORD1;
};

float3 trajectory(Seed seed, Burst burst, float t)
{
    float expansion = 1 - exp(-seed.burn.y * t);
    if (seed.light.z >= HeartContour)
    {
        // The first light is a small heart. All contour points share the same expansion and fall.
        float initial = min(.22, .112 / burst.location.w);
        float falling = max(0, t - .28);
        return seed.direction.xyz * lerp(initial, 1, expansion) +
            float3(0, .095 * burst.motion.y * falling * falling / (1 + .42 * falling), 0);
    }
    float3 displacement = seed.origin.xyz + seed.direction.xyz * expansion;
    float2 rotation = burst.rotation.xy;
    displacement.xy = float2(displacement.x * rotation.x - displacement.y * rotation.y,
                             displacement.x * rotation.y + displacement.y * rotation.x);
    float fall = t * t / (1 + .38 * t);
    displacement.xy += float2(burst.motion.w, seed.direction.w * burst.motion.y) * fall;
    return displacement;
}

float2 project(float3 offset, Burst burst)
{
    float radius = screen.y * .151 * burst.location.w;
    return burst.location.xy * screen.xy + offset.xy * radius / max(.69, 1 + .16 * offset.z);
}

float3 colorAt(Seed seed, Burst burst, float age, float depth, float life)
{
    float kind = seed.light.z;
    bool ember = kind == Ember || kind == HeartEmber;
    float hot = smoothstep(seed.early.w, seed.early.w + .20, age);
    float3 color = lerp(seed.early.rgb, seed.late.rgb, hot);
    if (burst.palette.y >= 1)
    {
        uint index = (uint)burst.palette.x + (uint)seed.late.w % (uint)burst.palette.y;
        float3 selected = palette[index].rgb;
        float3 base = ember ? selected * .66 : lerp(selected, float3(1, .69, .35), .38);
        color = lerp(base, selected, hot);
    }
    float ignite = smoothstep(0, .036, age);
    if (kind == Flash)
        ignite = exp(-14 * age) * (1 + .3 * exp(-80 * age));
    if (kind == HeartContour)
        ignite = .48 + .52 * smoothstep(0, .083, age);
    if (kind == HeartEmber)
        ignite = smoothstep(.10, .29, age);
    float envelope = pow(saturate(1 - age / life), .58);
    float phase = seed.light.y + burst.rotation.x * 5;
    float beat = .43 + 1.09 * pow(.5 + .5 * sin(age * (15 + frac(phase) * 18) + phase * 6), 3);
    float twinkle = lerp(1, beat, seed.light.w * exposure.w);
    float emberGain = ember ? exposure.z : 1;
    return gradeLayer(color, sparkGrade) * (seed.light.x * ignite * envelope * twinkle * emberGain * exposure.y *
                    burst.rotation.w / max(.75, 1 + .17 * depth));
}

SparkVertex sparkVS(uint vertex : SV_VertexID, uint instance : SV_InstanceID)
{
    static const float2 corners[6] = {
        float2(0, -1), float2(0, 1), float2(1, -1),
        float2(1, -1), float2(0, 1), float2(1, 1)
    };
    SparkVertex output = (SparkVertex)0;
    output.position = float4(-2,-2,0,1);
    uint perBurst = (uint)exposure.x;
    uint index = instance / perBurst;
    Burst burst = bursts[index];
    Seed seed = seeds[(uint)burst.rotation.z * PatternSize + instance % perBurst];
    float age = burst.location.z - seed.origin.w;
    float life = seed.burn.x * burst.motion.z;
    float tail = min(age, seed.burn.z * burst.motion.x);
    bool heart = seed.light.z >= HeartContour;
    if (heart)
        tail = min(tail, .019 + .26 * age);
    uint segment = vertex / 6;
    bool head = segment == TrailSegments;
    if (age < 0 || age >= life + tail || (head && age >= life) || (!head && tail < .019) ||
        ((seed.light.z == Ember || seed.light.z == HeartEmber) && exposure.z == 0))
        return output;

    output.kind = head ? 1 : 0;
    float2 corner = corners[vertex % 6];
    float px = screen.y / 720;
    float2 pixel;
    if (head)
    {
        float3 position = trajectory(seed, burst, age);
        output.field = float2(corner.x * 2 - 1, corner.y);
        float width = seed.burn.w * lerp(.93, 1.49, saturate((burst.location.w - .5) * .66));
        if (seed.light.z == Flash)
        {
            width *= 1 + 2.7 * age;
            output.kind = 2;
        }
        if (heart)
            width *= lerp(.54, 1, smoothstep(0, .38, age));
        pixel = project(position, burst) + output.field *
            (max(1.20, width * px * 2.35) / max(.74, 1 + position.z * .115));
        output.emission = colorAt(seed, burst, age, position.z, life);
    }
    else
    {
        float u0 = float(segment) / TrailSegments;
        float u1 = float(segment + 1) / TrailSegments;
        float u = lerp(u0, u1, corner.x);
        float ageA = age - tail * (1 - u0);
        float ageB = age - tail * (1 - u1);
        float3 a = trajectory(seed, burst, ageA);
        float3 b = trajectory(seed, burst, ageB);
        float2 start = project(a, burst);
        float2 end = project(b, burst);
        float2 direction = end - start;
        float2 tangent = direction * rsqrt(max(.001, dot(direction, direction)));
        float width = max(.79, seed.burn.w * px * lerp(.58, 1.44, u));
        pixel = lerp(start, end, corner.x) + corner.y * width * float2(-tangent.y, tangent.x);
        output.field = float2(0, corner.y);
        float sampleAge = lerp(ageA, ageB, corner.x);
        output.emission = colorAt(seed, burst, sampleAge, lerp(a.z, b.z, corner.x), life) *
                          pow(u, .78) * exp(-(age - sampleAge) * 1.15) * .73;
    }
    output.position = float4(pixel / screen.xy * float2(2, -2) + float2(-1, 1), 0, 1);
    return output;
}

float4 sparkPS(SparkVertex input) : SV_Target
{
    float r = dot(input.field, input.field);
    float compactness = input.kind == 2 ? 4.3 : (input.kind == 1 ? 9.6 : 5.7);
    float profile = (.86 * exp(-r * compactness) + .14 * exp(-r * 2.6)) *
                    (1 - smoothstep(.71, 1, r));
    return float4(input.emission * profile, 0);
}

struct ScreenVertex
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

ScreenVertex screenVS(uint vertex : SV_VertexID)
{
    ScreenVertex output;
    output.uv = float2((vertex << 1) & 2, vertex & 2);
    output.position = float4(output.uv * float2(2,-2) + float2(-1,1),0,1);
    return output;
}

float4 blurPS(ScreenVertex input) : SV_Target
{
    float2 uv = input.uv;
    float4 filterStep = passParameters;
    // Four bilinear gathers cover the full 4×4 footprint on downsample.
    if (filterStep.z > 0) {
        float2 dx = float2(filterStep.x,0);
        float2 dy = float2(0,filterStep.y);
        float3 corners = directLight.SampleLevel(linearClamp,uv + dx + dy,0).rgb +
                         directLight.SampleLevel(linearClamp,uv + dx - dy,0).rgb +
                         directLight.SampleLevel(linearClamp,uv - dx + dy,0).rgb +
                         directLight.SampleLevel(linearClamp,uv - dx - dy,0).rgb;
        return float4(corners * .25,0);
    }
    float2 d = filterStep.xy;
    float3 light = directLight.SampleLevel(linearClamp,uv,0).rgb * .227027;
    light += (directLight.SampleLevel(linearClamp,uv + d * 1.385,0).rgb +
              directLight.SampleLevel(linearClamp,uv - d * 1.385,0).rgb) * .316216;
    light += (directLight.SampleLevel(linearClamp,uv + d * 3.231,0).rgb +
              directLight.SampleLevel(linearClamp,uv - d * 3.231,0).rgb) * .070270;
    return float4(light,0);
}

float hash(float2 p)
{
    float3 seed = frac(float3(p.xyx) * .1031);
    seed += dot(seed,seed.yzx + 33.33);
    return frac((seed.x + seed.y) * seed.z);
}

float noise(float2 p)
{
    float2 cell = floor(p);
    float2 local = frac(p);
    local = local * local * (3 - 2 * local);
    return lerp(lerp(hash(cell),hash(cell + float2(1,0)),local.x),
                lerp(hash(cell + float2(0,1)),hash(cell + 1),local.x),local.y);
}

float3 night(float2 uv, float depth)
{
    if (skyGrade[1].w != 0) return 0;
    float shore = .015 + .044 * noise(float2(uv.x * 15,8)) + .0045 * noise(float2(uv.x * 84,6.2));
    float skyline = 1 - smoothstep(shore - .001,shore + .001,-depth);
    float tint = (.57 + .43 * noise(float2(uv.x * 6,4))) * exp(-max(0,-depth) * 5.1);
    float3 sky = float3(.0018,.0032,.009) + tint * float3(.033,.014,.024);
    float3 land = float3(.00049,.00064,.00105);
    float3 water = float3(.0042,.0056,.013) * (.55 + .70 * noise(float2(uv.x * 52,uv.y * 212))) *
                   exp(-max(0,depth) * 3.5);
    float3 result = depth < 0 ? lerp(sky,land,skyline) : water;
    float column = floor(uv.x * 303);
    float across = frac(uv.x * 303) - .5;
    float seed = hash(float2(column,15));
    float lamp = exp(-pow((depth + .0026 + .0021 * seed) * 1260,2) - 8 * across * across);
    float wink = step(.936,seed) * smoothstep(.37,.76,noise(float2(uv.x * 16,7)));
    float3 lights = lerp(float3(1,.46,.16),float3(.48,.61,1),step(.977,seed));
    return gradeLayer((result + lamp * wink * lights * .034) * background.rgb * background.w, skyGrade);
}

float3 toneMap(float3 radiance, float2 uv)
{
    float2 lens = (uv - .5) * float2(.61,.78);
    radiance *= 1 - dot(lens,lens);
    float peak = max(max(radiance.r,radiance.g),radiance.b);
    float shoulder = 1 - exp(-peak * 1.43);
    float3 color = radiance * (shoulder / max(.00001,peak));
    color = lerp(color,shoulder.xxx,smoothstep(1.35,4.7,peak) * .67);
    return color;
}

float4 response(float3 radiance, float2 uv)
{
    float3 color = toneMap(radiance,uv);
    if (skyGrade[1].w != 0) {
        float3 base = saturate(gradeLayer(background.rgb * background.w, skyGrade));
        base = lerp(12.92 * base, 1.055 * pow(base, 1/2.4) - .055, step(.0031308, base));
        return float4(saturate(base + pow(saturate(color),1 / 2.18)), 1);
    }
    return float4(saturate(pow(saturate(color),1 / 2.18) +
                  (hash(floor(uv * screen.xy)) - .5) * .0038),1);
}

float4 idlePS(ScreenVertex input) : SV_Target
{
    return response(night(input.uv,input.uv.y - screen.w),input.uv);
}

float4 translucentIdlePS(ScreenVertex input) : SV_Target
{
    return idlePS(input) * passParameters.y;
}

float4 composite(ScreenVertex input, float2 opacity)
{
    float2 uv = input.uv;
    float depth = uv.y - screen.w;
    float3 direct = directLight.Sample(linearClamp,uv).rgb;
    float3 bloom = 0;
    if (atmosphere.x > 0 || atmosphere.y > 0)
        bloom = bloomLight.Sample(linearClamp,uv).rgb;
    float3 scenery = opacity.y > 0 ? night(uv,depth) : 0;
    float3 radiance = scenery + direct + atmosphere.x * bloom * .82;
    float3 fireworks = direct + atmosphere.x * bloom * .82;
    if (atmosphere.y > 0 && dot(bloom,bloom) > .00000008)
    {
        float2 advect = uv * float2(44,18) + float2(-.23 * screen.z,.10 * screen.z);
        float squall = .67 * noise(advect) + .33 * noise(advect * 2.8 + 17);
        float3 smoke = atmosphere.y * bloom * smoothstep(.25,.72,squall) * .92;
        radiance += smoke;
        fireworks += smoke;
    }
    if (depth >= 0 && atmosphere.z > 0 && opacity.y > 0)
    {
        float wave = noise(float2(uv.x * 67,depth * 241 + screen.z * .89));
        float2 mirror = float2(uv.x + (wave - .5) * (.006 + depth * .153),screen.w - depth * 3.65);
        float3 water = directLight.Sample(linearClamp,mirror).rgb * .23;
        if (atmosphere.x > 0 || atmosphere.y > 0)
            water += bloomLight.Sample(linearClamp,mirror).rgb * .22;
        float ripples = sin(depth * 1640 + wave * 7.9 + screen.z * 1.9);
        radiance += water * atmosphere.z * exp(-depth * 8) *
                    (.25 + .75 * smoothstep(-.8,.76,ripples)) * step(0,mirror.y);
    }
    float4 overNight = response(radiance,uv);
    if (opacity.x >= 1 && opacity.y >= 1)
        return overNight;

    // Reflections belong to the water. Bloom and smoke remain in the firework's coverage.
    float3 emission = pow(saturate(toneMap(fireworks,uv)),1 / 2.18) * opacity.x;
    float4 overDesktop = float4(emission,max(emission.r,max(emission.g,emission.b)));
    if (opacity.y <= 0)
        return overDesktop;

    // Blend between the firework over the desktop and over an opaque night, both premultiplied.
    overNight = lerp(response(scenery,uv),overNight,opacity.x);
    return lerp(overDesktop,overNight,opacity.y);
}

float4 compositePS(ScreenVertex input) : SV_Target
{
    return composite(input,float2(1,1));
}

float4 translucentCompositePS(ScreenVertex input) : SV_Target
{
    return composite(input,passParameters.xy);
}
