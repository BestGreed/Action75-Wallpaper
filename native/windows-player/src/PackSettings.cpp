#include "PackSettings.h"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif

namespace action75 {
struct VariantPlayback {
  std::string selection, randomize, interval;
  std::array<std::string, VariantPlaybackRules::maxVariants> toggles;
  std::array<unsigned, VariantPlaybackRules::maxVariants> order{};

  bool uses(const std::string& id) const {
    return id==selection || id==randomize || id==interval ||
      std::find(toggles.begin(),toggles.end(),id)!=toggles.end();
  }
};

namespace {
using json=nlohmann::json;
using fs=std::filesystem::path;
constexpr std::size_t MaxSettingsBytes=32*1024;
constexpr std::size_t MaxProfileBytes=64*1024;
bool identifier(const std::string& value,std::size_t maximum=64) {
  return !value.empty() && value.size()<=maximum && std::all_of(value.begin(),value.end(),[](unsigned char c){
    return std::isalnum(c) && c<128 || c=='_' || c=='-';
  });
}
std::string readText(const fs& file,std::size_t maximum=MaxSettingsBytes) {
  std::ifstream input(file,std::ios::binary);
  if(!input) throw std::runtime_error("Cannot read wallpaper settings file");
  input.seekg(0,std::ios::end);
  const auto bytes=input.tellg();
  if(bytes<0 || bytes>static_cast<std::streamoff>(maximum))
    throw std::runtime_error(maximum==MaxSettingsBytes ? "Wallpaper settings file exceeds 32 KiB" :
                                                       "Wallpaper settings profile exceeds 64 KiB");
  input.seekg(0);
  std::string text(static_cast<std::size_t>(bytes), '\0');
  if(!input.read(text.data(),static_cast<std::streamsize>(bytes)) || input.peek()!=std::char_traits<char>::eof())
    throw std::runtime_error("Wallpaper settings file changed while reading");
  return text;
}
json parseSettings(const std::string& text) {
  return json::parse(text,[](int depth,json::parse_event_t,json&) {
    if(depth>24) throw std::runtime_error("Wallpaper settings nesting exceeds 24 levels");
    return true;
  });
}
json readFile(const fs& file,std::size_t maximum=MaxSettingsBytes) { return parseSettings(readText(file,maximum)); }
void atomicWrite(const fs& file,const json& value) {
  const auto text=value.dump();
  if(text.size()>MaxProfileBytes) throw std::runtime_error("Wallpaper settings profile exceeds 64 KiB");
  std::filesystem::create_directories(file.parent_path());
  auto temporary=file; temporary+=L".tmp";
  {
    std::ofstream output(temporary,std::ios::binary|std::ios::trunc);
    output.write(text.data(),static_cast<std::streamsize>(text.size()));
    output.flush();
    if(!output) throw std::runtime_error("Cannot write wallpaper settings");
  }
#ifdef _WIN32
  if(!MoveFileExW(temporary.c_str(),file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
    throw std::runtime_error("Cannot replace wallpaper settings file");
#else
  std::filesystem::rename(temporary,file);
#endif
}
bool sameProfileContents(const fs& first,const fs& second) {
  if(std::filesystem::file_size(first)!=std::filesystem::file_size(second)) return false;
  std::ifstream a(first,std::ios::binary),b(second,std::ios::binary);
  if(!a || !b) throw std::runtime_error("Cannot verify wallpaper settings recovery backup");
  std::array<char,65536> x{},y{};
  do {
    a.read(x.data(),static_cast<std::streamsize>(x.size()));
    b.read(y.data(),static_cast<std::streamsize>(y.size()));
    if(a.bad() || b.bad()) throw std::runtime_error("Cannot verify wallpaper settings recovery backup");
    if(a.gcount()!=b.gcount() || !std::equal(x.begin(),x.begin()+a.gcount(),y.begin())) return false;
  } while(!a.eof() && !b.eof());
  return a.eof() && b.eof();
}
fs backupDamagedProfile(const fs& file) {
  const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
  auto backup=file;backup+=L".before-recovery-"+std::to_wstring(stamp)+L".json";
  std::filesystem::copy_file(file,backup);
  if(!sameProfileContents(file,backup)) throw std::runtime_error("Wallpaper settings changed during recovery; refresh before restoring defaults");
  return backup;
}
double number(const json& value) {
  if(!value.is_number()) throw std::runtime_error("A wallpaper setting requires a number");
  const double result=value.get<double>();
  if(!std::isfinite(result) || std::abs(result)>1000000) throw std::runtime_error("Wallpaper setting number is out of range");
  return result;
}
EffectRgb rgb(const json& value) {
  if(!value.is_array() || value.size()!=3) throw std::runtime_error("A color requires three RGB channels");
  EffectRgb result{};
  for(std::size_t i=0;i<3;++i) {
    if(!value[i].is_number_integer()) throw std::runtime_error("RGB channels must be integers");
    const auto channel=value[i].get<std::int64_t>();
    if(channel<0 || channel>255) throw std::runtime_error("RGB channel is out of range");
    result[i]=static_cast<std::uint8_t>(channel);
  }
  return result;
}
std::vector<EffectRgb> colors(const json& value,unsigned maximum=9) {
  if(!value.is_array() || value.empty() || value.size()>maximum)
    throw std::runtime_error("A palette requires between one and nine colors");
  std::vector<EffectRgb> result;
  for(const auto& color:value) result.push_back(rgb(color));
  return result;
}
std::vector<const json*> controls(const json& schema) {
  std::vector<const json*> result;
  if(schema.is_null()) return result;
  for(const auto& group:schema.at("groups")) for(const auto& control:group.at("controls")) result.push_back(&control);
  return result;
}
const json* findControl(const json& schema,const std::string& id) {
  for(const auto* control:controls(schema)) if(control->at("id")==id) return control;
  return nullptr;
}
bool playbackControl(const VariantPlayback* playback,const json& control) {
  return playback && playback->uses(control.at("id").get_ref<const std::string&>());
}
unsigned variantId(const json& value) {
  const auto slot=number(value);
  if(slot<0 || slot>VariantPlaybackRules::maxVariants || slot!=std::trunc(slot))
    throw std::runtime_error("Variant playback option is outside the supported slot range");
  return static_cast<unsigned>(slot);
}
std::shared_ptr<const VariantPlayback> compileVariantPlayback(const json& schema) {
  std::shared_ptr<VariantPlayback> playback;
  for(const auto& group:schema.at("groups")) {
    if(!group.contains("presentation") || group.at("presentation")!="variant-playback") continue;
    if(playback) throw std::runtime_error("A wallpaper can declare one variant playback group");
    const auto& rule=group.at("playback");
    const auto& items=group.at("controls");
    if(!rule.is_object() || !items.is_array())
      throw std::runtime_error("Invalid variant playback group");
    playback=std::make_shared<VariantPlayback>();
    std::set<std::string> used;
    const auto useControl=[&](const std::string& id,const char* type)->const json& {
      const auto control=std::find_if(items.begin(),items.end(),[&](const json& item){
        return item.at("id")==id;
      });
      if(control==items.end() || control->at("type")!=type || !used.insert(id).second)
        throw std::runtime_error("Invalid or reused variant playback control: "+id);
      return *control;
    };
    playback->selection=rule.at("selection").get<std::string>();
    playback->randomize=rule.at("randomize").get<std::string>();
    playback->interval=rule.at("interval").get<std::string>();
    const auto& options=useControl(playback->selection,"select").at("options");
    useControl(playback->randomize,"toggle");
    const auto& interval=useControl(playback->interval,"slider");
    for(const auto* field:{"min","max","step"}) {
      const double v=number(interval.at(field));
      if(v<1 || std::trunc(v)!=v)
        throw std::runtime_error("Variant playback interval bounds and step must be positive integers");
    }
    if(!options.is_array() || options.size()<2 ||
       options.size()>VariantPlaybackRules::maxVariants+1)
      throw std::runtime_error("Variant playback needs a combination and at least one shape");
    unsigned variants=0;
    bool combination=false;
    for(const auto& option:options) {
      const auto value=variantId(option.at("value"));
      if(!value) {
        if(combination || (option.contains("toggleId") && !option.at("toggleId").is_null()))
          throw std::runtime_error("Variant playback combination has no toggle");
        combination=true;
        continue;
      }
      const auto bit=1u<<(value-1);
      if(variants&bit) throw std::runtime_error("Duplicate variant playback option");
      auto& toggle=playback->toggles[value-1];
      toggle=option.at("toggleId").get<std::string>();
      useControl(toggle,"toggle");
      variants|=bit;
    }
    const auto& order=rule.at("order");
    if(!combination || !order.is_array() || order.size()!=options.size()-1)
      throw std::runtime_error("Variant playback order must contain each declared shape");
    for(std::size_t i=0;i<order.size();++i) {
      const auto value=variantId(order[i]);
      if(!value || !(variants&(1u<<(value-1))))
        throw std::runtime_error("Variant playback order must contain each declared shape once");
      variants&=~(1u<<(value-1));
      playback->order[i]=value;
    }
  }
  return playback;
}
const json* findLocalPalette(const json& control,const std::string& id) {
  if(!control.contains("palettes")) return nullptr;
  for(const auto& palette:control.at("palettes")) if(palette.at("id")==id) return &palette;
  return nullptr;
}
json mapping(const json& control) {
  const auto& binding=control.at("binding");
  return binding.value("mapping",json{{"type","identity"}});
}
double mapNumber(const json& control,double value) {
  const auto map=mapping(control);
  const auto type=map.value("type",std::string("identity"));
  if(type=="identity") return value;
  if(type=="piecewise") {
    const auto& points=map.at("points");
    if(value<=number(points.front()[0])) return number(points.front()[1]);
    for(std::size_t i=1;i<points.size();++i) {
      const double x=number(points[i][0]);
      if(value<=x) {
        const double x0=number(points[i-1][0]),y0=number(points[i-1][1]);
        return y0+(number(points[i][1])-y0)*(value-x0)/(x-x0);
      }
    }
    return number(points.back()[1]);
  }
  const double lo=number(map.at("inputMin")),hi=number(map.at("inputMax"));
  double t=std::clamp((value-lo)/(hi-lo),0.0,1.0);
  if(type=="power") t=std::pow(t,number(map.at("exponent")));
  return number(map.at("outputMin"))+(number(map.at("outputMax"))-number(map.at("outputMin")))*t;
}
double inverseNumber(const json& control,double value) {
  const auto map=mapping(control);
  const auto type=map.value("type",std::string("identity"));
  double result=value;
  if(type=="piecewise") {
    const auto& points=map.at("points");
    const bool increasing=number(points.back()[1])>=number(points.front()[1]);
    if(increasing ? value<=number(points.front()[1]) : value>=number(points.front()[1])) result=number(points.front()[0]);
    else {
      result=number(points.back()[0]);
      for(std::size_t i=1;i<points.size();++i) {
        const double y=number(points[i][1]),previous=number(points[i-1][1]);
        if(increasing ? value<=y : value>=y) {
          const double x=number(points[i][0]),x0=number(points[i-1][0]);
          result=y==previous ? x0 : x0+(x-x0)*(value-previous)/(y-previous);
          break;
        }
      }
    }
  } else if(type=="linear" || type=="power") {
    const double y0=number(map.at("outputMin")),y1=number(map.at("outputMax"));
    double t=y0==y1 ? 0 : std::clamp((value-y0)/(y1-y0),0.0,1.0);
    if(type=="power") t=std::pow(t,1.0/number(map.at("exponent")));
    result=number(map.at("inputMin"))+(number(map.at("inputMax"))-number(map.at("inputMin")))*t;
  }
  if(control.at("type")=="slider") result=std::clamp(result,number(control.at("min")),number(control.at("max")));
  return result;
}
constexpr const char* fireworkChoices[]{"fireworks.willow","fireworks.chrys","fireworks.peony",
    "fireworks.palm","fireworks.kaleidoscope","fireworks.bouquet","fireworks.saturn","fireworks.heart"};
static_assert(std::size(fireworkChoices)<=FireworksAppearance::shapeCount);
std::pair<double,double> targetRange(const std::string& target) {
  static const std::unordered_map<std::string,std::pair<double,double>> capRanges={
    {"runtime.idleFps",{15,120}},{"runtime.idleDelay",{1,15}},
    {"blackHole.centerX",{0,1}},{"blackHole.centerY",{0,1}},
    {"blackHole.lensStrength",{0,1.25}},{"blackHole.diskBrightness",{.25,2}},
    {"blackHole.size",{.25,3.5}},{"blackHole.starFlare",{0,2.5}},
    {"blackHole.diskPalette",{0,2}},
    {"blackHole.sky",{0,2}},
    {"blackHole.nebulaBrightness",{0,2}},{"blackHole.nebulaSpeed",{0,2}},
    {"blackHole.voidBrightness",{0,2}},{"blackHole.voidSpeed",{0,2}},
    {"blackHole.starfieldBrightness",{0,2}},{"blackHole.starfieldSpeed",{0,2}},
    {"waterSurface.damping",{.8,3.6}},{"waterSurface.ambient",{0,1}},
    {"waterSurface.impactMin",{0,2.4}},{"waterSurface.impactMax",{0,2.4}},
    {"waterSurface.backdrop",{0,static_cast<int>(WaterBackdrop::Custom)}},
    {"waterSurface.texture",{0,2}},{"waterSurface.caustics",{0,2}},
    {"glassShatter.impactMin",{0,GlassShatterAppearance::maxImpactStrength}},
    {"glassShatter.impactMax",{0,GlassShatterAppearance::maxImpactStrength}},
    {"glassShatter.backdrop",{0,static_cast<int>(GlassBackdrop::Solid)}},
    {"glassShatter.spread",{.55,GlassShatterAppearance::maxSpread}},{"glassShatter.duration",{.4,2.8}},
    {"glassShatter.branches",{0,GlassShatterAppearance::maxBranches}},
    {"glassShatter.seamWidth",{.55,1.5}},{"glassShatter.glints",{0,2}},
    {"glassShatter.refraction",{0,2}},{"glassShatter.chips",{0,GlassShatterAppearance::maxChips}},
    {"fireworks.shape",{0,FireworksAppearance::shapeCount}},{"fireworks.quality",{0,2}},
    {"fireworks.keyInterval",{1,1000000}},
    {"fireworks.shuffle",{0,1}},{"fireworks.randomPalette",{0,1}},{"fireworks.spread",{.5,6}},
    {"fireworks.density",{.25,1}},
    {"fireworks.trail",{0,1.6}},{"fireworks.gravity",{0,2}},{"fireworks.lifetime",{.55,1.5}},
    {"fireworks.shimmer",{0,1}},{"fireworks.embers",{0,1.5}},{"fireworks.bloom",{0,1.5}},
    {"fireworks.smoke",{0,1}},{"fireworks.reflection",{0,1}},{"fireworks.horizon",{.69,.96}},
    {"field.persistence",{.4,8}},{"field.turbulence",{0,2}},{"field.propagation",{.2,2.5}},
    {"field.ambient",{0,1}},{"field.detail",{0,1}},{"field.bloom",{0,2}},
    {"field.filmShape",{0,6}},{"field.filmSpread",{.8,2.4}},{"field.filmStretch",{.4,2}},{"field.filmTear",{0,1}},
    {"field.fluidViscosity",{0,1.5}},{"field.fluidCohesion",{0,1.5}},{"field.fluidPressure",{.2,2}},{"field.fluidGravity",{0,2}},{"field.fluidVariation",{0,1}},
    {"field.pixelSize",{2,12}},{"field.fragmentDensity",{0,1}},{"field.shatterForce",{.2,2}},{"field.fragmentDrift",{0,2}},{"field.fragmentOpacity",{.15,1}},
    {"field.arcCount",{1,3}},{"field.arcWidth",{.4,2.5}},{"field.arcSweep",{.2,2}},{"field.arcEccentricity",{.25,.9}},
    {"field.wordmarkEnabled",{0,1}},{"field.wordmarkBrightness",{0,2}},{"field.wordmarkFlow",{0,2}},{"field.wordmarkScale",{.5,1}},{"field.wordmarkOffsetY",{-40,40}},
    {"field.diversity",{0,1}},{"field.density",{0,1}},{"field.opacityVariation",{0,1}},
    {"field.thickness",{0,1.5}},{"field.refraction",{0,1.5}},{"field.variation",{0,1}},{"field.melt",{0,1}},
    {"field.sparks",{0,1}},{"field.quality",{0,2}},
    {"keycap.design",{0,2}},{"keycap.flowSpeed",{0,2}},{"keycap.detail",{0,1}},
    {"keycap.chromatic",{0,1}},{"keycap.ambient",{0,1}},
    {"keycap.attack",{.01,.3}},{"keycap.hold",{0,.5}},{"keycap.release",{.1,2.5}},
    {"keycap.lineWidth",{.5,4}},{"keycap.radius",{0,24}},{"keycap.outline",{0,1}},
    {"keycap.softness",{0,1}},{"keycap.offsetX",{-120,120}},{"keycap.offsetY",{-80,80}},
    {"keycap.scaleX",{.8,1.05}},{"keycap.scaleY",{.8,1.05}},{"keycap.capScale",{.7,1}}};
  const auto cap=capRanges.find(target);if(cap!=capRanges.end())return cap->second;
  if(std::find(std::begin(fireworkChoices),std::end(fireworkChoices),target)!=std::end(fireworkChoices))
    return {0,1};
  if(target=="effectBrightness") return {0,4};
  if(target=="effectScale") return {.5,3};
  if(target=="effectIntensity") return {0,3};
  if(target=="effectSaturation") return {0,2.5};
  if(target=="effectHue") return {-180,180};
  if(target=="background.style" || target=="background.fit") return {0,1};
  if(target=="background.zoom") return {.25,4};
  if(target=="background.panX" || target=="background.panY") return {-1,1};
  if(target=="background.brightness") return {0,4};
  if(target=="background.saturation") return {0,2.5};
  if(target=="background.hue") return {0,360};
  if(target=="sprite.duration") return {.12,3};
  if(target=="effect.speed") return {.25,3};
  if(target=="effect.dynamicInput") return {-1000000,1000000};
  if(target=="material.uniform") return {-1000000,1000000};
  throw std::runtime_error("Unsupported wallpaper settings target: "+target);
}
bool colorTarget(const std::string& target) {
  return target=="effectColor" || target=="background.color" || target=="background.solidColor" || target=="keycap.outlineColor" ||
         target=="blackHole.nebulaTint" || target=="blackHole.voidTint" || target=="blackHole.starfieldTint";
}
void validateValue(const json& control,const json& value,const VariantPlayback* playback=nullptr,
                   bool authorValues=false) {
  const auto type=control.at("type").get<std::string>();
  if(type=="slider") {
    const double v=number(value);
    if(v<number(control.at("min")) || v>number(control.at("max"))) throw std::runtime_error("Wallpaper slider value is out of range");
  } else if(type=="toggle") {
    if(!value.is_boolean()) throw std::runtime_error("Wallpaper toggle requires a boolean");
  } else if(type=="select") {
    bool valid=false;
    for(const auto& option:control.at("options")) if(option.at("value")==value) valid=true;
    if(!valid) throw std::runtime_error("Unknown wallpaper setting option");
  } else if(type=="color") {
    rgb(value);
  } else if(type=="palette") {
    if(!value.is_object() || !value.contains("preset") || !value["preset"].is_string() || !value.contains("colors"))
      throw std::runtime_error("Invalid wallpaper palette value");
    for(auto item=value.begin();item!=value.end();++item)
      if(item.key()!="preset" && item.key()!="colors") throw std::runtime_error("Unknown palette property");
    const auto preset=value["preset"].get<std::string>();
    if(control.contains("palettes")) {
      if(preset!="custom" && !findLocalPalette(control,preset)) throw std::runtime_error("Color palette is not offered by this wallpaper");
    } else if(!findEffectPalette(preset)) throw std::runtime_error("Unknown wallpaper color palette");
    if(!control.contains("palettes") && control.contains("presets") && std::none_of(control["presets"].begin(),control["presets"].end(),
          [&](const json& allowed){return allowed==preset;})) throw std::runtime_error("Color palette is not offered by this wallpaper");
    colors(value.at("colors"),control.value("maxColors",9u));
  }
  if(authorValues) return;
  if(playbackControl(playback,control)) {
    if(control.at("id")==playback->interval) {
      const auto interval=number(value);
      if(interval!=std::trunc(interval))
        throw std::runtime_error("Variant playback interval must be a positive integer");
    }
    return;
  }
  const auto target=control.at("binding").at("target").get<std::string>();
  if(colorTarget(target) || target=="effectPalette") return;
  const double v=mapNumber(control,value.is_boolean() ? (value.get<bool>() ? 1.0 : 0.0) : number(value));
  const auto range=targetRange(target);
  if(!std::isfinite(v) || v<range.first-1e-6 || v>range.second+1e-6)
    throw std::runtime_error("Mapped wallpaper setting exceeds its renderer range");
  if(target=="fireworks.keyInterval" && v!=std::trunc(v))
    throw std::runtime_error("Fireworks key interval must be a positive integer");
}
void validateMapping(const json& control) {
  const auto map=mapping(control);
  if(!map.is_object()) throw std::runtime_error("Wallpaper setting mapping must be an object");
  const auto type=map.value("type",std::string("identity"));
  if(type=="identity") return;
  double domainMin=0,domainMax=1;
  if(type=="piecewise") {
    if(!map.contains("points") || !map["points"].is_array() || map["points"].size()<2 || map["points"].size()>32)
      throw std::runtime_error("Piecewise mapping requires 2..32 points");
    double previous=-std::numeric_limits<double>::infinity(),previousY=0;
    int direction=0;
    for(const auto& point:map["points"]) {
      if(!point.is_array() || point.size()!=2) throw std::runtime_error("Invalid mapping point");
      const double x=number(point[0]),y=number(point[1]);
      if(x<=previous) throw std::runtime_error("Mapping input points must increase");
      if(std::isfinite(previous) && y!=previousY) {
        const int next=y>previousY ? 1 : -1;
        if(direction && next!=direction) throw std::runtime_error("Mapping outputs must be monotonic");
        direction=next;
      }
      previous=x;previousY=y;
    }
    domainMin=number(map["points"].front()[0]);domainMax=number(map["points"].back()[0]);
  } else if(type=="linear" || type=="power") {
    domainMin=number(map.at("inputMin"));domainMax=number(map.at("inputMax"));
    number(map.at("outputMin"));number(map.at("outputMax"));
    if(domainMax<=domainMin) throw std::runtime_error("Invalid wallpaper mapping input range");
    if(type=="power") {
      const double exponent=number(map.at("exponent"));
      if(exponent<=0 || exponent>8) throw std::runtime_error("Mapping exponent must be in (0,8]");
    }
  } else throw std::runtime_error("Unknown wallpaper mapping type");
  if(control["type"]=="slider" &&
      (number(control.at("min"))<domainMin || number(control.at("max"))>domainMax))
    throw std::runtime_error("Slider exceeds its mapping input range");
}
json validateSchema(const json& schema,std::shared_ptr<const VariantPlayback>& playback,bool authorValues) {
  if(!schema.is_object() || schema.value("version",0)!=1 || !schema.contains("groups") ||
      !schema["groups"].is_array() || schema["groups"].size()>16) throw std::runtime_error("Unsupported wallpaper settings schema");
  playback=authorValues ? nullptr : compileVariantPlayback(schema);
  const auto* rules=playback.get();
  json defaults=json::object();
  std::set<std::string> groupIds,targets;
  unsigned count=0;
  for(const auto& group:schema["groups"]) {
    const auto id=group.at("id").get<std::string>(),label=group.at("label").get<std::string>();
    if(!identifier(id) || !groupIds.insert(id).second || label.empty() || label.size()>512 ||
        !group.at("controls").is_array()) throw std::runtime_error("Invalid wallpaper settings group");
    for(const auto& control:group["controls"]) {
      if(++count>64) throw std::runtime_error("A wallpaper exposes at most 64 settings");
      const auto cid=control.at("id").get<std::string>(),type=control.at("type").get<std::string>();
      const auto title=control.at("label").get<std::string>();
      if(!identifier(cid) || defaults.contains(cid) || title.empty() || title.size()>512)
        throw std::runtime_error("Invalid or duplicate wallpaper control ID");
      if(type!="slider" && type!="select" && type!="toggle" && type!="color" && type!="palette")
        throw std::runtime_error("Unsupported wallpaper control type");
      if(!authorValues && !playbackControl(rules,control)) {
        const auto& binding=control.at("binding");
        const auto target=binding.at("target").get<std::string>();
        std::string bindingId=target;
        if(colorTarget(target)) {
          if(type!="color") throw std::runtime_error("Color binding requires a color control");
        } else if(target=="effectPalette") {
          if(type!="palette") throw std::runtime_error("Effect palette requires a palette control");
        } else {
          targetRange(target);
          if(type!="slider" && type!="toggle" && type!="select") throw std::runtime_error("Numeric binding requires a numeric control");
          if(target=="material.uniform") {
            const auto name=binding.at("name").get<std::string>();
            const int component=binding.value("component",0);
            if(!identifier(name,128) || component<0 || component>3) throw std::runtime_error("Invalid material uniform binding");
            bindingId+="/"+name+"/"+std::to_string(component);
          } else if(target=="effect.dynamicInput") {
            const auto& index=binding.at("index");
            if(!index.is_number_integer() || index<0 || index>=4)
              throw std::runtime_error("Effekseer dynamic input index must be 0..3");
            bindingId+="/"+std::to_string(index.get<unsigned>());
          }
        }
        if(!targets.insert(bindingId).second) throw std::runtime_error("Duplicate wallpaper renderer binding");
        validateMapping(control);
      }
      if(type=="slider") {
        const double lo=number(control.at("min")),hi=number(control.at("max")),step=number(control.at("step"));
        if(hi<=lo || step<=0 || step>hi-lo) throw std::runtime_error("Invalid slider bounds or step");
      }
      if(type=="select") {
        const auto& options=control.at("options");
        if(!options.is_array() || options.empty() || options.size()>64) throw std::runtime_error("Invalid select options");
        for(const auto& option:options) {
          if(!option.is_object() || !option.contains("value") || !option.at("label").is_string())
            throw std::runtime_error("Invalid select option");
        }
      }
      if(type=="palette") {
        const int maximum=control.value("maxColors",9);
        if(maximum<1 || maximum>9) throw std::runtime_error("Palette maxColors must be 1..9");
        if(control.contains("palettes")) {
          const auto& palettes=control.at("palettes");
          if(!palettes.is_array() || palettes.empty() || palettes.size()>32)
            throw std::runtime_error("Wallpaper palettes require 1..32 named color sets");
          std::set<std::string> ids;
          for(const auto& palette:palettes) {
            if(!palette.is_object()) throw std::runtime_error("Invalid wallpaper palette descriptor");
            const auto pid=palette.at("id").get<std::string>(),name=palette.at("name").get<std::string>();
            if(!identifier(pid) || pid=="custom" || !ids.insert(pid).second || name.empty() || name.size()>256)
              throw std::runtime_error("Invalid or duplicate wallpaper palette ID or name");
            if(palette.contains("source") && !palette["source"].is_boolean())
              throw std::runtime_error("Wallpaper palette source flag must be boolean");
            colors(palette.at("colors"),static_cast<unsigned>(maximum));
          }
        } else if(control.contains("presets")) {
          if(!control["presets"].is_array() || control["presets"].empty() || control["presets"].size()>9)
            throw std::runtime_error("Invalid allowed palette list");
          for(const auto& preset:control["presets"])
            if(!preset.is_string() || !findEffectPalette(preset.get<std::string>())) throw std::runtime_error("Unknown allowed palette");
        }
      }
      validateValue(control,control.at("default"),rules,authorValues);
      if(type=="slider") {
        validateValue(control,control.at("min"),rules,authorValues);
        validateValue(control,control.at("max"),rules,authorValues);
      }
      if(type=="toggle") {
        validateValue(control,false,rules,authorValues);
        validateValue(control,true,rules,authorValues);
      }
      if(type=="select") for(const auto& option:control["options"])
        validateValue(control,option.at("value"),rules,authorValues);
      defaults[cid]=control.at("default");
    }
  }
  for(const auto* control:controls(schema)) if(control->contains("visibleWhen")) {
    const auto& condition=control->at("visibleWhen");
    const auto ref=condition.at("id").get<std::string>();
    const auto* referenced=findControl(schema,ref);
    if(!referenced || ref==control->at("id") || !condition.contains("equals")) throw std::runtime_error("Invalid control visibility condition");
    validateValue(*referenced,condition.at("equals"),rules,authorValues);
  }
  if(schema.contains("orderedRange")) {
    const auto& range=schema.at("orderedRange");
    if(!range.is_array() || range.size()!=2 || range[0]==range[1])
      throw std::runtime_error("An ordered range needs two different sliders");
    const auto* low=findControl(schema,range[0].get<std::string>());
    const auto* high=findControl(schema,range[1].get<std::string>());
    if(!low || !high || low->at("type")!="slider" || high->at("type")!="slider" ||
       low->at("min")!=high->at("min") || low->at("max")!=high->at("max"))
      throw std::runtime_error("Ordered sliders must have matching bounds");
  }
  return defaults;
}
void validateValues(const json& schema,const json& values,const VariantPlayback* playback,bool authorValues) {
  if(!values.is_object()) throw std::runtime_error("Wallpaper values must be an object");
  for(auto item=values.begin();item!=values.end();++item) {
    const auto* control=findControl(schema,item.key());
    if(!control) throw std::runtime_error("Unknown wallpaper setting: "+item.key());
    validateValue(*control,item.value(),playback,authorValues);
  }
}
bool restoreRenamedFireworksControls(PackSettingsCandidate& candidate,const json& saved) {
  if(candidate.id!="night-fireworks" || !candidate.playback) return false;
  const auto& rules=*candidate.playback;
  bool restored=false;
  const auto restore=[&](const std::string& id,const char* oldId) {
    if(id.empty() || saved.contains(id) || candidate.values.contains(oldId) || !saved.contains(oldId))
      return;
    try {
      validateValue(*findControl(candidate.schema,id),saved.at(oldId),&rules);
      candidate.values[id]=saved.at(oldId);
      restored=true;
    } catch(const std::exception&) { /* Keep the default if the option or range changed. */ }
  };
  restore(rules.selection,"shape");
  restore(rules.randomize,"shuffle");
  restore(rules.interval,"keyInterval");
  constexpr const char* legacyIds[]{"useWillow","useChrys","usePeony","usePalm",
      "useKaleidoscope","useBouquet","useSaturn","useHeart"};
  static_assert(std::size(legacyIds)<=FireworksAppearance::shapeCount);
  for(std::size_t i=0;i<std::size(legacyIds);++i) restore(rules.toggles[i],legacyIds[i]);
  return restored;
}
VariantPlaybackRules evaluateVariantPlayback(const VariantPlayback& playback,const json& values) {
  VariantPlaybackRules appearance;
  appearance.shape=values.at(playback.selection).get<unsigned>();
  appearance.shuffle=values.at(playback.randomize).get<bool>();
  appearance.keyInterval=values.at(playback.interval).get<unsigned>();
  appearance.order=playback.order;
  appearance.selected=0;
  for(std::size_t i=0;i<playback.toggles.size();++i) {
    const auto& toggle=playback.toggles[i];
    if(!toggle.empty() && values.at(toggle).get<bool>()) appearance.selected|=1u<<i;
  }
  return appearance;
}
bool normalizeImpactRange(PackSettingsCandidate& candidate, const json& patch) {
  auto& values=candidate.values;
  std::string low="impactMin", high="impactMax";
  if(candidate.schema.contains("orderedRange")) {
    const auto& range=candidate.schema.at("orderedRange");
    low=range.at(0).get<std::string>();
    high=range.at(1).get<std::string>();
  } else if(candidate.authorValues || (candidate.id!="stillwater" && candidate.id!="glass-shatter")) {
    return false;
  }
  if(!values.contains(low) || !values.contains(high)) return false;
  auto& lower=values.at(low);
  auto& upper=values.at(high);
  if(number(lower)<=number(upper)) return false;
  // Dragging one end past the other moves the opposite end with it.
  const bool movedLower=patch.contains(low),movedUpper=patch.contains(high);
  if(movedLower==movedUpper) std::swap(lower,upper);
  else if(movedLower) upper=lower;
  else lower=upper;
  return true;
}
std::uint64_t nextRevision(std::uint64_t revision) {
  if(revision==std::numeric_limits<std::uint64_t>::max()) throw std::runtime_error("Wallpaper settings revision is exhausted");
  return revision+1;
}
}

PackSettingsStore::PackSettingsStore(std::filesystem::path directory):stateDirectory_(std::move(directory)) {}
bool PackSettingsStore::validPackId(const std::string& id) {
  if(!identifier(id,80)) return false;
  std::string upper=id;for(auto& c:upper)c=static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  if(upper=="CON" || upper=="PRN" || upper=="AUX" || upper=="NUL") return false;
  if(upper.size()==4 && (upper.substr(0,3)=="COM" || upper.substr(0,3)=="LPT") && upper[3]>='1' && upper[3]<='9') return false;
  return true;
}
PackSettingsStore::Entry& PackSettingsStore::entry(const std::string& id) {
  const auto found=entries_.find(id);if(found==entries_.end()) throw std::runtime_error("Wallpaper is not registered");return found->second;
}
const PackSettingsStore::Entry& PackSettingsStore::entry(const std::string& id) const {
  const auto found=entries_.find(id);if(found==entries_.end()) throw std::runtime_error("Wallpaper is not registered");return found->second;
}
std::filesystem::path PackSettingsStore::profileFile(const std::string& id) const {
  if(!validPackId(id)) throw std::runtime_error("Invalid wallpaper ID");
  return stateDirectory_/L"wallpaper-settings"/std::filesystem::u8path(id+".json");
}
void PackSettingsStore::registerPack(const std::string& id,const std::filesystem::path& directory,
                                    const std::filesystem::path& schemaFile,std::string_view kind) {
  if(!validPackId(id)) throw std::runtime_error("Invalid wallpaper ID");
  for(const auto& existing:entries_) if(existing.first!=id && existing.first.size()==id.size() &&
      std::equal(id.begin(),id.end(),existing.first.begin(),[](unsigned char a,unsigned char b){
        return std::tolower(a)==std::tolower(b);
      })) throw std::runtime_error("Wallpaper IDs must not differ only by letter case");
  auto found=entries_.find(id);
  const bool authorValues=kind=="native";
  std::error_code ec;
  const auto stamp=schemaFile.empty() ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(schemaFile,ec);
  const auto bytes=schemaFile.empty() ? 0 : std::filesystem::file_size(schemaFile,ec);
  if(found!=entries_.end() && found->second.directory==directory && found->second.schemaFile==schemaFile &&
      found->second.authorValues==authorValues && !ec && found->second.schemaTime==stamp &&
      found->second.schemaBytes==bytes) return;
  Entry replacement;
  replacement.directory=directory;replacement.schemaFile=schemaFile;replacement.schemaTime=stamp;replacement.schemaBytes=bytes;
  replacement.authorValues=authorValues;
  if(found!=entries_.end() && found->second.loaded) replacement.revisionFloor=nextRevision(found->second.profile.revision);
  try {
    if(ec) throw std::runtime_error("Wallpaper settings file is unavailable");
    if(!schemaFile.empty()) {
      auto base=std::filesystem::weakly_canonical(directory),file=std::filesystem::weakly_canonical(schemaFile);
      const auto relative=file.lexically_relative(base);
      if(relative.empty() || *relative.begin()==L".." || !std::filesystem::is_regular_file(file))
        throw std::runtime_error("Wallpaper settings must remain inside the wallpaper directory");
      auto source=readText(file);
      replacement.defaults=validateSchema(parseSettings(source),replacement.playback,authorValues);
      replacement.schemaSource=std::move(source);
    }
  } catch(const std::exception& e) {replacement.error=e.what();}
  entries_[id]=std::move(replacement);
}
void PackSettingsStore::retainPacks(const std::unordered_set<std::string>& ids) {
  for(auto it=entries_.begin();it!=entries_.end();) {
    if(ids.count(it->first)) ++it;
    else it=entries_.erase(it);
  }
}
PackSettingsStore::CatalogSchemas PackSettingsStore::catalogSchemas() const {
  CatalogSchemas result;
  result.reserve(entries_.size());
  for(const auto& [id,entry]:entries_)
    result.emplace(id,SchemaStamp{entry.directory,entry.schemaFile,entry.schemaTime,
                                 entry.schemaBytes,entry.authorValues});
  return result;
}
bool PackSettingsStore::unchangedSchema(const SchemaStamp& previous,
                                       const std::filesystem::path& directory,
                                       const std::filesystem::path& file,std::string_view kind) {
  if(previous.directory!=directory || previous.file!=file || previous.authorValues!=(kind=="native"))
    return false;
  if(file.empty()) return true;
  std::error_code error;
  const auto modified=std::filesystem::last_write_time(file,error);
  if(error || modified!=previous.modified) return false;
  const auto size=std::filesystem::file_size(file,error);
  return !error && size==previous.size;
}
void PackSettingsStore::adoptCatalog(PackSettingsStore& prepared,
                                     const std::unordered_set<std::string>& unchanged,
                                     const std::string& keepActive,const std::string& rejectedId) {
  if(!rejectedId.empty()) prepared.entries_.erase(rejectedId);
  prepared.entries_.reserve(prepared.entries_.size()+unchanged.size());
  for(const auto& id:unchanged) if(id!=rejectedId) {
    auto retained=entries_.extract(id);
    if(!retained.empty()) prepared.entries_.insert(std::move(retained));
  }
  if(!keepActive.empty()) {
    auto active=entries_.extract(keepActive);
    if(!active.empty()) {
      prepared.entries_.erase(keepActive);
      prepared.entries_.insert(std::move(active));
    }
  }
  // The scan only reads schemas. Profiles saved or previewed while it ran
  // belong to the live store and keep their revision and loaded values.
  for(auto& [id,next]:prepared.entries_) {
    const auto it=entries_.find(id);
    if(it==entries_.end()) continue;
    auto& previous=it->second;
    if(next.directory==previous.directory && next.schemaFile==previous.schemaFile &&
       next.authorValues==previous.authorValues && next.schemaTime==previous.schemaTime &&
       next.schemaBytes==previous.schemaBytes && next.error==previous.error) {
      next.profile=std::move(previous.profile);
      next.loaded=previous.loaded;
      next.backupBeforeWrite=previous.backupBeforeWrite;
      next.persistedRevision=previous.persistedRevision;
      next.revisionFloor=previous.revisionFloor;
    } else {
      next.revisionFloor=previous.loaded ? nextRevision(previous.profile.revision) : previous.revisionFloor;
    }
  }
  entries_.swap(prepared.entries_);
}
void PackSettingsStore::refresh(const std::string& id) {
  const auto& registered=entry(id);
  const auto directory=registered.directory,file=registered.schemaFile;
  registerPack(id,directory,file,registered.authorValues ? "native" : "");
}
const json& PackSettingsStore::materializeSchema(const std::string& id, Entry& item) {
  // Keep only the currently inspected schema expanded. Values and revision survive eviction.
  if(expandedId_!=id) {
    if(auto old=entries_.find(expandedId_);old!=entries_.end()) old->second.profile.schema=nullptr;
    expandedId_=id;
  }
  if(item.profile.schema.is_null() && !item.schemaSource.empty())
    item.profile.schema=parseSettings(item.schemaSource);
  return item.profile.schema;
}
bool PackSettingsStore::hasSchema(const std::string& id) const {
  const auto& item=entry(id);return item.error.empty() && !item.schemaSource.empty();
}
std::string PackSettingsStore::schemaError(const std::string& id) const {return entry(id).error;}
PackSettingsCandidate PackSettingsStore::current(const std::string& id) {
  refresh(id);
  auto& item=entry(id);
  if(!item.error.empty()) throw std::runtime_error("Wallpaper settings: "+item.error);
  const auto& schema=materializeSchema(id,item);
  if(!item.loaded) {
    PackSettingsCandidate candidate{id,schema,item.defaults,item.revisionFloor,false,item.playback,item.authorValues};
    const auto file=profileFile(id);
    if(std::filesystem::exists(file)) {
      const auto stored=readFile(file,MaxProfileBytes);
      if(!stored.is_object() || stored.value("version",0)!=1 || !stored.contains("values") || !stored["values"].is_object())
        throw std::runtime_error("Invalid wallpaper settings profile");
      if(stored.contains("revision") && (!stored["revision"].is_number_integer() ||
         (!stored["revision"].is_number_unsigned() && stored["revision"].get<std::int64_t>()<=0)))
        throw std::runtime_error("Invalid wallpaper profile revision");
      const auto revision=stored.value("revision",std::uint64_t{1});
      if(!revision) throw std::runtime_error("Invalid wallpaper profile revision");
      bool evolved=stored["values"].size()!=item.defaults.size();
      for(auto value=stored["values"].begin();value!=stored["values"].end();++value) {
        const auto* control=findControl(schema,value.key());
        if(!control) {evolved=true;continue;}
        try {
          validateValue(*control,value.value(),candidate.playback.get(),candidate.authorValues);
          candidate.values[value.key()]=value.value();
        }
        catch(const std::exception&) {evolved=true;}
      }
      evolved=restoreRenamedFireworksControls(candidate,stored["values"]) || evolved;
      evolved=normalizeImpactRange(candidate,json::object()) || evolved;
      candidate.revision=(std::max)(item.revisionFloor,evolved ? nextRevision(revision) : revision);
      candidate.stored=true;
      item.backupBeforeWrite=evolved;
      item.persistedRevision=revision;
    } else {
      normalizeImpactRange(candidate,json::object());
    }
    item.profile=std::move(candidate);item.loaded=true;
  }
  return item.profile;
}
PackSettingsCandidate PackSettingsStore::prepare(const std::string& id,const json& patch,
                                                 std::optional<std::uint64_t> expected,bool reset) {
  PackSettingsCandidate candidate;
  try {candidate=current(id);}
  catch(const std::exception&) {
    if(!reset) throw;
    refresh(id);
    auto& item=entry(id);
    if(!item.error.empty()) throw std::runtime_error("Wallpaper settings: "+item.error);
    if(expected) throw std::runtime_error("Saved wallpaper settings cannot be verified; refresh before restoring defaults");
    candidate={id,materializeSchema(id,item),item.defaults,item.revisionFloor,false,item.playback,item.authorValues};
    candidate.recoveryBackup=backupDamagedProfile(profileFile(id));
  }
  if(expected && *expected!=candidate.revision) throw std::runtime_error("Wallpaper settings changed; refresh before applying");
  if(!patch.is_object()) throw std::runtime_error("Wallpaper settings patch must be an object");
  const auto old=candidate.values;
  if(reset) candidate.values=entry(id).defaults;
  else for(auto value=patch.begin();value!=patch.end();++value) {
    const auto* control=findControl(candidate.schema,value.key());
    if(!control) throw std::runtime_error("Unknown wallpaper setting: "+value.key());
    validateValue(*control,value.value(),candidate.playback.get(),candidate.authorValues);
    candidate.values[value.key()]=value.value();
  }
  normalizeImpactRange(candidate,patch);
  validateValues(candidate.schema,candidate.values,candidate.playback.get(),candidate.authorValues);
  if(candidate.values!=old) candidate.revision=nextRevision(candidate.revision);
  return candidate;
}
void PackSettingsStore::commit(const PackSettingsCandidate& candidate,bool persist) {
  auto& item=entry(candidate.id);
  if(candidate.authorValues!=item.authorValues)
    throw std::runtime_error("Wallpaper kind changed; refresh before applying");
  if(item.loaded && !item.backupBeforeWrite && item.persistedRevision==candidate.revision &&
     item.profile.revision==candidate.revision && item.profile.values==candidate.values)
    return;
  validateValues(materializeSchema(candidate.id,item),candidate.values,item.playback.get(),item.authorValues);
  (void)bundle(candidate);
  if(persist) {
    const auto file=profileFile(candidate.id);
    if(!candidate.recoveryBackup.empty() && !sameProfileContents(file,candidate.recoveryBackup))
      throw std::runtime_error("Wallpaper settings changed after recovery was requested; refresh before restoring defaults");
    if(item.backupBeforeWrite && std::filesystem::exists(file)) {
      auto backup=file;backup+=L".before-schema-"+std::to_wstring(candidate.revision)+L".json";
      std::filesystem::copy_file(file,backup,std::filesystem::copy_options::skip_existing);
    }
    atomicWrite(file,{{"version",1},{"revision",candidate.revision},{"values",candidate.values}});
  }
  item.persistedRevision=persist ? candidate.revision : 0;
  item.profile.id=candidate.id;
  item.profile.values=candidate.values;
  item.profile.revision=candidate.revision;
  item.profile.playback=item.playback;
  item.profile.authorValues=item.authorValues;
  item.profile.stored=persist;item.loaded=true;
  if(persist) item.backupBeforeWrite=false;
}
PackSettingsCandidate PackSettingsStore::packageControls(const std::filesystem::path& schemaFile) {
  PackSettingsCandidate profile;
  profile.schema=readFile(schemaFile);
  profile.values=validateSchema(profile.schema,profile.playback,false);
  return profile;
}
PackAppearance PackSettingsStore::evaluate(const PackSettingsCandidate& candidate) {
  const auto* playback=candidate.playback.get();
  validateValues(candidate.schema,candidate.values,playback,candidate.authorValues);
  PackAppearance result;
  if(candidate.authorValues) return result;
  std::optional<EffectRgb> solidColor;
  const json* localPalettes=nullptr;
  bool supportsPaletteRotation=false;
  for(const auto* control:controls(candidate.schema)) {
    if(playbackControl(playback,*control)) continue;
    const auto& binding=control->at("binding");const auto target=binding.at("target").get<std::string>();
    const auto& value=candidate.values.at(control->at("id").get<std::string>());
    if(target=="keycap.outlineColor") {result.keycapOutlineColor=rgb(value);continue;}
    if(target=="effectColor") {result.color=rgb(value);continue;}
    if(target=="background.color") {result.backgroundColor=rgb(value);continue;}
    if(target=="background.solidColor") {solidColor=rgb(value);continue;}
    // Legacy schemas still validate these bindings. The DLL consumes the original control values.
    if(target.rfind("blackHole.",0)==0) continue;
    if(target=="effectPalette") {
      result.palette=value.at("preset").get<std::string>();result.paletteColors=colors(value.at("colors"));
      if(control->contains("palettes")) {
        localPalettes=&control->at("palettes");
        const auto* selected=result.palette=="custom" ? nullptr : findLocalPalette(*control,result.palette);
        result.sourcePalette=selected && selected->value("source",false);
        result.paletteOverride=!result.sourcePalette;
        result.resolvedPaletteColors=selected ? colors(selected->at("colors")) : result.paletteColors;
      } else if(result.palette!="original") {
        result.paletteOverride=true;
        result.resolvedPaletteColors=result.palette=="custom" ? result.paletteColors : findEffectPalette(result.palette)->colors;
      }
      continue;
    }
    const float mapped=static_cast<float>(mapNumber(*control,value.is_boolean() ? (value.get<bool>() ? 1.0 : 0.0) : number(value)));
    if(target.rfind("keycap.",0)==0) result.keycap[target.substr(7)]=mapped;
    else if(target=="runtime.idleFps") result.idleFps=static_cast<unsigned>(mapped);
    else if(target=="runtime.idleDelay") result.idleDelay=mapped;
    else if(target.rfind("field.",0)==0) result.field[target.substr(6)]=mapped;
    else if(target=="waterSurface.damping") result.waterSurface.damping=mapped;
    else if(target=="waterSurface.ambient") result.waterSurface.ambient=mapped;
    else if(target=="waterSurface.impactMin") result.waterSurface.impactMin=mapped;
    else if(target=="waterSurface.impactMax") result.waterSurface.impactMax=mapped;
    else if(target=="waterSurface.backdrop") result.waterSurface.backdrop=static_cast<WaterBackdrop>(mapped);
    else if(target=="waterSurface.texture") result.waterSurface.texture=mapped;
    else if(target=="waterSurface.caustics") result.waterSurface.caustics=mapped;
    else if(target=="glassShatter.impactMin") result.glassShatter.impactMin=mapped;
    else if(target=="glassShatter.impactMax") result.glassShatter.impactMax=mapped;
    else if(target=="glassShatter.backdrop") result.glassShatter.backdrop=static_cast<GlassBackdrop>(mapped);
    else if(target=="glassShatter.spread") result.glassShatter.spread=mapped;
    else if(target=="glassShatter.duration") result.glassShatter.duration=mapped;
    else if(target=="glassShatter.branches") result.glassShatter.branches=static_cast<unsigned>(mapped);
    else if(target=="glassShatter.seamWidth") result.glassShatter.seamWidth=mapped;
    else if(target=="glassShatter.glints") result.glassShatter.glints=mapped;
    else if(target=="glassShatter.refraction") result.glassShatter.refraction=mapped;
    else if(target=="glassShatter.chips") result.glassShatter.chips=static_cast<unsigned>(mapped);
    else if(target=="fireworks.shape") result.fireworks.shape=static_cast<unsigned>(mapped);
    else if(target=="fireworks.keyInterval") result.fireworks.keyInterval=static_cast<unsigned>(mapped);
    else if(target=="fireworks.quality") result.fireworks.quality=static_cast<unsigned>(mapped);
    else if(target=="fireworks.shuffle") result.fireworks.shuffle=mapped!=0;
    else if(target=="fireworks.randomPalette") {
      supportsPaletteRotation=true;
      result.fireworks.randomPalette=mapped!=0;
    }
    else if(target=="fireworks.spread") result.scale=mapped;
    else if(target=="fireworks.density") result.fireworks.density=mapped;
    else if(target=="fireworks.trail") result.fireworks.trail=mapped;
    else if(target=="fireworks.gravity") result.fireworks.gravity=mapped;
    else if(target=="fireworks.lifetime") result.fireworks.lifetime=mapped;
    else if(target=="fireworks.shimmer") result.fireworks.shimmer=mapped;
    else if(target=="fireworks.embers") result.fireworks.embers=mapped;
    else if(target=="fireworks.bloom") result.fireworks.bloom=mapped;
    else if(target=="fireworks.smoke") result.fireworks.smoke=mapped;
    else if(target=="fireworks.reflection") result.fireworks.reflection=mapped;
    else if(target=="fireworks.horizon") result.fireworks.horizon=mapped;
    else if(target.rfind("fireworks.",0)==0) {
      const auto choice=std::find(std::begin(fireworkChoices),std::end(fireworkChoices),target);
      if(choice!=std::end(fireworkChoices)) {
        const unsigned bit=1u<<static_cast<unsigned>(choice-std::begin(fireworkChoices));
        if(mapped!=0) result.fireworks.selected|=bit;
        else result.fireworks.selected&=~bit;
      }
    }
    else if(target=="effectBrightness") result.brightness=mapped;
    else if(target=="effectScale") result.scale=mapped;
    else if(target=="effectIntensity") result.intensity=mapped;
    else if(target=="effectSaturation") result.saturation=mapped;
    else if(target=="effectHue") result.hue=mapped;
    else if(target=="background.fit") result.backgroundFit=static_cast<unsigned>(mapped);
    else if(target=="background.zoom") result.backgroundZoom=mapped;
    else if(target=="background.panX") result.backgroundPanX=mapped;
    else if(target=="background.panY") result.backgroundPanY=mapped;
    else if(target=="background.style") result.backgroundStyle=static_cast<unsigned>(mapped);
    else if(target=="background.brightness") result.backgroundBrightness=mapped;
    else if(target=="background.saturation") result.backgroundSaturation=mapped;
    else if(target=="background.hue") result.backgroundHue=mapped;
    else if(target=="sprite.duration") result.spriteDuration=mapped;
    else if(target=="effect.speed") result.speed=mapped;
    else if(target=="effect.dynamicInput") result.dynamicInputs[binding.at("index").get<unsigned>()]=mapped;
    else if(target=="material.uniform") result.uniforms.push_back({binding.at("name").get<std::string>(),binding.value("component",0u),mapped});
  }
  if(result.backgroundStyle==1 && solidColor) result.backgroundColor=*solidColor;
  if(playback) {
    result.playback=evaluateVariantPlayback(*playback,candidate.values);
    static_cast<VariantPlaybackRules&>(result.fireworks)=*result.playback;
  }
  if(supportsPaletteRotation && localPalettes) {
    for(const auto& preset:*localPalettes) {
      if(result.fireworks.paletteChoices.size()==FireworksAppearance::paletteLimit) break;
      result.fireworks.paletteChoices.push_back(preset.value("source",false)
          ? std::vector<EffectRgb>{} : colors(preset.at("colors")));
    }
  }
  return result;
}
json PackSettingsStore::legacyPatch(const std::string& id,const json& legacy) {
  const auto profile=current(id);
  if(profile.authorValues) {
    if(!legacy.empty())
      throw std::runtime_error("Native wallpaper settings are addressed by their author IDs");
    return json::object();
  }
  const auto before=evaluate(profile);
  json patch=json::object();
  const std::vector<std::pair<std::string,double>> factors{
    {"effectBrightness",before.brightness},{"effectScale",before.scale},{"effectIntensity",before.intensity},
    {"effectSaturation",before.saturation},{"effect.speed",before.speed},{"sprite.duration",before.spriteDuration}};
  for(const auto& factor:factors) if(legacy.contains(factor.first)) {
    const double requested=number(legacy.at(factor.first));
    const json* bound=nullptr;
    for(const auto* control:controls(profile.schema)) {
      if(playbackControl(profile.playback.get(),*control)) continue;
      const auto& target=control->at("binding").at("target");
      if(target==factor.first || (factor.first=="effectScale" && target=="fireworks.spread"))
        bound=control;
    }
    const auto range=targetRange(bound ? bound->at("binding").at("target").get<std::string>() : factor.first);
    if(requested<range.first || requested>range.second) throw std::runtime_error("Legacy wallpaper setting is out of range");
    if(!bound) {
      if(std::abs(requested-factor.second)>1e-6) throw std::runtime_error("This wallpaper does not expose "+factor.first);
      continue;
    }
    const auto type=bound->at("type").get<std::string>();
    const double inverse=inverseNumber(*bound,requested);
    json value=inverse;
    if(type=="toggle") value=inverse>=.5;
    else if(type=="select") {
      double nearest=std::numeric_limits<double>::infinity();
      for(const auto& option:bound->at("options")) {
        const double distance=std::abs(mapNumber(*bound,number(option.at("value")))-requested);
        if(distance<nearest) {nearest=distance;value=option.at("value");}
      }
    }
    patch[bound->at("id").get<std::string>()]=value;
  }
  for(const auto* control:controls(profile.schema)) {
    if(playbackControl(profile.playback.get(),*control)) continue;
    const auto target=control->at("binding").at("target").get<std::string>();
    const auto cid=control->at("id").get<std::string>();
    if(target=="effectColor" && legacy.contains("effectColor")) patch[cid]=legacy.at("effectColor");
    if(target=="effectPalette" && (legacy.contains("effectPalette") || legacy.contains("effectPaletteColors"))) {
      auto value=profile.values.at(cid);
      if(legacy.contains("effectPalette")) value["preset"]=legacy["effectPalette"];
      if(legacy.contains("effectPaletteColors")) value["colors"]=legacy["effectPaletteColors"];
      patch[cid]=value;
    }
  }
  if(legacy.contains("effectColor") && rgb(legacy.at("effectColor"))!=before.color) {
    bool bound=false;
    for(const auto* control:controls(profile.schema))
      if(!playbackControl(profile.playback.get(),*control) &&
          control->at("binding").at("target")=="effectColor") bound=true;
    if(!bound) throw std::runtime_error("This wallpaper does not expose a global color filter");
  }
  const auto hue=legacy.contains("effectHue") ? number(legacy.at("effectHue")) : 0;
  if(hue < -180 || hue>180) throw std::runtime_error("Legacy hue is out of range");
  // Canonical palette RGB values carry color; no hidden hue survives a schema profile.
  if(hue!=0) {
    auto candidate=prepare(id,patch);const auto appearance=evaluate(candidate);
    const auto count=appearance.resolvedPaletteColors.empty() ? 9 : appearance.resolvedPaletteColors.size();
    if(count==1) throw std::runtime_error("Change the single palette color instead of a separate hue");
  }
  return patch;
}
PackSettingsCandidate PackSettingsStore::migrate(const std::string& id,const json& legacy) {
  auto candidate=current(id);
  if(candidate.authorValues || candidate.stored || candidate.schema.is_null()) return candidate;
  json patch=json::object();
  // Only migrate targets exposed by this pack; missing controls retain defaults.
  for(const auto* control:controls(candidate.schema)) {
    if(playbackControl(candidate.playback.get(),*control)) continue;
    const auto target=control->at("binding").at("target").get<std::string>();
    try {
      const auto cid=control->at("id").get<std::string>();
      json value=control->at("default");
      if(target=="effectPalette") {
        if(legacy.contains("effectPalette")) value["preset"]=legacy["effectPalette"];
        if(legacy.contains("effectPaletteColors")) value["colors"]=legacy["effectPaletteColors"];
      } else if(target=="effectColor") {
        if(legacy.contains(target)) value=legacy.at(target);
      } else {
        const auto legacyTarget=target=="fireworks.spread" ? "effectScale" : target;
        if(legacy.contains(legacyTarget)) {
          value=inverseNumber(*control,number(legacy.at(legacyTarget)));
          if(control->at("type")=="toggle") value=number(value)>=.5;
        }
      }
      validateValue(*control,value);patch[cid]=value;
    } catch(const std::exception&) { /* Retain this control's valid declared default. */ }
  }
  return prepare(id,patch);
}
json PackSettingsStore::bundle(const PackSettingsCandidate& candidate) const {
  json result={{"packId",candidate.id},{"schema",candidate.schema},{"values",candidate.values},{"revision",candidate.revision}};
  if(result.dump().size()>60*1024) throw std::runtime_error("Wallpaper settings reply exceeds its protocol limit");
  return result;
}
}
