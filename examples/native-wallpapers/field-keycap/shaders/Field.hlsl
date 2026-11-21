cbuffer Field:register(b0) {
 float4 clock,dynamics,look,finish,geometry;
 float4 hits[24],hitInfo[24];
 float4 wordmark,wordmarkRect[2],wordmarkUv[2];
 float4 variety,material,snowInfo[24];
 float4 arcs[32],arcInfo[32],arcAxis[32],arcShape[32];
 uint4 simulationTiles[96],imprintTiles[96],arcTiles[96];
#ifdef GRADE_BACKGROUND
 float4 backgroundRows[3],solidBackground;
#endif
};
#ifdef GRADE_BACKGROUND
#include "ProceduralBackground.hlsli"
#endif
Texture2D stateTex:register(t0);
Texture2D paletteTex:register(t1);
Texture2D imageTex:register(t2);
Texture2D noiseTex:register(t3);
Texture2D bloomTex:register(t4);
Texture2D wordmarkTex:register(t5);
SamplerState clampLinear:register(s0);
SamplerState repeatLinear:register(s1);
struct V {float4 position:SV_POSITION;float2 uv:TEXCOORD0;};
V vs(uint id:SV_VertexID){V v;v.uv=float2((id<<1)&2,id&2);v.position=float4(v.uv*float2(2,-2)+float2(-1,1),0,1);return v;}
float hash(float2 p){return frac(sin(dot(p,float2(127.1,311.7)))*43758.5453);}
float noise(float2 p){float2 i=floor(p),f=frac(p);f=f*f*f*(f*(f*6-15)+10);return noiseTex.SampleLevel(repeatLinear,(i+f+.5)/256,0).r;}
float fbm(float2 p){float sum=0,a=.5;[unroll]for(int i=0;i<5;++i){if(i>2 && finish.z<.5)break;sum+=a*noise(p);p=mul(float2x2(.8,-.6,.6,.8),p)*2.03+13.7;a*=.5;}return sum;}
float ridge(float2 p){return 1-abs(2*fbm(p)-1);}
float3 ramp(float v){return paletteTex.SampleLevel(clampLinear,float2(saturate(v),.5),0).rgb;}
float gauss(float d,float width){return exp(-d*d/max(width*width,.0000001));}
uint tileIndex(float2 uv) {
 uint2 tile=min(uint2(uv*float2(32,12)),uint2(31,11));
 return tile.y*32+tile.x;
}
float stroke(float d,float width,float footprint){float filtered=sqrt(width*width+footprint*footprint*.25);return gauss(d,filtered)*width/filtered;}
float starfield(float2 uv,float t){float2 p=uv*float2(220,83);float2 cell=floor(p),f=frac(p)-.5;return step(.985,hash(cell))*pow(saturate(1-length(f)*2),12)*(.7+.3*sin(t+hash(cell)*35));}
float4 sim(V v):SV_TARGET {
 float2 uv=v.uv;float dt=clock.y;
 float4 old=stateTex.SampleLevel(clampLinear,uv,0);
 float2 base=float2(sin(uv.y*12+clock.x*.13),cos(uv.x*9-clock.x*.11))*.006*dynamics.y*dynamics.w;
 float4 advected=stateTex.SampleLevel(clampLinear,uv-(old.rg+base)*dt,0);
 float2 velocity=advected.rg*exp(-dt*1.6)+base*dt;
 float density=advected.b*exp(-dt/max(dynamics.x,.1));float heat=advected.a*exp(-dt*1.2);
 [branch] if(geometry.x>0){
  uint tile=tileIndex(uv);
  uint localHits=simulationTiles[tile/4][tile%4];
  [loop]while(localHits!=0){
   uint i=firstbitlow(localHits);
   localHits&=localHits-1;
   float2 d=(uv-hits[i].xy)*float2(clock.z,1);float r=length(d);
   float brush=gauss(r,.033*dynamics.z);
   float impulse=exp(-hits[i].z*5)*hits[i].w;
   velocity+=float2(-d.y/clock.z,d.x)*brush*impulse*dt*18*dynamics.y;
   density+=brush*impulse*dt*13*look.y;heat+=brush*impulse*dt*9*look.y;
  }
 }
 return float4(clamp(velocity,-.2,.2),clamp(density,0,4),clamp(heat,0,4));
}
#ifdef BACKGROUND_ONLY
#define visibleHits 0
#define visibleArcs 0
#else
#define visibleHits geometry.x
#define visibleArcs finish.w
#endif
float3 nebula(float2 uv,float4 fluid) {
 float t=clock.x;float2 p=uv*float2(clock.z,1)*3.4/dynamics.z;
 float2 warp=float2(fbm(p+float2(t*.05,0)),fbm(p+float2(5.2,-t*.04)));
 p+=(warp-.5)*dynamics.y*2+fluid.rg*7;
 // Keystrokes bend the material itself; they do not stamp a stock ring sprite.
 float disturbance=0;
 [loop]for(int k=0;k<(int)visibleHits;++k){
  float2 d=(uv-hits[k].xy)*float2(clock.z,1);float r=length(d);
  float twist=exp(-r*9/(dynamics.z*dynamics.w))*hits[k].w;
  p+=float2(-d.y,d.x)*twist*3*dynamics.y;
  disturbance+=twist;
 }
 float trans=1;float3 cloud=0;
 // Front-to-back accumulation through eight warped density layers.
 [unroll]for(int z=0;z<8;++z){if(z>=5 && finish.z<1.5)break;
  float2 q=p+float2(z*.31,z*.23)+float2(t*.017,-t*.021);
  float n=fbm(q+warp*.7);float fil=pow(ridge(q*2.1+warp),12);
  float engraving=pow(saturate(.5+.5*cos(n*105+float(z)*.7)),16);
  float density=smoothstep(.28,.84,n)*(.06+fluid.b*.16)+fil*.055*look.w+engraving*.026*look.w;
  float alpha=1-exp(-density*(look.z*1.9+.35));
  float grade=saturate((n-.25)*1.4+fluid.a*.16+float(z)*.025);
  cloud+=trans*alpha*ramp(grade)*(.55+fil*2.5+fluid.a*.2+engraving*.7);trans*=1-alpha;
 }
 // With no effective hit, disturbance is zero and the mineral term contributes nothing.
 float mineral=0;
 [branch] if(visibleHits>0)mineral=pow(ridge(p*2.7+warp),18);
#ifdef GRADE_BACKGROUND
 cloud=gradeBackground(cloud);
#endif
 cloud+=ramp(.67)*(disturbance*(mineral*.65+.04)+fluid.b*.18)*look.y;
 // A broad quiet diagonal gives the denser etching room to breathe.
 float composition=.35+.65*smoothstep(.1,.7,fbm(uv*float2(2.5,1.8)+4));
 cloud*=composition;
#ifdef GRADE_BACKGROUND
 cloud+=gradeBackground(ramp(.96)*starfield(uv,t)*finish.y*.7);
#else
 cloud+=ramp(.96)*starfield(uv,t)*finish.y*.7;
#endif
 return cloud;
}
float3 electric(float2 uv,float4 fluid) {
 float2 p=uv*float2(clock.z,1);float t=clock.x;
 float energy=0,glow=0;
 float imprints=0;
 [branch] if(visibleArcs>0 || visibleHits>0){
  uint tile=tileIndex(uv);
  uint localArcs=arcTiles[tile/4][tile%4];
  [loop]while(localArcs!=0){
   uint i=firstbitlow(localArcs);
   localArcs&=localArcs-1;
   float4 arc=arcs[i];float2 a=arc.xy*float2(clock.z,1);
   float2 dir=arcAxis[i].xy,normal=arcAxis[i].zw;
   float2 delta=p-a;float u=dot(delta,dir)*arcShape[i].x;
   if(u<-.05 || u>1.05)continue;
   float across=dot(delta,normal);if(abs(across)>arcShape[i].z)continue;
   float along=saturate(u);float seed=arcInfo[i].z;float age=arcInfo[i].x;
   float rough=(noise(float2(along*18+seed*31,t*7))-.5)*.025+(noise(float2(along*53,t*13+seed))-.5)*.01;
   rough*=sin(along*3.14159)*dynamics.y;
   float d=abs(across-rough);
   float fade=arcInfo[i].y*arcInfo[i].w;
   float taper=smoothstep(-.02,.055,u)*(1-smoothstep(.95,1.02,u));
   float pixel=geometry.w;
   energy+=stroke(d,.0018*dynamics.z,pixel)*fade*taper;
   energy+=stroke(abs(across-rough-.0038*dynamics.z),.0007*dynamics.z,pixel)*fade*.18*taper*look.w;
   glow+=gauss(d,.013*dynamics.z)*fade*.25;
   // Fork into a smaller tributary with its own fractal displacement.
   float branchU=saturate((along-.34)/.5);
   float branch=rough+sin(branchU*2.3)*arcShape[i].y;
   energy+=stroke(abs(across-branch),.0010*dynamics.z,pixel)*fade*.55*smoothstep(.32,.38,along)*(1-smoothstep(.8,.86,along))*look.w;
  }
  uint localHits=imprintTiles[tile/4][tile%4];
  [loop]while(localHits!=0){
   uint k=firstbitlow(localHits);
   localHits&=localHits-1;
   float d=length(p-hits[k].xy*float2(clock.z,1));
   if(d>.2*dynamics.z)continue;
   float2 local=(p-hits[k].xy*float2(clock.z,1))*12;
   float cut=pow(ridge(local*3+hits[k].xy*40),16);
   imprints+=gauss(d,.065*dynamics.z)*cut*hits[k].w*.4;
   glow+=gauss(d,.033*dynamics.z)*hits[k].w*.25;
  }
 }
 float2 g=abs(frac(uv*float2(48,18))-.5);
 float grid=pow(saturate(1-min(g.x,g.y)*25),8)*.012;
#ifdef GRADE_BACKGROUND
 float3 color=gradeBackground(ramp(.35)*grid*look.z)+ramp(.35)*fluid.b*.06;
#else
 float3 color=ramp(.35)*(grid*look.z+fluid.b*.06);
#endif
 color+=ramp(.63)*(glow+imprints)*look.y;
 color+=ramp(.96)*energy*look.y*1.7;
#ifdef GRADE_BACKGROUND
 float3 stars=ramp(.87)*starfield(uv,t)*finish.y*.16;
 color+=gradeBackground(stars*look.z)+stars*glow;
#else
 color+=ramp(.87)*starfield(uv,t)*finish.y*.16*(look.z+glow);
#endif
 return color;
}
// Signed distances for a finite hexagonal crystal skeleton, not periodic combs.
float snowHex(float2 p,float apothem) {
 return max(abs(p.y),max(abs(dot(p,float2(.8660254,.5))),abs(dot(p,float2(-.8660254,.5)))))-apothem;
}
float snowStem(float2 p,float2 a,float2 b,float rootWidth,float tipWidth) {
 float2 axis=b-a;float u=saturate(dot(p-a,axis)/max(dot(axis,axis),.000001));
 return length(p-a-axis*u)-lerp(rootWidth,tipWidth,u);
}
float snowPlate(float2 p,float2 center,float2 axis,float length,float width) {
 float2 d=p-center;float2 q=float2(dot(d,axis)/max(length,.0001),dot(d,float2(-axis.y,axis.x))/max(width,.0001));
 return snowHex(q,.8660254)*min(length,width);
}

