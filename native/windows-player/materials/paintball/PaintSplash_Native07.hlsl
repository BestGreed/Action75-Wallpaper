// Original FM Paintball material mathematics, adapted to Effekseer variables.
// customData1.xy = Erosion / BGTexture-or-Panning; customData2.xy = GradRow / RandomSplat.
float4 pbDynamic = float4(customData1.xy, customData2.xy);
float4 pbResult = float4(0.0,0.0,0.0,0.0);

// Source: M_PaintSplash_01.027.pixel.glsl, original BLEND_Masked.
// RuntimePipeline.y scales the opening ripple around its center.
float pbCenterScale=RuntimePipeline.y>0.0?RuntimePipeline.y:1.0;
float2 pbP=(uv1-float2(0.5,0.5))/pbCenterScale;
float pbInside=step(max(abs(pbP.x),abs(pbP.y)),0.5);
float pbAngle=frac(atan2(pbP.x,pbP.y)/6.283184)*1.0;
float pbRadius=sqrt(dot(pbP,pbP));
float2 pbWaveUV=float2(pbAngle,pbRadius*(2.0*pbDynamic.y+1.0));
float pbWave=($TEX_P0$pbWaveUV$TEX_S0$).x;
pbWave=(pbWave<=0.04045)?(pbWave/12.92):pow((pbWave+0.055)/1.055,2.4);
float pbGray=clamp(max(pbWave*1.5+0.2,0.3),0.0,1.0);
float pbErode=(0.0+pbDynamic.x)*(pbWave+0.99);
float pbRim=1.0-pbRadius/0.7;
float pbRimX=pbRim*3.0;
float pbGaussian=1.0/pow(2.718282,pbRimX*pbRimX);
float pbRimValue=(abs(pbRim)>0.00001)?((pbRim>=0.0)?pbGaussian:1.0):1.0;
float pbMask=pbDynamic.w*pbErode*clamp(1.0-pbRimValue,0.0,1.0)-1.0;
pbResult=float4(pbGray*float3(0.274677,0.122139,0.376262)*pbDynamic.z,1.0);

// All four vertices share custom data. With a UNORM wave, even its highest
// sample cannot pass the mask below this product. Collapse the invisible quad.
float3 worldPositionOffset=(pbDynamic.x*pbDynamic.w<0.5)?-worldPos:float3(0.0,0.0,0.0);
float3 baseColor=float3(0.0,0.0,0.0);

float metallic=0.0; float roughness=0.5; float ambientOcclusion=1.0;
float opacity=clamp(pbResult.w,0.0,1.0); float opacityMask=pbInside*step(0.0,pbMask); float refraction=0.0;

// Effekseer discards zero mask or alpha. Shade only surviving fragments and
// keep implicit-LOD texture samples before the divergent branch.
float3 emissive=float3(0.0,0.0,0.0);
[branch] if (opacityMask>0.0 && opacity>0.0) {
    // Optional palette/grade. Neutral defaults leave recovered color untouched.
    if ((A75PaletteColor.w>0.5)||(A75Intensity.y!=1.0)||(A75Intensity.z!=0.0)) {
        float3 a75Grade=pbResult.xyz;
        if (A75PaletteColor.w>0.5) {
            float a75Min=min(a75Grade.x,min(a75Grade.y,a75Grade.z));
            float a75Max=max(a75Grade.x,max(a75Grade.y,a75Grade.z));
            a75Grade=float3(a75Min,a75Min,a75Min)+(a75Max-a75Min)*A75PaletteColor.xyz;
        }
        if (A75Intensity.z!=0.0) {
            float a75Luma=dot(a75Grade,float3(0.2126,0.7152,0.0722));
            float a75Cos=cos(A75Intensity.z);float a75Sin=sin(A75Intensity.z);
            float a75Mean=(a75Grade.x+a75Grade.y+a75Grade.z)/3.0;
            float3 a75Cross=float3(a75Grade.z-a75Grade.y,a75Grade.x-a75Grade.z,a75Grade.y-a75Grade.x)*0.577350269;
            a75Grade=a75Grade*a75Cos+a75Cross*a75Sin+float3(a75Mean,a75Mean,a75Mean)*(1.0-a75Cos);
            float a75Correction=a75Luma-dot(a75Grade,float3(0.2126,0.7152,0.0722));
            a75Grade+=float3(a75Correction,a75Correction,a75Correction);
        }
        if (A75Intensity.y!=1.0) {
            float a75Luma=dot(a75Grade,float3(0.2126,0.7152,0.0722));
            a75Grade=float3(a75Luma,a75Luma,a75Luma)+(a75Grade-float3(a75Luma,a75Luma,a75Luma))*A75Intensity.y;
        }
        pbResult.xyz=max(a75Grade,float3(0.0,0.0,0.0));
    }

    // Concentration changes pigment absorption only; erosion and opacity stay unchanged.
    float3 pigment=saturate(pbResult.xyz*RuntimePipeline.x*A75TintBrightness.rgb*vcolor.rgb);
    emissive=pow(max(pigment,float3(0.0001,0.0001,0.0001)),float3(A75Intensity.x,A75Intensity.x,A75Intensity.x));
    // Brightness changes the visible pigment, without HDR peak normalization cancelling it.
    float exposure=A75TintBrightness.w;
    emissive=(exposure<=1.0)?emissive*exposure:LERP(emissive,float3(1.0,1.0,1.0),1.0-1.0/exposure);
    emissive=LERP(emissive*12.92,1.055*pow(emissive,float3(0.416666667,0.416666667,0.416666667))-0.055,step(float3(0.0031308,0.0031308,0.0031308),emissive));
}
