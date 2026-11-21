cbuffer Burst:register(b0){float4 display,look,motion;};
struct I {float4 center:INSTANCE0;float4 shape:INSTANCE1;float4 velocity:INSTANCE2;float4 color:INSTANCE3;float4 data:INSTANCE4;};
struct V {float4 position:SV_POSITION;float2 local:TEXCOORD0;nointerpolation float4 center:TEXCOORD1;nointerpolation float4 velocity:TEXCOORD2;nointerpolation float4 color:TEXCOORD3;nointerpolation float4 data:TEXCOORD4;};
V vs(uint vertex:SV_VertexID,I input){
 float2 uv=float2(vertex&1,(vertex>>1)&1)*2-1;
 float t=input.center.z;float2 p;
 if(look.z<.5){
  // Actual particle positions are integrated by the shared fluid solver.
  // Only pixel-sized geometry is emitted here; no radial animation shortcut.
  float2 axis=input.shape.xy,normal=float2(-axis.y,axis.x);
  p=(axis*uv.x*input.shape.w+normal*uv.y/max(input.shape.w,.5))*input.shape.z*.5;
 }else p=uv*input.shape.xy;
 V o;o.position=float4((input.center.xy+p*display.z/display.xy)*float2(2,-2)+float2(-1,1),0,1);
 o.local=look.z<.5?uv:p;o.center=input.center;o.velocity=input.velocity;o.color=input.color;o.data=input.data;return o;
}
float3 toSrgb(float3 c){return lerp(c*12.92,1.055*pow(max(c,0),1/2.4)-.055,step(.0031308,c));}
float filteredLine(float distance,float width){float aa=max(fwidth(distance),1/display.z);float filtered=sqrt(width*width+aa*aa*.25);return exp(-distance*distance/(filtered*filtered))*width/filtered;}
float4 ps(V v):SV_TARGET{
 float t=v.center.z,alpha=0,energy=1;float3 color=v.color.rgb;
 if(look.z<.5){
  float kernel=pow(saturate(1-dot(v.local,v.local)),2);
  float end=.70+.3*v.data.x,start=.28+.24*v.data.y;
  float weight=kernel*smoothstep(0,.026,t)*(1-smoothstep(start,end,t))*v.velocity.w*v.data.w;
  return float4(color*v.velocity.z*weight,weight);
 }else{
  float2 q=float2(v.local.x*v.velocity.y+v.local.y*v.velocity.z,-v.local.x*v.velocity.z+v.local.y*v.velocity.y);
  float2 head=v.data.xy;float arcEnergy=0,glow=0;
  [unroll]for(int k=0;k<3;++k){
   if(k>=(int)motion.z)break;
   float eccentricity=max(.25,v.velocity.w-k*.04);
   float2 ellipse=q/float2(1,eccentricity);
   float r=length(ellipse),radius=v.velocity.x*(1-k*.12);
   float d=(r-radius)*eccentricity;
   float alignment=dot(ellipse/max(r,.00001),head);
   float arc=smoothstep(-.72,.45,alignment);
   float width=(.85-.18*k)*motion.w/display.z;
   float temporalFootprint=v.data.z*eccentricity*.0012;
   width=sqrt(width*width+temporalFootprint*temporalFootprint);
   float glint=pow(saturate(.5+.5*alignment),70);
   arcEnergy+=filteredLine(d,width)*arc*(.52+glint*1.3)/(1+k*.65);
   glow+=exp(-d*d*display.z*display.z/22)*arc*.028/(1+k);
   q=mul(float2x2(.7960838,-.6051864,.6051864,.7960838),q);
   head=mul(float2x2(.8253356,.5646425,-.5646425,.8253356),head);
  }
  float core=exp(-dot(v.local,v.local)*800)*exp(-t*18)*.2;
  energy=(arcEnergy+glow+core)*smoothstep(0,.028,t)*(1-smoothstep(.35,1,t));
  alpha=saturate(energy*v.data.w);color*=look.x;
 }
 float peak=max(color.r,max(color.g,color.b));color/=1+peak*.32;
 color=toSrgb(color);
 return float4(color*alpha,look.z<.5?alpha:0);
}
