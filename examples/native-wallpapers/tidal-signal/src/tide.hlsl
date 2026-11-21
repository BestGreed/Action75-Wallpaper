cbuffer Beacon : register(b0) {
    float2 size; float2 center;
    float3 tint; float strength;
    float age; float visible; float foreground; float background;
};
float4 vs(uint id : SV_VertexID) : SV_Position {
    return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1);
}
float4 ps(float4 position : SV_Position) : SV_Target {
    float2 uv = position.xy / size;
    float2 delta = (uv - center) * float2(size.x / size.y, 1);
    float distance = length(delta);
    float3 sky = lerp(float3(.019,.034,.066), float3(.058,.094,.15), saturate(uv.y));
    float tide = sin(uv.x * 6.6 + uv.y * 2.7) * .058 + sin(uv.x * 18 - uv.y * 3) * .011;
    float arc = abs(frac((uv.y + tide) * 17) - .5);
    sky += float3(.025,.046,.074) * (1 - smoothstep(.008,.045,arc));
    float ring = abs(distance - (.035 + .13 * frac(age * .68)));
    float halo = exp(-distance * 20) * .44 + (1-smoothstep(.001,.005,ring)) * .56;
    float core = 1 - smoothstep(.010,.018,distance);
    float fade = 1 - smoothstep(1.55,2.05,age);
    float3 signal = lerp(sky + tint * halo * strength, tint * strength, core);
    float envelope=fade*visible;
    float3 full=lerp(sky,signal,envelope);
    if (foreground==1 && background==1) return float4(full,1);
    float3 emission=saturate(tint*strength*lerp(halo,1,core))*envelope;
    float coverage=max(core*envelope,max(emission.r,max(emission.g,emission.b)));
    return float4(background*lerp(sky,full,foreground)+(1-background)*foreground*emission,
                  background+(1-background)*foreground*coverage);
}
