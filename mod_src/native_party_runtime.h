#pragma once
#include "native_party_menu.h"
#include "native_repeat.h"
#include <atomic>

namespace AutoPlay::PartyRuntime {
namespace N=NativeNotifications;
inline Party::BattleParty battle;
inline Party::PartySettings configured;
inline Party::PausableClock clock;
inline std::atomic<bool> toggleRequested{false};
inline std::atomic<WORD> physicalPad{0};
inline std::atomic<bool> releasing{false};
inline uint64_t scopeRevision=0, appliedScope=0;
inline bool loadCaptured=false, wasTitle=false;
inline uintptr_t loadTask=0;
inline std::string identityError;
inline uint64_t identityErrorSave=0;
inline void (*invalidatePlanning)()=nullptr;
inline void (*pollKeys)()=nullptr;
inline FnBattleExecute originalLoad=nullptr;
inline NativeScenes::Update originalHub=nullptr, originalTitle=nullptr;
inline void TryInstallSceneHooks();
constexpr uintptr_t kLoadVtable=0x98E650, kLoadExecute=0x74EE60;
constexpr uintptr_t kSaveDirectory=0x7B58F20;
constexpr uintptr_t kHubUpdate=0x6A6540, kTitleUpdate=0x765630;

inline uint64_t Now() { return clock.Now(GetTickCount64()); }
inline bool OwnsInput() { return NativePartyMenu::object || releasing.load(); }
inline bool IsScopePending() {
    const auto state=PartyIO::service.Read();
    return scopeRevision && state.scopeRevision!=scopeRevision;
}
inline void RefreshSettings() {
    const auto state=PartyIO::service.Read();
    if(state.scopeRevision==scopeRevision && state.scopeReady && state.ready) {
        configured=state.settings;appliedScope=scopeRevision;
        if(!identityError.empty() && state.completedSave!=identityErrorSave && state.saveSucceeded) identityError.clear();
        if(!identityError.empty()) {configured.enabled=false;configured.members.clear();}
    }
}
inline Party::DeploymentPolicy Policy() {
    return battle.inCombat?battle.Policy():Party::DeploymentPolicy::From(configured);
}
inline void ObserveBattle(uintptr_t base, uintptr_t task) {
    int state=-1;
    const auto phase=N::BattlePhase(base,task,&state);
    if(state==0x32 || state==0x20 || state==0x22 || state==0x26) battle.Finished();
    if(phase==N::Phase::Combat && !battle.inCombat && !IsScopePending()) {
        RefreshSettings();
        if(configured.enabled) {
            std::vector<Party::RosterEntry> roster;
            if(!NativePartyMenu::ReadRoster(base,roster)) {
                identityError="Custom party OFF: ambiguous roster identity. Reselect in F3.";
                configured.enabled=false;configured.members.clear();
                identityErrorSave=PartyIO::service.Save(configured);
                LogMsg("[Party] %s",identityError.c_str());
            }
        }
        battle.Started(configured);
        LogMsg("[Party] Battle policy custom=%d target=%d selected=%zu",battle.active.enabled,battle.Policy().limit,battle.active.members.size());
    }
}
inline bool PhysicalClosingKeysHeld(uintptr_t base) {
    if(GetAsyncKeyState(VK_F3)&0x8000) return true;
    if(physicalPad.load() & (0x1000|0x2000|XBTN_BACK|0x0040)) return true;
    uintptr_t config=0;
    if(!N::Read(base+RVA::PTR_KeyConfig,config) || !config) return false;
    for(int action:{ACT_CONFIRM,ACT_CANCEL}) for(size_t table:{size_t(0x50),size_t(0x420)}) {
        uint64_t key=0;
        if(N::Read(config+table+static_cast<size_t>(action)*8,key) && key>0 && key<256 &&
            (GetAsyncKeyState(static_cast<int>(key))&0x8000)) return true;
    }
    return false;
}
inline bool StillQueued(uintptr_t base,uintptr_t sought) {
    uintptr_t manager=0;int head=0,tail=0;
    if(!N::Read(base+N::kQueuePointerRva,manager) || !manager ||
        !N::Read(manager+0x200,head) || !N::Read(manager+0x204,tail) ||
        head<0 || head>=64 || tail<0 || tail>=64) return false;
    for(int i=head;i!=tail;i=(i+1)%64) {uintptr_t entry=0;if(!N::Read(manager+static_cast<uintptr_t>(i)*8,entry))return true;if(entry==sought)return true;}
    return false;
}
inline void CheckClosed(uintptr_t base) {
    if(NativePartyMenu::object && !StillQueued(base,NativePartyMenu::object)) {
        NativePartyMenu::object=0;NativePartyMenu::closing=false;NativePartyMenu::savingRevision=0;
        releasing.store(true);RefreshSettings();
    }
    if(releasing.load() && !PhysicalClosingKeysHeld(base)) {
        releasing.store(false);clock.Resume(GetTickCount64());
        if(invalidatePlanning) invalidatePlanning();
        LogMsg("[Party] Editor closed; automation input and timeout clocks resumed");
    }
}
inline bool Pump(uintptr_t base,bool safe) {
    CheckClosed(base);RefreshSettings();
    if(toggleRequested.exchange(false)) {
        if(NativePartyMenu::object && !NativePartyMenu::savingRevision) {NativePartyMenu::editor.Cancel();NativePartyMenu::Close();}
        else if(!OwnsInput() && safe && !IsScopePending()) {
            auto settings=PartyIO::service.Read();
            if(!identityError.empty()) {settings.settings=configured;settings.error=identityError;}
            if(NativePartyMenu::Open(base,settings,battle.inCombat)) {
                clock.Pause(GetTickCount64());
                if(invalidatePlanning) invalidatePlanning();
            } else {
                LogMsg("[Party] Menu unavailable: roster, settings, or modal is not ready");
                if(fnPlaySoundEffect)fnPlaySoundEffect(2,0,1.0f,1.0f,0);
            }
        } else {LogMsg("[Party] Menu request ignored during animation, confirmation, or transition");if(fnPlaySoundEffect)fnPlaySoundEffect(2,0,1.0f,1.0f,0);}
    }
    return OwnsInput();
}

// Preserve actual key state around the original scene update while consuming
// the closing input. The menu itself uses normal configured native controls.
class ScopedReleaseInput {
    uintptr_t base_=0;
    std::array<std::array<uint8_t,256>,3> saved_{};
public:
    explicit ScopedReleaseInput(uintptr_t base) {
        if(!releasing.load())return;
        const uintptr_t arrays[]={RVA::ADDR_KeyJustPressed,RVA::ADDR_KeyHeld,RVA::ADDR_KeyRepeat};
        for(int i=0;i<3;++i)if(!NativeGeometry::IsReadableRange(base+arrays[i],256))return;
        base_=base;
        for(int i=0;i<3;++i){memcpy(saved_[i].data(),reinterpret_cast<void*>(base+arrays[i]),256);memset(reinterpret_cast<void*>(base+arrays[i]),0,256);}
    }
    ~ScopedReleaseInput(){if(base_){const uintptr_t arrays[]={RVA::ADDR_KeyJustPressed,RVA::ADDR_KeyHeld,RVA::ADDR_KeyRepeat};for(int i=0;i<3;++i)memcpy(reinterpret_cast<void*>(base_+arrays[i]),saved_[i].data(),256);}}
};

inline void ObserveLoaded(uintptr_t base,uintptr_t task) {
    uintptr_t type=0;int state=-1,status=-1,slot=-1,error=-1;
    if(!N::Read(task,type)||type!=base+kLoadVtable||!N::Read(task+0x14,state))return;
    if(task!=loadTask || state<2){loadTask=task;loadCaptured=false;}
    // 74DAF0 copies the decompressed state into4FCCB00 only when error==0,
    // then emits status2. Browsing/canceling a load menu never satisfies this.
    if(loadCaptured || state!=2 || !N::Read(task+0x18,status)||status!=2 ||
        !N::Read(task+0x8A75D0,error)||error || !N::Read(task+0xBCF0,slot)||slot<0||slot>=100)return;
    char directory[1024]{};
    if(!NativeGeometry::IsReadableRange(base+kSaveDirectory,sizeof(directory)))return;
    memcpy(directory,reinterpret_cast<const void*>(base+kSaveDirectory),sizeof(directory));
    const auto end=static_cast<const char*>(memchr(directory,0,sizeof(directory)));
    if(!end || end==directory)return;
    const int length=MultiByteToWideChar(CP_ACP,0,directory,static_cast<int>(end-directory),nullptr,0);
    if(length<1||length>1800)return;
    std::wstring path(static_cast<size_t>(length),L'\0');
    MultiByteToWideChar(CP_ACP,0,directory,static_cast<int>(end-directory),path.data(),length);
    wchar_t name[40]{};
    if(slot==0)wcscpy_s(name,L"/AutoSave.sav");else swprintf_s(name,L"/Save_%03d.sav",slot);
    path+=name;loadCaptured=true;battle.Finished();configured={};identityError.clear();
    PartyIO::service.RequestScope(std::move(path),++scopeRevision);
    LogMsg("[Party] Observed successful native save load, slot=%d scopeRevision=%llu",slot,scopeRevision);
}
inline void __fastcall HookedLoad(void* task,int callType) {
    originalLoad(task,callType);
    if(callType==4 && task) {
        TryInstallSceneHooks();
        ObserveLoaded(g_Base,reinterpret_cast<uintptr_t>(task));
    }
}
inline uint64_t __fastcall HookedHub() {
    if(pollKeys)pollKeys();
    wasTitle=false;battle.Finished();
    NativeScenes::Registry registry;
    uintptr_t task=0,type=0;int state=-1,transition=-1;
    const bool safe=NativeScenes::ReadRegistry(g_Base,registry) && NativeScenes::Settled(registry,13) &&
        N::Read(g_Base+0x7B58EC0,task) && task && N::Read(task,type) && type==g_Base+0x98D298 &&
        N::Read(task+0x14,state) && state==0 && N::Read(task+0x2C,transition) && transition==1;
    Pump(g_Base,safe);
    ScopedReleaseInput blocked(g_Base);
    return originalHub();
}
inline uint64_t __fastcall HookedTitle() {
    if(pollKeys)pollKeys();
    if(!wasTitle){wasTitle=true;battle.Finished();configured={};identityError.clear();PartyIO::service.RequestScope({},++scopeRevision);loadCaptured=false;}
    Pump(g_Base,false);
    ScopedReleaseInput blocked(g_Base);
    return originalTitle();
}
inline void TryInstallSceneHooks() {
    if(!originalHub && NativeScenes::InstallUpdate(g_Base,13,kHubUpdate,&HookedHub,originalHub))LogMsg("[Party] Hooked native hub scene13");
    if(!originalTitle && NativeScenes::InstallUpdate(g_Base,3,kTitleUpdate,&HookedTitle,originalTitle))LogMsg("[Party] Hooked native title/save reset scene3");
}
inline void Initialize(HMODULE module) {
    if(!NativePartyMenu::Initialize(g_Base)){LogMsg("[Party] Native menu contract mismatch; party menu disabled");return;}
    PartyIO::service.Start(module);
    NativeRepeat::readClock=&Now;
    uintptr_t* slot=reinterpret_cast<uintptr_t*>(g_Base+kLoadVtable+9*8);
    if(*slot==g_Base+kLoadExecute) {
        DWORD protection=0;
        if(VirtualProtect(slot,sizeof(*slot),PAGE_READWRITE,&protection)) {
            originalLoad=reinterpret_cast<FnBattleExecute>(*slot);*slot=reinterpret_cast<uintptr_t>(&HookedLoad);
            DWORD ignored=0;VirtualProtect(slot,sizeof(*slot),protection,&ignored);
            LogMsg("[Party] Hooked verified native load-completion observer");
        }
    }
    TryInstallSceneHooks();
}
} // namespace AutoPlay::PartyRuntime
