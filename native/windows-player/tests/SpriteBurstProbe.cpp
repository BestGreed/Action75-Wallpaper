#include "NativeRenderer.h"
#include "SpriteBurst.h"
#include <algorithm>
#include <cstdio>
#include <chrono>
#include <cwchar>
#include <thread>
#include <cstdint>
#include <vector>
#include <objbase.h>

using Microsoft::WRL::ComPtr;

// Windows software D3D path: same compiled shaders as the player but no
// desktop, keyboard, monitor, animation timer or permanent file writes.
static bool capture(action75::NativeRenderer &r, std::vector<std::uint8_t> &pixels) {
  auto *ctx=r.context(); ComPtr<ID3D11RenderTargetView> view;
  ctx->OMGetRenderTargets(1,&view,nullptr);
  if (!view) return false;
  ComPtr<ID3D11Resource> resource; view->GetResource(&resource);
  ComPtr<ID3D11Texture2D> target;
  if (FAILED(resource.As(&target))) return false;
  D3D11_TEXTURE2D_DESC desc{}; target->GetDesc(&desc);
  desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=0;
  desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> staging;
  if (FAILED(r.device()->CreateTexture2D(&desc,nullptr,&staging))) return false;
  ctx->CopyResource(staging.Get(),target.Get());
  D3D11_MAPPED_SUBRESOURCE mp{};
  if (FAILED(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&mp))) return false;
  const auto w=desc.Width*4;
  for (UINT y=0;y<desc.Height;++y) {
    const auto* row=static_cast<const std::uint8_t*>(mp.pData)+y*mp.RowPitch;
    pixels.insert(pixels.end(),row,row+w);
  }
  ctx->Unmap(staging.Get(),0);
  return true;
}
static int probe(wchar_t** argv, bool multiply) {
  action75::NativeRenderer r;
  unsigned w=480,h=180;
  std::string error;
  auto report=[&](int code){ std::fprintf(stderr,"sprite probe %d: %s\n",code,error.c_str());return code; };
  if (!r.initialize(nullptr,w,h,true,error) || !r.loadBackground(argv[1],error)) return report(1);
  action75::SpriteBurst sprite;
  if (!sprite.load(r.device(),argv[2],{6,4,multiply?1.33f:.60f,multiply?420.f:348.f,multiply},error)) return report(2);
  std::vector<std::uint8_t> idle,hit,fade;
  r.beginFrame();
  if (!capture(r,idle)) return report(3);
  if (!sprite.push(3.0,.05f,.25f)) return report(4);
  r.beginFrame();
  if (!sprite.draw(r.context(),w,h,multiply?3.29:3.08,error) || !capture(r,hit)) return report(5);
  if (hit.size()!=idle.size()) return report(6);
  unsigned changed=0;
  for (unsigned y=0;y<h;++y)
    for (unsigned x=0;x<w;++x) {
      const auto i=(y*w+x)*4;
      const auto delta=[&](unsigned c){return multiply?int(idle[i+c])-hit[i+c]:int(hit[i+c])-idle[i+c];};
      const bool differs=(delta(0)>2 || delta(1)>2 || delta(2)>2);
      if (hit[i+3]!=255 || delta(0)<0 || delta(1)<0 || delta(2)<0 ||
          (differs && (x>78 || y>98))) return report(7);
      changed+=differs;
    }
  if (changed<30) return report(8);
  sprite.setBrightness(2);
  r.beginFrame();
  std::vector<std::uint8_t> stronger;
  if (!sprite.draw(r.context(),w,h,multiply?3.29:3.08,error) || !capture(r,stronger)) return report(11);
  if (stronger.size()!=hit.size()) return report(12);
  unsigned enhanced=0;
  for (std::size_t i=0;i<hit.size();++i) {
    if (i%4==3) { if (stronger[i]!=255) return report(13); continue; }
    if ((!multiply && stronger[i]<hit[i]) || (multiply && stronger[i]>hit[i])) return report(14);
    enhanced+=std::abs(int(stronger[i])-int(hit[i]))>2;
  }
  if (!enhanced) return report(15);
  sprite.setBrightness(1);
  sprite.setColor({255,0,0});
  r.beginFrame();
  std::vector<std::uint8_t> tinted;
  if (!sprite.draw(r.context(),w,h,multiply?3.29:3.08,error) || !capture(r,tinted)) return report(16);
  for (std::size_t i=0;i<tinted.size();i+=4) {
    if (std::abs(int(tinted[i])-int(hit[i]))>1 ||
        tinted[i+1]!=idle[i+1] || tinted[i+2]!=idle[i+2] || tinted[i+3]!=255) return report(17);
  }
  sprite.setColor({255,255,255});
  sprite.setScale(2);
  r.beginFrame();
  std::vector<std::uint8_t> enlarged;
  if (!sprite.draw(r.context(),w,h,multiply?3.29:3.08,error) || !capture(r,enlarged)) return report(18);
  unsigned enlargedPixels=0;
  for (std::size_t i=0;i<enlarged.size();i+=4) {
    const bool differs=std::abs(int(enlarged[i])-int(idle[i]))>2 ||
       std::abs(int(enlarged[i+1])-int(idle[i+1]))>2 || std::abs(int(enlarged[i+2])-int(idle[i+2]))>2;
    enlargedPixels+=differs;
    if (enlarged[i+3]!=255) return report(19);
  }
  if (enlargedPixels<changed*3/2) return report(27);
  sprite.setScale(1);
  for(const bool zeroBrightness : {true,false}) {
    sprite.setBrightness(zeroBrightness ? 0.0f : 1.0f);
    sprite.setIntensity(zeroBrightness ? 1.0f : 0.0f);
    r.beginFrame();
    std::vector<std::uint8_t> zero;
    if (!sprite.draw(r.context(),w,h,multiply?3.29:3.08,error) || !capture(r,zero) || zero!=idle)
      return report(62);
  }
  sprite.setBrightness(1);
  sprite.setIntensity(1);
  if (!multiply) {
    sprite.clear();
    if (!sprite.push(3,.25f,.5f) || !sprite.push(3,.75f,.5f)) return report(50);
    const auto drawGrade=[&](std::vector<std::uint8_t>& pixels) {
      r.beginFrame(); return sprite.draw(r.context(),w,h,3.08,error) && capture(r,pixels);
    };
    std::vector<std::uint8_t> neutral, gray, hue, palette;
    if (!drawGrade(neutral)) return report(51);
    sprite.setColorGrade(0,0,{},false);
    if (!drawGrade(gray)) return report(52);
    std::uint64_t beforeChroma=0,grayChroma=0;
    for (std::size_t i=0;i<gray.size();i+=4) {
      if (gray[i+3]!=255) return report(53);
      const int nr=int(neutral[i])-idle[i],ng=int(neutral[i+1])-idle[i+1],nb=int(neutral[i+2])-idle[i+2];
      const int gr=int(gray[i])-idle[i],gg=int(gray[i+1])-idle[i+1],gb=int(gray[i+2])-idle[i+2];
      beforeChroma+=std::abs(nr-ng)+std::abs(ng-nb);
      grayChroma+=std::abs(gr-gg)+std::abs(gg-gb);
    }
    if (!beforeChroma || grayChroma*4>beforeChroma) return report(54);
    sprite.setColorGrade(1,90,{},false);
    if (!drawGrade(hue)) return report(55);
    unsigned hueChanges=0;
    for(std::size_t i=0;i<hue.size();i+=4) {
      hueChanges+=std::abs(int(hue[i])-neutral[i])>2 || std::abs(int(hue[i+1])-neutral[i+1])>2 ||
                  std::abs(int(hue[i+2])-neutral[i+2])>2;
      if (hue[i+3]!=255) return report(56);
    }
    if (hueChanges<30) return report(57);
    sprite.setColorGrade(1,0,{{255,0,0},{0,0,255}},true);
    if (!drawGrade(palette)) return report(58);
    std::uint64_t leftRed=0,leftBlue=0,rightRed=0,rightBlue=0;
    for(unsigned y=0;y<h;++y) for(unsigned x=0;x<w;++x) {
      const auto i=(y*w+x)*4;
      const auto red=static_cast<unsigned>(palette[i])-idle[i];
      const auto blue=static_cast<unsigned>(palette[i+2])-idle[i+2];
      if(x<w/2) {leftRed+=red;leftBlue+=blue;} else {rightRed+=red;rightBlue+=blue;}
      if (palette[i+3]!=255) return report(59);
    }
    if (!((leftRed*10>leftBlue*12 && rightBlue*10>rightRed*12) ||
          (leftBlue*10>leftRed*12 && rightRed*10>rightBlue*12))) return report(60);
    sprite.setColorGrade(1,0,{},false);
    std::vector<std::uint8_t> reset;
    if (!drawGrade(reset) || reset!=neutral) return report(61);
    std::printf("Saturation zero reduces chroma; hue90 changes %u pixels; per-hit red/blue palette and exact neutral reset verified.\n",hueChanges);
    r.clearBackground();
    sprite.clear();
    if (!sprite.push(3,.5f,.5f)) return report(63);
    std::vector<std::uint8_t> lowGain,highGain;
    if (!drawGrade(lowGain)) return report(64);
    sprite.setBrightness(4);
    if (!drawGrade(highGain)) return report(65);
    unsigned compressedPixels=0;
    for(std::size_t i=0;i<lowGain.size();i+=4) {
      const auto peak=(std::max)({lowGain[i],lowGain[i+1],lowGain[i+2]});
      if (highGain[i+3]!=255) return report(66);
      if (peak<=70) continue;
      ++compressedPixels;
      for(unsigned channel=0;channel<3;++channel) {
        const float expected=255.0f*lowGain[i+channel]/peak;
        if(std::abs(float(highGain[i+channel])-expected)>4) return report(67);
      }
    }
    if (compressedPixels<5) return report(68);
    sprite.setBrightness(1);
    if (!r.loadBackground(argv[1],error)) return report(69);
    std::printf("Zero brightness/intensity preserve the backdrop; 4x gain preserves RGB ratios over %u compressed pixels.\n",compressedPixels);
  } else {
    sprite.clear();sprite.setBrightness(4);
    sprite.setColorGrade(1,0,{{232,74,40}},true);
    if(!sprite.push(3,.5f,.5f)) return report(70);
    r.beginFrame();
    ComPtr<ID3D11RenderTargetView> target;
    r.context()->OMGetRenderTargets(1,&target,nullptr);
    const float white[4]={1,1,1,1};r.context()->ClearRenderTargetView(target.Get(),white);
    std::vector<std::uint8_t> paper;
    if(!sprite.draw(r.context(),w,h,3.29,error) || !capture(r,paper)) return report(71);
    unsigned matched=0;
    for(std::size_t i=0;i<paper.size();i+=4) {
      if(paper[i]<paper[i+1] || paper[i+1]<paper[i+2] || paper[i+3]!=255) return report(72);
      matched+=std::abs(int(paper[i])-232)<=2 && std::abs(int(paper[i+1])-74)<=2 && std::abs(int(paper[i+2])-40)<=2;
    }
    if(matched<5) return report(73);
    sprite.setBrightness(1);sprite.setColorGrade(1,0,{},false);
    std::printf("Paper palette produces requested RGB(232,74,40), not its complement: %u matching pixels.\n",matched);
  }
  r.beginFrame();
  if (!sprite.draw(r.context(),w,h,5,error) || sprite.count()!=0 || !capture(r,fade)) return report(9);
  if (idle!=fade) return report(10);
  std::printf("SpriteBurst WARP: %u changed pixels localized to normalized trigger; exact idle reset.\n",changed);
  std::printf("Effect brightness 2x: %u color samples enhanced, backdrop and alpha preserved.\n",enhanced);
  std::printf("Tint isolates red; scale 2x grows to %u pixels with unchanged idle backdrop.\n",enlargedPixels);
  return 0;
}
// Real D3D11 WARP verification of the opaque-copy shortcut, not a GPU benchmark.
static int backdropProbe(wchar_t** argv) {
  constexpr unsigned w=1920,h=720;
  action75::NativeRenderer r;
  std::string error;
  auto report=[&](int n){std::fprintf(stderr,"backdrop probe %d: %s\n",n,error.c_str());return n;};
  if (!r.initialize(nullptr,w,h,true,error) || !r.loadBackground(argv[1],error)) return report(30);
  std::vector<std::uint8_t> direct, shader;
  r.beginFrame();
  if (!r.usingBackdropCopy() || !capture(r,direct)) return report(31);
  r.allowBackdropCopy(false);
  r.beginFrame();
  if (r.usingBackdropCopy() || !capture(r,shader) || direct.size()!=shader.size()) return report(32);
  unsigned peak=0; std::uint64_t sum=0;
  for (std::size_t i=0;i<direct.size();++i) {
    const auto delta=static_cast<unsigned>(std::abs(int(direct[i])-int(shader[i])));
    peak=std::max(peak,delta); sum+=delta;
    if (i%4==3 && (direct[i]!=255 || shader[i]!=255)) return report(33);
  }
  if (peak>1) return report(34);
  r.allowBackdropCopy(true);
  if (!r.resize(960,360,error)) return report(35);
  r.beginFrame();
  if (r.usingBackdropCopy()) return report(36);
  std::printf("{\"opaqueBackdropCopyEqualWithinOneCode\":true,\"peakCodeDifference\":%u,\"sumCodeDifference\":%llu}\n",
              peak,static_cast<unsigned long long>(sum));
  return 0;
}
static int atlasProbe(wchar_t** argv) {
  constexpr unsigned w=480,h=180;
  action75::NativeRenderer r;
  std::string error;
  auto report=[&](int n){std::fprintf(stderr,"atlas parity probe %d: %s\n",n,error.c_str());return n;};
  if (!r.initialize(nullptr,w,h,true,error) || !r.loadBackground(argv[1],error)) return report(40);
  action75::SpriteBurst volume, flat;
  if (!volume.load(r.device(),argv[2],{6,4,.60f,348},error) ||
      !flat.load(r.device(),argv[2],{6,4,.60f,348},error,true)) return report(41);
  if (!volume.volumeAtlas()) return 77; // CTest explicitly reports an unsupported volume path.
  unsigned peak=0; std::uint64_t absolute=0,pixels=0;
  for(double time : {0.0,.003,.049,.107,.211,.375,.575,.6,.62}) {
    std::vector<std::uint8_t> a,b;
    for (auto *it : {&volume,&flat}) {
      it->clear();
      for (float n : {0.f,.04f,.25f,.53f,.81f,1.f}) it->push(0,n,1-n);
      r.beginFrame();
      if (!it->draw(r.context(),w,h,time,error) || !capture(r,it==&volume?a:b))
        return report(42);
    }
    if (a.size()!=b.size() || a.empty()) return report(43);
    for (std::size_t i=0;i<a.size();++i) {
      const auto d=static_cast<unsigned>(std::abs(int(a[i])-int(b[i])));
      peak=std::max(peak,d);absolute+=d;++pixels;
    }
  }
  const auto mean=static_cast<double>(absolute)/static_cast<double>(pixels);
  if (peak>3 || mean>.030) return report(44);
  std::printf("{\"maxCodeDifference\":%u,\"meanCodeDifference\":%.6f}\n",peak,mean);
  return 0;
}
// Optional hardware benchmark: measures submitted GPU draw work, never
// software playback, screen present time or end-to-end keyboard latency.
static int benchmark(wchar_t** argv,bool compare,bool multiply) {
  constexpr unsigned w=1920,h=720;
  action75::NativeRenderer r;
  std::string error;
  auto report=[&](int code){ std::fprintf(stderr,"benchmark %d: %s\n",code,error.c_str());return code; };
  if (!r.initialize(nullptr,w,h,true,error,true) || !r.loadBackground(argv[1],error)) return report(20);
  const auto diameter=static_cast<float>(std::wcstod(argv[4],nullptr));
  const auto duration=static_cast<float>(std::wcstod(argv[5],nullptr));
  action75::SpriteBurst effect;
  if (!effect.load(r.device(),argv[2],{6,4,duration,diameter,multiply},error)) return report(21);
  action75::SpriteBurst legacy;
  if (compare && !legacy.load(r.device(),argv[2],{6,4,duration,diameter,multiply},error,true)) return report(27);
  auto *device=r.device(); auto *ctx=r.context();
  D3D11_QUERY_DESC descriptor{D3D11_QUERY_TIMESTAMP_DISJOINT,0};
  ComPtr<ID3D11Query> disjoint, start, end;
  if (FAILED(device->CreateQuery(&descriptor,&disjoint))) return report(22);
  descriptor.Query=D3D11_QUERY_TIMESTAMP;
  if (FAILED(device->CreateQuery(&descriptor,&start)) || FAILED(device->CreateQuery(&descriptor,&end))) return report(23);
  const auto query=[&](ID3D11Query *q,auto &data) {
    auto until=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    for (;;) {
      auto code=ctx->GetData(q,&data,static_cast<UINT>(sizeof(data)),D3D11_ASYNC_GETDATA_DONOTFLUSH);
      if (code!=S_FALSE) return SUCCEEDED(code);
      if (std::chrono::steady_clock::now()>=until) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  };
  for (unsigned mode=0;mode<(compare?4u:1u);++mode) {
    auto &atlas=(mode&2u)?legacy:effect;
    r.allowBackdropCopy((mode&1u)==0);
    for (int load : {1,4,24}) {
      std::vector<double> gpu,cpu;
      for (int i=0;i<112;++i) {
        atlas.clear();
        for (int j=0;j<load;++j)
          atlas.push(-duration*(0.15+((i+j*3)%31)*.023),
            .12f+.15f*static_cast<float>(j%6),.17f+.22f*static_cast<float>(j/6));
        ctx->Begin(disjoint.Get());ctx->End(start.Get());
        auto t0=std::chrono::steady_clock::now();
        r.beginFrame();
        if (!atlas.draw(ctx,w,h,0,error)) return report(24);
        auto t1=std::chrono::steady_clock::now();
        ctx->End(end.Get());ctx->End(disjoint.Get());ctx->Flush();
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT timing{};
        UINT64 from=0,to=0;
        if (!query(disjoint.Get(),timing) || !query(start.Get(),from) || !query(end.Get(),to)) return report(25);
        if (i<16 || timing.Disjoint || !timing.Frequency || to<from) continue;
        gpu.push_back(1000.0*static_cast<double>(to-from)/static_cast<double>(timing.Frequency));
        cpu.push_back(std::chrono::duration<double,std::milli>(t1-t0).count());
      }
      if (gpu.size()<48) return report(26);
      std::sort(gpu.begin(),gpu.end());std::sort(cpu.begin(),cpu.end());
      const auto percentile=[](const auto &v,double p){return v[static_cast<std::size_t>((v.size()-1)*p)];};
      std::printf("{\"atlas\":\"%s\",\"backdrop\":\"%s\",\"bursts\":%d,\"samples\":%zu,\"gpuMedianMs\":%.4f,\"gpuP95Ms\":%.4f,\"cpuSubmitP95Ms\":%.4f}\n",
        atlas.volumeAtlas()?"3d":"2d",r.usingBackdropCopy()?"copy":"shader",load,gpu.size(),percentile(gpu,.5),percentile(gpu,.95),percentile(cpu,.95));
    }
  }
  return 0;
}
int wmain(int argc,wchar_t** argv) {
  const bool multiply=argc==4 && std::wcscmp(argv[3],L"--multiply")==0;
  const bool atlasParity=argc==4 && std::wcscmp(argv[3],L"--atlas-parity")==0;
  const bool backdrop=argc==3 && std::wcscmp(argv[2],L"--backdrop")==0;
  const bool benchArgs=argc==6 || (argc==7 && std::wcscmp(argv[6],L"--multiply")==0);
  const bool compare=benchArgs && std::wcscmp(argv[3],L"--benchmark-compare")==0;
  const bool profile=compare || (benchArgs && std::wcscmp(argv[3],L"--benchmark")==0);
  if (argc!=3 && !multiply && !atlasParity && !profile) {
    std::fprintf(stderr,"usage: action75_sprite_burst_probe background.png [--backdrop | atlas.png [--multiply | --atlas-parity | --benchmark[-compare] diameter duration [--multiply]]]\n");
    return 99;
  }
  const auto hr=CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(hr)) return 98;
  const int result=backdrop ? backdropProbe(argv) :
      (atlasParity ? atlasProbe(argv) : (profile ? benchmark(argv,compare,argc==7) : probe(argv,multiply)));
  CoUninitialize();
  return result;
}