// Each finite ice primitive carries a distance, outward normal and local width.
// Three projected sections produce a visible extruded sidewall, not a contour.
float4 iceStem(float2 p,float2 a,float2 b,float wa,float wb) {
 float2 axis=b-a;float u=saturate(dot(p-a,axis)/max(dot(axis,axis),.000001));
 float2 delta=p-a-axis*u;float len=length(delta),w=lerp(wa,wb,u);
 return float4(len-w,delta/max(len,.00001),w);
}
float4 iceHex(float2 p,float apothem) {
 float3 distances=float3(abs(p.y),abs(dot(p,float2(.8660254,.5))),abs(dot(p,float2(-.8660254,.5))));
 float2 n=float2(0,sign(p.y));float d=distances.x;
 if(distances.y>d){d=distances.y;n=float2(.8660254,.5)*sign(dot(p,float2(.8660254,.5)));}
 if(distances.z>d){d=distances.z;n=float2(-.8660254,.5)*sign(dot(p,float2(-.8660254,.5)));}
 return float4(d-apothem,n,apothem);
}
float4 icePlate(float2 p,float2 center,float2 axis,float len,float width) {
 float2 perpendicular=float2(-axis.y,axis.x);float2 delta=p-center;
 float4 h=iceHex(float2(dot(delta,axis)/max(len,.0001),dot(delta,perpendicular)/max(width,.0001)),.8660254);
 float2 gradient=axis*h.y/max(len,.0001)+perpendicular*h.z/max(width,.0001);
 return float4(h.x*min(len,width),gradient/max(length(gradient),.00001),min(len,width));
}
void iceUnion(inout float4 surface,float4 candidate){if(candidate.x<surface.x)surface=candidate;}
float4 iceGeometry(float2 local,float grow,float seed,int kind,float density) {
 float sector=round(atan2(local.y,local.x)/1.04719755)*1.04719755;
 float2 radial=float2(cos(sector),sin(sector)),tangent=float2(-radial.y,radial.x);
 float symmetry=dot(local,tangent)>=0?1:-1;
 float variation=1+sin(sector*2.731+seed*37)*.045*material.z;
 float2 q=float2(dot(local,radial),abs(dot(local,tangent)))/variation;
 float habit=smoothstep(.45,.9,seed),core=smoothstep(0,.24,grow);
 float4 s=iceHex(q,(kind==0?.58:kind==1?.12:.15)*core);
 if(kind==0){s.yz=radial*s.y+tangent*(s.z*symmetry);s.x*=variation;return s;}
 float tip=min(.99,grow);
 if(tip>.1){
  iceUnion(s,iceStem(q,float2(.09,0),float2(tip,0),lerp(.033,.062,habit),.006));
  float tipGrow=smoothstep(.72,1,grow);
  if(tipGrow>0)iceUnion(s,icePlate(q,float2(.88,0),float2(1,0),.12*tipGrow,lerp(.046,.082,habit)*tipGrow));
 }
 const float2 direction=float2(.5,.8660254);
 int tiers=kind<2?0:kind==2?1:kind==3?2:3;
 tiers=max(0,tiers+(density>.8?1:0)-(density<.2?1:0));
 if(finish.z<.5)tiers=min(tiers,2);
 [loop]for(int j=0;j<tiers;++j){
  float base=.31+j*.21+.025*sin(seed*23+j*1.9);
  if(grow<=base)continue;
  float branch=smoothstep(0,1,saturate((grow-base)/(.33+.06*seed)));
  float len=(.32-j*.065)*(1+.15*sin(seed*31+j))*(.65+.55*density);
  float2 root=float2(base,0),end=root+direction*len*branch;
  float width=lerp(.022,.034,habit)*sqrt(branch)*(1-j*.09);
  iceUnion(s,iceStem(q,root,end,width,.004));
  iceUnion(s,icePlate(q,end,direction,.054*branch,lerp(.023,.043,habit)*branch));
  if(kind==4 && density>.65 && finish.z>=.5){
   [unroll]for(int k=0;k<2;++k){
    float anchor=.4+k*.27;if(branch<=anchor)continue;
    float maturity=smoothstep(0,1,saturate((branch-anchor)/(.9-anchor)));
    float2 secondary=root+direction*len*anchor;float side=len*(.25-k*.065)*maturity;
    iceUnion(s,iceStem(q,secondary,secondary+float2(side,0),.008*maturity,.002));
    iceUnion(s,iceStem(q,secondary,secondary+float2(-.5,.8660254)*side*.72,.006*maturity,.0015));
   }
  }
 }
 s.yz=radial*s.y+tangent*(s.z*symmetry);s.x*=variation;
 return s;
}
float iceCoverage(float distance,float pixel){return 1-smoothstep(-pixel*.65,pixel*.65,distance);}
float3 crystal(float2 uv,float4 fluid) {
 float2 p=uv*float2(clock.z,1);float3 color=0;
 [loop]for(int i=0;i<(int)visibleHits;++i){
  float2 delta=(p-hits[i].xy*float2(clock.z,1))/dynamics.z;
  if(dot(delta,delta)>hitInfo[i].w*hitInfo[i].w)continue;
  float seed=hitInfo[i].x;float2 rotation=hitInfo[i].yz;
  float radius=snowInfo[i].y*dynamics.w;
  int kind=(int)floor(lerp(2.5,snowInfo[i].w*4.999,variety.x));
  float density=saturate(variety.y+(frac(seed*19.137)-.5)*.28);
  float pixel=geometry.w/(dynamics.z*radius),age=hits[i].z;
  float grow=smoothstep(0,1,saturate(age/(.8+.75*seed)));
  float melt=smoothstep(.85,2,snowInfo[i].x);
  float maturity=smoothstep(.1,.95,grow);
  float2 local=mul(float2x2(rotation.x,-rotation.y,rotation.y,rotation.x),delta)/radius;
  // A small oblique projection exposes the lower side of every crystal.
  local.y/=.88;
  if(length(local)>grow+.27)continue;
  float4 middle=iceGeometry(local,grow,seed,kind,density);
  float body=material.x*(.22+.78*maturity)*(1-.78*melt);
  // The farthest projected section is half the maximum depth away. Reject
  // empty space conservatively before evaluating the front and back sections.
  if(middle.x>body*.056+pixel*1.4)continue;
  float depth=min(.105*body,max(middle.w,.002)*1.65);
  float2 screenDepth=float2(.48,.88)*depth;
  float2 offset=mul(float2x2(rotation.x,-rotation.y,rotation.y,rotation.x),screenDepth);
  float4 front=iceGeometry(local+offset*.5,grow,seed,kind,density);
  float4 back=iceGeometry(local-offset*.5,grow,seed,kind,density);
  float thawNoise=noise(local*10+seed*31);
  float thaw=melt*(1.16+length(local)*.34)+(thawNoise-.5)*melt*.23*material.w;
  float remaining=1-smoothstep(1.02,1.18+material.w*.16,thaw+length(local)*.33);
  float erosion=melt*melt*.03*(.55+material.w);
  float top=iceCoverage(front.x+erosion,pixel)*remaining;
  float volume=iceCoverage(min(middle.x,min(front.x,back.x))+erosion,pixel)*remaining;
  if(volume<.0001)continue;
  float side=saturate(volume-top);
  float inside=saturate(-(front.x+erosion)/max(front.w,.002));
  // Flat prism roof changes into a rounded water bevel during thaw.
  float bevel=1-smoothstep(.08,lerp(.67,.95,melt),inside);
  float2 worldNormal=mul(float2x2(rotation.x,rotation.y,-rotation.y,rotation.x),front.yz);
  float slope=lerp(1.35,.65,melt)*bevel;
  float2 facetNormal=worldNormal*slope;
  float faceSector=round(atan2(local.y,local.x)/1.04719755)*1.04719755;
  float2 roof=mul(float2x2(rotation.x,rotation.y,-rotation.y,rotation.x),float2(cos(faceSector),sin(faceSector)));
  facetNormal+=roof*.19*body*(1-bevel);
  float3 normal=normalize(float3(facetNormal,1));
  float lightTravel=age*.14+seed*6;
  float3 light=normalize(float3(-.7+sin(lightTravel)*.1,-.85,1.1));
  float3 halfway=normalize(light+float3(0,0,1));
  float roughness=lerp(22,70,maturity)*(1+melt*.65);
  float spec=pow(saturate(dot(normal,halfway)),roughness);
  float fresnel=.025+.975*pow(1-saturate(normal.z),5);
  // Reflections of two area lights move across facets as the ice thickens,
  // then become narrower and wetter as the bevel melts.
  float3 reflection=reflect(float3(0,0,-1),normal);
  float softbox=exp(-pow((reflection.x*.8+reflection.y*.55+.38+body*.11)/(.10+.04*(1-maturity)),2));
  softbox*=smoothstep(-.65,.1,reflection.y)*(.3+.7*bevel);
  float secondBox=exp(-pow((reflection.x-.5*reflection.y-.63)/.045,2))*.22;
  float wallLight=saturate(dot(worldNormal,normalize(float2(-.7,-.85))));
  float3 sideColor=ramp(.32)*(.06+.08*wallLight)+ramp(.87)*pow(wallLight,9)*.5;
  float rearRim=gauss(back.x+erosion,pixel*.65)*(.03+.2*wallLight)*material.y;
  sideColor+=ramp(.82)*rearRim;
  float opticalDepth=body*(.18+.82*inside);
  float2 bent=normal.xy*depth*material.y*(.4+.6*maturity)*(1-melt*.45);
  float2 innerPoint=local+bent*1.7;
  float chamber=snowHex(innerPoint,.125*smoothstep(0,.24,grow));
  float cavity=(1-smoothstep(.0,.018,chamber))*(1-smoothstep(.12,.23,length(local)));
  float internal=noise(innerPoint*7+seed*17);
  float strata=pow(saturate(.5+.5*cos((innerPoint.x*.8-innerPoint.y*.35)*65+internal*.6)),28);
  float caustic=pow(saturate(.5+.5*cos(dot(innerPoint,roof)*35+opticalDepth*8+seed*6)),22);
  float internalRim=stroke(chamber+opticalDepth*.025,.002,pixel)*.12;
  float2 refractedUV=uv+bent*radius*dynamics.z/float2(clock.z,1);
  float substrate=stateTex.SampleLevel(clampLinear,refractedUV,0).b;
  float3 transmission=ramp(.48)*(.018+.025*inside)*(1-cavity*.66);
  transmission+=ramp(.72)*(strata*.045+caustic*.075+internalRim)*material.y*look.w*(.2+.8*maturity);
  transmission+=ramp(.55)*substrate*.035*material.y;
  float3 highlights=ramp(.97)*(spec*.82+softbox*.32+secondBox*.18)*(.15+.85*material.x);
  highlights+=ramp(.88)*fresnel*(.08+.35*wallLight);
  // A restrained highlight follows the growing tips, then the wet thaw front.
  float activeEdge=gauss(length(local)-grow*.95,.065)*(1-maturity)+gauss(thaw+length(local)*.33-1.07,.07)*melt;
  highlights+=ramp(.94)*activeEdge*bevel*.1;
  float sparkle=starfield(uv+hits[i].xy,age*.65)*finish.y*spec*.3;
  float fade=hits[i].w*snowInfo[i].z;
  float3 crystal=(transmission+highlights+ramp(.96)*sparkle)*top+sideColor*side;
  color=color*(1-volume*fade*(.13+opticalDepth*.16))+crystal*fade;
 }
 float haze=fbm(p*4+float2(clock.x*.01,0));
 return ramp(.22)*haze*.012*look.z+color*look.y;
}


