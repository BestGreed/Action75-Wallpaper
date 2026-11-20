// Original FM Paintball material mathematics, adapted to Effekseer variables.
// customData1.xy = Erosion / BGTexture-or-Panning; customData2.xy = GradRow / RandomSplat.
float4 pbDynamic = float4(customData1.xy, customData2.xy);
float4 pbResult = float4(0.0,0.0,0.0,0.0);

float3 worldPositionOffset=float3(0.0,0.0,0.0);
float3 baseColor=float3(0.0,0.0,0.0);

float metallic=0.0; float roughness=0.5; float ambientOcclusion=1.0;
// Zero in the packed uniform preserves the original timing for standalone playback.
float pbHoldSeconds=RuntimePipeline.z-1.0;
float pbRetention=1.0;
if (pbHoldSeconds>=0.0) {
    // Only the settled pigment fades. Local effect time advances at the selected spray speed.
    float pbSecondsAfterSplash=($LOCALTIME$-0.75)/max(A75Intensity.w,0.25);
    float pbFadeSeconds=RuntimePipeline.w>0.0?RuntimePipeline.w:0.25;
    pbRetention=1.0-smoothstep(pbHoldSeconds,pbHoldSeconds+pbFadeSeconds,pbSecondsAfterSplash);
}

// Both stages get the same draw time. A fully transparent sprite can be
// collapsed before rasterization while the effect still updates normally.
if (pbRetention<=0.0 || pbDynamic.w==floor(pbDynamic.w))
    worldPositionOffset=-worldPos;

float opacity=0.0; float opacityMask=1.0; float refraction=0.0;
float3 emissive=float3(0.0,0.0,0.0);
// Retention comes from draw uniforms. A fully faded batch needs no texture
// fetches. The texture samples still execute together before the alpha test.
[branch] if (pbRetention>0.0) {
    // Source: M_PaintSplat_02.001.pixel.glsl; immutable constants are resolved MI values.
    float2 pbAtlasSize=float2(3.0,3.0);
    float pbPacked=floor(pbDynamic.w);
    float pbAtlasIndex=pbPacked-9.0*floor(pbPacked/9.0);
    float pbGradientRow=floor(pbPacked/9.0)+1.0;
    float pbColorGain=pbDynamic.z;
    float pbMaskAlpha=2.0*(pbDynamic.w-pbPacked);
    float2 pbAtlasUV=uv1/pbAtlasSize+float2(pbAtlasIndex,floor(pbAtlasIndex/3.0))/pbAtlasSize;
    float4 pbMasks=$TEX_P2$pbAtlasUV$TEX_S2$;
    pbMasks.x=LERP(pbMasks.x/12.92,pow((pbMasks.x+0.055)/1.055,2.4),step(0.04045,pbMasks.x));
    float pbAgeInverse=1.0-pbDynamic.x;
    float pbGradEdge=-1.0+(0.99+pbAgeInverse)*0.45;
    float pbGradU=clamp(smoothstep(pbGradEdge,pbGradEdge+1.5,pbMasks.x),0.0,1.0);
    float pbGradV=pbGradientRow/9.0-0.0555555556;
    float3 pbGradient=($TEX_P1$float2(pbGradU,pbGradV)$TEX_S1$).xyz;
    // NativeWorldToUEXY maps native worldPos to the original ABSOLUTE UE world XY.
    float2 pbUEWorld=worldPos.xy*NativeWorldToUEXY.xy+NativeWorldToUEXY.zw;
    float pbCanvas=($TEX_P0$pbUEWorld*0.01*3.5$TEX_S0$).x;
    pbCanvas=(pbCanvas<=0.04045)?(pbCanvas/12.92):pow((pbCanvas+0.055)/1.055,2.4);
    float pbCanvasMask=LERP(1.0,1.0-pbCanvas,pbDynamic.y);
    float pbAlphaEdge=-0.1+(0.1+pbAgeInverse);
    float pbAlpha=clamp(smoothstep(pbAlphaEdge,pbAlphaEdge+0.15,pbCanvasMask*pbMasks.x),0.0,1.0)*pbMaskAlpha;
    pbResult.w=pbAlpha;
    opacity=clamp(pbResult.w,0.0,1.0)*pbRetention;

    [branch] if (opacity>0.0) {
        pbMasks.yz=LERP(pbMasks.yz/12.92,pow((pbMasks.yz+0.055)/1.055,float2(2.4,2.4)),step(float2(0.04045,0.04045),pbMasks.yz));
        pbGradient=LERP(pbGradient/12.92,pow((pbGradient+0.055)/1.055,float3(2.4,2.4,2.4)),step(float3(0.04045,0.04045,0.04045),pbGradient));
        float pbShadowEdge=0.85+(1.0-(0.9*pbDynamic.x+0.99));
        float pbShadow=smoothstep(pbShadowEdge,pbShadowEdge+0.4,pbMasks.z);
        float pbHighlightEdge=0.25+(1.0-(0.1*pbDynamic.x+0.99));
        float pbHighlight=smoothstep(pbHighlightEdge,pbHighlightEdge+0.6,pbMasks.y)*pbShadow;
        pbResult.xyz=pbColorGain*clamp(pbGradient*clamp(0.5+pbShadow,0.0,1.0)+float3(1.0,1.0,1.0)*pbHighlight,0.0,1.0);
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
}
