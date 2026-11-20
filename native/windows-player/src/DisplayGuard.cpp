#include "DisplayGuard.h"
#include "DisplayGuardGeometry.h"
#include "MonitorSelection.h"
#include <dwmapi.h>
#include <atomic>
#include <climits>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace action75 {
namespace dg=display_guard;
namespace {
constexpr UINT WindowQueueMessage=WM_APP+51;
constexpr UINT ClipCheckMessage=WM_APP+52;
constexpr UINT_PTR CheckTimer=1;
dg::Rect fromWin32(const RECT& r) {return {r.left,r.top,r.right,r.bottom};}
RECT toWin32(const dg::Rect& r) {return {static_cast<LONG>(r.left),static_cast<LONG>(r.top),static_cast<LONG>(r.right),static_cast<LONG>(r.bottom)};}
dg::Rect virtualBounds() {
  const auto x=GetSystemMetrics(SM_XVIRTUALSCREEN),y=GetSystemMetrics(SM_YVIRTUALSCREEN);
  return {x,y,static_cast<std::int64_t>(x)+GetSystemMetrics(SM_CXVIRTUALSCREEN),static_cast<std::int64_t>(y)+GetSystemMetrics(SM_CYVIRTUALSCREEN)};
}
std::string winError(const char* operation,DWORD code=GetLastError()) {
  return std::string(operation)+" failed (Windows error "+std::to_string(code)+")";
}
bool captureOverlayOwner(DWORD pid,std::wstring_view className) noexcept {
  // Called only after the cheap class/style/full-desktop geometry checks.
  // No process inventory, input hooks, or periodic window scan is introduced.
  const HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
  if(!process) return false;
  wchar_t path[32768];DWORD length=static_cast<DWORD>(std::size(path));
  const bool queried=QueryFullProcessImageNameW(process,0,path,&length)!=FALSE;
  CloseHandle(process);
  return queried && dg::captureOverlayProcess(className,std::wstring_view(path,length));
}
bool sameTopology(const dg::Topology& a,const dg::Topology& b) {
  if(a.supported!=b.supported || a.targetPresent!=b.targetPresent || a.target!=b.target || a.allowed!=b.allowed || a.virtualBounds!=b.virtualBounds || a.allowedWorkAreas.size()!=b.allowedWorkAreas.size()) return false;
  for(std::size_t i=0;i<a.allowedWorkAreas.size();++i) if(a.allowedWorkAreas[i]!=b.allowedWorkAreas[i]) return false;
  return true;
}
}

struct DisplayGuard::Impl {
  struct Request {
    HWND wallpaper=nullptr;
    std::wstring target;
    DisplayIdentity identity;
    bool enabled=false,suspended=false;
    std::uint64_t generation=0;
  };
  struct Status {bool active=false;std::string mode="disabled",error;};
  mutable std::mutex mutex;
  Request request;
  Status status;
  std::thread worker;
  HANDLE wake=nullptr;
  bool stopping=false;
  std::atomic<bool> stopRequested{false};
  std::atomic<bool> exited{false};

  // Everything below this line is confined to the worker thread.
  Request appliedRequest;
  HWND eventWindow=nullptr;
  std::vector<HWINEVENTHOOK> hooks;
  dg::Topology topology;
  dg::ClipPolicy clip;
  dg::MoveThrottle moveThrottle;
  std::unordered_set<HWND> pendingWindows;
  std::unordered_set<HWND> deferredWindows;
  std::unordered_map<HWND,unsigned> moveAttempts;
  bool queuePosted=false,clipPosted=false,operating=false,releasePending=false;
  bool timerInstalled=false;
  std::string releaseMode="disabled",releaseError,releaseDiagnostic,windowError;
  inline static thread_local Impl* eventOwner=nullptr;

