Texture2D<float2> bath : register(t0);
SamplerState clampLinear : register(s0);
cbuffer Material : register(b0) { float shine; float tone; float2 padding; float4 bounds; float4 poles[2]; };
cbuffer Appearance : register(b1) {float4 floorDark, floorLight; float4 fluidRows[3];};
static const float2 cell=float2(1./961,1./361);
float2 fieldUV(float2 uv){return (uv*float2(960,360)+.5)*cell;}
float2 sampleBath(float2 uv){return bath.SampleLevel(clampLinear,fieldUV(uv),0);}
struct Vary {float4 pos:SV_POSITION; float2 uv:TEXCOORD0; float4 wall:TEXCOORD1;};
Vary floorVS(uint id:SV_VertexID){
    float2 uv=float2((id<<1)&2,id&2);
    Vary o;o.pos=float4(uv*float2(2,-2)+float2(-1,1),.99,1);o.uv=uv;o.wall=0;return o;
}
Vary fluidVS(uint id:SV_VertexID, uint2 tile:ORIGIN){
    uint along=id%13,level=id/13;
    bool boundary=tile.x>=961;
    uint side=boundary?tile.x-961:0;
    uint2 p=tile+uint2(along,level);
    if(boundary){
        uint sampleIndex=tile.y+along;
        p=side<2 ? uint2(side==0?0:960,sampleIndex)
                 : uint2(sampleIndex,side==2?0:360);
    }
    float2 uv=p/float2(960,360);
    float fraction=boundary?level/12.:1.;
    float h=bath.Load(int3(p,0)).x*.32*fraction;
    float2 displaced=uv;
    // Tilt upper portions of the peaks along the local field. The wetted foot stays put.
    [unroll] for(int i=0;i<2;i++){
        [branch] if(poles[i].z==0)continue;
        float2 toward=poles[i].xy-uv*float2(8./3,1);
        float radius=max(.01,poles[i].w);
        float gain=poles[i].z*exp(-dot(toward,toward)/(radius*radius));
        displaced+=toward*float2(3./8,1)*(.28*h/radius)*gain;
    }
    Vary o; o.pos=float4(displaced.x*2-1,1-2*(displaced.y-.97*h),.85-.45*displaced.y-.55*h,1);o.uv=uv;
    float2 normal=side<2?float2(side==0?-1:1,0):float2(0,side==2?-1:1);
    o.wall=boundary?float4(normal,fraction,1):0;
    return o;
}
float hash(float2 p){return frac(52.9829189*frac(dot(p,float2(.06711056,.00583715))));}
float4 floorPS(Vary i):SV_TARGET {
    float2 uv=i.uv;
    float2 pos=(uv-float2(.46,.41))*float2(1.35,.89);
    float light=exp(-2.6*dot(pos,pos))+.12*(.5-uv.y);
    float3 base=lerp(floorDark.rgb,floorLight.rgb,light);
    float shadow=0;
    // An area light produces a continuous contact penumbra around both rims.
    [branch] if(all(uv>=bounds.xy) && all(uv<=bounds.zw))
    [unroll] for(int k=0;k<8;k++){
        float a=k*2.39996323;
        float2 shift=float2(cos(a)*.0045,sin(a)*.012)*sqrt((k+.5)/8.);
        shadow+=sampleBath(uv-float2(.003,.010)+shift).y;
    }
    base*=1-.24*shadow/8.;
    base+=(hash(floor(i.pos.xy))-.5)*.009;
    return float4(base,1);
}
float band(float v,float center,float radius,float fringe){
    return 1-smoothstep(radius,radius+fringe,abs(v-center));
}
float4 fluidPS(Vary i):SV_TARGET {
    float2 uv=i.uv;
    float2 tex=fieldUV(uv);
    float2 data=bath.SampleLevel(clampLinear,tex,0);
    clip(data.y-.019);
    float hx=(bath.SampleLevel(clampLinear,tex+float2(cell.x,0),0).x-bath.SampleLevel(clampLinear,tex-float2(cell.x,0),0).x)*57.6;
    float hy=(bath.SampleLevel(clampLinear,tex+float2(0,cell.y),0).x-bath.SampleLevel(clampLinear,tex-float2(0,cell.y),0).x)*57.6;
    float3 n=normalize(float3(-hx,-hy,1)), view=float3(0,.59,.8074);
    if(i.wall.w>.5){
        // The cut face joins the wetted footprint to exactly the displaced top edge.
        // Round its rim into the top lighting instead of lighting it like a second top surface.
        float3 face=normalize(float3(i.wall.xy,.20+.24*i.wall.z));
        n=normalize(lerp(face,n,.38*smoothstep(.76,1,i.wall.z)));
    }
    float3 r=reflect(-view,n);
    float fres=pow(1-saturate(dot(view,n)),3);
    // Studio environment with finite softboxes, a broad sky gradient and dark flags.
    float3 col=float3(.007,.010,.014)+(.014+.15*fres)*float3(.64,.76,.81);
    col+=shine*(.005+.027*smoothstep(-.45,.95,r.z))*float3(.81,.91,1);
    float u=.89*r.x-.45*r.y, v=.45*r.x+.89*r.y;
    float key=band(v,-.61,.10,.14)*band(u,-.18,.46,.20);
    float card=band(r.x-.27*r.y,.74,.10,.18)*band(r.y,.04,.52,.16);
    float rim=band(r.y+.11*r.x,.77,.021,.08)*band(r.x,-.1,.66,.18);
    float wash=exp(-3.4*((u+.44)*(u+.44)+3*(v+.53)*(v+.53)));
    col+=shine*(key*float3(.16,.19,.23)+card*float3(.11,.115,.12)+rim*float3(.37,.41,.46)+wash*float3(.045,.06,.07));
    float strip=band(v,-.78,.022,.055)*band(u,-.22,.46,.18);
    col+=shine*.70*exp(-145*dot(r.xy-float2(-.32,-.53),r.xy-float2(-.32,-.53)))*saturate(r.z);
    col+=shine*strip*float3(.32,.35,.35);
    [branch] if(fluidRows[0].w!=0)
        col=float3(dot(fluidRows[0].rgb,col),dot(fluidRows[1].rgb,col),dot(fluidRows[2].rgb,col));
    return float4(saturate(col),1);
}

