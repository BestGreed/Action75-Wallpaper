#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace author_particles {
using json=nlohmann::json;
inline json readJson(const std::filesystem::path& file) {
    std::ifstream stream(file,std::ios::binary);
    if(!stream) throw std::runtime_error("Cannot read particle package JSON");
    return json::parse(stream);
}

// Package-side playback state. Roles, slot order and bindings are read from the
// author's own schema; the runtime never branches on a wallpaper ID.
struct Program {
    json schema,values;
    std::vector<std::filesystem::path> resources;
    std::string selection,shuffle,interval;
    std::vector<unsigned> order;
    std::vector<std::string> toggles;
    unsigned cursor=0,keys=0;
    std::uint64_t random=0x9e3779b97f4a7c15ull;
    float baseScale=1,scale=1,speed=1,brightness=1;
    std::array<float,4> dynamic{};
    std::array<bool,4> dynamicBound{};
    float backgroundBrightness=1,backgroundHue=0,backgroundSaturation=1;
    float backgroundOpacity=1,foregroundOpacity=1;
    std::array<float,3> backgroundColor{1,1,1};
    bool solid=false;

    explicit Program(const std::filesystem::path& directory) {
        const auto definition=readJson(directory/L"particles.json");
        baseScale=definition.value("scale",1.f);
        for(const auto& name:definition.at("effects")) resources.push_back(directory/std::filesystem::u8path(name.get<std::string>()));
        if(resources.empty() || resources.size()>10) throw std::runtime_error("Invalid authored particle resources");
        schema=readJson(directory/L"settings.json");
        values=json::object();
        for(const auto& group:schema.at("groups")) {
            for(const auto& control:group.at("controls")) values[control.at("id").get<std::string>()]=control.at("default");
            if(group.value("presentation",std::string())=="variant-playback") {
                const auto& roles=group.at("playback");
                selection=roles.at("selection").get<std::string>();
                shuffle=roles.at("randomize").get<std::string>();
                interval=roles.at("interval").get<std::string>();
                order=roles.at("order").get<std::vector<unsigned>>();
                toggles.resize(resources.size()+1);
                for(const auto& control:group.at("controls")) if(control.at("id")==selection)
                    for(const auto& option:control.at("options")) {
                        const auto slot=option.at("value").get<unsigned>();
                        if(slot && slot<toggles.size()) toggles[slot]=option.at("toggleId").get<std::string>();
                    }
            }
        }
        configure(nullptr);
    }
    void configure(const char* text) {
        const unsigned oldInterval=interval.empty()?1:values.at(interval).get<unsigned>();
        if(text && *text) {
            const auto patch=json::parse(text);
            if(!patch.is_object()) throw std::runtime_error("Particle parameters must be an object");
            for(const auto& item:patch.items()) if(values.contains(item.key())) values[item.key()]=item.value();
        }
        scale=speed=brightness=1;
        backgroundOpacity=foregroundOpacity=1;
        dynamicBound.fill(false);
        for(const auto& group:schema.at("groups")) for(const auto& control:group.at("controls")) {
            if(!control.contains("binding")) continue;
            const auto& binding=control.at("binding");
            const auto target=binding.value("target",std::string());
            const auto& value=values.at(control.at("id").get<std::string>());
            if(target=="background.color") {
                for(unsigned c=0;c<3;++c) backgroundColor[c]=std::clamp(value.at(c).get<float>()/255.f,0.f,1.f);
                continue;
            }
            const float v=value.is_boolean()?(value.get<bool>()?1.f:0.f):value.get<float>();
            if(target=="effectScale") scale=v;
            else if(target=="effect.speed") speed=v;
            else if(target=="effectBrightness") brightness=v;
            else if(target=="effect.dynamicInput") {
                const auto slot=binding.at("index").get<unsigned>();
                if(slot>=4) throw std::runtime_error("Dynamic input outside 0..3");
                dynamic[slot]=v;dynamicBound[slot]=true;
            } else if(target=="background.style") solid=v!=0;
            else if(target=="background.brightness") backgroundBrightness=v;
            else if(target=="background.hue") backgroundHue=v/360.f;
            else if(target=="background.saturation") backgroundSaturation=v;
            else if(target=="background.opacity") backgroundOpacity=std::clamp(v/100.f,0.f,1.f);
            else if(target=="effect.opacity") foregroundOpacity=std::clamp(v/100.f,0.f,1.f);
        }
        if(!interval.empty() && values.at(interval).get<unsigned>()!=oldInterval) keys=0;
    }
    int choose(bool keyboard) {
        if(selection.empty()) return 0;
        const auto spacing=std::clamp(values.at(interval).get<unsigned>(),1u,99u);
        if(keyboard && ++keys<spacing) return -1;
        if(keyboard) keys=0;
        const auto fixed=values.at(selection).get<unsigned>();
        if(fixed) return fixed<=resources.size()?static_cast<int>(fixed-1):-1;
        const auto enabled=[&](unsigned slot) {return slot && slot<toggles.size() && !toggles[slot].empty() && values.at(toggles[slot]).get<bool>();};
        if(values.at(shuffle).get<bool>()) {
            std::vector<unsigned> selected;
            for(auto slot:order) if(enabled(slot)) selected.push_back(slot);
            if(selected.empty()) return -1;
            random^=random>>12;random^=random<<25;random^=random>>27;
            return static_cast<int>(selected[(random*2685821657736338717ull)%selected.size()]-1);
        }
        for(unsigned n=0;n<order.size();++n) {
            const auto position=(cursor+n)%order.size();
            const auto slot=order[position];
            if(enabled(slot)) {cursor=(position+1)%order.size();return static_cast<int>(slot-1);}
        }
        return -1;
    }
    std::array<float,4> background() const {
        if(!solid) return {0,0,0,backgroundOpacity};
        auto rgb=backgroundColor;
        const float peak=std::max({rgb[0],rgb[1],rgb[2]}),chroma=peak-std::min({rgb[0],rgb[1],rgb[2]});
        if(chroma>0 && (backgroundHue!=0 || backgroundSaturation!=1)) {
            float hue=peak==rgb[0]?(rgb[1]-rgb[2])/chroma:peak==rgb[1]?(rgb[2]-rgb[0])/chroma+2:(rgb[0]-rgb[1])/chroma+4;
            hue=hue/6+backgroundHue;
            const float saturation=std::clamp(chroma/peak*backgroundSaturation,0.f,1.f);
            constexpr float offsets[]={0,2.f/3,1.f/3};
            for(unsigned c=0;c<3;++c) {
                const float h=hue+offsets[c];
                const float pure=std::clamp(std::abs((h-std::floor(h))*6-3)-1,0.f,1.f);
                rgb[c]=peak*(1+saturation*(pure-1));
            }
        }
        const float gain=backgroundBrightness<=1?backgroundBrightness:backgroundBrightness/(1+(backgroundBrightness-1)*peak);
        return {rgb[0]*gain*backgroundOpacity,rgb[1]*gain*backgroundOpacity,rgb[2]*gain*backgroundOpacity,backgroundOpacity};
    }
};
}
