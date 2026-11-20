#include "PixelFluid.h"
#include <cassert>
#include <cmath>
#include <iostream>
using namespace action75;
PixelFluid::Params isolated(){PixelFluid::Params p;p.gravity=0;p.drag=0;p.viscosity=0;p.cohesion=0;p.pressure=0;return p;}
int main(){
 constexpr float dt=1.f/120;
 {PixelFluid solver;auto p=isolated();FluidParticle a{500,300,500,300,0,0,4,16},b{503,300,503,300,0,0,4,32};std::vector<FluidParticle*> v{&a,&b};float center=(a.x*a.mass+b.x*b.mass)/(a.mass+b.mass);solver.step(v,dt,p);assert(b.x-a.x>3);assert(std::abs((a.x*a.mass+b.x*b.mass)/(a.mass+b.mass)-center)<.001f);}
 {auto p=isolated();p.viscosity=1.5f;PixelFluid viscous,dry;FluidParticle a{500,300,500,300,0,50,4,16},b{505,300,505,300,0,-50,4,16},c=a,d=b;std::vector<FluidParticle*> x{&a,&b},y{&c,&d};viscous.step(x,dt,p);p.viscosity=0;dry.step(y,dt,p);assert(std::abs(a.vy-b.vy)<std::abs(c.vy-d.vy));assert(std::abs(a.vy+b.vy)<.001f);}
 {auto p=isolated();p.cohesion=1;PixelFluid solver;FluidParticle a{500,300,500,300,0,0,4,16},b{507,300,507,300,0,0,4,16};std::vector<FluidParticle*> v{&a,&b};for(int i=0;i<12;++i)solver.step(v,dt,p);assert(b.x-a.x<7);}
 {PixelFluid solver;auto p=isolated();p.gravity=180;FluidParticle a{500,718,500,718,30,180,4,16};std::vector<FluidParticle*> v{&a};solver.step(v,dt,p);assert(a.y<=720-a.spacing*.42f);assert(a.vy<=0);assert(a.vx>0);}
 {PixelFluid solver;auto p=isolated();p.pressure=1;p.cohesion=.8f;std::vector<FluidParticle> bodies(PixelFluid::capacity);std::vector<FluidParticle*> pointers;for(auto& b:bodies){b.x=b.previousX=960;b.y=b.previousY=360;pointers.push_back(&b);}for(int i=0;i<4;++i)solver.step(pointers,dt,p);assert(solver.pairCount()<=PixelFluid::capacity*48);for(auto& b:bodies){assert(std::isfinite(b.x)&&std::isfinite(b.y)&&std::isfinite(b.vx)&&std::isfinite(b.vy));assert(b.x>=0&&b.x<=1920&&b.y>=0&&b.y<=720);}solver.clear();assert(solver.particleCount()==0);}
 std::cout<<"Mass-weighted separation, viscosity, cohesion, floor collision and 4096-body saturation passed\n";
}
