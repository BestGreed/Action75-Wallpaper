#include "LayerColor.hlsli"
float3 gradeBackground(float3 rgb){
 return backgroundRows[1].w>0 ? 0 : gradeLayer(rgb,backgroundRows);
}
float3 overSolidBackground(float3 rgb){
 return rgb+(1-max(rgb.r,max(rgb.g,rgb.b)))*solidBackground.rgb;
}
