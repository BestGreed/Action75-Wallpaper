#include "ProceduralBurst.h"
#include "SpriteBurst.h"
#include "NativeRenderer.h"
#include <fstream>
#include <iostream>
using namespace action75;
int main(int argc,char** argv){try{
 if(argc<3||argc>4)throw std::runtime_error("Need staged packs, output, optional saved profiles");
 CoInitializeEx(nullptr,COINIT_MULTITHREADED);std::filesystem::path packs=argv[1],out=argv[2];std::filesystem::create_directories(out);
 NativeRenderer renderer;std::string error;if(!renderer.initialize(nullptr,1920,720,true,error,true))throw std::runtime_error(error);
 const char* names[]={"pixel-watercolor","meridian"};
 for(unsigned style=0;style<2;++style){auto pack=packs/names[style],folder=out/names[style];std::filesystem::create_directories(folder);
  PackSettingsStore settings(folder/"profile");settings.registerPack(names[style],pack,pack/"settings.json");auto profile=settings.prepare(names[style],nlohmann::json::object(),{},true);
  if(argc==4){auto file=std::filesystem::path(argv[3])/(std::string(names[style])+".json");if(std::filesystem::exists(file)){std::ifstream in(file);nlohmann::json saved;in>>saved;profile=settings.prepare(names[style],saved.at("values"));}}
  auto a=settings.evaluate(profile);float diameter=style?410.f:240.f,duration=style?.98f:1.35f;
  renderer.clearBackground();if(style&&!renderer.loadBackground(pack/"background.png",error))throw std::runtime_error(error);
  renderer.setBackgroundAppearance(a.backgroundBrightness,a.backgroundSaturation,a.backgroundHue,a.backgroundColor);
  ProceduralBurst burst;if(!burst.load(renderer.device(),style,diameter,duration,error,pack/"film-shapes.png"))throw std::runtime_error(error);burst.configure(a);
  auto frame=[&](double at,bool paused,const std::string& file){renderer.beginFrame();if(!burst.draw(renderer.context(),1920,720,at,paused,error)||(!file.empty()&&!renderer.saveFrame(folder/file,error)))throw std::runtime_error(error);};
  frame(9,false,"idle.png");burst.push(10,.5f,.5f);
  for(int i=0;i<=240;++i)frame(10+i/120.,false,"frame-"+std::to_string(i)+".png");
  ProceduralBurst sixty;if(!sixty.load(renderer.device(),style,diameter,duration,error,pack/"film-shapes.png"))throw std::runtime_error(error);sixty.configure(a);sixty.push(10,.5f,.5f);
  for(int i=0;i<=60;++i){renderer.beginFrame();if(!sixty.draw(renderer.context(),1920,720,10+i/60.,false,error))throw std::runtime_error(error);if((i==30||i==60)&&!renderer.saveFrame(folder/("sixty-"+std::to_string(i)+".png"),error))throw std::runtime_error(error);}
  burst.clear();burst.push(20,.5f,.5f);frame(20.3,false,"");frame(20.3,true,"pause-a.png");frame(25,true,"pause-b.png");frame(26,false,"resume.png");
  if(burst.count()!=1)throw std::runtime_error("Paused effect expired");auto dark=a;dark.brightness=0;burst.configure(dark);frame(26,true,"zero.png");
  for(int i=0;i<100;++i)burst.push(30,float(i%16)/16,.5f);if(!burst.count()||burst.count()>24||burst.push(30,-1,.5f))throw std::runtime_error("Bounded input failed");burst.clear();if(burst.count())throw std::runtime_error("Clear failed");
  burst.configure(a);burst.push(40,.3f,.45f);burst.push(40,.5f,.5f);burst.push(40,.7f,.45f);frame(40.28,false,"gallery.png");
  if(style==0){for(int variant=0;variant<8;++variant){burst.clear();double born=60+variant*3.;burst.push(born,.5f,.5f);frame(born+.28,false,"variant-"+std::to_string(variant)+".png");}
   for(int shape=1;shape<=6;++shape){auto values=profile.values;values["filmShape"]=shape;values["fluidVariation"]=0.;auto tunedAppearance=settings.evaluate(settings.prepare(names[style],values));
    ProceduralBurst shaped;if(!shaped.load(renderer.device(),0,diameter,duration,error,pack/"film-shapes.png"))throw std::runtime_error(error);shaped.configure(tunedAppearance);shaped.push(100,.5f,.5f);
    for(int tick=0;tick<=108;++tick){renderer.beginFrame();if(!shaped.draw(renderer.context(),1920,720,100+tick/120.,false,error))throw std::runtime_error(error);
     if(tick==12||tick==36||tick==72||tick==108)if(!renderer.saveFrame(folder/("shape-"+std::to_string(shape)+"-"+std::to_string(tick)+".png"),error))throw std::runtime_error(error);
    }
   }
   for(int rate=1;rate<=2;++rate){ProceduralBurst timed;if(!timed.load(renderer.device(),0,diameter,duration,error,pack/"film-shapes.png"))throw std::runtime_error(error);auto changed=a;changed.speed*=rate;timed.configure(changed);timed.push(100,.5f,.5f);renderer.beginFrame();if(!timed.draw(renderer.context(),1920,720,100+.30/(a.speed*rate),false,error)||!renderer.saveFrame(folder/("speed-"+std::to_string(rate)+".png"),error))throw std::runtime_error(error);}
   burst.clear();burst.push(90,.48f,.5f);burst.push(90,.52f,.5f);frame(90.35,false,"overlap.png");
   for(const char* key:{"fluidViscosity","fluidCohesion","fluidPressure","fluidGravity","fluidVariation"})for(int side=0;side<2;++side){
    auto values=profile.values;values[key]=std::string(key)=="fluidPressure"?(side?2.:.2):std::string(key)=="fluidVariation"?(side?1.:0.):(side?(std::string(key)=="fluidGravity"?2.:1.5):0.);
    auto changed=settings.evaluate(settings.prepare(names[style],values));ProceduralBurst tuned;
    if(!tuned.load(renderer.device(),0,diameter,duration,error,pack/"film-shapes.png"))throw std::runtime_error(error);tuned.configure(changed);tuned.push(100,.5f,.5f);
    renderer.beginFrame();if(!tuned.draw(renderer.context(),1920,720,100.45,false,error)||!renderer.saveFrame(folder/(std::string(key)+(side?"-high.png":"-low.png")),error))throw std::runtime_error(error);
   }
   ProceduralBurst interactive;if(!interactive.load(renderer.device(),0,diameter,duration,error,pack/"film-shapes.png"))throw std::runtime_error(error);interactive.configure(a);
   auto sequence=folder/"interactive";std::filesystem::create_directories(sequence);
   struct Press{int tick;float x,y;};const Press presses[]={{0,.42f,.42f},{22,.5f,.45f},{44,.47f,.5f},{78,.53f,.48f},{120,.45f,.6f},{156,.5f,.8f},{188,.54f,.82f},{218,.5f,.85f}};
   for(int tick=0;tick<=480;++tick){double at=1000+tick/120.;for(auto press:presses)if(press.tick==tick)interactive.push(at,press.x,press.y);
    renderer.beginFrame();if(!interactive.draw(renderer.context(),1920,720,at,false,error)||!renderer.saveFrame(sequence/("frame-"+std::to_string(tick)+".png"),error))throw std::runtime_error(error);
   }
  }
  if(style){SpriteBurst old;if(!old.load(renderer.device(),pack/"burst.png",{6,4,.98f,410,false},error))throw std::runtime_error(error);old.setBrightness(a.brightness);old.setScale(a.scale);old.setIntensity(a.intensity);old.setDuration(a.spriteDuration);old.setSpeed(a.speed);old.setColorGrade(a.saturation,0,a.resolvedPaletteColors,a.paletteOverride);old.push(10,.5f,.5f);
   for(int i=0;i<=144;++i){renderer.beginFrame();if(!old.draw(renderer.context(),1920,720,10+i/120.,error)||!renderer.saveFrame(folder/("old-"+std::to_string(i)+".png"),error))throw std::runtime_error(error);}
  }
  std::cout<<names[style]<<" continuous 120/60 FPS, pause and bounded-input captures passed\n";
 }
 return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
