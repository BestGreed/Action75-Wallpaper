#include "KeycapEffect.h"
#include "NativeRenderer.h"
#include <iostream>
#include <stdexcept>
using namespace action75;
int main(int argc,char** argv) {
 try {
  if(argc!=3)throw std::runtime_error("pack and output paths required");
  CoInitializeEx(nullptr,COINIT_MULTITHREADED);
  const std::filesystem::path pack=argv[1],out=argv[2];std::filesystem::create_directories(out);
  PackSettingsStore settings(out/"profiles");settings.registerPack("keycap-glow",pack,pack/"settings.json");
  auto profile=settings.prepare("keycap-glow",nlohmann::json::object(),{},true);
  auto appearance=settings.evaluate(profile);
  if(appearance.keycap.size()!=17)throw std::runtime_error("Missing keycap bindings");
  appearance.keycap["flowSpeed"]=0; // Freeze ambient motion for strict pixel comparisons.
  NativeRenderer renderer;KeycapEffect caps;std::string error;
  if(!renderer.initialize(nullptr,1920,720,true,error)||!caps.load(renderer.device(),pack/"layout.json",error))throw std::runtime_error(error);
  caps.configure(appearance);
  auto frame=[&](double time,const std::string& name){renderer.beginFrame();if(!caps.draw(renderer.context(),1920,720,time,error)||!renderer.saveFrame(out/name,error))throw std::runtime_error(error);};
  frame(10,"idle.png");
  if(!caps.push("KeyA",10)||caps.push("InvalidKey",10))throw std::runtime_error("Identity routing failed");
  frame(10.001,"attack.png");frame(10.08,"peak.png");frame(10.4,"fade.png");frame(11,"done.png");
  if(caps.count()!=0)throw std::runtime_error("Animation never became idle");
  caps.push("Enter",12);caps.push("Space",12);caps.push("Fn",12);frame(12.08,"chord.png");
  for(int design=0;design<3;++design){auto a=appearance;a.keycap["design"]=design;a.keycap["outline"]=.7;caps.configure(a);frame(12.08,"style-"+std::to_string(design)+".png");}
  caps.configure(appearance);
  caps.clear();frame(13,"clear.png");
  auto patch=settings.prepare("keycap-glow",{{"lineWidth",3},{"outline",.6},{"attack",.1},{"color",{255,100,40}},{"offsetX",-5}});
  settings.commit(patch);caps.configure(settings.evaluate(patch));frame(14,"adjusted.png");
  for(int i=0;i<40;++i){caps.clear();caps.push("KeyA",20);frame(20+i*.025,"anim-"+std::to_string(i)+".png");}
  for(int design=0;design<3;++design) {
    auto a=appearance;a.keycap["design"]=design;a.keycap["flowSpeed"]=.45;a.keycap["outline"]=.85;
    caps.configure(a);caps.clear();frame(30,"ambient-"+std::to_string(design)+"-a.png");frame(31,"ambient-"+std::to_string(design)+"-b.png");
    caps.clear();frame(30.08,"baseline-"+std::to_string(design)+".png");
    caps.push("KeyA",30);frame(30.08,"pressed-"+std::to_string(design)+".png");
    for(int i=0;i<32;++i) {
      caps.clear();caps.push("KeyA",30);caps.push("KeyS",30.10);caps.push("KeyD",30.20);caps.push("Space",30.28);
      frame(30+i*.03,"showcase-"+std::to_string(design)+"-"+std::to_string(i)+".png");
    }
  }
  std::cout<<"Schema evaluation, identity routing, expiry, chord, reset and settings write passed\n";
  CoUninitialize();return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
