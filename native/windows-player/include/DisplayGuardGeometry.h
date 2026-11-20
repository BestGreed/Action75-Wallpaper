#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace action75::display_guard {
struct Rect {
  std::int64_t left=0,top=0,right=0,bottom=0;
  bool valid() const noexcept {return right>left && bottom>top;}
  std::int64_t width() const noexcept {return right-left;}
  std::int64_t height() const noexcept {return bottom-top;}
};
inline bool operator==(const Rect& a,const Rect& b) noexcept {
  return a.left==b.left && a.top==b.top && a.right==b.right && a.bottom==b.bottom;
}
inline bool operator!=(const Rect& a,const Rect& b) noexcept {return !(a==b);}
inline Rect intersection(const Rect& a,const Rect& b) noexcept {
  return {(std::max)(a.left,b.left),(std::max)(a.top,b.top),(std::min)(a.right,b.right),(std::min)(a.bottom,b.bottom)};
}
inline bool intersects(const Rect& a,const Rect& b) noexcept {return intersection(a,b).valid();}
inline bool contains(const Rect& outer,const Rect& inner) noexcept {
  return outer.valid() && inner.valid() && inner.left>=outer.left && inner.top>=outer.top && inner.right<=outer.right && inner.bottom<=outer.bottom;
}
inline Rect unite(const Rect& a,const Rect& b) noexcept {
  if(!a.valid()) return b;if(!b.valid()) return a;
  return {(std::min)(a.left,b.left),(std::min)(a.top,b.top),(std::max)(a.right,b.right),(std::max)(a.bottom,b.bottom)};
}
struct Monitor {std::wstring device;Rect bounds,work;bool primary=false;};
struct Topology {
  bool targetPresent=false,supported=false;
  Rect target,allowed,virtualBounds;
  std::vector<Rect> allowedWorkAreas;
  std::string reason;
};
inline bool rectangularUnion(const std::vector<Rect>& rectangles,const Rect& bounds) {
  if(!bounds.valid() || rectangles.empty() || rectangles.size()>32) return false;
  std::vector<std::int64_t> xs;
  for(const auto& rectangle:rectangles) {if(!rectangle.valid()) return false;xs.push_back(rectangle.left);xs.push_back(rectangle.right);}
  std::sort(xs.begin(),xs.end());xs.erase(std::unique(xs.begin(),xs.end()),xs.end());
  for(std::size_t x=1;x<xs.size();++x) {
    std::vector<std::pair<std::int64_t,std::int64_t>> intervals;
    for(const auto& rectangle:rectangles)
      if(rectangle.left<=xs[x-1] && rectangle.right>=xs[x]) intervals.emplace_back(rectangle.top,rectangle.bottom);
    std::sort(intervals.begin(),intervals.end());
    auto covered=bounds.top;
    for(const auto& interval:intervals) {
      if(interval.first>covered) return false;
      covered=(std::max)(covered,interval.second);
    }
    if(covered<bounds.bottom) return false;
  }
  return true;
}
inline Topology analyze(const std::vector<Monitor>& monitors,const std::wstring& targetDevice) {
  Topology result;
  if(monitors.size()>32) {result.reason="Monitor topology exceeds the supported limit";return result;}
  std::vector<Rect> others;
  for(const auto& monitor:monitors) {
    if(!monitor.bounds.valid()) {result.reason="Invalid monitor bounds";return result;}
    result.virtualBounds=unite(result.virtualBounds,monitor.bounds);
  }
  for(const auto& monitor:monitors) {
    if(monitor.device==targetDevice) {
      if(result.targetPresent) {result.reason="Target display is ambiguous";return result;}
      result.target=monitor.bounds;result.targetPresent=true;
      if(monitor.primary) {result.reason="The primary display cannot be reserved";return result;}
    }
    else {others.push_back(monitor.bounds);result.allowed=unite(result.allowed,monitor.bounds);result.allowedWorkAreas.push_back(monitor.work.valid() ? monitor.work : monitor.bounds);}
  }
  if(!result.targetPresent) {result.reason="Target display is disconnected";return result;}
  if(others.empty()) {result.reason="No other display remains available";return result;}
  if(intersects(result.allowed,result.target) || !rectangularUnion(others,result.allowed)) {
    result.reason="Remaining displays cannot be represented by one safe rectangle";return result;
  }
  result.supported=true;return result;
}