float3 textile(float2 uv,float4 fluid) {
 float2 p=uv*float2(clock.z,1);float t=clock.x;
 float bend=0,pulse=0;
 [loop]for(int i=0;i<(int)visibleHits;++i){
  float2 d=p-hits[i].xy*float2(clock.z,1);float r=length(d);
  bend+=sin(r*28-hits[i].z*5)*exp(-r*3)*hits[i].w*.075*dynamics.w;
  pulse+=gauss(r,.11*dynamics.z)*hits[i].w;
 }
 float2 q=p+float2(sin(p.y*4+t*.15),cos(p.x*3-t*.11))*.13*dynamics.y;
 q+=fluid.rg*3+bend;
 float warp=sin(q.x*2.4+q.y*1.7-t*.18)*.12+fbm(q*2+t*.025)*.15;
 float phaseA=(q.y+warp)*130/dynamics.z,phaseB=(q.y*.83+q.x*.24-warp*.6)*137/dynamics.z;
 float aa=max(fwidth(phaseA),.001),bb=max(fwidth(phaseB),.001);
 float a=sin(phaseA)*abs(sin(aa*.5)/(aa*.5));
 float b=sin(phaseB)*abs(sin(bb*.5)/(bb*.5));
 float weave=pow(saturate(.5+.5*a*b),9);
 float sheen=pow(saturate(.5+.5*sin(q.x*2+q.y*3+warp*9-t*.22)),5);
 float thread=pow(saturate(.5+.5*a),30)*.2;
 float fiber=noise(float2(q.x*240,q.y*7+t*.015));
 thread*=.72+.4*fiber;
 float colorPosition=saturate(.22+sheen*.48+bend+fluid.b*.08);
 float3 color=ramp(colorPosition)*(weave*.23+thread*look.w+sheen*.045)*look.z;
#ifdef GRADE_BACKGROUND
 color=gradeBackground(color);
#endif
 color+=ramp(saturate(.5+pulse*.12))*(weave*.6+thread+sheen*.1)*pulse*look.y;
#ifdef GRADE_BACKGROUND
 color+=gradeBackground(ramp(.88)*starfield(uv,t)*finish.y*.1*sheen);
#else
 color+=ramp(.88)*starfield(uv,t)*finish.y*.1*sheen;
#endif
 return color;
}
float4 shade(V v):SV_TARGET {
 float4 fluid=stateTex.SampleLevel(clampLinear,v.uv,0);
 #ifdef BACKGROUND_ONLY
 fluid=0;
 #endif
 float3 color;
#if FIELD_THEME == 0
 color=nebula(v.uv,fluid);
#elif FIELD_THEME == 1
 color=electric(v.uv,fluid);
#elif FIELD_THEME == 2
 color=crystal(v.uv,fluid);
#else
 color=textile(v.uv,fluid);
#endif
 float vignette=saturate(1-dot(v.uv-.5,v.uv-.5)*.5);
 return float4(max(color,0)*vignette,1);
}
float3 toSrgb(float3 c){return lerp(12.92*c,1.055*pow(max(c,0),1/2.4)-.055,step(.0031308,c));}
float4 gaussianBlur(float2 uv,bool horizontal):SV_TARGET {
 float2 stepSize=(horizontal?float2(geometry.y,0):float2(0,geometry.z));
#if FIELD_THEME == 2
 stepSize*=.85;
#else
 stepSize*=1.6;
#endif
 float3 sum=imageTex.SampleLevel(clampLinear,uv,0).rgb*.227027;
 sum+=(imageTex.SampleLevel(clampLinear,uv+stepSize*1.384615,0).rgb+imageTex.SampleLevel(clampLinear,uv-stepSize*1.384615,0).rgb)*.316216;
 sum+=(imageTex.SampleLevel(clampLinear,uv+stepSize*3.230769,0).rgb+imageTex.SampleLevel(clampLinear,uv-stepSize*3.230769,0).rgb)*.0702703;
 return float4(sum,1);
}
float3 snowBloomSource(float2 uv) {
 uint w,h;imageTex.GetDimensions(w,h);float2 texel=1/float2(w,h);
 // Proper 4x downsample footprint: thin branches must not leave shifted ghosts.
 return (imageTex.SampleLevel(clampLinear,uv+texel,0).rgb+imageTex.SampleLevel(clampLinear,uv-texel,0).rgb+
         imageTex.SampleLevel(clampLinear,uv+texel*float2(1,-1),0).rgb+imageTex.SampleLevel(clampLinear,uv+texel*float2(-1,1),0).rgb)*.25;
}
float4 blurHorizontal(V v):SV_TARGET {
#if FIELD_THEME == 2
 float2 stepSize=float2(geometry.y*.85,0);
 float3 sum=snowBloomSource(v.uv)*.227027;
 sum+=(snowBloomSource(v.uv+stepSize*1.384615)+snowBloomSource(v.uv-stepSize*1.384615))*.316216;
 sum+=(snowBloomSource(v.uv+stepSize*3.230769)+snowBloomSource(v.uv-stepSize*3.230769))*.0702703;
 return float4(sum,1);
#else
 return gaussianBlur(v.uv,true);
#endif
}
float4 blurVertical(V v):SV_TARGET {return gaussianBlur(v.uv,false);}
float4 composite(V v):SV_TARGET {
 uint w,h;imageTex.GetDimensions(w,h);float2 texel=1/float2(w,h);
 float3 color=imageTex.SampleLevel(clampLinear,v.uv,0).rgb;
#if FIELD_THEME == 0
 float3 neighbors=(imageTex.SampleLevel(clampLinear,v.uv+float2(texel.x,0),0).rgb+imageTex.SampleLevel(clampLinear,v.uv-float2(texel.x,0),0).rgb+imageTex.SampleLevel(clampLinear,v.uv+float2(0,texel.y),0).rgb+imageTex.SampleLevel(clampLinear,v.uv-float2(0,texel.y),0).rgb)*.25;
 color=max(0,color+(color-neighbors)*.18*look.w);
#endif
 float3 bloom=finish.x>0 ? bloomTex.SampleLevel(clampLinear,v.uv,0).rgb : 0;
 color=color+bloom*finish.x*.8;
#if FIELD_THEME == 2
 if(wordmark.x>.5 && wordmark.y>0){
  [unroll]for(int part=0;part<2;++part){
   float2 q=(v.uv-wordmarkRect[part].xy)/max(wordmarkRect[part].zw,.000001);
   if(all(q>=0) && all(q<=1)){
    float4 ink=wordmarkTex.SampleLevel(clampLinear,wordmarkUv[part].xy+q*wordmarkUv[part].zw,0);
    float alpha=smoothstep(.015,.14,ink.a)*ink.a;
    float phase=frac(clock.x*wordmark.z*.13+part*.37);
    float gleam=exp(-pow((q.x-phase)*12,2));
    float3 tint=lerp(float3(.72,.84,1),float3(.92,.84,1),q.x);
    float3 lettering=ink.rgb*lerp(tint,1,gleam*.65)*(.72+.28*gleam);
    color=color*(1-alpha*.28)+lettering*alpha*wordmark.y;
   }
  }
 }
#endif
 color*=look.x;
 // Scale all channels together: high-energy glows retain their hue.
 float peak=max(color.r,max(color.g,color.b));color/=1+peak;
#ifdef GRADE_BACKGROUND
 #ifdef FOREGROUND_ONLY
 float3 emissive=saturate(toSrgb(color));
 return float4(emissive,max(emissive.r,max(emissive.g,emissive.b)));
 #else
 return float4(overSolidBackground(saturate(toSrgb(color))),1);
 #endif
#else
 return float4(saturate(toSrgb(color)),1);
#endif
}
