struct Impact {
    float4 position; // Normalized center, crack reach, opacity.
    float4 motion;   // Rotation cosine and sine, peak force, pattern index.
    float4 reaction; // Time since press, latest force.
};
cbuffer Frame : register(b0) {
    float4 screen;   // Width, height, 720p scale, background style.
    float4 geometry; // Crack and pane counts per pattern, reference radius, core scale.
    float4 shadow;
    float4 reflection; // RGB and background brightness.
    float4 rim;
    float4 prism;
    float4 optics;   // Spread, seam width, glints, refraction.
    float4 baseShadow, baseReflection;
    Impact impacts[24];
};
struct Crack {
    float2 start, end, normalAtStart, normalAtEnd;
    float begin, endTime, widthAtStart, widthAtEnd;
    float phase, luster, minForce;
};
struct Pane {
    float2 corners[4];
    float2 pivot, drift, slope;
    float spin, delay, thickness, luster;
    float kind, reserved;
};
StructuredBuffer<Crack> cracks : register(t0);
StructuredBuffer<Pane> panes : register(t1);

float4 clipPosition(float2 pixel) {
    return float4(pixel / screen.xy * float2(2, -2) + float2(-1, 1), 0, 1);
}
float2 rotate(float2 p, float2 rotation) {
    return float2(p.x * rotation.x - p.y * rotation.y, p.x * rotation.y + p.y * rotation.x);
}
float2 impactPixel(Impact impact, float2 p) {
    return impact.position.xy * screen.xy + rotate(p, impact.motion.xy) * geometry.z * screen.z;
}
float highlightBand(float distance, float width) {
    float scaled = distance / width;
    return exp2(-scaled * scaled);
}

float3 glassBackdrop(float2 uv) {
    if (screen.w == 3) return baseShadow.rgb * reflection.a;
    float2 p = uv - .5;
    float broad, edge, wash;
    if (screen.w == 0) {
        float angle = p.y + p.x * .62;
        broad = .48 * highlightBand(angle + .18, .115) + .16 * highlightBand(angle - .01, .22);
        edge = .10 * highlightBand(angle + .256, .007) + .042 * highlightBand(angle + .228, .004);
        wash = .23 * highlightBand(uv.x - .22, .39) * highlightBand(uv.y - .27, .49);
    } else if (screen.w == 1) {
        float angle = p.y - p.x * .45;
        broad = .39 * highlightBand(angle - .28, .155) + .12 * highlightBand(angle - .11, .27);
        edge = .13 * highlightBand(angle - .32, .012) + .055 * highlightBand(angle - .365, .005);
        wash = .15 * highlightBand(uv.x - .8, .32) * highlightBand(uv.y - .09, .39);
    } else {
        float angle = p.x + p.y * .24;
        broad = .43 * highlightBand(angle - .29, .096) + .15 * highlightBand(angle - .17, .19);
        edge = .13 * highlightBand(angle - .329, .004) + .073 * highlightBand(angle - .281, .008);
        wash = .15 * highlightBand(uv.x - .12, .32) * highlightBand(uv.y - .73, .62);
    }
    float vignette = 1 - dot(p, p * float2(.54, .74));
    float glaze = broad * (.46 + .54 * highlightBand(p.x, .45));
    return (baseShadow.rgb + baseReflection.rgb * (glaze + edge + wash)) * vignette * reflection.a;
}