enum class ClipAction {None,Set,Release};
struct ClipPlan {
  ClipAction action=ClipAction::None;
  Rect desired,observed;
  std::optional<Rect> restore;
  bool active=false;
  const char* mode="inactive";
};
// ClipCursor has no owner token. Ownership is retained only while GetClipCursor
// still matches our last successful write. An observed external change revokes it.
class ClipPolicy {
public:
  ClipPlan check(const Topology& topology,const Rect& current) {
    observe(current);
    ClipPlan result;result.observed=current;
    if(!topology.supported || !current.valid()) {result.mode="unsupported";return result;}
    const auto constraint=owned_ ? restore_.value_or(topology.virtualBounds) : current;
    const auto safe=intersection(topology.allowed,constraint);
    if(safe==current && safe.valid()) {
      result.active=true;result.mode=owned_ ? "active" : "external-clip";return result;
    }
    if(!safe.valid()) {result.mode="external-clip-conflict";return result;}
    result.action=ClipAction::Set;result.desired=safe;result.active=true;result.mode="active";
    if(owned_) result.restore=restore_;
    else if(current!=topology.virtualBounds) result.restore=current;
    return result;
  }
  void applied(const ClipPlan& plan) noexcept {
    if(plan.action!=ClipAction::Set) return;
    // If we are tightening our own previous clip after a topology change, keep
    // the baseline from before this guard first acquired it.
    if(!owned_ || plan.observed!=installed_) restore_=plan.restore;
    installed_=plan.desired;owned_=true;
  }
  ClipPlan release(const Rect& current,const Rect& currentVirtual) {
    observe(current);
    ClipPlan plan;plan.observed=current;plan.mode="disabled";
    if(!owned_) return plan;
    plan.action=ClipAction::Release;
    if(restore_) {
      const auto safe=intersection(*restore_,currentVirtual);
      if(safe.valid()) plan.restore=safe;
    }
    return plan;
  }
  void released() noexcept {owned_=false;restore_.reset();installed_={};}
  bool owned() const noexcept {return owned_;}
  Rect installed() const noexcept {return installed_;}
private:
  void observe(const Rect& current) noexcept {
    if(owned_ && current!=installed_) released();
  }
  bool owned_=false;
  Rect installed_;
  std::optional<Rect> restore_;
};

