#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// The author's 24-event envelope. This preserves the legacy atlas timing and
// storm gain budget without importing a player implementation or library.
namespace soundings {
struct Sample { float x, y, frame, gain; };
class EchoState {
public:
    static constexpr unsigned capacity=24;
    static constexpr float lifetime=.85f;
    void clear() noexcept { count_=0; }
    void push(double time,float x,float y) noexcept {
        if(!std::isfinite(time)||!std::isfinite(x)||!std::isfinite(y)||x<0||x>1||y<0||y>1) return;
        expire(time);
        if(count_==capacity) {std::move(events_.begin()+1,events_.end(),events_.begin());--count_;}
        events_[count_++]={time,x,y};
    }
    unsigned sample(double time,std::array<Sample,capacity>& samples) noexcept {
        expire(time);
        const float gain=count_>8?std::sqrt(8.f/static_cast<float>(count_)):1.f;
        for(unsigned i=0;i<count_;++i) {
            const auto& e=events_[i];
            const double age=std::clamp((time-e.born)/lifetime,0.,1.);
            samples[i]={e.x,e.y,static_cast<float>(age*23),gain};
        }
        return count_;
    }
private:
    struct Hit { double born;float x,y; };
    void expire(double time) noexcept {
        unsigned live=0;
        for(unsigned i=0;i<count_;++i) if(time-events_[i].born<lifetime) events_[live++]=events_[i];
        count_=live;
    }
    std::array<Hit,capacity> events_{};
    unsigned count_=0;
};
}
