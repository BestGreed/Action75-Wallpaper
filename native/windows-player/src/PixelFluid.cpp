#include "PixelFluid.h"
#include <algorithm>
#include <cmath>
namespace action75 {namespace {
float clampLength(float x,float y,float maximum){float d=x*x+y*y;return d>maximum*maximum?maximum/std::sqrt(d):1;}
unsigned hash(unsigned x){x^=x>>16;x*=0x7feb352d;x^=x>>15;x*=0x846ca68b;return x^(x>>16);}
}
void PixelFluid::step(const std::vector<FluidParticle*>& bodies,float dt,const Params& settings){
 const unsigned n=static_cast<unsigned>(std::min(bodies.size(),size_t(capacity)));particleCount_=n;pairs_.clear();if(!n)return;
 float maxSpacing=1;for(unsigned i=0;i<n;++i)maxSpacing=std::max(maxSpacing,bodies[i]->spacing);
 const float cell=std::max(6.f,maxSpacing*2.65f);const unsigned cols=unsigned(std::ceil(settings.width/cell))+1,rows=unsigned(std::ceil(settings.height/cell))+1;
 offsets_.assign(cols*rows+1,0);order_.resize(n);pairs_.reserve(capacity*48);
 const auto bucket=[&](const FluidParticle& p){unsigned x=unsigned(std::clamp(int(p.x/cell),0,int(cols)-1)),y=unsigned(std::clamp(int(p.y/cell),0,int(rows)-1));return y*cols+x;};
 const float drag=std::exp(-settings.drag*dt);
 for(unsigned i=0;i<n;++i){auto& p=*bodies[i];p.previousX=p.x;p.previousY=p.y;
  p.vx*=drag;p.vy=(p.vy+settings.gravity*dt)*drag;float limit=std::clamp(p.spacing*70,120.f,720.f),gain=clampLength(p.vx,p.vy,limit);p.vx*=gain;p.vy*=gain;
  p.x+=p.vx*dt;p.y+=p.vy*dt;
  ++offsets_[bucket(p)+1];
 }
 for(size_t i=1;i<offsets_.size();++i)offsets_[i]+=offsets_[i-1];cursor_=offsets_;
 for(unsigned i=0;i<n;++i)order_[cursor_[bucket(*bodies[i])]++]=i;
 // Dense storms use a deterministic, rotating sample of each populated cell.
 // This bounds neighbour work without an O(n^2) all-pairs fallback.
 for(unsigned i=0;i<n;++i){const auto& a=*bodies[i];unsigned b=bucket(a),cx=b%cols,cy=b/cols,accepted=0;
  for(int oy=-1;oy<=1&&accepted<48;++oy)for(int ox=-1;ox<=1&&accepted<48;++ox){int x=int(cx)+ox,y=int(cy)+oy;if(x<0||y<0||x>=int(cols)||y>=int(rows))continue;
   unsigned id=unsigned(y)*cols+unsigned(x),begin=offsets_[id],count=offsets_[id+1]-begin;if(!count)continue;
   unsigned samples=std::min(count,24u),start=count>24?hash(i*37+id*101+serial_*13)%count:0;
   for(unsigned k=0;k<samples&&accepted<48;++k){unsigned j=order_[begin+(start+k)%count];if(j<=i)continue;const auto& other=*bodies[j];float dx=a.x-other.x,dy=a.y-other.y,h=std::max(a.spacing,other.spacing)*2.4f;
    if(dx*dx+dy*dy<h*h*1.12f){pairs_.push_back({i,j,h,(a.spacing+other.spacing)*.5f,other.mass/(a.mass+other.mass),std::clamp(other.mass/(a.spacing*a.spacing),.2f,4.f),std::clamp(a.mass/(other.spacing*other.spacing),.2f,4.f)});++accepted;}
   }
  }
 }
 ++serial_;
 for(unsigned iteration=0;iteration<2;++iteration){
  for(unsigned i=0;i<n;++i){auto& p=*bodies[i];p.density=p.nearDensity=p.nx=p.ny=p.dx=p.dy=0;}
  // Density and displacement use the same positions in this iteration. Cache
  // pair geometry once instead of repeating distance roots and mass divisions.
  for(auto& pair:pairs_){auto& a=*bodies[pair.i];auto& b=*bodies[pair.j];float dx=a.x-b.x,dy=a.y-b.y,d2=dx*dx+dy*dy;pair.q=0;if(d2>=pair.h*pair.h)continue;
   float d=std::sqrt(std::max(d2,.000001f)),q=1-d/pair.h,q2=q*q;pair.distance=d;pair.q=q;
   pair.nx=dx/d;pair.ny=dy/d;
   if(d2<.000001f){unsigned direction=hash(pair.i*71+pair.j*101)&3;pair.nx=direction==0?1.f:direction==1?-1.f:0;pair.ny=direction==2?1.f:direction==3?-1.f:0;}
   a.density+=q2*pair.densityWeightA;b.density+=q2*pair.densityWeightB;a.nearDensity+=q2*q*pair.densityWeightA;b.nearDensity+=q2*q*pair.densityWeightB;
   a.nx+=pair.nx*q2;a.ny+=pair.ny*q2;b.nx-=pair.nx*q2;b.ny-=pair.ny*q2;
  }
  for(const auto& pair:pairs_){if(pair.q<=0)continue;auto& a=*bodies[pair.i];auto& b=*bodies[pair.j];float spacing=pair.spacing,q=pair.q;
   float pressure=std::max(0.f,std::min(a.density,12.f)-2.6f)+std::max(0.f,std::min(b.density,12.f)-2.6f);
   float near=std::min(a.nearDensity+b.nearDensity,12.f);
   float correction=spacing*settings.pressure*(pressure*q*.015f+near*q*q*.004f);
   if(pair.distance>spacing*1.04f)correction-=spacing*settings.cohesion*q*.028f;
   const float separation=spacing*.82f-pair.distance;if(separation>0)correction+=separation*.8f;
   const float wa=pair.massFractionA,wb=1-wa;
   a.dx+=pair.nx*correction*wa;a.dy+=pair.ny*correction*wa;b.dx-=pair.nx*correction*wb;b.dy-=pair.ny*correction*wb;
  }
  for(unsigned i=0;i<n;++i){auto& p=*bodies[i];float gain=clampLength(p.dx,p.dy,p.spacing*.20f);p.x+=p.dx*gain;p.y+=p.dy*gain;}
 }
 for(unsigned i=0;i<n;++i){auto& p=*bodies[i];p.vx=(p.x-p.previousX)/dt;p.vy=(p.y-p.previousY)/dt;p.dx=p.dy=0;float length=std::hypot(p.nx,p.ny);if(length>.0001f){p.nx/=length;p.ny/=length;}}
 const float viscous=1-std::exp(-settings.viscosity*dt*10);
 for(const auto& pair:pairs_){auto& a=*bodies[pair.i];auto& b=*bodies[pair.j];float dx=a.x-b.x,dy=a.y-b.y,d2=dx*dx+dy*dy;if(d2>=pair.h*pair.h)continue;
  float q=1-std::sqrt(d2)/pair.h,blend=viscous*q*q/std::max(1.f,(a.density+b.density)*.5f),wa=pair.massFractionA,wb=1-wa;
  float vx=(a.vx-b.vx)*blend,vy=(a.vy-b.vy)*blend;a.dx-=vx*wa;a.dy-=vy*wa;b.dx+=vx*wb;b.dy+=vy*wb;
 }
 for(unsigned i=0;i<n;++i){auto& p=*bodies[i];p.vx+=p.dx;p.vy+=p.dy;float radius=p.spacing*.42f;
  if(p.x<radius){p.x=radius;p.vx=std::abs(p.vx)*.15f;}if(p.x>settings.width-radius){p.x=settings.width-radius;p.vx=-std::abs(p.vx)*.15f;}
  if(p.y<radius){p.y=radius;p.vy=std::abs(p.vy)*.15f;}if(p.y>settings.height-radius){p.y=settings.height-radius;p.vy=-std::abs(p.vy)*.12f;p.vx*=.94f;}
  float gain=clampLength(p.vx,p.vy,std::clamp(p.spacing*70,120.f,720.f));p.vx*=gain;p.vy*=gain;
 }
}
}
