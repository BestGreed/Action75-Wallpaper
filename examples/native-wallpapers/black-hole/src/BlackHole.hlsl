cbuffer Scene : register(b0)
{
    float2 resolution;
    float2 centerPx;
    float shadowPx;
    float lensStrength;
    float diskBrightness;
    float orbitPhase;
    float2 skySize;
    float starFlare;
    float hasEmissionMaps;
    float2 bloomTexel;
    float einsteinRadiusSquared;
    float scenePadding;
    float4 diskOuter;
    float4 diskMiddle;
    float4 diskInner;
    float2 skyDrift;
    float skyPhase;
    float skyBrightness;
    float3 skyTint;
    uint skyMode;
    float skyHue, skySaturation, starHue, starSaturation;
    float3 starTint;
    float starGradeEnabled;
};

// The inverse point-mass lens samples the source sky in screen space. The
// accretion disk is an artistic approximation of the direct and bent images.
Texture2D<float4> sky : register(t0);
Texture2D<float2> turbulence : register(t1);
Texture2D<float4> sourceImage : register(t2);
Texture2D<float4> bloomImage : register(t3);
Texture2D<float2> starField : register(t4);
SamplerState skySampler : register(s0);
SamplerState turbulenceSampler : register(s1);
SamplerState emissionSampler : register(s2);

struct ScreenVertex
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

