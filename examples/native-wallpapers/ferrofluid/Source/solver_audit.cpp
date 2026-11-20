#include "Bath.hpp"
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <cstring>
// Deterministic solver-only measurement, in screen-height units.
int main(int argc,char** argv){
    auto sim=std::make_unique<Bath>();
    Vec keys[3]={{.16f,.57f},{2.24f,.58f},{.63f,.32f}};
    double time=0,maxDrift=0;
    float minDepth=1,maxDepth=0;
    std::puts("phase,seconds,mass,relativeError,centroidX,centroidY,rmsSpeed,peakDepth,awake");
    auto run=[&](const char* phase,int count,int frames){
        for(int i=0;i<frames;i++){
            bool awake=sim->step(keys,count,1,1.f/60);time+=1./60;
            double mass=0,x=0,y=0,kinetic=0;float peak=0;
            for(int n=0;n<Bath::S;n++){
                auto q=sim->film[n];if(!std::isfinite(q.h+q.x+q.y)||q.h<0)throw std::runtime_error("invalid state");
                mass+=q.h;x+=q.h*(n%Bath::NX+.5)*Bath::dx;y+=q.h*(n/Bath::NX+.5)*Bath::dx;
                if(q.h>1e-7)kinetic+=(q.x*q.x+q.y*q.y)/q.h;
                peak=std::max(peak,q.h);
            }
            double drift=fabs(mass/sim->initialMass-1);maxDrift=std::max(maxDrift,drift);
            if(i%30==0||!awake||i+1==frames)
                std::printf("%s,%.3f,%.8f,%.9f,%.5f,%.5f,%.6f,%.6f,%d\n",phase,time,mass*Bath::dx*Bath::dx,drift,x/mass,y/mass,sqrt(kinetic/mass),peak,awake?1:0);
            if(!awake)break;
        }
    };
    run("rest",0,180);
    sim->tap(keys[0],1);run("first",1,600);
    sim->tap(keys[1],1);run("transfer",2,22);
    sim->tap(keys[2],1);run("turn",3,600);
    run("release-newest",2,600);run("release-second",1,600);run("return",0,1200);
    return maxDrift>.001?2:0;
}