  void publish(bool active,std::string mode,std::string error={}) {
    std::lock_guard<std::mutex> lock(mutex);
    status.active=active;
    if(!stopRequested.load(std::memory_order_relaxed) && appliedRequest.generation!=request.generation) {
      status.mode="pending";status.error.clear();return;
    }
    status.mode=std::move(mode);status.error=std::move(error);
  }
  void setTimer(bool enabled) {
    if(enabled==timerInstalled || !eventWindow) return;
    if(enabled) timerInstalled=SetTimer(eventWindow,CheckTimer,1000,nullptr)!=0;
    else {KillTimer(eventWindow,CheckTimer);timerInstalled=false;}
  }
  bool currentRequest() const {
    std::lock_guard<std::mutex> lock(mutex);
    return !stopRequested.load(std::memory_order_relaxed) && appliedRequest.generation==request.generation && request.enabled && !request.suspended;
  }
  static LRESULT CALLBACK windowProc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp) {
    auto* self=reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
    if(message==WM_NCCREATE) {
      self=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
      SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
    }
    if(self) {
      switch(message) {
      case WM_DISPLAYCHANGE:
        self->applyRequest();return 0;
      case WM_SETTINGCHANGE:
        if(wp==SPI_SETWORKAREA) self->applyRequest();
        return 0;
      case WM_TIMER:
        if(wp==CheckTimer) {
          if(self->releasePending) self->finishRelease();
          else if(self->operating) {
            self->checkClip();
            // Only windows with outstanding events/moves are rechecked. Never
            // enumerate all desktop windows on a timer or per mouse movement.
            self->processPendingWindows();
            auto deferred=std::move(self->deferredWindows);self->deferredWindows.clear();
            for(const auto hwnd:deferred) self->processWindow(hwnd);
          }
        }
        return 0;
      case WindowQueueMessage:
        self->queuePosted=false;self->processPendingWindows();return 0;
      case ClipCheckMessage:
        self->clipPosted=false;
        if(self->operating) self->checkClip();
        return 0;
      }
    }
    return DefWindowProcW(hwnd,message,wp,lp);
  }
  static void CALLBACK eventProc(HWINEVENTHOOK,DWORD event,HWND hwnd,LONG object,LONG child,DWORD,DWORD) {
    auto* self=eventOwner;
    if(!self || !self->operating || !hwnd || self->stopRequested.load(std::memory_order_relaxed)) return;
    if((event==EVENT_OBJECT_SHOW || event==EVENT_OBJECT_LOCATIONCHANGE) && (object!=OBJID_WINDOW || child!=CHILDID_SELF)) return;
    if(event==EVENT_SYSTEM_FOREGROUND && !self->clipPosted) {
      self->clipPosted=PostMessageW(self->eventWindow,ClipCheckMessage,0,0)!=FALSE;
    }
    if(self->pendingWindows.size()<512) self->pendingWindows.insert(hwnd);
    if(!self->queuePosted && !self->pendingWindows.empty()) self->queuePosted=PostMessageW(self->eventWindow,WindowQueueMessage,0,0)!=FALSE;
  }
  bool installHooks() {
    if(!hooks.empty()) return true;
    constexpr DWORD events[]={EVENT_SYSTEM_FOREGROUND,EVENT_OBJECT_SHOW,EVENT_OBJECT_LOCATIONCHANGE,EVENT_SYSTEM_MOVESIZEEND};
    for(const auto event:events) {
      const auto hook=SetWinEventHook(event,event,nullptr,eventProc,0,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);
      if(!hook) {const auto code=GetLastError();removeHooks();releaseError=winError("SetWinEventHook",code);return false;}
      hooks.push_back(hook);
    }
    return true;
  }
  void removeHooks() noexcept {
    for(const auto hook:hooks) UnhookWinEvent(hook);
    hooks.clear();pendingWindows.clear();deferredWindows.clear();moveAttempts.clear();queuePosted=false;clipPosted=false;moveThrottle.clear();
  }
  bool releaseClip() {
    if(!clip.owned()) return true;
    RECT observed{};
    if(!GetClipCursor(&observed)) {releaseError=winError("GetClipCursor during release");return false;}
    // Always use the complete current virtual desktop, including on unsupported
    // primary/duplicate/disconnected target paths.
    const auto plan=clip.release(fromWin32(observed),virtualBounds());
    if(plan.action==dg::ClipAction::None) return true;
    RECT restore{};const RECT* restorePointer=nullptr;
    if(plan.restore) {restore=toWin32(*plan.restore);restorePointer=&restore;}
    if(!ClipCursor(restorePointer)) {releaseError=winError("ClipCursor restore");return false;}
    clip.released();return true;
  }
  void finishRelease() {
    if(!releaseClip()) {
      releasePending=true;setTimer(true);publish(clip.owned(),"error",releaseError);return;
    }
    releasePending=false;setTimer(false);
    publish(false,releaseMode,releaseDiagnostic);
  }
  void deactivate(std::string mode,std::string error={}) {
    operating=false;removeHooks();releaseMode=std::move(mode);releaseDiagnostic=std::move(error);releaseError.clear();windowError.clear();finishRelease();
  }
  void checkClip() {
    if(!operating || !currentRequest()) return;
    RECT observed{};
    if(!GetClipCursor(&observed)) {publish(clip.owned(),"error",winError("GetClipCursor"));return;}
    const auto plan=clip.check(topology,fromWin32(observed));
    if(plan.action==dg::ClipAction::Set) {
      if(!currentRequest()) return;
      const auto desired=toWin32(plan.desired);
      if(!ClipCursor(&desired)) {publish(clip.owned(),"error",winError("ClipCursor"));return;}
      clip.applied(plan);
    }
    publish(plan.active,windowError.empty() ? plan.mode : "error",windowError);
  }
  void applyRequest() {
    if(stopRequested.load(std::memory_order_relaxed)) return;
    Request next;
    {std::lock_guard<std::mutex> lock(mutex);next=request;}
    const auto previousTarget=appliedRequest.target;
    appliedRequest=next;
    if(!next.enabled) {deactivate("disabled");return;}
    if(next.suspended) {deactivate("suspended");return;}
    const auto displays=monitors();
    const auto* target=resolveMonitor(displays,next.identity);
    // The player may still be handling the same display-change notification.
    if(!target || target->device!=next.target) {
      deactivate("disconnected","键盘屏未连接或正在重新识别");return;
    }
    if(displays.size()>32) {deactivate("unsupported","Too many displays");return;}
    std::vector<dg::Monitor> geometry;
    for(const auto& display:displays)
      geometry.push_back({display.device,fromWin32(display.bounds),fromWin32(display.work),display.primary});
    auto nextTopology=dg::analyze(geometry,next.target);
    if(!nextTopology.supported) {
      topology=std::move(nextTopology);deactivate(topology.targetPresent ? "unsupported" : "disconnected",topology.reason);return;
    }
    const bool enumerate=!operating || next.target!=previousTarget || !sameTopology(topology,nextTopology);
    topology=std::move(nextTopology);releasePending=false;releaseError.clear();windowError.clear();
    if(!installHooks()) {deactivate("error",releaseError);return;}
    operating=true;setTimer(true);
    if(!timerInstalled) {deactivate("error",winError("SetTimer"));return;}
    checkClip();
    if(enumerate) {
      moveThrottle.clear();
      EnumWindows([](HWND hwnd,LPARAM parameter)->BOOL {
        auto* self=reinterpret_cast<Impl*>(parameter);
        if(self->stopRequested.load(std::memory_order_relaxed)) return FALSE;
        self->processWindow(hwnd);return TRUE;
      },reinterpret_cast<LPARAM>(this));
    }
  }
  void processPendingWindows() {
    auto windows=std::move(pendingWindows);pendingWindows.clear();
    for(const auto hwnd:windows) {
      if(!operating || stopRequested.load(std::memory_order_relaxed)) break;
      processWindow(hwnd);
    }
  }
  void processWindow(HWND hwnd) {
    if(!operating || !currentRequest() || !hwnd || hwnd==appliedRequest.wallpaper) return;
    const auto forget=[&] {moveAttempts.erase(hwnd);deferredWindows.erase(hwnd);};
    DWORD pid=0;
    if(!GetWindowThreadProcessId(hwnd,&pid) || !pid || pid==GetCurrentProcessId()) {forget();return;}
    if(!IsWindowVisible(hwnd) || IsIconic(hwnd) || GetAncestor(hwnd,GA_ROOT)!=hwnd) {forget();return;}
    if(hwnd==GetDesktopWindow() || hwnd==GetShellWindow()) {forget();return;}
    const auto style=GetWindowLongPtrW(hwnd,GWL_STYLE);
    if(style&WS_CHILD) {forget();return;}
    wchar_t className[128]{};
    if(!GetClassNameW(hwnd,className,static_cast<int>(std::size(className)))) {forget();return;}
    if(wcscmp(className,L"Progman")==0 || wcscmp(className,L"WorkerW")==0 || wcscmp(className,L"Shell_TrayWnd")==0 || wcscmp(className,L"Shell_SecondaryTrayWnd")==0) {forget();return;}
    DWORD cloaked=0;
    if(FAILED(DwmGetWindowAttribute(hwnd,DWMWA_CLOAKED,&cloaked,sizeof(cloaked))) || cloaked) {forget();return;}
    RECT outer{},frame{};
    if(!GetWindowRect(hwnd,&outer)) {forget();return;}
    if(FAILED(DwmGetWindowAttribute(hwnd,DWMWA_EXTENDED_FRAME_BOUNDS,&frame,sizeof(frame)))) {
      // An invisible maximized resize border may overlap the keyboard while
      // the actual visible window does not. Never move it using that border.
      if(IsZoomed(hwnd)) {forget();return;}
      frame=outer;
    }
    const auto extendedStyle=GetWindowLongPtrW(hwnd,GWL_EXSTYLE);
    dg::WindowTraits traits;traits.outer=fromWin32(outer);traits.frame=fromWin32(frame);
    traits.popup=(style&WS_POPUP)!=0;
    traits.caption=(style&WS_CAPTION)!=0;
    traits.resizable=(style&WS_THICKFRAME)!=0;
    traits.topmost=(extendedStyle&WS_EX_TOPMOST)!=0;
    traits.toolWindow=(extendedStyle&WS_EX_TOOLWINDOW)!=0;
    traits.noActivate=(extendedStyle&WS_EX_NOACTIVATE)!=0;
    if(dg::captureOverlayCandidate(topology,traits,className) && captureOverlayOwner(pid,className)) {
      forget();return;
    }
    const auto plan=dg::relocate(topology,traits);
    if(!plan) {moveAttempts.erase(hwnd);return;}
    if(plan->x<INT_MIN || plan->x>INT_MAX || plan->y<INT_MIN || plan->y>INT_MAX) return;
    const auto now=GetTickCount64();
    if(!moveThrottle.ready(reinterpret_cast<std::uintptr_t>(hwnd),now)) {
      if(deferredWindows.size()<128) deferredWindows.insert(hwnd);
      return;
    }
    DWORD currentPid=0;
    if(!GetWindowThreadProcessId(hwnd,&currentPid) || currentPid!=pid) return;
    if(!currentRequest()) return;
    if(moveAttempts.size()>=128 && !moveAttempts.count(hwnd)) {
      for(auto entry=moveAttempts.begin();entry!=moveAttempts.end();) {
        if(!IsWindow(entry->first) || !IsWindowVisible(entry->first) || IsIconic(entry->first)) entry=moveAttempts.erase(entry);
        else ++entry;
      }
      if(moveAttempts.size()>=128) {
        windowError="Too many windows are awaiting relocation from the keyboard display";
        publish(clip.owned(),"error",windowError);return;
      }
    }
    auto& attempts=moveAttempts[hwnd];
    if(attempts>=3) {
      windowError="A window did not accept relocation from the keyboard display";
      publish(clip.owned(),"error",windowError);return;
    }
    ++attempts;moveThrottle.mark(reinterpret_cast<std::uintptr_t>(hwnd),now);
    if(deferredWindows.size()<128) deferredWindows.insert(hwnd);
    // No foreign WndProc is waited on, including during stop()/WM_DESTROY.
    if(!SetWindowPos(hwnd,nullptr,static_cast<int>(plan->x),static_cast<int>(plan->y),0,0,
        SWP_ASYNCWINDOWPOS|SWP_NOACTIVATE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOOWNERZORDER)) {
      windowError=winError("SetWindowPos");publish(clip.owned(),"error",windowError);
    }
  }
  void run() noexcept {
    const auto oldDpi=SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    eventOwner=this;
    try {
      static std::once_flag registerFlag;
      static bool registered=false;
      std::call_once(registerFlag,[] {
        WNDCLASSEXW definition{};definition.cbSize=sizeof(definition);definition.lpfnWndProc=windowProc;
        definition.hInstance=GetModuleHandleW(nullptr);definition.lpszClassName=L"Action75DisplayGuardEvents";
        registered=RegisterClassExW(&definition)!=0 || GetLastError()==ERROR_CLASS_ALREADY_EXISTS;
      });
      if(!oldDpi || !registered) {
        {std::lock_guard<std::mutex> lock(mutex);appliedRequest=request;}
        publish(false,"error",winError(!oldDpi ? "SetThreadDpiAwarenessContext" : "RegisterClassExW"));
      } else {
        eventWindow=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"Action75DisplayGuardEvents",L"",WS_POPUP,
          0,0,0,0,nullptr,nullptr,GetModuleHandleW(nullptr),this);
        if(!eventWindow) {
          {std::lock_guard<std::mutex> lock(mutex);appliedRequest=request;}
          publish(false,"error",winError("CreateWindowExW"));
        } else {
          applyRequest();
          while(!stopRequested.load(std::memory_order_relaxed)) {
            const auto result=MsgWaitForMultipleObjectsEx(1,&wake,INFINITE,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
            if(stopRequested.load(std::memory_order_relaxed)) break;
            if(result==WAIT_OBJECT_0) applyRequest();
            else if(result==WAIT_FAILED) {deactivate("error",winError("MsgWaitForMultipleObjectsEx"));break;}
            MSG message{};
            for(unsigned count=0;count<64 && !stopRequested.load(std::memory_order_relaxed) && PeekMessageW(&message,nullptr,0,0,PM_REMOVE);++count) {
              TranslateMessage(&message);DispatchMessageW(&message);
            }
          }
        }
      }
    } catch(const std::exception& exception) {
      {std::lock_guard<std::mutex> lock(mutex);appliedRequest=request;}
      deactivate("error",std::string("Display guard worker: ")+exception.what());
    } catch(...) {
      {std::lock_guard<std::mutex> lock(mutex);appliedRequest=request;}
      deactivate("error","Display guard worker failed");
    }
    operating=false;removeHooks();
    if(eventWindow) {
      if(stopRequested.load(std::memory_order_relaxed)) {releaseMode="disabled";releaseError.clear();releaseDiagnostic.clear();}
      // An OS restore failure must not become a false inactive/suspended ACK.
      // Retry briefly during shutdown, preserving the error if the OS refuses.
      for(unsigned attempt=0;attempt<5;++attempt) {
        finishRelease();
        if(!releasePending) break;
        Sleep(10);
      }
      setTimer(false);DestroyWindow(eventWindow);eventWindow=nullptr;
    }
    eventOwner=nullptr;
    if(oldDpi) SetThreadDpiAwarenessContext(oldDpi);
    exited.store(true,std::memory_order_release);
  }
  void configure(HWND wallpaper,const std::wstring& target,const DisplayIdentity& identity,
                 bool enabled,bool suspended) {
    std::lock_guard<std::mutex> lock(mutex);
    request={wallpaper,target,identity,enabled,suspended,request.generation+1};
    if(stopping) {status.mode="error";status.error="Display guard is stopping";return;}
    if(worker.joinable() && exited.load(std::memory_order_acquire)) {
      // Exited is set after all worker publications and desktop cleanup, so
      // this join cannot wait on the state mutex or a foreign window thread.
      worker.join();
      if(wake) {CloseHandle(wake);wake=nullptr;}
    }
    if(!worker.joinable() && (!enabled || suspended) && !clip.owned()) {
      status={false,enabled ? "suspended" : "disabled",{}};return;
    }
    status.mode="pending";status.error.clear();
    if(!worker.joinable()) {
      stopRequested.store(false,std::memory_order_relaxed);
      exited.store(false,std::memory_order_relaxed);
      wake=CreateEventW(nullptr,FALSE,FALSE,nullptr);
      if(!wake) {status={false,"error",winError("CreateEventW")};return;}
      try {worker=std::thread([this] {run();});}
      catch(const std::exception& exception) {CloseHandle(wake);wake=nullptr;status={false,"error",exception.what()};return;}
    }
    SetEvent(wake);
  }
  void stop() noexcept {
    std::thread joining;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if(stopping || !worker.joinable()) return;
      stopping=true;stopRequested.store(true,std::memory_order_relaxed);
      SetEvent(wake);joining=std::move(worker);
    }
    if(joining.joinable()) joining.join();
    {
      std::lock_guard<std::mutex> lock(mutex);
      if(wake) {CloseHandle(wake);wake=nullptr;}
      stopping=false;
    }
  }
};

DisplayGuard::DisplayGuard():impl_(std::make_unique<Impl>()) {}
DisplayGuard::~DisplayGuard() {stop();}
void DisplayGuard::configure(HWND wallpaper,const std::wstring& targetDevice,const DisplayIdentity& identity,
                             bool enabled,bool suspended) {
  impl_->configure(wallpaper,targetDevice,identity,enabled,suspended);
}
void DisplayGuard::stop() noexcept {impl_->stop();}
bool DisplayGuard::active() const {std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->status.active;}
std::string DisplayGuard::mode() const {std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->status.mode;}
std::string DisplayGuard::error() const {std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->status.error;}
}
