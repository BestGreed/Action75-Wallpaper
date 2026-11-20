#include "FieldEffect.h"
#include "NativeRenderer.h"
#include <iostream>
#include <fstream>
using namespace action75;
int main(int argc,char** argv) {
 try {
  if(argc<3||argc>4)throw std::runtime_error("Need pack and output directories, optional saved profile");
  CoInitializeEx(nullptr,COINIT_MULTITHREADED);
  std::filesystem::path pack=argv[1],out=argv[2];std::filesystem::create_directories(out);
  PackSettingsStore store(out/"profile");store.registerPack("crystal-herbarium",pack,pack/"settings.json");
  auto candidate=store.prepare("crystal-herbarium",nlohmann::json::object(),{},true);
  if(argc==4){std::ifstream in(argv[3]);nlohmann::json saved;in>>saved;candidate=store.prepare("crystal-herbarium",saved.at("values"),{},false);}
  auto a=store.evaluate(candidate);a.field["sparks"]=0;a.field["ambient"]=0;
  NativeRenderer renderer;FieldEffect snow;std::string error;
  if(!renderer.initialize(nullptr,1920,720,true,error,true)||!snow.load(renderer.device(),pack/"layout.json",2,error))throw std::runtime_error(error);
  snow.configure(a);snow.push(10,.5f,.5f);
  for(int i=0;i<=150;++i) {
   renderer.beginFrame();if(!snow.draw(renderer.context(),1920,720,10+i/15.,false,error))throw std::runtime_error(error);
   if(!renderer.saveFrame(out/("snow-"+std::to_string(i)+".png"),error))throw std::runtime_error(error);
  }
  for(int i=0;i<12;++i){snow.clear();double born=30+i*10.;snow.push(born,.5f,.5f);renderer.beginFrame();
   if(!snow.draw(renderer.context(),1920,720,born+1.8,false,error)||!renderer.saveFrame(out/("variant-"+std::to_string(i)+".png"),error))throw std::runtime_error(error);
  }
  std::cout<<"Single crystal growth through complete fade rendered at 1920x720\n";
  return 0;
 } catch(const std::exception& e) {std::cerr<<e.what()<<"\n";return 1;}
}
