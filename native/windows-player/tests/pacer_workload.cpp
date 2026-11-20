#include "FramePacer.h"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>

using action75::FramePacer;
using namespace std::chrono;
using namespace std::chrono_literals;
void check(bool v,const char* why) { if(!v) throw std::runtime_error(why); }

struct Timing {
    unsigned count=0;
    double gapMinMs=std::numeric_limits<double>::infinity(),gapMaxMs=0;
    double lastMs=0;
    void add(FramePacer::Time now) {
        const double ms=duration<double,std::milli>(now.time_since_epoch()).count();
        if(count){const double gap=ms-lastMs;gapMinMs=std::min(gapMinMs,gap);gapMaxMs=std::max(gapMaxMs,gap);}
        lastMs=ms;++count;
    }
};

Timing cadence(unsigned fps, bool jitter, bool stall) {
    FramePacer pacer;
    FramePacer::Time now{};
    const auto p=FramePacer::period(fps);
    Timing t;
    check(pacer.plan(now,true,false,true).due,"first active frame must be immediate");
    for (unsigned i=0;i<=fps*10;++i) {
        check(pacer.plan(now,true,false,true).due,"timer not due");
        t.add(now);
        pacer.rendered(now,fps,true);
        const auto next=pacer.plan(now,true,false,true);
        check(!next.due&&next.timed&&next.remaining>FramePacer::Duration::zero(),"missing positive deadline");
        if (i==fps*5 && stall) now+=next.remaining+62ms;
        else now+=next.remaining+(jitter?250us:0us);
    }
    constexpr double epsilon=0.001;
    const double nominal=duration<double,std::milli>(p).count();
    check(t.count==fps*10+1,"cadence lost events");
    check(t.gapMinMs+epsilon>=nominal*.5,"post-stall catch-up burst");
    if(!stall) {
        check(t.lastMs<10001,"phase drift accumulated under small jitter");
        check(t.gapMaxMs<=nominal+.251,"timer overshoot");
    }
    return t;
}

Timing messagePressure(unsigned fps, unsigned constraint, bool saturated) {
    FramePacer pacer;
    FramePacer::Time now{0ns};
    const auto arrivesEvery=saturated?40us:1ms; // 25,000 or 1,000 messages/s, synthetic only
    constexpr auto dispatchCost=65us;
    constexpr auto sourceEnds=2000ms;
    constexpr auto ends=3500ms;
    std::int64_t consumed=0;
    auto arrived=[&](FramePacer::Time tt) {
        return std::min(duration_cast<nanoseconds>(tt.time_since_epoch()).count(),
                         duration_cast<nanoseconds>(sourceEnds).count() - 1)
            / duration_cast<nanoseconds>(arrivesEvery).count();
    };
    Timing t;
    while(now.time_since_epoch()<ends) {
        unsigned batch=0;
        const auto limit=now+1ms;
        while(consumed<arrived(now) && (!constraint || batch<64u)) {
            now+=dispatchCost;
            ++consumed;++batch;
            if (constraint==2 && now>=limit) break;
        }
        const auto p=pacer.plan(now,true,false,true);
        if(p.due) { t.add(now);pacer.rendered(now,fps,true);continue; }
        if(consumed<arrived(now)) continue; // do not wait with queued messages
        const auto due=now+p.remaining;
        const auto nextArrival=FramePacer::Time{(consumed+1)*arrivesEvery};
        now=(nextArrival.time_since_epoch()<sourceEnds)?std::min(due,nextArrival):due;
    }
    check(t.count>1,"render never resumed");
    if (saturated&&constraint) check(t.gapMaxMs<45,"bounded pumping starves the timed render");
    if (saturated&&!constraint) check(t.gapMaxMs>3000,"storm model does not exercise starvation");
    if (saturated&&constraint==2) {
        const auto pMs=duration<double,std::milli>(FramePacer::period(fps)).count();
        check(t.gapMaxMs<pMs+1.1,"1ms budget did not cap synthetic timer lag");
    }
    return t;
}

void edges() {
    FramePacer pacer;
    FramePacer::Time now{};
    check(!pacer.plan(now,true,false,false).timed,"idle periodic deadline");
    check(!pacer.plan(now,false,true,true).due,"hidden dirty scene must not render");
    check(pacer.plan(now,true,true,false).due,"dirty static scene must draw once");
    pacer.rendered(now,60,false);
    check(!pacer.plan(now,true,false,false).due,"static second draw");
    pacer.rendered(now,60,true);
    check(!pacer.plan(now+1ms,true,false,true).due,"active due too early");
    check(pacer.plan(now+1ms,true,true,true).due,"active dirty not immediate");
    pacer.rendered(now+1ms,60,false);
    check(!pacer.plan(now+2ms,true,false,false).timed,"finished animation retains timer");
    check(pacer.plan(now+20s,true,false,true).due,"first hit after idle not immediate");
    check(FramePacer::ceilingMilliseconds(1ns)==1,"sub-ms timeout rounded downward");
    check(FramePacer::ceilingMilliseconds(1000001ns)==2,"fractional ms timeout rounded downward");
    check(FramePacer::relativeHundredNanoseconds(101ns)==-2,"invalid relative timeout rounding");
    check(FramePacer::relativeHundredNanoseconds(0ns)==-1,"zero timeout raw tick");
}

void inputRateCap() {
 for(unsigned fps:{30,60,120}) {
  FramePacer pacer;FramePacer::Time now{};const auto ends=now+10s;
  auto nextInput=now;unsigned count=0;Timing timing;
  while(now<ends){
   if(now>=nextInput)nextInput+=25ms; // continuous motion: keys do not force dirty
   pacer.setRate(fps);const auto plan=pacer.plan(now,true,false,true);
   if(plan.due){timing.add(now);++count;pacer.rendered(now,fps,true);continue;}
   now=std::min(now+plan.remaining,nextInput);
  }
  check(count<=fps*10+1,"40Hz input exceeded the selected frame cap");
  check(timing.gapMinMs+0.001>=1000./fps,"input inserted an early frame");
 }
 FramePacer pacer;FramePacer::Time now{};
 pacer.rendered(now,30,true);pacer.setRate(120);
 check(!pacer.plan(now+3ms,true,false,true).due,"idle wake ignored active cap");
 check(pacer.plan(now+FramePacer::period(120),true,false,true).due,"idle wake waited for old 30fps deadline");
 pacer.rendered(now,120,true);pacer.setRate(30);
 check(!pacer.plan(now+20ms,true,false,true).due,"idle did not reduce cadence");
}

int main() {
    try {
        edges();
        inputRateCap();
        std::cout<<"scenario,fps,frames,shortest_gap_ms,longest_gap_ms,last_ms\n";
        for(unsigned fps:{30,60,120}) {
            auto show=[&](const char* mode,const Timing& t) {
                std::cout<<mode<<','<<fps<<','<<t.count<<','<<std::fixed<<std::setprecision(3)
                         <<t.gapMinMs<<','<<t.gapMaxMs<<','<<t.lastMs<<'\n';
            };
            show("cadence_exact",cadence(fps,false,false));
            show("cadence_250us_jitter",cadence(fps,true,false));
            show("cadence_single_62ms_stall",cadence(fps,true,true));
            show("msgs_1khz_64_or_1ms",messagePressure(fps,2,false));
            show("msgs_25khz_64_only",messagePressure(fps,1,true));
            show("msgs_25khz_64_or_1ms",messagePressure(fps,2,true));
            show("msgs_25khz_unbounded",messagePressure(fps,0,true));
        }
    } catch(const std::exception& e) {
        std::cerr<<e.what()<<'\n';return 1;
    }
}