struct WindowTraits {
  bool visible=true,topLevel=true,cloaked=false,minimized=false,ownProcess=false,shell=false;
  bool toolWindow=false,noActivate=false,ordinary=true,reliableFrame=true;
  bool popup=false,caption=false,resizable=false,topmost=false;
  Rect frame,outer;
};
// A desktop capture mask has a fixed desktop-sized coordinate system. Moving it
// breaks both its captured pixels and its selection coordinates. Keep this
// exception narrow: ordinary Qt/chat/tool windows still use relocation below.
inline bool qtCaptureWindowClass(std::wstring_view name) noexcept {
  if(name.size()<3 || name.substr(0,2)!=L"Qt") return false;
  std::size_t offset=2;
  while(offset<name.size() && name[offset]>=L'0' && name[offset]<=L'9') ++offset;
  return offset>2 && name.substr(offset)==L"QWindowIcon";
}
inline std::wstring_view codexOverlayExecutable(std::wstring_view className) noexcept {
  if(className==L"CodexComputerUseCursorOverlay") return L"codex-computer-use.exe";
  if(className==L"CodexComputerUseSwiftOverlay") return L"codex-computer-use-swift.exe";
  return {};
}
inline bool captureOverlayProcess(std::wstring_view className,std::wstring_view imagePath) noexcept {
  const auto separator=imagePath.find_last_of(L"\\/");
  const auto name=imagePath.substr(separator==std::wstring_view::npos ? 0 : separator+1);
  const auto equals=[&](std::wstring_view expected) {
    if(name.size()!=expected.size()) return false;
    for(std::size_t i=0;i<name.size();++i) {
      auto c=name[i];if(c>=L'A' && c<=L'Z') c+=L'a'-L'A';
      if(c!=expected[i]) return false;
    }
    return true;
  };
  const auto codex=codexOverlayExecutable(className);
  if(!codex.empty()) return equals(codex);
  return qtCaptureWindowClass(className) && (equals(L"weixin.exe") || equals(L"wechat.exe"));
}
inline bool captureSurface(const Rect& frame,const Rect& surface) noexcept {
  if(!frame.valid() || !surface.valid()) return false;
  const auto nearCoordinate=[](std::int64_t a,std::int64_t b) {
    const auto distance=static_cast<long double>(a)-static_cast<long double>(b);
    return distance>=-2 && distance<=2;
  };
  // Weixin's observed mask has the complete virtual-desktop size but is
  // translated vertically by one physical pixel. Bound both edges and size.
  return nearCoordinate(frame.left,surface.left) && nearCoordinate(frame.top,surface.top) &&
         nearCoordinate(frame.right,surface.right) && nearCoordinate(frame.bottom,surface.bottom) &&
         nearCoordinate(frame.width(),surface.width()) && nearCoordinate(frame.height(),surface.height());
}
inline bool captureOverlayCandidate(const Topology& topology,const WindowTraits& window,
                                    std::wstring_view className) noexcept {
  if(!topology.supported || !window.visible || !window.topLevel || window.cloaked ||
     window.minimized || window.ownProcess || window.shell || !window.reliableFrame ||
     !window.popup || window.caption || window.resizable || !window.topmost) return false;
  const bool fullDesktop=captureSurface(window.frame,topology.virtualBounds) ||
                         captureSurface(window.outer,topology.virtualBounds);
  if(!codexOverlayExecutable(className).empty()) {
    return window.toolWindow && window.noActivate && fullDesktop;
  }
  return qtCaptureWindowClass(className) && (fullDesktop ||
         captureSurface(window.frame,topology.target) || captureSurface(window.outer,topology.target));
}
inline bool captureOverlay(const Topology& topology,const WindowTraits& window,
                           std::wstring_view className,std::wstring_view imagePath) noexcept {
  return captureOverlayCandidate(topology,window,className) && captureOverlayProcess(className,imagePath);
}
inline bool ordinaryWindow(const WindowTraits& window) noexcept {
  return window.visible && window.topLevel && !window.cloaked && !window.minimized && !window.ownProcess &&
         !window.shell && window.ordinary && window.reliableFrame &&
         window.frame.valid() && window.outer.valid();
}
struct MovePlan {std::int64_t x=0,y=0;Rect frame;};
inline std::optional<MovePlan> relocate(const Topology& topology,const WindowTraits& window) {
  if(!topology.supported || !ordinaryWindow(window) || !intersects(window.frame,topology.target)) return {};
  const long double cx=(static_cast<long double>(window.frame.left)+window.frame.right)/2;
  const long double cy=(static_cast<long double>(window.frame.top)+window.frame.bottom)/2;
  Rect destination=topology.allowed;
  long double distance=-1;
  for(const auto& work:topology.allowedWorkAreas) {
    const long double dx=cx-std::clamp(cx,static_cast<long double>(work.left),static_cast<long double>(work.right));
    const long double dy=cy-std::clamp(cy,static_cast<long double>(work.top),static_cast<long double>(work.bottom));
    if(distance<0 || dx*dx+dy*dy<distance) {distance=dx*dx+dy*dy;destination=work;}
  }
  auto x=destination.left,y=destination.top;
  if(window.frame.width()<=destination.width()) x=std::clamp(window.frame.left,destination.left,destination.right-window.frame.width());
  if(window.frame.height()<=destination.height()) y=std::clamp(window.frame.top,destination.top,destination.bottom-window.frame.height());
  Rect next{x,y,x+window.frame.width(),y+window.frame.height()};
  if(intersects(next,topology.target)) {
    if(topology.target.right<=topology.allowed.left) x=(std::max)(x,topology.allowed.left);
    else if(topology.target.left>=topology.allowed.right) x=(std::min)(x,topology.allowed.right-window.frame.width());
    else if(topology.target.bottom<=topology.allowed.top) y=(std::max)(y,topology.allowed.top);
    else if(topology.target.top>=topology.allowed.bottom) y=(std::min)(y,topology.allowed.bottom-window.frame.height());
    next={x,y,x+window.frame.width(),y+window.frame.height()};
  }
  if(intersects(next,topology.target)) return {};
  return MovePlan{window.outer.left+x-window.frame.left,window.outer.top+y-window.frame.top,next};
}
class MoveThrottle {
public:
  bool ready(std::uintptr_t window,std::uint64_t now) const noexcept {
    for(const auto& item:entries_) if(item.window==window) return now>=item.until;
    return true;
  }
  void mark(std::uintptr_t window,std::uint64_t now) {
    for(auto& item:entries_) if(item.window==window) {item.until=now+500;return;}
    if(entries_.size()>=128) entries_.erase(entries_.begin());
    entries_.push_back({window,now+500});
  }
  void clear() noexcept {entries_.clear();}
private:
  struct Entry {std::uintptr_t window;std::uint64_t until;};
  std::vector<Entry> entries_;
};
}
