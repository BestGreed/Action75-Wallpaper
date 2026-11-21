cbuffer Caps:register(b0) {
 float4 display,style,motion,lineColor,glowColor;
 float4 rects[96]; float4 pulses[96];
#ifdef GRADE_BACKGROUND
 float4 backgroundRows[3],solidBackground;
#endif
};
#ifdef GRADE_BACKGROUND
#include "ProceduralBackground.hlsli"
#endif
struct V {float4 position:SV_POSITION;float2 local:TEXCOORD0;nointerpolation float4 shape:TEXCOORD1;nointerpolation float3 pulse:TEXCOORD2;};
V vs(uint vertex:SV_VertexID,uint instance:SV_InstanceID) {
 float4 r=rects[instance];float2 uv=float2(vertex&1,(vertex>>1)&1);
 V o;o.local=(uv-.5)*r.zw;o.shape=float4(r.zw*.5,display.z,display.w);o.pulse=float3(pulses[instance].xy,frac(instance*.6180339));
 float2 pos=r.xy+o.local;
 o.position=float4(pos/display.xy*float2(2,-2)+float2(-1,1),0,1);return o;
}
float hash(float2 p) {return frac(sin(dot(p,float2(127.1,311.7)))*43758.5453);}
float3 spectrum(float phase) {return .5+.5*cos(6.2831853*(phase+float3(0,.33,.67)));}
float band(float x,float width) {return exp(-x*x/max(width*width,.001));}
float4 ps(V i):SV_TARGET {
 float2 halfSize=i.shape.xy;
 float radius=min(i.shape.w,min(halfSize.x,halfSize.y));
 float2 q=abs(i.local)-(halfSize-radius);
 float sdf=length(max(q,0))+min(max(q.x,q.y),0)-radius;
 // All radiance is masked by the same rounded cap; nothing enters the gap.
 float mask=1-smoothstep(-1.2,0,sdf);
 float depth=-sdf;
 float edge=1-smoothstep(i.shape.z,i.shape.z+1.1,depth);
 float2 uv=i.local/halfSize;
 float spread=lerp(3.5,.8,style.z);
 float pool=exp(-dot(uv,uv)*spread);
 float pulse=i.pulse.x,age=i.pulse.y,seed=i.pulse.z;
 float t=motion.x;
 float angle=atan2(uv.y,uv.x)/6.2831853+.5;
 float run=frac(angle-t*.16+seed);
 float comet=exp(-run*22)+.35*exp(-frac(run+.5)*30);
 float3 spectral=spectrum(angle*.65-t*.12+seed*.22);
 float3 rim=lerp(lineColor.rgb,spectral,motion.z);
 float3 lightColor=lerp(glowColor.rgb,spectrum(seed*.4-t*.06+.6),motion.z*.65);
 float inner=band(depth-5.5,1.0);
 float halo=exp(-depth/7);
 float3 color=0;
#ifdef GRADE_BACKGROUND
 float3 base=0;
#endif
 if(style.w<.5) {
   // Etched circuits: twin rails, traveling charge, corner brackets and ticks.
   float dash=smoothstep(.32,.5,frac(angle*24+seed));
   float corner=step(.57,abs(uv.x))*step(.57,abs(uv.y));
   float rail=edge*(.18+comet*2.8)+inner*dash*.22*motion.y;
   float ticks=step(.91,frac((i.local.x+i.local.y)*.11))*band(depth-10,1.8);
   float grid=(band(frac(i.local.x/16)-.5,.025)+band(frac(i.local.y/16)-.5,.025))*.02;
   color=rim*style.x*(rail+corner*edge*.85+ticks*motion.y*.35);
   color+=rim*motion.w*(halo*.09+grid*motion.y);
#ifdef GRADE_BACKGROUND
   base=color=gradeBackground(color);
#endif
   float ring=band(length(uv)-age*2.8,.07+style.z*.1);
   color+=lightColor*pulse*style.y*(pool*.65+ring*.5+inner*.6+halo*.35);
 } else if(style.w<1.5) {
   // Liquid prism: interferometric ribbons flowing just beneath the surface.
   float wave=sin(uv.x*4+uv.y*3-t*1.6+seed*6)*sin(uv.y*5-t*.8);
   float caustic=pow(saturate(.5+.5*sin(wave*7+depth*.12-t)),5);
   float ribbon=band(depth-(8+4*sin(angle*18-t*1.8)),1.8);
   color=rim*style.x*(edge*(.45+comet*2)+ribbon*motion.y*.65);
   color+=rim*motion.w*(halo*.16+caustic*.045*motion.y);
#ifdef GRADE_BACKGROUND
   base=color=gradeBackground(color);
#endif
   float ripple=.5+.5*cos(length(uv)*18-age*16+wave*2);
   color+=lightColor*pulse*style.y*(pool*.65+caustic*.45+pow(ripple,4)*.23+halo*.45);
 } else {
   // Orbital glass: a quiet starfield, perimeter comets and a kinetic orbit.
   float2 cell=floor(i.local/9);
   float star=step(.972,hash(cell+seed*80))*pow(saturate(1-length(frac(i.local/9)-.5)*2),10);
   float twinkle=.55+.45*sin(t*2+hash(cell)*30);
   color=rim*style.x*(edge*(.16+comet*3.8)+inner*.12*motion.y);
   color+=lightColor*motion.w*(star*twinkle*.8*motion.y+halo*.07);
#ifdef GRADE_BACKGROUND
   base=color=gradeBackground(color);
#endif
   float2 orbit=uv;orbit.y+=sin(uv.x*2+t+seed*6)*.18;
   float arc=band(length(orbit)-(.25+age*.9),.025+style.z*.025);
   color+=lightColor*pulse*style.y*(pool*.6+arc*.85+star*motion.y*1.5+halo*.35);
 }
 color*=mask;
#ifdef GRADE_BACKGROUND
 float3 full=overSolidBackground(1-exp(-color));
 float f=backgroundRows[2].w,b=solidBackground.a;
 if(f==1 && b==1) return float4(full,1);
 float3 backdrop=overSolidBackground(1-exp(-base*mask));
 float3 light=1-exp(-max(0,color-base*mask));
 return float4(full*(f*b)+backdrop*(b*(1-f))+light*(f*(1-b)),
               b+f*(1-b)*max(light.r,max(light.g,light.b)));
#else
 return float4(1-exp(-color),1);
#endif
}
