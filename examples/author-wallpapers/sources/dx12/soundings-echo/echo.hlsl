cbuffer Constants : register(b0) {
    float4 displayInfo; // width, height, diameter, 1/columns
    float4 atlasInfo;   // 1/rows, atlas width, atlas height, columns
    float4 tint;
    float4 grade;
    float4 hits[24];
    float4 bounds[24];
    float4 palettes[24];
    float4 backgroundAppearance; // brightness, saturation, hue turns, enabled
    float4 backgroundTint; // RGB and solid flag
};
Texture3D atlas : register(t0);
Texture2D backgroundImage : register(t1);
SamplerState linearClamp : register(s0);
struct Fullscreen { float4 clip:SV_POSITION;float2 uv:TEXCOORD0; };
Fullscreen backgroundVS(uint id:SV_VertexID) {
    Fullscreen v;float2 uv=float2((id<<1)&2,id&2);
    v.clip=float4(uv*float2(2,-2)+float2(-1,1),0,1);v.uv=uv;return v;
}
float3 gradeBackground(float3 color) {
    if(backgroundAppearance.y!=1 || backgroundAppearance.z!=0) {
        float highest=max(color.r,max(color.g,color.b));
        float lowest=min(color.r,min(color.g,color.b));
        float chroma=highest-lowest;
        if(chroma>0) {
            float hue;
            if(highest==color.r) hue=(color.g-color.b)/chroma;
            else if(highest==color.g) hue=(color.b-color.r)/chroma+2;
            else hue=(color.r-color.g)/chroma+4;
            hue=frac(hue/6+backgroundAppearance.z);
            float saturation=saturate((chroma/highest)*backgroundAppearance.y);
            float3 pure=saturate(abs(frac(hue+float3(0,.666666667,.333333333))*6-3)-1);
            color=highest*lerp(float3(1,1,1),pure,saturation);
        }
    }
    if(backgroundAppearance.x<=1) return color*backgroundAppearance.x;
    float peak=max(color.r,max(color.g,color.b));
    return color*(backgroundAppearance.x/(1+(backgroundAppearance.x-1)*peak));
}
float4 backgroundPS(Fullscreen v):SV_TARGET {
    float3 color=backgroundTint.w!=0?backgroundTint.rgb:backgroundImage.Sample(linearClamp,v.uv).rgb*backgroundTint.rgb;
    if(backgroundAppearance.w!=0 || backgroundTint.w!=0) color=gradeBackground(color);
    return float4(color,1);
}
struct Burst {float4 clip:SV_POSITION;float3 uvw:TEXCOORD0;nointerpolation float gain:TEXCOORD1;};
Burst echoVS(uint vertex:SV_VertexID,uint instance:SV_InstanceID) {
    float2 uv=float2(vertex&1u,(vertex>>1u)&1u);
    uv=lerp(bounds[instance].xy,bounds[instance].zw,uv);
    float4 hit=hits[instance];
    float2 position=hit.xy+(uv-.5)*displayInfo.z/displayInfo.xy;
    Burst v;v.clip=float4(position*float2(2,-2)+float2(-1,1),0,1);
    float guard=.5/(atlasInfo.y*displayInfo.w);
    v.uvw=float3(guard+uv*(1-2*guard),(hit.z+.5)*(displayInfo.w*atlasInfo.x));
    v.gain=hit.w;return v;
}
float4 echoPS(Burst v):SV_TARGET {
    float3 color=atlas.SampleLevel(linearClamp,v.uvw,0).rgb*v.gain;
    float peak=max(color.r,max(color.g,color.b));if(peak>1) color/=peak;
    return float4(color,0);
}
