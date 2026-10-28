#pragma once
#include "native_notifications.h"
#include "native_deployment.h"
#include "party_io.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <set>

namespace AutoPlay::NativePartyMenu {
namespace N = NativeNotifications;
constexpr uintptr_t kTextInitRva = 0x0988E0;
constexpr uintptr_t kCreateRva = 0x2B3F50;
constexpr uintptr_t kActionRva = 0x2258B0;
constexpr size_t kTextRecordSize = 0x2E8;
constexpr int kLines = 16;
using InitText = void(__fastcall*)(void*,const char*,const void*,uintptr_t);
using Create = void(__fastcall*)(int,int,int,int,int,float,uintptr_t,const void*,const void*,int16_t);
using ReadAction = int(__fastcall*)(uint32_t,int,uint32_t);

inline uintptr_t object = 0;
inline bool closing = false;
inline uint64_t savingRevision = 0;
inline bool available = false;
inline InitText initText = nullptr;
inline Create create = nullptr;
inline ReadAction readAction = nullptr;
inline Party::Editor editor;
inline std::array<std::string,kLines> rendered;

inline std::string Bounded(std::string text, size_t bytes = 120) {
    if (text.size() <= bytes) return text;
    while (bytes && (static_cast<unsigned char>(text[bytes]) & 0xC0) == 0x80) --bytes;
    text.resize(bytes); return text;
}
inline bool ReadRoster(uintptr_t base, std::vector<Party::RosterEntry>& entries) {
    entries.clear(); int16_t count=0;
    if(!base || !N::Read(base+NativeDeployment::kRosterTotalCountRva,count) || count<1 || count>128 ||
        !NativeGeometry::IsReadableRange(base+NativeDeployment::kRosterBaseRva,static_cast<size_t>(count)*NativeDeployment::kRosterStride)) return false;
    std::set<uintptr_t> deployed;
    const uintptr_t units=base+RVA::ADDR_BattleUnitArray;
    if(!NativeGeometry::IsReadableRange(units,256*0x1260)) return false;
    for(int i=0;i<256;++i) {
        const uintptr_t u=units+static_cast<uintptr_t>(i)*0x1260;
        if(*reinterpret_cast<const uint8_t*>(u+0x2E8)) deployed.insert(*reinterpret_cast<const uintptr_t*>(u+0x240));
    }
    entries.reserve(count);
    for(int i=0;i<count;++i) {
        const uintptr_t c=base+NativeDeployment::kRosterBaseRva+static_cast<uintptr_t>(i)*NativeDeployment::kRosterStride;
        if(!*reinterpret_cast<const uint32_t*>(c)) return false;
        Party::RosterEntry entry;
        entry.key=*reinterpret_cast<const uint64_t*>(c+0x3060);
        const char* name=reinterpret_cast<const char*>(c+0xBC8);
        const auto end=static_cast<const char*>(memchr(name,0,0x34));
        if(!end) return false;
        entry.name.assign(name,end);entry.ordinal=i;
        entry.level=*reinterpret_cast<const uint16_t*>(c+0x2DFC);
        entry.alive=*reinterpret_cast<const int64_t*>(c+0x2CC8)>0;
        entry.deployed=deployed.count(c)!=0;
        entries.push_back(std::move(entry));
    }
    return Party::UniqueRoster(entries);
}
inline const Party::RosterEntry* Find(Party::CharacterKey key) {
    const auto r=std::find_if(editor.roster.begin(),editor.roster.end(),[key](const auto& item){return item.key==key;});
    return r==editor.roster.end()?nullptr:&*r;
}
inline std::array<std::string,kLines> Lines() {
    std::array<std::string,kLines> lines;
    lines[0]="AUTOPLAY PARTY";
    lines[1]=std::string("Custom party: ")+(editor.draft.enabled?"ON":"OFF")+"    Selected: "+std::to_string(editor.draft.members.size())+"/10";
    const auto page=editor.page;
    lines[2]=page==Party::Page::Options?"<  OPTIONS  >":page==Party::Page::Roster?"<  CHOOSE HEROES  >":page==Party::Page::Selected?"<  DEPLOYMENT PRIORITY  >":"HERO ACTIONS";
    const size_t first=(editor.cursor/10)*10;
    for(size_t row=0;row<10;++row) {
        const size_t index=first+row;
        if(index>=editor.RowCount()) break;
        std::string text=index==editor.cursor?">  ":"    ";
        if(page==Party::Page::Options) {
            const char* options[]={"Custom party ON / OFF","Choose heroes","Change deployment order","Apply and save","Cancel"};
            text+=options[index];
        } else if(page==Party::Page::SelectedActions) {
            const char* actions[]={"Move Up","Move Down","Remove from party","Back"}; text+=actions[index];
        } else {
            const Party::RosterEntry* entry=nullptr;
            if(page==Party::Page::Roster) {
                entry=&editor.roster[index];
                const auto selected=std::find(editor.draft.members.begin(),editor.draft.members.end(),entry->key);
                text+=selected==editor.draft.members.end()?"[  ] ":"["+std::to_string(selected-editor.draft.members.begin()+1)+"] ";
            } else { entry=Find(editor.draft.members[index]);text+=std::to_string(index+1)+". "; }
            if(entry) text+=Bounded(entry->name,52)+"  Lv"+std::to_string(entry->level)+(entry->alive?(entry->deployed?"  On map":""):"  Fainted");
            else text+="Unavailable saved hero (replacement allowed)";
        }
        lines[3+row]=Bounded(text);
    }
    if(savingRevision) lines[13]="Saving party settings...";
    else if(!editor.message.empty()) lines[13]=Bounded(editor.message);
    else lines[13]=editor.appliesNextBattle?"Changes apply NEXT BATTLE. Current heroes stay deployed.":"Changes apply when the upcoming battle starts.";
    lines[14]="Left/Right: page   Up/Down: navigate   Confirm: select";
    lines[15]="Cancel: back   F3: discard/close   Replacements: ON";
    return lines;
}
inline void Refresh(uintptr_t base) {
    const auto lines=Lines();
    const auto init=initText;
    for(int i=0;i<kLines;++i) if(lines[i]!=rendered[i]) {
        init(reinterpret_cast<void*>(object+0x20+static_cast<size_t>(i)*kTextRecordSize),lines[i].c_str(),nullptr,0);
        rendered[i]=lines[i];
    }
}
inline bool Open(uintptr_t base, const PartyIO::Snapshot& settings, bool inCombat) {
    uintptr_t foreground=0;
    if(!available || !initText || !create || !readAction || object || !settings.ready || !N::Foreground(base,foreground) || foreground) return false;
    std::vector<Party::RosterEntry> entries;
    if(!ReadRoster(base,entries)) return false;
    editor.Open(settings.settings,std::move(entries),inCombat);
    if(!settings.error.empty()) editor.message=settings.error;
    rendered=Lines();
    alignas(8) std::array<uint8_t,kTextRecordSize*kLines> records{};
    const auto init=initText;
    for(int i=0;i<kLines;++i) init(records.data()+static_cast<size_t>(i)*kTextRecordSize,rendered[i].c_str(),nullptr,0);
    // Verified native notice factory; input0 hides its OK prompt. The native
    // manager owns initialization, rendering, exit animation and destruction.
    create(1,0,-1,0,0x37,1.0f,0,
        reinterpret_cast<const void*>(base+0xA17C34),records.data(),kLines);
    if(!N::Foreground(base,foreground) || !foreground) return false;
    uintptr_t type=0;
    if(!N::Read(foreground,type) || type!=base+N::kVtableRva) return false;
    object=foreground;closing=false;savingRevision=0;
    LogMsg("[Party] Opened native party editor, roster=%zu custom=%d selected=%zu nextBattle=%d",
        editor.roster.size(),editor.draft.enabled,editor.draft.members.size(),inCombat);
    return true;
}
inline bool Ready(uintptr_t base) {
    uintptr_t child=0,animation=0; int frame=0; float scale=0,ready=0;
    return object && N::Read(object+0x2EB0,child) && child && N::Read(child+0x60,animation) && animation &&
        N::Read(animation+0x28,frame) && N::Read(base+N::kAnimationScaleRva,scale) &&
        N::Read(base+N::kAnimationReadyRva,ready) && scale>0 && ready>0 && static_cast<float>(frame)*scale==ready;
}
inline void Close() {
    if(!object || closing) return;
    int frame=0;
    if(N::Read(object+0x2EA8,frame)) {
        // The next native update increments age, then starts the six-frame exit.
        *reinterpret_cast<int32_t*>(object+0x2EAC)=frame+7;
        closing=true;
    }
}
inline uint64_t Update(uintptr_t base, N::Update original) {
    if(!object || !original) return 0;
    if(!closing && Ready(base)) {
        if(savingRevision) {
            const auto snapshot=PartyIO::service.Read();
            if(snapshot.completedSave==savingRevision) {
                savingRevision=0;
                if(snapshot.saveSucceeded) { LogMsg("[Party] Applied custom=%d members=%zu, nextBattle=%d",editor.draft.enabled,editor.draft.members.size(),editor.appliesNextBattle); Close(); }
                else {editor.result=Party::EditorResult::None;editor.message=snapshot.error;}
            }
        } else {
            const auto action=readAction;
            if(action(ACT_CANCEL,1,0)==2) editor.Back();
            else if(action(ACT_UP,1,0)>0) editor.Move(-1);
            else if(action(ACT_DOWN,1,0)>0) editor.Move(1);
            else if(action(ACT_LEFT,1,0)==2) editor.SwitchPage(-1);
            else if(action(ACT_RIGHT,1,0)==2) editor.SwitchPage(1);
            else if(action(ACT_CONFIRM,1,0)==2) editor.Confirm();
            if(editor.result==Party::EditorResult::Canceled) Close();
            else if(editor.result==Party::EditorResult::Applied) {
                savingRevision=PartyIO::service.Save(editor.draft);
                if(!savingRevision) {editor.result=Party::EditorResult::None;editor.message="Settings worker is unavailable";}
            }
        }
        if(!closing) Refresh(base);
    }
    return original(reinterpret_cast<void*>(object));
}
inline bool Initialize(uintptr_t base) {
    const uint8_t first[]={0x48,0x89,0x5c,0x24,0x08,0x48,0x89};
    for(uintptr_t rva:{kTextInitRva,kCreateRva,kActionRva}) {
        if(!NativeGeometry::IsReadableRange(base+rva,sizeof(first)) ||
            memcmp(reinterpret_cast<const void*>(base+rva),first,sizeof(first)))return false;
    }
    initText=reinterpret_cast<InitText>(base+kTextInitRva);
    create=reinterpret_cast<Create>(base+kCreateRva);
    readAction=reinterpret_cast<ReadAction>(base+kActionRva);
    available=true;return true;
}
} // namespace AutoPlay::NativePartyMenu
