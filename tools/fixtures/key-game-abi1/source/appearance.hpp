#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <nlohmann/json.hpp>

struct SurfaceLook {
    float brightness=1, hue=0, saturation=1;
    std::array<float,3> tint{1,1,1};

    void read(const nlohmann::json& values,const std::string& prefix){
        auto number=[&](const char* name,float& target,float lo,float hi){
            auto i=values.find(prefix+name);
            if(i!=values.end() && i->is_number())target=std::clamp(i->get<float>(),lo,hi);
        };
        number("Brightness",brightness,0,2);
        number("Hue",hue,-180,180);
        number("Saturation",saturation,0,3);
        auto i=values.find(prefix+"Color");
        if(i==values.end() || !i->is_array() || i->size()!=3)return;
        std::array<float,3> rgb;
        for(size_t k=0;k<3;++k){
            if(!(*i)[k].is_number())return;
            rgb[k]=std::clamp((*i)[k].get<float>(),0.f,255.f)/255;
        }
        tint=rgb;
    }
    using RGB=std::array<float,3>;
    RGB transform(RGB rgb)const{
        for(size_t k=0;k<3;++k)rgb[k]*=tint[k]*brightness;
        if(hue==0 && saturation==1)return rgb;
        // Rotate chroma about the gray axis, then apply saturation around luma.
        float angle=hue*.01745329252f,c=std::cos(angle),s=std::sin(angle)*.57735026919f;
        float mean=(rgb[0]+rgb[1]+rgb[2])/3;
        RGB rotated{};
        for(int k=0;k<3;++k)
            rotated[k]=mean+(rgb[k]-mean)*c+(rgb[(k+2)%3]-rgb[(k+1)%3])*s;
        float luma=.2126f*rotated[0]+.7152f*rotated[1]+.0722f*rotated[2];
        for(auto& value:rotated)value=luma+(value-luma)*saturation;
        return rotated;
    }
    bool neutral()const{return brightness==1 && hue==0 && saturation==1 && tint==RGB{1,1,1};}
};