float4 backdropVS(uint vertex : SV_VertexID) : SV_Position {
    float2 p = float2((vertex << 1) & 2, vertex & 2);
    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 backdropPS(float4 position : SV_Position) : SV_Target {
    float grain = frac(52.9829189 * frac(dot(floor(position.xy), float2(.06711056, .00583715)))) - .5;
    float3 color = glassBackdrop(position.xy / screen.xy) + grain * (.007 * saturate(reflection.a)) * (screen.w != 3);
    return float4(saturate(color) * baseShadow.a, baseShadow.a);
}

struct CrackVertex {
    float4 position : SV_Position;
    float4 coordinates : TEXCOORD0; // Across in pixels, seam width, segment progress, remaining reach.
    nointerpolation float4 surface : TEXCOORD1; // Opacity, facing, phase, luster.
    nointerpolation float2 normal : TEXCOORD2;
};
CrackVertex crackVS(uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
    Impact impact = impacts[instance];
    Crack crack = cracks[uint(impact.motion.w) * uint(geometry.x) + vertex / 6];
    CrackVertex result = (CrackVertex)0;
    if (impact.position.z <= crack.begin || impact.motion.z < crack.minForce) {
        result.position = float4(-3, -3, 0, 1);
        return result;
    }
    float reveal = saturate((impact.position.z - crack.begin) / (crack.endTime - crack.begin));
    static const uint corners[] = {0, 1, 2, 2, 1, 3};
    uint corner = corners[vertex % 6];
    float along = (corner & 2) ? reveal : 0;
    float2 across = rotate(lerp(crack.normalAtStart, crack.normalAtEnd, along), impact.motion.xy);
    float2 normal = normalize(rotate(crack.normalAtStart + crack.normalAtEnd, impact.motion.xy));
    float side = (corner & 1) ? 1 : -1;
    float width = lerp(crack.widthAtStart, crack.widthAtEnd, along) * optics.y * screen.z;
    // Bound each end by its widest visible band as the fracture tapers.
    float lipReach = 1.36 * width + .365 * screen.z + .88;
    float lensReach = optics.w > 0 ? width + 1.68 * screen.z : 0;
    float support = max(1.2, max(lipReach, lensReach));
    float time = lerp(crack.begin, crack.endTime, along);
    result.coordinates = float4(side * support, width, along, impact.position.z - time);
    result.position = clipPosition(impactPixel(impact, lerp(crack.start, crack.end, along) * optics.x) +
                                   across * support * side);
    result.surface = float4(impact.position.w * (.69 + .31 * saturate(impact.motion.z)),
                            dot(normal, float2(-.56, -.83)), crack.phase, crack.luster);
    result.normal = normal;
    return result;
}
float antialiasedBand(float distance, float halfWidth, float pixel) {
    float inverse = .5 / pixel;
    return saturate(.5 + (distance + halfWidth) * inverse) -
           saturate(.5 + (distance - halfWidth) * inverse);
}
float4 crackPS(CrackVertex input) : SV_Target {
    float side = input.coordinates.x;
    float width = input.coordinates.y;
    float along = input.coordinates.z;
    float facing = input.surface.y;
    float phase = input.surface.z;
    float rough = 1 - abs(2 * frac(dot(input.position.xy, float2(.076, .053)) + phase) - 1);
    float glintCenter = .17 + .66 * phase;
    float glintExtent = .11 + .21 * frac(phase * 8.191);
    float catching = 1 - smoothstep(glintExtent * .13, glintExtent, abs(along - glintCenter));
    float aa = max(.42, .5 * fwidth(side));
    float lightSide = facing < 0 ? -1 : 1;
    float seamWidth = width * (.56 + .20 * rough);
    float offset = lightSide * (seamWidth + .30 * screen.z);
    float3 transmitted = (screen.w == 3 ? baseShadow.rgb : baseShadow.rgb + .20 * baseReflection.rgb) * reflection.a;
    float lens = 0;
    [branch] if (optics.w > 0) {
        float2 displacement = input.normal * sign(side) * optics.w * (1.65 + 2.65 * abs(facing)) * screen.z;
        transmitted = glassBackdrop((input.position.xy + displacement) / screen.xy);
        lens = (1 - smoothstep(width + .18, width + 1.68 * screen.z, abs(side))) * saturate(optics.w * .48);
    }
    float backdropLight = smoothstep(.038, .21, dot(transmitted, float3(.2126, .7152, .0722)));
    float lip = antialiasedBand(side - offset, .065 * screen.z + width * (.37 + .23 * rough), aa);
    float glintGain = optics.z * input.surface.w * (.14 + 2.33 * catching * catching) *
                      (.44 + .56 * abs(facing)) * lerp(1.65, 1.20, backdropLight);
    lip = saturate(lip * glintGain);
    float shadowGain = lerp(.71, .95, backdropLight) * (.77 + .23 * rough);
    float dark = shadowGain * antialiasedBand(side, seamWidth, aa);

    float alpha = dark + lip * (1 - dark);
    float3 color = shadow.rgb * .065 * dark +
                   lerp(prism.rgb, rim.rgb, .76 + .24 * catching) * lip * (1 - dark);
    lens *= 1 - alpha;
    color += transmitted * lens;
    alpha += lens;
    float tip = smoothstep(0, .041, input.coordinates.w);
    return float4(color / max(.0001, alpha), alpha * input.surface.x * tip);
}

struct PaneVertex {
    float4 position : SV_Position;
    float3 barycentric : TEXCOORD0;
    float2 local : TEXCOORD1;
    nointerpolation float4 material : TEXCOORD2; // Refraction offset, reflection, opacity.
    nointerpolation float4 face : TEXCOORD3; // Boundary and lighting data, then piece kind.
    nointerpolation float3 optical : TEXCOORD4; // Attached face tint, chip slope and thickness, or contact flash.
};

static const uint raySegments = 7;
static const uint rayEdges = raySegments + 1; // The first edge joins the ray to the impact.
static const uint contourEdges = 4;
static const uint verticesPerAttachedPane = 3 * (2 * rayEdges + contourEdges);
static const uint attachedVertices = 5 * verticesPerAttachedPane;

struct RayPortion {
    float2 tip;
    uint start;
    uint lastSegment;
};

float2 pointOnRadius(float2 a, float2 b, float radiusSquared) {
    float2 edge = b - a;
    float lengthSquared = dot(edge, edge);
    float projection = dot(a, edge);
    float remaining = max(0, projection * projection -
                                  lengthSquared * (dot(a, a) - radiusSquared));
    return lerp(a, b, saturate((sqrt(remaining) - projection) / max(lengthSquared, .00001)));
}

RayPortion tracePaneEdge(Impact impact, uint ray, float radius) {
    RayPortion section = (RayPortion)0;
    section.start = uint(impact.motion.w) * uint(geometry.x) + ray * raySegments;
    if (impact.motion.z < cracks[section.start].minForce)
        return section;
    float reach = min(impact.position.z * .91, cracks[section.start + raySegments - 1].endTime * .87);
    float scaledRadius = radius / optics.x;
    float limit = scaledRadius * scaledRadius;
    [loop] for (uint segment = 0; segment < raySegments; ++segment) {
        Crack crack = cracks[section.start + segment];
        float reveal = saturate((reach - crack.begin) / (crack.endTime - crack.begin));
        if (reveal <= 0)
            break;
        float2 end = lerp(crack.start, crack.end, reveal);
        float distance = dot(end, end);
        section.tip = distance >= limit ? pointOnRadius(crack.start, end, limit) : end;
        section.lastSegment = segment;
        if (distance >= limit || reveal < 1)
            break;
    }
    section.tip *= optics.x;
    return section;
}

float2 paneRayPoint(RayPortion ray, uint node) {
    if (node == 0)
        return 0;
    if (node > ray.lastSegment + 1)
        return ray.tip;
    return cracks[ray.start + node - 1].start * optics.x;
}

float2 paneContourPoint(float2 start, float2 end, float2 variation, uint node, float flex) {
    if (node == 0)
        return start;
    if (node == contourEdges)
        return end;
    float fraction = node * .25;
    float along = fraction + .095 * (node == 2 ? variation.x : -variation.y);
    float2 chord = end - start;
    float2 outward = float2(chord.y, -chord.x);
    float bend = 4 * fraction * (1 - fraction) * (.83 + .29 * variation.y);
    return lerp(start, end, along) + outward * (flex * bend);
}

PaneVertex attachedPaneVS(uint vertex, Impact impact) {
    PaneVertex result = (PaneVertex)0;
    if (geometry.w <= 0) {
        result.position = float4(-3, -3, 0, 1);
        return result;
    }
    // Principal rays are stored in length order. This is their order around the impact.
    static const uint rayOrder[] = {0, 3, 1, 4, 2};
    // Neighboring pieces end at different points along a shared ray, creating T-junctions.
    static const float2 lengths[] = {
        float2(1.04, 1.26), float2(.84, .21), float2(.26, .60),
        float2(.32, 1.24), float2(.17, .47)
    };
    static const float curvature[] = {.18, -.084, .042, -.105, -.13};
    static const float clarity[] = {1, .66, .91, .95, .71};
    uint paneIndex = vertex / verticesPerAttachedPane;
    uint leftRay = rayOrder[paneIndex], rightRay = rayOrder[(paneIndex + 1) % 5];
    uint pattern = uint(impact.motion.w);
    uint shape = (paneIndex + pattern + 3) % 5;
    float2 variation = frac(float2(.75488, .56984) * (1.7 + pattern * 2.3 + float(paneIndex) * 1.86));
    float force = .23 + .77 * sqrt(saturate(impact.motion.z / 2.4));
    float radius = (.14 + geometry.w * (.12 + .165 * geometry.w)) * force;
    float2 size = radius * lengths[shape] * (.93 + .15 * variation);
    RayPortion a = tracePaneEdge(impact, leftRay, size.x);
    RayPortion b = tracePaneEdge(impact, rightRay, size.y);
    if (dot(a.tip, a.tip) == 0 || dot(b.tip, b.tip) == 0) {
        result.position = float4(-3, -3, 0, 1);
        return result;
    }
    uint edge = (vertex % verticesPerAttachedPane) / 3;
    uint corner = vertex % 3;
    bool contour = edge >= 2 * rayEdges;
    // Every rim point is either on the bowed contour or on the exact main-crack polyline.
    float2 cornerPosition = (a.tip + b.tip) * .16;
    if (corner != 0) {
        uint node = corner - 1;
        if (contour) {
            float scale = geometry.w / 3;
            float flex = curvature[shape] + (shape == 0 ? .33 * scale * scale : 0);
            cornerPosition = paneContourPoint(a.tip, b.tip, variation, edge - 2 * rayEdges + node, flex);
        } else if (edge < rayEdges) {
            cornerPosition = paneRayPoint(a, edge + node);
        } else {
            cornerPosition = paneRayPoint(b, edge - rayEdges + node);
        }
    }
    float2 slope = rotate(variation * 2 - 1, impact.motion.xy);
    float angle = saturate(.47 + .53 * dot(slope, float2(-.48, -.87)));
    float tint = variation.y;
    result.position = clipPosition(impactPixel(impact, cornerPosition));
    result.barycentric.x = corner == 0;
    result.local = cornerPosition;
    result.material = float4(slope * (16 * optics.w * screen.z) / screen.xy,
                             tint, clarity[shape] * saturate(.48 + impact.motion.z * .41) * impact.position.w);
    result.face.x = contour;
    result.face.y = angle;
    result.optical = lerp(shadow.rgb * (.48 + .38 * tint), reflection.rgb * .91,
                          (.27 + .42 * tint) * angle);
    return result;
}

PaneVertex paneVS(uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
    Impact impact = impacts[instance];
    if (vertex < attachedVertices)
        return attachedPaneVS(vertex, impact);
    vertex -= attachedVertices;
    Pane pane = panes[uint(impact.motion.w) * uint(geometry.y) + vertex / 6];
    PaneVertex result = (PaneVertex)0;
    uint index = vertex % 6;
    static const uint corners[] = {0, 1, 2, 0, 2, 3};
    float2 pieceCorner = pane.corners[corners[index]];
    result.face.w = pane.kind;
    if (pane.kind == 2) {
        if (geometry.w <= 0) {
            result.position = float4(-3, -3, 0, 1);
            return result;
        }
        float extent = .18 + .52 * sqrt(saturate(impact.motion.z / 2.4));
        result.position = clipPosition(impactPixel(impact, pieceCorner * extent));
        result.local = pieceCorner / .095;
        result.material.w = (.57 + .30 * saturate(impact.motion.z)) * impact.position.w;
        result.optical.z = 1 - smoothstep(.002, .060, impact.reaction.x);
        result.optical.x = impact.motion.w;
        return result;
    }

    float time = impact.reaction.x - pane.delay;
    if (time <= 0 || time >= .46 || impact.reaction.y < .25) {
        result.position = float4(-3, -3, 0, 1);
        return result;
    }

    float strength = impact.reaction.y;
    float distance = time * (.92 - .44 * time) * (.42 + .45 * sqrt(strength));
    float angle = pane.spin * time;
    float2 rotation;
    sincos(angle, rotation.y, rotation.x);
    float tilt = cos(time * (6.9 + abs(pane.spin)) + pane.luster * 1.6);
    float2 local = pieceCorner - pane.pivot;
    local.x *= .18 + .82 * abs(tilt);
    float2 position = impactPixel(impact, pane.pivot + pane.drift * distance + rotate(local, rotation));
    position.y += time * time * 101 * screen.z;
    result.position = clipPosition(position);
    result.local = local;
    uint corner = index % 3;
    result.barycentric = float3(corner == 0, corner == 1, corner == 2);
    // The diagonal shared by the two triangles is inside one piece of glass.
    result.face.xyz = index < 3 ? float3(1, 0, 1) : float3(1, 1, 0);
    float2 slope = rotate(rotate(pane.slope, rotation), impact.motion.xy);
    float2 refraction = slope * (9 + 15 * (1 - abs(tilt))) * pane.thickness * optics.w;
    float glint = saturate(dot(slope, float2(-.48, -.87)) * .58 + .42);
    float reflection = saturate(pane.luster * optics.z * (.13 + .51 * glint * glint + .17 * (1 - abs(tilt))));
    float life = smoothstep(0, .030, time) * (1 - smoothstep(.20, .46, time));
    float opacity = .83 * life * saturate(.51 + .31 * strength) * impact.position.w;
    result.material = float4(refraction * screen.z / screen.xy, reflection, opacity);
    result.optical = float3(slope, pane.thickness * screen.z);
    return result;
}
float4 panePS(PaneVertex input) : SV_Target {
    if (input.face.w == 2) {
        float2 p = float2(input.local.x + .18 * input.local.y, input.local.y * 1.23);
        float radius = length(p);
        float contour = .57 + .16 * abs(p.x - .50 * p.y) - .12 * abs(p.y + p.x);
        float coverage = 1 - smoothstep(contour - .27, contour, radius);
        float split = highlightBand(p.x - .38 * p.y + .078, .065) * saturate(-p.y * 1.6 + .34);
        float glint = highlightBand(radius - contour + .12, .14) *
                      saturate(-p.x - .76 * p.y + .26);
        float light = saturate(optics.z * (.034 + .57 * glint + .54 * split + .18 * input.optical.z));
        return float4(lerp(shadow.rgb * .17, rim.rgb, light), input.material.w * coverage);
    }

    if (input.face.w == 0) {
        float dx = ddx(input.barycentric.x), dy = ddy(input.barycentric.x);
        float grad = max(.0001, abs(dx) + abs(dy));
        float nearest = input.barycentric.x / grad;
        float tint = input.material.z;
        float alpha = .17 + .15 * tint;
        float lensWidth = max(4, 12 * screen.z);
        [branch] if (nearest >= lensWidth)
            return float4(input.optical, alpha * input.material.w);

        bool contour = input.face.x > 0;
        float feather = contour ? smoothstep(0, max(3.5, 5.7 * screen.z), nearest) : 1;
        alpha *= feather;
        float3 color = input.optical * alpha;

        // Only the narrow real edges resample the backdrop. The wide faces transmit the existing image.
        float refraction = (1 - smoothstep(1.5, lensWidth, nearest)) *
                           saturate(.62 * optics.w) * feather * (1 - alpha);
        [branch] if (refraction > .006) {
            float3 transmitted = glassBackdrop(input.position.xy / screen.xy + input.material.xy);
            color += transmitted * refraction;
            alpha += refraction;
        }
        float facing = (dx * .48 + dy * .87) / grad;
        float catchLight = smoothstep(.25, .82, facing + input.face.y * .29);
        float lengthwise = abs(2 * frac(dot(input.local, float2(1.17, .81)) + tint) - 1);
        float intermittent = smoothstep(.57, .98, lengthwise);
        float lip = (1 - smoothstep(.19, max(.84, 1.30 * screen.z), nearest)) *
                    (contour ? .95 : .09);
        float reflection = saturate(lip * catchLight * (.038 + .73 * intermittent) * optics.z);
        float dark = lip * (.085 + .15 * tint) * (1 - catchLight);
        float edgeAlpha = saturate(reflection + dark);
        color = color * (1 - edgeAlpha) + rim.rgb * reflection + shadow.rgb * .10 * dark;
        alpha = alpha * (1 - edgeAlpha) + edgeAlpha;
        return float4(color / max(alpha, .0001), alpha * input.material.w);
    }

    float3 dx = ddx(input.barycentric);
    float3 dy = ddy(input.barycentric);
    float3 edgeDistance = input.barycentric / max(abs(dx) + abs(dy), .0001);
    edgeDistance += (1 - step(.1, input.face.xyz)) * 1000;
    float nearest = min(edgeDistance.x, min(edgeDistance.y, edgeDistance.z));
    float3 edgeMask = float3(nearest == edgeDistance.x, nearest == edgeDistance.y, nearest == edgeDistance.z);
    float facing = dot(dx * .48 + dy * .87, edgeMask) /
                   max(.001, dot(abs(dx) + abs(dy), edgeMask));
    float bevel = 1 - smoothstep(.28, max(1.12, 1.90 * input.optical.z), nearest);
    float3 transmitted = glassBackdrop(input.position.xy / screen.xy + input.material.xy);
    float shimmer = input.material.z;
    float reflectionBand = highlightBand(dot(input.local, float2(.61, -.79)) +
                                         .055 * input.optical.y, .031 + .043 * shimmer);
    float3 glass = transmitted * (.89 + .17 * shimmer) +
                   rim.rgb * (.20 + .14 * reflectionBand) * shimmer;
    float edgeLight = smoothstep(.18, .82, facing + .17 * dot(input.optical.xy, float2(-.48, -.87)));
    float3 edgeColor = lerp(transmitted * .56, lerp(prism.rgb, rim.rgb, .80),
                            edgeLight * .77 * shimmer);
    return float4(lerp(glass, edgeColor, bevel * .86), input.material.w);
}
