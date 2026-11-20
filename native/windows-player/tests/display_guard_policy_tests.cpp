#include "DisplayGuardGeometry.h"
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// Policy-only tests: no Windows headers, DisplayGuard linkage, mouse, hooks,
// windows, process startup, or production files. FakeClipOS models ClipCursor.
namespace dg = action75::display_guard;
namespace {
void require(bool condition,const char* message) {
  if(!condition) throw std::runtime_error(message);
}
const dg::Rect Primary{0,0,2560,1440};
const dg::Rect Target{-1920,-648,0,72};
std::vector<dg::Monitor> monitors() {
  return {{L"primary",Primary,Primary,true},{L"target",Target,Target,false}};
}
dg::Topology topology() {return dg::analyze(monitors(),L"target");}
struct FakeClipOS {
  dg::Rect current;
  std::vector<dg::ClipPlan> writes;
  bool failNext=false;
  bool apply(const dg::ClipPlan& plan,const dg::Rect& virtualBounds) {
    if(plan.action==dg::ClipAction::None) return true;
    if(failNext) {failNext=false;return false;}
    writes.push_back(plan);
    current=plan.action==dg::ClipAction::Set ? plan.desired : plan.restore.value_or(virtualBounds);
    return true;
  }
};
struct FakeGuard {
  dg::ClipPolicy policy;
  FakeClipOS os;
  bool active=false;
  std::string mode="disabled";
  explicit FakeGuard(const dg::Rect& initial) {os.current=initial;}
  // A transition publishes inactive only after releasing our owned clip.
  // An external write always revokes ownership through policy.release/check.
  void configure(const dg::Topology& current,bool enabled=true,bool suspended=false) {
    if(!enabled || suspended || !current.supported) {
      const auto plan=policy.release(os.current,current.virtualBounds);
      if(!os.apply(plan,current.virtualBounds)) {active=policy.owned();mode="error";return;}
      if(plan.action==dg::ClipAction::Release) policy.released();
      active=false;mode=suspended ? "suspended" : enabled ? "unsupported" : "disabled";
      return;
    }
    const auto plan=policy.check(current,os.current);
    if(!os.apply(plan,current.virtualBounds)) {active=false;mode="error";return;}
    if(plan.action==dg::ClipAction::Set) policy.applied(plan);
    active=plan.active;mode=plan.mode;
  }
  void stop(const dg::Topology& current) {configure(current,false);}
};
dg::WindowTraits targetWindow() {
  dg::WindowTraits window;
  window.frame={-1820,-548,-1340,-308};
  window.outer={-1828,-556,-1332,-300};
  return window;
}
dg::WindowTraits observedWeixinCaptureMask() {
  dg::WindowTraits window;
  // Captured twice: Qt51514QWindowIcon, style 0x96000000, exStyle 0x8.
  // It has neither TOOLWINDOW nor LAYERED nor NOACTIVATE extended styles.
  window.popup=true;window.topmost=true;
  window.frame={-1920,-647,2560,1441};window.outer=window.frame;
  return window;
}
bool movedWithCaptureClassification(const dg::WindowTraits& input,
                                    std::wstring_view className,
                                    std::wstring_view imagePath) {
  auto window=input;
  window.ordinary=!dg::captureOverlay(topology(),window,className,imagePath);
  return dg::relocate(topology(),window).has_value();
}
}
int main() {
  unsigned passed=0;
  const auto test=[&](const char* name,const std::function<void()>& run) {
    try {run();++passed;std::cout<<"PASS "<<name<<'\n';}
    catch(const std::exception& error) {std::cerr<<"FAIL "<<name<<": "<<error.what()<<'\n';throw;}
  };
  try {
    test("negative PMv2 topology preserves physical monitor coordinates",[] {
      const auto t=topology();
      require(t.targetPresent && t.supported,"Current primary/keyboard topology must be supported");
      require(t.target==Target && t.allowed==Primary,"Monitor pixels were scaled, shifted or expanded");
      require(t.virtualBounds==dg::Rect{-1920,-648,2560,1440},"Virtual bounds lost negative coordinates");
      require(!dg::intersects(t.allowed,t.target),"Allowed cursor rectangle intersects keyboard screen");
    });
    test("all other monitors are supported only as an exact safe rectangle",[] {
      auto list=monitors();list.push_back({L"right",{2560,0,3840,1440},{2560,0,3840,1440},false});
      const auto t=dg::analyze(list,L"target");
      require(t.supported && t.allowed==dg::Rect{0,0,3840,1440},"Adjacent non-target screens were unnecessarily excluded");
      require(t.allowedWorkAreas.size()==2,"Allowed display work areas were lost");
      list.back().bounds={2560,720,3840,1440};list.back().work=list.back().bounds;
      const auto lShape=dg::analyze(list,L"target");
      require(!lShape.supported && !lShape.reason.empty(),"L-shaped remainder was incorrectly restricted to its bounding rectangle");
    });
    test("disconnected primary sole and ambiguous target configurations fail open",[] {
      auto list=monitors();
      require(!dg::analyze(list,L"missing").supported,"Disconnected target was accepted");
      require(!dg::analyze(list,L"primary").supported,"Primary display could be reserved");
      require(!dg::analyze({list[1]},L"target").supported,"Only display could be reserved");
      list.push_back(list[1]);
      require(!dg::analyze(list,L"target").supported,"Duplicate target identity was accepted");
    });
    test("first acquisition and owned release use explicit policy commits",[] {
      const auto t=topology();dg::ClipPolicy policy;
      const auto plan=policy.check(t,t.virtualBounds);
      require(plan.action==dg::ClipAction::Set && plan.desired==Primary,"Initial unrestricted cursor was not constrained to allowed screens");
      require(!policy.owned(),"Planning a write falsely claimed OS ownership");
      policy.applied(plan);
      require(policy.owned() && policy.installed()==Primary,"Successful set did not record ownership");
      const auto release=policy.release(Primary,t.virtualBounds);
      require(release.action==dg::ClipAction::Release && !release.restore,"Unrestricted baseline should release rather than restore a stale virtual rectangle");
      require(policy.owned(),"Planning a release prematurely discarded OS ownership");
      policy.released();require(!policy.owned(),"Successful release did not clear ownership");
    });
    test("preexisting stricter safe game clip is left exactly unchanged",[] {
      const auto t=topology();const dg::Rect game{100,120,1280,900};FakeGuard guard(game);
      guard.configure(t);
      require(guard.active && guard.mode=="external-clip" && !guard.policy.owned(),"Safe game constraint was claimed as our own");
      require(guard.os.current==game && guard.os.writes.empty(),"Existing stricter game clip was modified");
      guard.stop(t);
      require(guard.os.current==game && guard.os.writes.empty(),"Stop cleared a game-owned clip");
    });
    test("external stricter rewrite revokes our ownership and is never restored over",[] {
      const auto t=topology();FakeGuard guard(t.virtualBounds);guard.configure(t);
      const auto priorWrites=guard.os.writes.size();const dg::Rect game{200,200,1000,800};
      guard.os.current=game;guard.configure(t);
      require(!guard.policy.owned() && guard.mode=="external-clip","External writer did not revoke our ownership");
      guard.stop(t);
      require(guard.os.current==game && guard.os.writes.size()==priorWrites,"Stop overwrote the external writer's constraint");
    });
    test("external conflicting game clip is respected without false active state",[] {
      const auto t=topology();FakeGuard guard(Target);guard.configure(t);
      require(!guard.active && guard.mode=="external-clip-conflict","Conflicting game clip was reported as active protection");
      require(!guard.policy.owned() && guard.os.current==Target && guard.os.writes.empty(),"Conflicting game clip was overridden");
    });
    test("guard restores only the still-owned prior external baseline",[] {
      const auto t=topology();const dg::Rect baseline{-500,-100,2560,1440};FakeGuard guard(baseline);
      guard.configure(t);require(guard.policy.owned() && guard.os.current==Primary,"Baseline was not tightened safely");
      guard.stop(t);
      require(guard.os.current==baseline && !guard.policy.owned(),"Valid preexisting external baseline was not restored");
    });
    test("game release permits a fresh guard acquisition",[] {
      const auto t=topology();FakeGuard guard(t.virtualBounds);guard.configure(t);
      guard.os.current={50,50,900,700};guard.configure(t);require(!guard.policy.owned(),"Game write did not replace guard ownership");
      guard.os.current=t.virtualBounds;guard.configure(t);
      require(guard.active && guard.policy.owned() && guard.os.current==Primary,"Guard did not reacquire after game release");
      guard.stop(t);require(guard.os.current==t.virtualBounds,"Fresh acquisition restored a stale game clip");
    });
    test("topology expansion updates our clip and releases to current virtual space",[] {
      const auto original=topology();FakeGuard guard(original.virtualBounds);guard.configure(original);
      auto list=monitors();list.push_back({L"right",{2560,0,3840,1440},{2560,0,3840,1440},false});
      const auto expanded=dg::analyze(list,L"target");guard.configure(expanded);
      require(guard.active && guard.policy.owned() && guard.os.current==expanded.allowed,"Own old clip prevented newly added safe monitor access");
      guard.stop(expanded);require(guard.os.current==expanded.virtualBounds,"Stop restored stale pre-expansion desktop bounds");
    });
    test("suspend stop disconnect and unsupported topology release owned constraints",[] {
      const auto t=topology();
      for(unsigned transition=0;transition<4;++transition) {
        FakeGuard guard(t.virtualBounds);guard.configure(t);require(guard.active,"Setup did not acquire guard");
        if(transition==0) guard.configure(t,true,true);
        if(transition==1) guard.stop(t);
        if(transition==2) {auto list=monitors();list.erase(list.begin()+1);guard.configure(dg::analyze(list,L"target"));}
        if(transition==3) {auto bad=t;bad.supported=false;guard.configure(bad);}
        require(!guard.active && !guard.policy.owned(),"Inactive state retained an owned cursor restriction");
        require(guard.os.writes.size()==2 && guard.os.writes.back().action==dg::ClipAction::Release,
          "Transition published inactive without the release operation");
      }
    });
    test("failed fake OS acquisition never claims ownership or active success",[] {
      const auto t=topology();FakeGuard guard(t.virtualBounds);guard.os.failNext=true;guard.configure(t);
      require(!guard.active && !guard.policy.owned() && guard.mode=="error","Failed write was reported as active");
      require(guard.os.current==t.virtualBounds && guard.os.writes.empty(),"Failed fake OS write changed desktop state");
      guard.configure(t);require(guard.active && guard.policy.owned(),"Recovery after failed acquisition was blocked");
    });
    test("failed release retains ownership until a later successful cleanup",[] {
      const auto t=topology();FakeGuard guard(t.virtualBounds);guard.configure(t);
      guard.os.failNext=true;guard.configure(t,true,true);
      require(guard.policy.owned() && guard.active && guard.mode=="error" && guard.os.current==Primary,
        "Failed suspend release falsely reported that the cursor was unrestricted");
      guard.configure(t,true,true);
      require(!guard.policy.owned() && !guard.active && guard.mode=="suspended" && guard.os.current==t.virtualBounds,
        "Successful cleanup retry failed to release the cursor before publishing suspension");
    });
    test("DWM visible frame avoids false relocation from invisible maximize borders",[] {
      const auto t=topology();dg::WindowTraits window;window.frame=Primary;window.outer={-8,-8,2568,1448};
      require(dg::intersects(window.outer,Target),"Fixture does not reproduce invisible-border overlap");
      require(!dg::relocate(t,window),"Maximized main-screen window was moved because of its invisible border");
    });
    test("ordinary target window moves with DWM offsets while filtered windows stay put",[] {
      const auto t=topology();const auto original=targetWindow();const auto plan=dg::relocate(t,original);
      require(plan.has_value() && dg::contains(Primary,plan->frame),"Ordinary target window was not relocated safely");
      require(plan->x==-8 && plan->y==-8 && plan->frame==dg::Rect{0,0,480,240},"DWM frame-to-outer offset was not retained");
      std::vector<dg::WindowTraits> skipped;
      auto w=original;w.visible=false;skipped.push_back(w);
      w=original;w.topLevel=false;skipped.push_back(w);
      w=original;w.cloaked=true;skipped.push_back(w);
      w=original;w.minimized=true;skipped.push_back(w);
      w=original;w.ownProcess=true;skipped.push_back(w);
      w=original;w.shell=true;skipped.push_back(w);
      w=original;w.ordinary=false;skipped.push_back(w);
      w=original;w.reliableFrame=false;skipped.push_back(w);
      for(const auto& excluded:skipped) require(!dg::relocate(t,excluded),"Excluded own/shell/cloaked/minimized window was moved");
      w=original;w.toolWindow=true;require(dg::relocate(t,w).has_value(),"Foreign tool overlay entering target was exempted");
      w=original;w.noActivate=true;require(dg::relocate(t,w).has_value(),"Foreign no-activate overlay entering target was exempted");
    });
    test("observed Weixin virtual capture mask stays at its one-pixel desktop offset",[] {
      const auto mask=observedWeixinCaptureMask();
      require(dg::intersects(mask.frame,Target),"Recorded capture mask does not overlap target");
      require(dg::captureOverlayCandidate(topology(),mask,L"Qt51514QWindowIcon"),"Recorded topmost-only mask was not a cheap candidate");
      require(!mask.toolWindow && !mask.noActivate,"Fixture accidentally requires absent styles");
      require(!movedWithCaptureClassification(mask,L"Qt51514QWindowIcon",L"D:\\Daily\\Weixin\\Weixin.exe"),"Recorded Weixin screenshot mask was relocated");
    });
    test("Codex desktop overlays keep the physical virtual origin",[] {
      const auto t=topology();
      dg::WindowTraits overlay;
      overlay.popup=true;overlay.topmost=true;overlay.toolWindow=true;overlay.noActivate=true;
      overlay.outer=t.virtualBounds;overlay.frame=t.virtualBounds;
      const auto move=dg::relocate(t,overlay);
      require(move && move->frame==dg::Rect{0,0,4480,2088},
              "Unclassified overlay did not reproduce the observed shift");
      constexpr auto cursorClass=L"CodexComputerUseCursorOverlay";
      constexpr auto swiftClass=L"CodexComputerUseSwiftOverlay";
      constexpr auto cursorExe=L"C:/OpenAI/codex-computer-use.exe";
      constexpr auto swiftExe=L"C:/OpenAI/codex-computer-use-swift.exe";
      require(!movedWithCaptureClassification(overlay,cursorClass,cursorExe),
              "Moving the cursor overlay offsets both monitor outlines");
      require(!movedWithCaptureClassification(overlay,swiftClass,swiftExe),
              "Moving the Swift overlay offsets its desktop surface");
      require(movedWithCaptureClassification(overlay,cursorClass,swiftExe) &&
              movedWithCaptureClassification(overlay,swiftClass,cursorExe) &&
              movedWithCaptureClassification(overlay,L"Chrome_WidgetWin_1",cursorExe),
              "The exception accepted an unrelated class or process pair");
      overlay.noActivate=false;
      require(movedWithCaptureClassification(overlay,cursorClass,cursorExe),
              "The exception accepted an activatable window");
      overlay.noActivate=true;overlay.outer=Target;overlay.frame=Target;
      require(movedWithCaptureClassification(overlay,cursorClass,cursorExe),
              "The exception accepted a window sized only to the keyboard display");
    });
    test("complete target-monitor capture mask is also exempt without tool or layered flags",[] {
      auto mask=observedWeixinCaptureMask();mask.frame=Target;mask.outer=Target;
      require(!movedWithCaptureClassification(mask,L"Qt51514QWindowIcon",L"C:\\Apps\\WeChat\\WeChat.exe"),"Per-monitor screenshot mask was relocated");
    });
    test("capture identity accepts Qt numeric versions and case-insensitive exact executable names",[] {
      const auto mask=observedWeixinCaptureMask();
      for(const auto* name:{L"Qt51514QWindowIcon",L"Qt66QWindowIcon",L"Qt6100QWindowIcon"})
        require(dg::captureOverlay(topology(),mask,name,L"D:/Apps/WEIXIN.EXE"),"Legitimate numeric Qt version or uppercase image was rejected");
      for(const auto* name:{L"QtQWindowIcon",L"Qt51514OtherWindow",L"OtherQt51514QWindowIcon",L"Qt51514QWindowIconExtra"})
        require(!dg::captureOverlayCandidate(topology(),mask,name),"Unverified window class was treated as a capture mask");
      for(const auto* image:{L"D:\\Apps\\Weixin.exe.bak",L"D:\\Apps\\WeixinCapture.exe",L"D:\\Apps\\OtherQt.exe"})
        require(movedWithCaptureClassification(mask,L"Qt51514QWindowIcon",image),"Other Qt application's fullscreen popup was exempted");
    });
    test("ordinary Weixin chat window on the target remains protected",[] {
      auto chat=observedWeixinCaptureMask();chat.frame=Target;chat.outer=Target;
      // Observed main-window style 0x96c70000 includes caption/thickframe.
      chat.caption=true;chat.resizable=true;
      require(!dg::captureOverlayCandidate(topology(),chat,L"Qt51514QWindowIcon"),"Chat chrome was classified as a screenshot mask");
      require(movedWithCaptureClassification(chat,L"Qt51514QWindowIcon",L"D:\\Daily\\Weixin\\Weixin.exe"),"Ordinary Weixin chat window could occupy the target");
    });
    test("small Weixin tools and no-activate overlays are not blanket exemptions",[] {
      auto tool=targetWindow();tool.popup=true;tool.topmost=true;tool.toolWindow=true;tool.noActivate=true;
      require(!dg::captureOverlayCandidate(topology(),tool,L"Qt51514QWindowIcon"),"Small tool was accepted as a desktop capture surface");
      require(movedWithCaptureClassification(tool,L"Qt51514QWindowIcon",L"D:\\Daily\\Weixin\\Weixin.exe"),"Small foreign tool/no-activate window was exempted");
    });
    test("caption resizable non-popup and non-topmost masks remain ordinary windows",[] {
      const auto original=observedWeixinCaptureMask();
      for(unsigned variant=0;variant<4;++variant) {
        auto window=original;
        if(variant==0) window.caption=true;
        if(variant==1) window.resizable=true;
        if(variant==2) window.popup=false;
        if(variant==3) window.topmost=false;
        require(!dg::captureOverlayCandidate(topology(),window,L"Qt51514QWindowIcon"),"Incomplete popup/style identity was accepted");
        require(movedWithCaptureClassification(window,L"Qt51514QWindowIcon",L"D:\\Daily\\Weixin\\Weixin.exe"),"Ordinary fullscreen window was exempted");
      }
    });
    test("capture tolerance bounds each edge and total physical size",[] {
      auto mask=observedWeixinCaptureMask();const auto desktop=topology().virtualBounds;
      mask.frame={desktop.left+2,desktop.top+2,desktop.right+2,desktop.bottom+2};mask.outer=mask.frame;
      require(dg::captureOverlayCandidate(topology(),mask,L"Qt51514QWindowIcon"),"Two-pixel physical desktop translation was rejected");
      mask.frame={desktop.left,desktop.top+3,desktop.right,desktop.bottom+3};mask.outer=mask.frame;
      require(!dg::captureOverlayCandidate(topology(),mask,L"Qt51514QWindowIcon"),"Unbounded position mismatch was accepted");
      mask.frame={desktop.left-2,desktop.top,desktop.right+2,desktop.bottom};mask.outer=mask.frame;
      require(!dg::captureOverlayCandidate(topology(),mask,L"Qt51514QWindowIcon"),"Four-pixel size mismatch passed the edge tolerance");
      mask.frame={-1800,-600,3200,1400};mask.outer=mask.frame;
      require(movedWithCaptureClassification(mask,L"Qt51514QWindowIcon",L"D:\\Daily\\Weixin\\Weixin.exe"),"Arbitrary oversized Weixin window was exempted");
    });
    test("hidden child own and unsupported windows are never capture exceptions",[] {
      const auto original=observedWeixinCaptureMask();
      for(unsigned variant=0;variant<5;++variant) {
        auto window=original;
        if(variant==0) window.visible=false;
        if(variant==1) window.topLevel=false;
        if(variant==2) window.ownProcess=true;
        if(variant==3) window.cloaked=true;
        if(variant==4) window.minimized=true;
        require(!dg::captureOverlayCandidate(topology(),window,L"Qt51514QWindowIcon"),"Filtered window was accepted as a capture candidate");
      }
      auto unsupported=topology();unsupported.supported=false;
      require(!dg::captureOverlayCandidate(unsupported,original,L"Qt51514QWindowIcon"),"Unsupported topology produced a capture exception");
    });
    test("oversized windows remain outside target and event storms are throttled",[] {
      const auto t=topology();auto window=targetWindow();window.frame={-1800,-600,3200,1400};window.outer=window.frame;
      const auto large=dg::relocate(t,window);
      require(large.has_value() && !dg::intersects(large->frame,Target),"Oversized window would be moved across protected display");
      dg::MoveThrottle throttle;require(throttle.ready(42,1000),"Fresh window should be eligible");throttle.mark(42,1000);
      for(std::uint64_t time=1000;time<1500;++time) require(!throttle.ready(42,time),"500ms feedback suppression failed");
      require(throttle.ready(42,1500) && throttle.ready(43,1100),"Throttle blocked expiration or unrelated windows");
      throttle.mark(42,1500);require(!throttle.ready(42,1999) && throttle.ready(42,2000),"Repeated move did not reset 500ms suppression");
      for(std::uintptr_t id=100;id<228;++id) throttle.mark(id,1501);
      require(throttle.ready(42,1502),"Bounded 128-entry history did not evict old entries");
      throttle.clear();require(throttle.ready(100,1502),"Topology/suspend clear left stale move suppression");
    });
  } catch(const std::exception&) {return 1;}
  std::cout<<"DisplayGuardPolicy: "<<passed<<" tests passed; FakeOS only, no Win32 desktop calls.\n";
  return 0;
}
