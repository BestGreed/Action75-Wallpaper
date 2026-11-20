#include "SpriteBurstState.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <limits>
using namespace action75;

int main() {
  SpriteBurstState state;
  std::array<SpriteBurstSample, SpriteBurstState::capacity> samples;
  assert(state.sample(0, .75f, 24, samples)==0);
  assert(!state.push(0,.75f,-.01f, .5f));
  assert(!state.push(0,.75f,std::numeric_limits<float>::quiet_NaN(), .5f));
  assert(!state.push(0,-.75f,.5f,.5f));
  assert(state.push(100,.75f,0,1,1));
  assert(state.push(100,.75f,1,0,.5f));
  assert(state.sample(100.375,.75f,24,samples)==2);
  assert(samples[0].x==0 && samples[0].y==1 && samples[0].frame==11.5f && samples[0].gain==1);
  assert(samples[1].x==1 && samples[1].y==0 && samples[1].frame==11.5f && samples[1].gain==.5f);
  assert(state.sample(100.75,.75f,24,samples)==0);
  for (int n=0;n<1000;n++) assert(state.push(200.0+0.000001*n,.75f,n%2,n%3==0));
  assert(state.count()==SpriteBurstState::capacity);
  assert(state.sample(200.01,.75f,24,samples)==SpriteBurstState::capacity);
  assert(samples[0].x==0 && samples[0].gain<.6f && samples[0].gain>.5f);
  assert(samples.back().x==1 && samples.back().y==1 && samples.back().frame<samples[0].frame);
  state.reset();
  assert(state.sample(200.01,.75f,24,samples)==0);
}
