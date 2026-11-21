#include "LayerColor.hlsli"
float3 gradeBackground(float3 rgb){
#ifdef FOREGROUND_ONLY
 return 0;
#else
 return backgroundRows[1].w>0 ? 0 : gradeLayer(rgb,backgroundRows);
#endif
}
float3 overSolidBackground(float3 rgb){
 return rgb+(1-max(rgb.r,max(rgb.g,rgb.b)))*solidBackground.rgb;
}
