#include "FieldEffect.h"
#include "NativeRenderer.h"
#include <iostream>
#include <fstream>
#include <chrono>
using namespace action75;
int main(int argc,char** argv){
 try{
  if(argc!=3)throw std::runtime_error("Need packs and output paths");CoInitializeEx(nullptr,COINIT_MULTITHREADED);
  std::filesystem::path packs=argv[1],out=argv[2];std::filesystem::create_directories(out);
  NativeRenderer renderer;std::string error;if(!renderer.initialize(nullptr,960,360,true,error,true))throw std::runtime_error(error);
  const char* ids[]={"silk-nebula","gilded-synapse","crystal-herbarium","harmonic-textile"};
  for(unsigned theme=0;theme<4;++theme){
   auto pack=packs/ids[theme];auto folder=out/ids[theme];std::filesystem::create_directories(folder);
   PackSettingsStore settings(folder/"profile");settings.registerPack(ids[theme],pack,pack/"settings.json");auto candidate=settings.current(ids[theme]);auto a=settings.evaluate(candidate);
   if(a.field.size()<8 || a.resolvedPaletteColors.empty())throw std::runtime_error("Field settings incomplete");
   FieldEffect field;if(!field.load(renderer.device(),pack/"layout.json",theme,error))throw std::runtime_error(error);field.configure(a);
   auto frame=[&](double now,bool paused,const std::string& name){renderer.beginFrame();if(!field.draw(renderer.context(),960,360,now,paused,error))throw std::runtime_error(error);if(!name.empty()&&!renderer.saveFrame(folder/name,error))throw std::runtime_error(error);};
   const auto begin=std::chrono::steady_clock::now();
   frame(100,false,"idle.png");
   for(int i=1;i<=160;++i){double time=100+i/60.;
    if(i==10)field.push(time,.28f,.42f);if(i==28)field.push(time,.4f,.58f);if(i==46)field.push(time,.53f,.42f);if(i==64)field.push(time,.66f,.58f);if(i==82)field.push(time,.74f,.76f);if(i==100)field.push(time,.42f,.93f);
    frame(time,false,i%4==0?"frame-"+std::to_string(i/4)+".png":"");
   }
   frame(103,true,"paused-a.png");frame(104,true,"paused-b.png");
   auto values=candidate.values.at("palette");values["preset"]="magma";
   auto changedPalette=settings.prepare(ids[theme],{{"palette",values}});
   field.configure(settings.evaluate(changedPalette));frame(104,true,"palette.png");
   auto zero=a;zero.brightness=0;field.configure(zero);frame(104,true,"zero.png");
   field.configure(a);for(int i=0;i<128;++i)field.push(105,float(i%16)/16,.5f);
   if(field.count()>24 || field.push(105,-1,.5f))throw std::runtime_error("Invalid bounded field inputs");
   frame(105.1,false,"stress.png");field.clear();frame(106,false,"clear.png");if(field.count()!=0)throw std::runtime_error("Clear failed");
   for(int quality=0;quality<3;++quality){auto changed=a;changed.field["quality"]=quality;field.configure(changed);frame(107+quality,false,"quality-"+std::to_string(quality)+".png");}
   FieldEffect control;if(!control.load(renderer.device(),pack/"layout.json",theme,error))throw std::runtime_error(error);control.configure(a);
   for(int i=0;i<=160;++i){renderer.beginFrame();if(!control.draw(renderer.context(),960,360,100+i/60.,false,error))throw std::runtime_error(error);}
   if(!renderer.saveFrame(folder/"no-input-control.png",error))throw std::runtime_error(error);
   if(theme==2){
    field.clear();field.configure(a);field.push(200,.5f,.5f);field.push(200,.2f,.4f);field.push(200,.8f,.6f);frame(201.5,false,"");
    const char* controls[]={"thickness","refraction","variation","diversity","density","opacityVariation","melt"};
    for(auto key:controls){
     if(std::string(key)=="melt")frame(200+a.field.value("persistence",3.f)*1.55,false,"");
     double at=std::string(key)=="melt"?200+a.field.value("persistence",3.f)*1.55:201.5;
     double maximum=std::string(key)=="thickness"||std::string(key)=="refraction"?1.5:1.;
     for(int side=0;side<2;++side){auto changed=settings.prepare(ids[theme],{{key,side?maximum:0.}});field.configure(settings.evaluate(changed));frame(at,true,std::string(key)+(side?"-high.png":"-low.png"));}
    }
    field.clear();field.configure(a);frame(300,false,"wordmark-on.png");
    auto off=settings.prepare(ids[theme],{{"wordmarkEnabled",false}});field.configure(settings.evaluate(off));frame(300,true,"wordmark-off.png");
    auto dark=settings.prepare(ids[theme],{{"wordmarkBrightness",0.}});field.configure(settings.evaluate(dark));frame(300,true,"wordmark-dark.png");
    field.configure(a);frame(300,true,"wordmark-restored.png");
    for(int tick=1;tick<=60;++tick)frame(300+tick/60.,false,tick==60?"wordmark-flow.png":"");
   }
   std::cout<<ids[theme]<<" passed in "<<std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count()<<"s\n";
  }
  CoUninitialize();return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
