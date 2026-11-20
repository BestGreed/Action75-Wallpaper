#pragma once
#include <d3d11.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <nlohmann/json.hpp>
#include <vector>
#include <array>
#include <chrono>
#include <algorithm>

namespace action75 {
// Opt-in measurement only. Queries are read asynchronously without GPU flushes.
class PerfStats {
 using Clock=std::chrono::steady_clock;
 struct Slot {Microsoft::WRL::ComPtr<ID3D11Query> disjoint,start,end;bool pending=false;unsigned generation=0;};
 std::array<Slot,12> slots_{};
 Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter_;
 std::string adapterName_;
 std::vector<double> gpu_,cpu_,interval_;
 unsigned generation_=0,next_=0,active_=999,skipped_=0,disjoint_=0;
 Clock::time_point previous_{};
 bool initialized_=false;
 static nlohmann::json summary(std::vector<double> values){
  if(values.empty())return {{"count",0},{"mean",nullptr},{"p50",nullptr},{"p95",nullptr},{"max",nullptr}};
  double total=0;for(double v:values)total+=v;std::sort(values.begin(),values.end());
  auto percentile=[&](double p){return values[static_cast<std::size_t>(p*(values.size()-1))];};
  return {{"count",values.size()},{"mean",total/values.size()},{"p50",percentile(.5)},{"p95",percentile(.95)},{"max",values.back()}};
 }
 void collect(ID3D11DeviceContext* c){
  for(auto& slot:slots_){if(!slot.pending)continue;D3D11_QUERY_DATA_TIMESTAMP_DISJOINT clock{};
   if(c->GetData(slot.disjoint.Get(),&clock,sizeof(clock),D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK)continue;
   UINT64 a=0,b=0;if(c->GetData(slot.start.Get(),&a,sizeof(a),D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK||c->GetData(slot.end.Get(),&b,sizeof(b),D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK)continue;
   slot.pending=false;if(slot.generation!=generation_)continue;
   if(clock.Disjoint || !clock.Frequency || b<a){++disjoint_;continue;}
   if(gpu_.size()<50000)gpu_.push_back((b-a)*1000.0/clock.Frequency);
  }
 }
public:
 bool initialize(ID3D11Device* device){
  for(auto& slot:slots_){D3D11_QUERY_DESC q{};q.Query=D3D11_QUERY_TIMESTAMP_DISJOINT;if(FAILED(device->CreateQuery(&q,&slot.disjoint)))return false;q.Query=D3D11_QUERY_TIMESTAMP;if(FAILED(device->CreateQuery(&q,&slot.start))||FAILED(device->CreateQuery(&q,&slot.end)))return false;}
  Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  if(SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi)))&&SUCCEEDED(dxgi->GetAdapter(&adapter))){DXGI_ADAPTER_DESC desc{};adapter->GetDesc(&desc);for(wchar_t ch:desc.Description){if(!ch)break;adapterName_.push_back(ch<128?static_cast<char>(ch):'?');}adapter.As(&adapter_);}
  initialized_=true;return true;
 }
 void reset(){++generation_;gpu_.clear();cpu_.clear();interval_.clear();skipped_=disjoint_=0;previous_={};}
 void begin(ID3D11DeviceContext* c){
  if(!initialized_)return;collect(c);auto now=Clock::now();
  if(previous_!=Clock::time_point{}&&interval_.size()<50000)interval_.push_back(std::chrono::duration<double,std::milli>(now-previous_).count());previous_=now;
  auto& slot=slots_[next_++%slots_.size()];active_=999;
  if(slot.pending){++skipped_;return;}active_=static_cast<unsigned>(&slot-slots_.data());slot.generation=generation_;c->Begin(slot.disjoint.Get());c->End(slot.start.Get());
 }
 void end(ID3D11DeviceContext* c,double cpuMs){
  if(!initialized_)return;if(cpu_.size()<50000)cpu_.push_back(cpuMs);if(active_==999)return;
  auto& slot=slots_[active_];c->End(slot.end.Get());c->End(slot.disjoint.Get());slot.pending=true;active_=999;
  // Offscreen runs have no Present. Flush submits their work, without waiting.
  c->Flush();
 }
 nlohmann::json snapshot(ID3D11DeviceContext* c){
  collect(c);nlohmann::json local=nullptr,shared=nullptr;
  if(adapter_){DXGI_QUERY_VIDEO_MEMORY_INFO info{};if(SUCCEEDED(adapter_->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&info)))local=info.CurrentUsage/1048576.;if(SUCCEEDED(adapter_->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL,&info)))shared=info.CurrentUsage/1048576.;}
  return {{"adapter",adapterName_},{"gpuMs",summary(gpu_)},{"cpuSubmitMs",summary(cpu_)},{"frameIntervalMs",summary(interval_)},{"gpuSkipped",skipped_},{"gpuDisjoint",disjoint_},{"gpuLocalMB",local},{"gpuNonLocalMB",shared},{"logicalProcessors",GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)}};
 }
};
}
