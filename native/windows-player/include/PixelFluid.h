#pragma once
#include <vector>
#include <cstddef>
namespace action75 {
struct FluidParticle {
 float x=0,y=0,previousX=0,previousY=0,vx=0,vy=0,spacing=4,mass=16;
 float density=0,nearDensity=0,nx=0,ny=0,dx=0,dy=0;
};
// A bounded 2D density-relaxation solver. Bodies from every keystroke share the
// same grid; pressure, viscosity and cohesion exchange momentum across hits.
class PixelFluid {
public:
 static constexpr unsigned capacity=4096;
 struct Params {float viscosity=.45f,cohesion=.8f,pressure=1,gravity=180,drag=.22f,width=1920,height=720;};
 void step(const std::vector<FluidParticle*>&,float dt,const Params&);
 void clear(){pairs_.clear();particleCount_=0;serial_=0;}
 size_t particleCount()const{return particleCount_;}
 size_t pairCount()const{return pairs_.size();}
private:
 struct Pair {
  unsigned i,j;
  float h,spacing,massFractionA,densityWeightA,densityWeightB;
  float q=0,nx=0,ny=0,distance=0;
 };
 std::vector<unsigned> offsets_,cursor_,order_;
 std::vector<Pair> pairs_;
 size_t particleCount_=0;unsigned serial_=0;
};
}