ScreenVertex ScreenVs(uint vertex : SV_VertexID)
{
    ScreenVertex output;
    output.uv = float2((vertex << 1) & 2, vertex & 2);
    output.position = float4(output.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return output;
}

float Hash(float2 p)
{
    float3 h = frac(float3(p.xyx) * float3(.1031, .11369, .13787));
    h += dot(h, h.yzx + 19.19);
    return frac((h.x + h.y) * h.z);
}

float Noise(float2 p)
{
    float2 cell = floor(p);
    float2 f = frac(p);
    f = f * f * (3 - 2 * f);
    return lerp(lerp(Hash(cell), Hash(cell + float2(1, 0)), f.x),
                lerp(Hash(cell + float2(0, 1)), Hash(cell + 1), f.x), f.y);
}

float Clouds(float2 p)
{
    const float2x2 turn = float2x2(1.76, -1.29, 1.29, 1.76);
    float v = .54 * Noise(p);
    p = mul(turn, p) + 11.6;
    v += .26 * Noise(p);
    p = mul(turn, p) - 3.7;
    v += .13 * Noise(p);
    return v + .07 * Noise(mul(turn, p) + 29.0);
}

float3 Starlight(float2 pixel, float cellSize, float density, float energy)
{
    float2 grid = pixel / cellSize;
    float2 cell = floor(grid);
    float sample = Hash(cell + float2(33.8, 9.7));
    float2 offset = float2(Hash(cell + 6.24), Hash(cell + 75.33));
    float2 p = (frac(grid) - lerp(.22, .78, offset)) * cellSize;
    float magnitude = lerp(.36, 1.3, sample * sample * sample);
    float width = lerp(.56, 1.08, magnitude);
    float radiusSquared = dot(p, p) / (width * width);
    float light = exp2(-1.65 * radiusSquared) + .10 * exp2(-.19 * radiusSquared);
    float3 tint = lerp(float3(.48, .68, 1), float3(1, .76, .56), Hash(cell - 11.6));
    return (sample < density) * energy * magnitude * light * tint;
}

// Baked once into a mipmapped sRGB texture, including all dust and faint stars.
float4 SkyPs(ScreenVertex input) : SV_Target
{
    float2 p = (input.uv - .5) * float2(2.666667, 1);
    float2 warped = p + .22 * (float2(Clouds(p * 2.1 + 27), Clouds(p * 2.1 - 45)) - .5);
    float ridge = warped.y + .23 * warped.x + .12 * sin(2.8 * warped.x - .4);
    float band = exp2(-18 * ridge * ridge);
    float gas = Clouds(warped * float2(4.2, 9.5));
    float strands = Clouds(warped * float2(11, 26) + 20 * gas);
    float mist = smoothstep(.30, .79, gas) * (.38 + .62 * strands);
    float mottling = smoothstep(.30, .68, Clouds(warped * float2(9, 18) + 29));
    float lane = smoothstep(.085, .22, abs(ridge + .025 + .14 * (mottling - .5)));
    float colorMix = smoothstep(-.64, .96, warped.x + .5 * gas);
    float3 cloudColor = lerp(float3(.0025, .0090, .024), float3(.015, .0060, .023), colorMix);
    float edge = 1 - .45 * smoothstep(.15, .75, length(input.uv - .5));
    float3 color = float3(.0010, .0017, .0044) + cloudColor * mist * band * lane * edge;
    color += float3(.0025, .0029, .0057) * band * max(0, strands - .39);
    color += Starlight(input.position.xy, 9.5, .16 + .16 * band, .047);
    color += Starlight(input.position.xy + 9.3, 20.5, .24, .115);
    color += Starlight(input.position.xy + 31, 57, .070, .59);
    return float4(color, 1);
}

float3 ToLinear(float3 srgb)
{
    return lerp(srgb / 12.92, pow((srgb + .055) / 1.055, 2.4), step(.04045, srgb));
}

float4 BakeArtworkPs(ScreenVertex input) : SV_Target
{
    uint width, height;
    sourceImage.GetDimensions(width, height);
    float aspect = float(width) / float(height);
    float2 crop = min(1, float2(2.666667 / aspect, aspect / 2.666667));
    float2 uv = .5 + (input.uv - .5) * crop;
    float3 color = ToLinear(sourceImage.SampleLevel(skySampler, uv, 0).rgb) * .43;
    color += Starlight(input.position.xy, 12.8, .16, .075);
    color += Starlight(input.position.xy + 39, 49.1, .046, .59);
    return float4(color + float3(.00038, .00052, .00093), 1);
}

float FieldStar(float2 pixel, float spacing, float density, float peak, float2 radiusRange, bool spikes)
{
    float2 grid = pixel / spacing;
    float2 cell = floor(grid);
    if (Hash(cell + 19.71) > density)
        return 0;
    float roll = Hash(cell + 73.27);
    float magnitude = roll * roll * roll;
    float radius = lerp(radiusRange.x, radiusRange.y, magnitude);
    float2 offset = float2(Hash(cell + 31.24), Hash(cell - 83.16));
    float2 p = (frac(grid) - lerp(.31, .69, offset)) * spacing / radius;
    float r2 = dot(p, p);
    float light = exp2(-2.05 * r2) + .056 * exp2(-.27 * r2);
    if (spikes)
    {
        float2 axis = p * p;
        light += .085 * magnitude * (exp2(-.13 * axis.x - 8 * axis.y)
                                    + exp2(-8 * axis.x - .13 * axis.y));
    }
    return peak * lerp(.15, 1, magnitude) * light;
}

// The two parallax layers share a mipmapped texture generated at load time.
float4 BakeStarsPs(ScreenVertex input) : SV_Target
{
    float2 p = (input.uv - .5) * float2(2.666667, 1);
    float grain = Clouds(p * float2(4.7, 10.4) + 57);
    float lane = p.y + .27 * p.x + .08 * sin(3 * p.x + .3) + .19 * (grain - .5);
    float band = exp2(-33 * lane * lane) * smoothstep(.24, .71, grain);
    float2 pixel = input.position.xy;
    float farLayer = .07 * band * (.25 + .75 * Clouds(p * float2(11, 24) + 11));
    farLayer += FieldStar(pixel, 6.8, .067 + .53 * band, .47, float2(.49, 1.06), false);
    farLayer += FieldStar(pixel + 93, 16.1, .18 + .37 * band, .88, float2(.58, 1.32), false);
    float nearLayer = FieldStar(pixel - 53, 44.5, .24 + .25 * band, .92, float2(.84, 2.42), true);
    return float4(farLayer, nearLayer, 0, 1);
}

float3 SkyLight(float2 uv)
{
    float2 offset = uv - .5;
    float2 nearUv = .5 + .875 * offset - 1.15 * skyDrift;
    float closeStars = starField.Sample(skySampler, nearUv).g;
    float shimmer = .88 + .12 * sin(dot(nearUv, float2(311, 196)) + 7 * skyPhase);
    float3 blueWhite = float3(.66, .81, 1);

    [branch] if (skyMode == 0)
    {
        float2 shear = float2(offset.y, -offset.x) * (.42 * skyDrift.y);
        float3 cloud = sky.Sample(skySampler, .5 + .915 * offset + .72 * skyDrift + shear).rgb;
        return cloud + blueWhite * (.22 * closeStars * shimmer);
    }
    [branch] if (skyMode == 1)
    {
        float2 veilUv = uv + skyDrift * float2(-1.8, .72);
        float2 dustUv = float2(.41, .73) + veilUv * float2(.062, .038);
        float dustNoise = turbulence.SampleLevel(turbulenceSampler, dustUv, 0).g;
        float lane = veilUv.y - .58 + .18 * (veilUv.x - .38) + .07 * (dustNoise - .5);
        float mist = exp2(-23 * lane * lane - 4.8 * (veilUv.x - .37) * (veilUv.x - .37));
        float veil = mist * (.22 + .78 * dustNoise) * .0105;
        float dust = .055 * closeStars * closeStars * shimmer;
        return float3(.00018, .00029, .0006) + float3(.24, .52, 1) * veil + blueWhite * dust;
    }

    float farStars = starField.Sample(skySampler, .5 + .963 * offset + .37 * skyDrift).r;
    float spec = .5 + .5 * sin(dot(nearUv, float2(137, 233)));
    float3 nearColor = lerp(blueWhite, float3(1, .82, .62), spec);
    return float3(.00028, .00047, .00095) + float3(.56, .67, 1) * (.48 * farStars)
           + nearColor * (.66 * closeStars * shimmer);
}

float Bell(float x)
{
    return exp2(-x * x);
}

float2 DiskAxes(float2 p)
{
    return float2(dot(p, float2(.962, -.273)), dot(p, float2(.273, .962)));
}

float ShadowCoverage(float radius, float aa)
{
    return smoothstep(1 - aa, 1 + aa, radius);
}

float FrontDisk(float y)
{
    return smoothstep(.18, .37, y);
}

float2 PlasmaPlane(float2 q)
{
    // Backtrace the far-side stream radially before removing the disk tilt.
    // Its nested arcs follow the shadow and spread into the outer disk.
    float farSide = 1 - smoothstep(-.56, .16, q.y);
    float strength = farSide * lensStrength;
    float deflection = saturate(1.08 * strength / max(dot(q, q), .01));
    float2 source = q * (1 - deflection);
    source.y -= .12 * lensStrength * Bell(.81 * q.x) * smoothstep(.025, .39, q.y);
    source.y /= .205 + .047 * strength;
    return source;
}

float PlasmaDensity(float distance)
{
    return smoothstep(1.16, 1.32, distance) * (1 - smoothstep(2.1, 4.72, distance));
}

float3 PlasmaTint(float farSide, float beamed, float heat)
{
    // Local heat sets the emission color. The approaching gas shifts toward the hotter end.
    float band = 2 * saturate(.88 * heat + .20 * beamed - .055 * farSide);
    float3 cooler = lerp(diskOuter.rgb, diskMiddle.rgb, saturate(band));
    return lerp(cooler, diskInner.rgb, saturate(band - 1));
}

float3 Plasma(float2 q, float visible, out float foregroundDensity)
{
    float farSide = 1 - smoothstep(-.04, .08, q.y);
    float2 plane = PlasmaPlane(q);
    float distance = length(plane);
    float gasDensity = PlasmaDensity(distance);
    foregroundDensity = gasDensity * smoothstep(-.03, .33, q.y);
    float density = gasDensity * max(visible, FrontDisk(q.y));
    [branch] if (density < .0005)
        return 0;

    float turn = atan2(plane.y, plane.x) * .159154943;
    float spiral = log2(distance);
    float2 coarse = turbulence.SampleLevel(turbulenceSampler,
        float2(turn - orbitPhase + .19 * spiral, distance * .018), 0);
    float2 filaments = turbulence.SampleLevel(turbulenceSampler,
        float2(2 * turn - 2 * orbitPhase + .31 * spiral, distance * .088 + .018 * coarse.x), 0);
    float2 fine = turbulence.SampleLevel(turbulenceSampler,
        float2(3 * turn - 4 * orbitPhase + .42 * spiral, distance * .186 + .027 * filaments.y), 0);
    float turbulent = .46 * coarse.x + .36 * filaments.x + .18 * fine.x;
    float streams = .16 + 1.08 * smoothstep(.20, .80, turbulent) * (.53 + .72 * filaments.y);
    float temperature = Bell((distance - 1.38) * 1.16);
    float knots = smoothstep(.56, .84, .49 * coarse.y + .26 * filaments.x + .25 * fine.y);
    float beamed = smoothstep(-.94, .66, -plane.x / distance);
    float3 color = PlasmaTint(farSide, beamed, temperature * (.56 + .47 * filaments.y));
    color = lerp(color, diskInner.rgb, temperature * knots * .62);
    float energy = density * streams * lerp(.66, 1.84, beamed * beamed) * lerp(.71, 1, 1 - farSide);
    return color * energy * (.32 + .92 * temperature + .81 * knots * temperature);
}

float3 LensArcs(float2 q, float radius, float visible, float aa)
{
    float night = 1 - smoothstep(-.44, .18, q.y);
    float left = 1 - smoothstep(-.92, .71, q.x);
    float glowWidth = max(.011, aa * 1.25);
    float photon = Bell((radius - 1.016) / glowWidth);
    float3 rimColor = lerp(.73 * diskMiddle.rgb, 1.14 * diskInner.rgb, left * (1 - night * .49));
    float crest = Bell((radius - 1.103 - .038 * night) / max(.024, aa));
    float turnover = Bell((radius - 1.26 - .13 * night + .037 * q.x) / max(.031, aa));
    float flare = smoothstep(.06, .92, .54 - .26 * q.x - .24 * q.y);
    float pulse = .78 + .22 * sin(6.2831853 * orbitPhase + 4.3 * q.x - 2.3 * q.y);
    float3 echo = diskMiddle.rgb * (.83 * crest * flare + .23 * turnover * night * pulse);
    float3 rim = rimColor * photon * (.32 + .94 * flare);
    return (rim + echo * lensStrength) * visible;
}

float3 Accretion(float2 q, float radius, float visible, float aa, out float foregroundDensity)
{
    float innerMist = Bell(1.23 * (radius - 1.25));
    float wings = exp2(-.14 * q.x * q.x - 4.3 * q.y * q.y);
    float3 airColor = lerp(.10 * diskOuter.rgb, .084 * diskMiddle.rgb,
                           1 - smoothstep(-2.3, .90, q.x));
    float3 light = (.024 * diskMiddle.rgb * innerMist + airColor * wings) * visible;
    light += Plasma(q, visible, foregroundDensity);
    [branch] if (radius > .965 && radius < 1.65)
        light += LensArcs(q, radius, visible, aa);
    return light;
}

float3 AdjustColor(float3 color, float turns, float saturation)
{
    float high = max(color.r, max(color.g, color.b));
    float chroma = high - min(color.r, min(color.g, color.b));
    if (chroma <= 0) return color;
    float hue = high == color.r ? (color.g - color.b) / chroma :
                high == color.g ? (color.b - color.r) / chroma + 2 : (color.r - color.g) / chroma + 4;
    float3 rgb = saturate(abs(frac(hue / 6 + turns + float3(0, 2./3., 1./3.)) * 6 - 3) - 1);
    return high * lerp(1, rgb, saturate(saturation * chroma / high));
}

float4 ScenePs(ScreenVertex input) : SV_Target
{
    float2 delta = input.position.xy - centerPx;
    float2 p = delta / shadowPx;
    float r2 = dot(p, p);
    float reach = 1 - smoothstep(8.5, 27, r2);
    // Backtrace each view ray to its source-plane angle. A negative factor
    // crosses the optical axis and samples the image on the longer path.
    float ray = 1 - einsteinRadiusSquared * reach / max(r2, .94);
    float2 source = centerPx + delta * ray;
    float3 light = 0;
    [branch] if (skyBrightness > 0)
    {
        float3 skyColor = skyMode == 3 ? 1 : SkyLight(.5 + (source - .5 * resolution) / skySize);
        light = skyColor * skyTint * skyBrightness;
        if (skyHue != 0 || skySaturation != 1) light = AdjustColor(light, skyHue, skySaturation);
    }
    [branch] if (r2 < 27)
    {
        float r = sqrt(r2);
        float aa = max(.003, .72 / shadowPx);
        float visible = ShadowCoverage(r, aa);
        float2 q = DiskAxes(p);
        light *= visible;
        float foregroundDensity = 0;
        [branch] if (diskBrightness > 0 && abs(q.y) < 3.05)
        {
            float diskFade = (1 - smoothstep(2.25, 3.05, abs(q.y))) * (1 - smoothstep(19, 27, r2));
            light += diskBrightness * Accretion(q, r, visible, aa, foregroundDensity) * diskFade;
            foregroundDensity *= diskFade;
        }
        [branch] if (hasEmissionMaps > 0 && visible > .0001)
        {
            float2 uv = source / resolution;
            float3 tracedLight;
            [branch] if (ray > .025)
                tracedLight = sourceImage.SampleLevel(emissionSampler, uv, 0).rgb;
            else if (ray < -.025)
                tracedLight = bloomImage.SampleLevel(emissionSampler, uv, 0).rgb;
            else
                tracedLight = lerp(bloomImage.SampleLevel(emissionSampler, uv, 0).rgb,
                                   sourceImage.SampleLevel(emissionSampler, uv, 0).rgb,
                                   smoothstep(-.025, .025, ray));
            float transmission = exp2(-1.3 * foregroundDensity * diskBrightness);
            light += visible * transmission * (1 - smoothstep(21, 27, r2)) * tracedLight;
        }
    }
    return float4(light, 1);
}

struct StarData
{
    float4 geometry; // Center in pixels, radius in pixels, rotation.
    float4 light;    // Spectrum and radiance in the HDR scene.
    float4 motion;   // Age, seed, temperature, optical source weight.
    float4 echo;     // The same spectrum and radiance at the retarded time.
    float4 echoGeometry; // Emitter radius at the retarded time.
};

cbuffer Stars : register(b1)
{
    StarData stars[BLACK_HOLE_MAX_STARS];
};

struct StarVertex
{
    float4 position : SV_Position;
    float2 local : TEXCOORD0;
    nointerpolation float4 light : TEXCOORD1;
    nointerpolation float lensMix : TEXCOORD2;
    nointerpolation float4 motion : TEXCOORD3;
    nointerpolation float4 diskFlow : TEXCOORD4;
    nointerpolation float4 echo : TEXCOORD5;
    nointerpolation float echoScale : TEXCOORD6;
};

StarVertex StarVs(uint vertex : SV_VertexID, uint instance : SV_InstanceID)
{
    StarData star = stars[instance];
    StarVertex output;
    float2 corner = float2(vertex & 1, vertex >> 1) * 2 - 1;
    float size = star.geometry.z;
    float2 pixel = star.geometry.xy + corner * size;
    output.position = float4(pixel / resolution * float2(2, -2) + float2(-1, 1), 0, 1);
    float sine, cosine;
    sincos(star.geometry.w, sine, cosine);
    float2x2 rotation = float2x2(cosine, -sine, sine, cosine);
    output.local = mul(rotation, corner);
    output.light = star.light;
    output.echo = star.echo;
    output.echoScale = size / star.echoGeometry.x;
    float2 delta = star.geometry.xy - centerPx;
    float shadowRadius = length(delta) / shadowPx;
    float2 q = DiskAxes(delta / shadowPx);
    float behind = 1 - smoothstep(-.11, .42, q.y);
    output.lensMix = star.motion.w * lerp(.57, .88, behind);
    output.motion = float4(star.motion.xy, 1 / size, star.motion.z);
    output.diskFlow = float4(1, 0, 0, 0);

    // Sample the same projected disk as the background, once per vertex.
    // The gradient of orbital radius gives the local direction of the gas.
    [branch] if (shadowRadius < 4.8 && diskBrightness > 0)
    {
        float2 plane = PlasmaPlane(q);
        float orbitalRadius = length(plane);
        float coverage = max(FrontDisk(q.y), ShadowCoverage(shadowRadius, .72 / shadowPx));
        float density = PlasmaDensity(orbitalRadius) * coverage;
        float mixWithDisk = smoothstep(.035, .56, density) * smoothstep(.05, .75, diskBrightness);
        if (mixWithDisk > 0)
        {
            const float stepSize = .008;
            float2 gradient = float2(length(PlasmaPlane(q + float2(stepSize, 0))),
                                     length(PlasmaPlane(q + float2(0, stepSize)))) - orbitalRadius;
            float2 tangent = float2(-gradient.y, gradient.x);
            float2 screen = float2(dot(tangent, float2(.962, .273)), dot(tangent, float2(-.273, .962)));
            float2 flow = normalize(mul(rotation, screen) + float2(1e-6, 0));
            float farSide = 1 - smoothstep(-.04, .08, q.y);
            float beamed = smoothstep(-.94, .66, -plane.x / orbitalRadius);
            float3 gas = PlasmaTint(farSide, beamed, .8 * Bell((orbitalRadius - 1.38) * 1.16));
            gas /= max(gas.r, max(gas.g, gas.b));
            output.light.rgb = lerp(star.light.rgb, gas, .42 * mixWithDisk);
            output.echo.rgb = lerp(star.echo.rgb, gas, .42 * mixWithDisk);
            float scaledRadius = orbitalRadius / 1.65;
            float angularSpeed = 1.27 * rsqrt(scaledRadius) / scaledRadius;
            output.diskFlow = float4(flow, mixWithDisk, angularSpeed);
        }
    }
    if (starGradeEnabled != 0)
    {
        output.light.rgb = AdjustColor(output.light.rgb * starTint, starHue, starSaturation);
        output.echo.rgb = AdjustColor(output.echo.rgb * starTint, starHue, starSaturation);
    }
    return output;
}

float DiffractionRay(float along, float across, float extent, float width)
{
    float taper = 1 - smoothstep(.025, extent, along);
    float slit = across / (width + .009 * taper);
    float wing = taper / (1 + slit * slit);
    return wing * wing;
}

struct LightPaths
{
    float4 primary : SV_Target0;
    float4 secondary : SV_Target1;
};

float EmitterGlow(float shape, float r2, float texelWidth)
{
    // Blur the narrow core by the quarter-resolution texel footprint and preserve its energy.
    float filter = 1 + 17.8 * texelWidth * texelWidth;
    float glow = (1.14 / filter) * exp2(-154 * shape / filter)
               + .61 * exp2(-29 * shape) + .075 * exp2(-7 * shape);
    return glow * (1 - smoothstep(.58, 1, r2));
}

LightPaths LightSourcePs(StarVertex input)
{
    float2 p = input.local;
    float r2 = dot(p, p);
    clip(1 - r2);
    clip(input.lensMix - .0001);

    // A finite photosphere and diffuse envelope are stretched by the ray mapping.
    float2 flow = input.diskFlow.xy;
    float2 plasma = float2(.78 * dot(p, flow), dot(p, float2(-flow.y, flow.x)));
    float shape = lerp(r2, dot(plasma, plasma), input.diskFlow.z);
    float width = input.motion.z * max(resolution.x * bloomTexel.x, resolution.y * bloomTexel.y);
    float scale = input.echoScale * input.echoScale;
    float primary = EmitterGlow(shape, r2, width);
    float secondary = EmitterGlow(shape * scale, r2 * scale, width * input.echoScale);
    LightPaths result;
    result.primary = float4(lerp(input.light.rgb, 1, .27) * (input.light.a * input.lensMix * primary), 0);
    result.secondary = float4(lerp(input.echo.rgb, 1, .27) * (input.echo.a * input.lensMix * secondary), 0);
    return result;
}

float4 StarPs(StarVertex input) : SV_Target
{
    float2 p = input.local;
    float r2 = dot(p, p);
    clip(1 - r2);
    float shadowRadius = length(input.position.xy - centerPx) / shadowPx;
    float visible = ShadowCoverage(shadowRadius, .72 / shadowPx);
    clip(visible - .0001);

    float age = input.motion.x;
    float seed = input.motion.y;
    float pixelWidth = input.motion.z;
    float heat = saturate(input.motion.w);
    float disk = input.diskFlow.z;
    float3 color = input.light.rgb;
    float3 hotColor = lerp(color, float3(1, .97, .91), .23 + .65 * heat);
    float core = exp2(-lerp(840, 510, disk) * r2 / (.73 + .27 * heat));
    float photosphere = exp2(-lerp(147, 104, disk) * r2);
    float3 light = hotColor * (lerp(3.15, 1.58, disk) * core + lerp(.86, .51, disk) * photosphere);

    // A widening Gaussian spreads the same energy over a larger area as it cools.
    float haloVariance = .008 + .026 * min(age, .95);
    float halo = (.008 / haloVariance) * exp2(-.7213475 * r2 / haloVariance);
    float wing = rcp(1 + 14 * r2);
    light += (1 - disk) * color * (.53 * halo + .034 * wing * wing);

    float raysVisible = smoothstep(.29, .82, heat);
    [branch] if (starFlare > 0 && disk < .999 && raysVisible > 0)
    {
        float2 axis = abs(p);
        float rayWidth = max(.0038, pixelWidth * .69);
        float extent = lerp(.41, .93, heat);
        float horizontal = DiffractionRay(axis.x, axis.y, extent, rayWidth);
        float vertical = DiffractionRay(axis.y, axis.x, extent * (.66 + .21 * seed), rayWidth);
        float2 diagonal = abs(float2(p.x + p.y, p.x - p.y) * .7071068);
        float split = DiffractionRay(diagonal.x, diagonal.y, extent * .59, rayWidth * .82) +
                      .62 * DiffractionRay(diagonal.y, diagonal.x, extent * .51, rayWidth * .79);
        float3 beam = lerp(color, hotColor, .36) * (.62 * horizontal + .46 * vertical);
        beam += color * (.16 * split + .13 * DiffractionRay(axis.x, axis.y, extent, rayWidth * 2.8));
        light += ((1 - disk) * starFlare * raysVisible) * beam;
    }
    [branch] if (disk > 0)
    {
        float2 tangent = input.diskFlow.xy;
        float along = dot(p, tangent);
        float crossFlow = dot(p, float2(-tangent.y, tangent.x));
        crossFlow -= .15 * min(1.2, 1 / (shadowPx * pixelWidth)) * along * along;
        // Local Keplerian shear. The inward side advances faster. Backtrace the
        // wake only, leaving the excitation and its core centered on the key.
        float diffusionTime = min(age, 1.55);
        float phase = input.diskFlow.w * diffusionTime;
        float drift = .60 * (1 - exp2(-1.65 * phase));
        float sheared = along - drift - 1.5 * phase * crossFlow;
        float longVariance = .014 + .096 * diffusionTime;
        float crossVariance = .0026 + .0085 * diffusionTime;
        float distance = sheared * sheared / longVariance + crossFlow * crossFlow / crossVariance;
        float dilution = sqrt((.014 * .0026) / (longVariance * crossVariance));
        float spread = exp2(-.7213475 * distance);
        float sheath = exp2(-.29 * distance);
        float wake = dilution * ((.51 + .38 * starFlare) * spread + .11 * sheath);
        light += disk * lerp(color, hotColor, .27) * wake * smoothstep(.025, .165, age);
    }
    float edge = 1 - smoothstep(.57, 1, r2);
    return float4(light * (1 - .78 * input.lensMix) * input.light.a * edge * visible, 0);
}

float3 BloomSeed(float2 uv)
{
    float3 c = sourceImage.SampleLevel(skySampler, uv, 0).rgb;
    return max(0, c - .12) * smoothstep(.24, .92, max(c.r, max(c.g, c.b)));
}

float4 BloomHorizontalPs(ScreenVertex input) : SV_Target
{
    float2 stepX = float2(bloomTexel.x * 1.62, 0);
    float2 down = float2(0, bloomTexel.y * .31);
    float2 uv = input.uv;
    float3 sum = (BloomSeed(uv - down) + BloomSeed(uv + down)) * .1135;
    sum += (BloomSeed(uv + stepX * 1.3846) + BloomSeed(uv - stepX * 1.3846)) * .3162;
    sum += (BloomSeed(uv + stepX * 3.2308) + BloomSeed(uv - stepX * 3.2308)) * .0703;
    return float4(sum, 1);
}

float4 BloomVerticalPs(ScreenVertex input) : SV_Target
{
    float2 stepY = float2(0, bloomTexel.y * 1.12);
    float2 uv = input.uv;
    float3 sum = sourceImage.SampleLevel(skySampler, uv, 0).rgb * .227;
    sum += (sourceImage.SampleLevel(skySampler, uv + stepY * 1.3846, 0).rgb +
            sourceImage.SampleLevel(skySampler, uv - stepY * 1.3846, 0).rgb) * .3162;
    sum += (sourceImage.SampleLevel(skySampler, uv + stepY * 3.2308, 0).rgb +
            sourceImage.SampleLevel(skySampler, uv - stepY * 3.2308, 0).rgb) * .0703;
    return float4(sum, 1);
}

float4 CompositePs(ScreenVertex input) : SV_Target
{
    float3 scene = sourceImage.SampleLevel(skySampler, input.uv, 0).rgb;
    float3 bloom = bloomImage.SampleLevel(skySampler, input.uv, 0).rgb;
    float2 p = (input.position.xy - centerPx) / shadowPx;
    float bloomMask = max(smoothstep(.92, 1.08, length(p)), smoothstep(.20, .41, DiskAxes(p).y));
    float3 light = scene + bloom * .55 * bloomMask;
    float3 film = 1 - exp2(-1.29 * light);
    float3 display = lerp(film * 12.92, 1.055 * pow(film, 1.0 / 2.4) - .055, step(.0031308, film));
    float grain = frac(52.9829189 * frac(dot(input.position.xy, float2(.06711056, .00583715)))) - .5;
    return float4(display + grain / 255, 1);
}
