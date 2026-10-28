// Reuse the isolated synthetic process image, never real game/native addresses.
#define main NotificationFixtureMain
#include "notification_tests.cpp"
#undef main
namespace R = AutoPlay::NativeRepeat;
namespace P = AutoPlay::StageRepeat;
namespace {
uintptr_t selector = 0, selection = 0, fullRecord = 0, stageRecord = 0, scene = 0;
bool __fastcall Player(void* u, int) { return *reinterpret_cast<int16_t*>(reinterpret_cast<uintptr_t>(u)+0x2F4) == 16; }
int __fastcall Faction(void* u, uint8_t) { return Player(u,0) ? 0 : 3; }
int __fastcall MapObject(void*) { return 0; }
void World() {
    Reset();
    R::controller = {}; R::verifiedClear = false; R::observedBattle = 0; R::lastBattleState = -1; R::reportedCycles = 0;
    selector = image+0x2500000; selection = image+0x2600000; fullRecord = image+0x2700000; scene=image+0x2800000;
    At<uintptr_t>(image+R::kSelectorPointer)=selector;
    stageRecord = image+R::kRecords+141*0x80;
    At<uintptr_t>(image+R::kSelectionManager)=selection;
    At<uintptr_t>(selection)=stageRecord; At<uintptr_t>(selection+8)=fullRecord; At<int16_t>(selection+0x10)=141;
    At<int>(stageRecord)=2101; At<int>(fullRecord+4)=2101; At<uint16_t>(stageRecord+4)=83;
    At<uint8_t>(image+R::kBattleMode)=1;
    At<uintptr_t>(image+NativeScenes::kManagerPointerRva)=scene;
    At<int>(scene+4)=5; At<int>(scene+8)=1;
    At<int>(task+0x14)=2; At<int>(task+0x2C)=1;
    At<int>(queue+0x200)=1;
    auto unit=GetUnitPtr(16); auto character=image+0x2900000;
    At<uint8_t>(unit+0x2E8)=1; At<int16_t>(unit+0x2EE)=-1; At<int16_t>(unit+0x2F4)=16;
    At<uintptr_t>(unit+0x240)=character; At<int64_t>(character+0x2CC8)=100;
    fnIsControllableUnit=&Player; fnGetUnitFaction=&Faction; fnIsMapObject=&MapObject;
}
bool BattleCases() {
    World(); P::Observation o; uint16_t clears=0; P::StageKey key;
    bool ok=Check(R::CurrentStage(image,key,&clears) && key.stage==2101 && clears==83,
        "native stage ID matches selected progress and full dungeon records");
    R::controller.Toggle();
    ok &= Check(R::ReadBattle(image,task,o) && o.normalStage && R::Tick(o).command==P::Command::None,
        "F2 binds current normal battle without sending combat input");
    At<int>(task+0x14)=0x2F;
    ok &= Check(R::ReadBattle(image,task,o) && o.screen==P::Screen::Battle &&
        R::Tick(o).command==P::Command::None && R::controller.enabled(), "temporary battle status state2F preserves replay state");
    At<int>(task+0x14)=0x20; At<uintptr_t>(task+N::kClearTaskOffset)=image+N::kClearTaskVtableRva;
    At<int>(task+N::kClearTaskOffset+0x14)=2; At<int>(task+N::kClearTaskOffset+0x2C)=1;
    ok &= Check(R::ReadBattle(image,task,o) && o.outcome==P::Outcome::Victory && o.ready &&
        R::Tick(o).command==P::Command::AdvanceResults,
        "clear context plus surviving party and zero enemies permits native result confirmation");
    World(); auto enemy=GetUnitPtr(0); auto data=image+0x2910000;
    At<uint8_t>(enemy+0x2E8)=1; At<int16_t>(enemy+0x2EE)=-1; At<int16_t>(enemy+0x2F4)=0;
    At<uintptr_t>(enemy+0x240)=data; At<int64_t>(data+0x2CC8)=100;
    At<int>(task+0x14)=0x20; At<uintptr_t>(task+N::kClearTaskOffset)=image+N::kClearTaskVtableRva;
    ok &= Check(R::ReadBattle(image,task,o) && o.outcome!=P::Outcome::Victory,
        "live enemy prevents inferred victory even if a clear state is present");
    World(); At<int64_t>(At<uintptr_t>(GetUnitPtr(16)+0x240)+0x2CC8)=0;
    At<int>(task+0x14)=0x20; At<uintptr_t>(task+N::kClearTaskOffset)=image+N::kClearTaskVtableRva;
    ok &= Check(R::ReadBattle(image,task,o) && o.outcome!=P::Outcome::Victory, "defeated party cannot authorize replay");
    World(); At<int>(fullRecord+4)=999;
    ok &= Check(!R::CurrentStage(image,key), "mismatched full dungeon record fails pre-confirmation identity checks");
    World(); At<uint8_t>(image+R::kVariantFlag)=1; At<uint16_t>(stageRecord+0x12)=7;
    ok &= Check(R::CurrentStage(image,key,&clears) && key.variant==1 && clears==7,
        "native variant bit selects matching identity and counter without changing configuration");
    World(); At<uint8_t>(image+R::kBattleMode)=2;
    ok &= Check(R::ReadBattle(image,task,o) && !o.normalStage, "unverified stage modes excluded");
    return ok;
}
bool SelectorCases() {
    World(); At<int>(scene+4)=9; At<uintptr_t>(selector)=image+R::kSelectorVtable;
    At<int>(selector+0x14)=5; At<int>(selector+0x2C)=1; At<int16_t>(selector+0x978)=2;
    At<int16_t>(selector+0x950)=142; At<int16_t>(selector+0x954)=141;
    At<int>(stageRecord+0x80)=2102;
    R::Selector o;
    bool ok=Check(R::ReadSelector(image,selector,{2101,0},o) && o.observation.stageAvailable &&
        o.targetRow==1 && o.observation.selected.stage==2102,
        "reordered stage rows resolve recorded native ID rather than cached cursor");
    ok &= Check(R::SelectStage(image,selector,{2101,0}) && At<int>(selector+0x1630)==1 &&
        R::ReadSelector(image,selector,{2101,0},o) && o.observation.selected.stage==2101,
        "accepted cursor update revalidates the same stage immediately before native confirmation");
    At<int16_t>(selector+0x950)=141;
    ok &= Check(R::ReadSelector(image,selector,{2101,0},o) && !o.observation.stageAvailable,
        "duplicate native identities fail closed");
    At<int16_t>(selector+0x950)=142; At<uint8_t>(selector+0x1646)=1;
    ok &= Check(R::ReadSelector(image,selector,{2101,0},o) && !o.observation.stageAvailable,
        "changed mode variant prevents stale stage confirmation");
    At<uint8_t>(selector+0x1646)=0; At<int>(queue+0x200)=0;
    ok &= Check(R::ReadSelector(image,selector,{2101,0},o) && o.observation.screen==P::Screen::Unknown && !o.observation.ready,
        "unexpected selector dialog blocks automatic start");
    At<int>(queue+0x200)=1; At<uint8_t>(selector+0x163E)=1;
    ok &= Check(R::ReadSelector(image,selector,{2101,0},o) && !o.observation.ready,
        "pending native confirmation cannot receive another press");
    return ok;
}
int nativePresses = 0, nativeUpdates = 0;
void __fastcall OriginalBattle(void* p, int callType) {
    if (callType != 4) return;
    ++nativeUpdates;
    if (At<uint8_t>(image+RVA::ADDR_KeyJustPressed+VK_SPACE)) {
        ++nativePresses;
        At<int>(reinterpret_cast<uintptr_t>(p)+N::kClearTaskOffset+8)=3;
        At<int>(reinterpret_cast<uintptr_t>(p)+N::kClearTaskOffset+0x2C)=2;
    }
}
void __fastcall OriginalSelector(void* p, int callType) {
    if (callType != 4) return;
    ++nativeUpdates;
    if (At<uint8_t>(image+RVA::ADDR_KeyJustPressed+VK_SPACE)) {
        ++nativePresses; At<int>(reinterpret_cast<uintptr_t>(p)+0x14)=6;
        At<int>(reinterpret_cast<uintptr_t>(p)+0x2C)=2;
    }
}
bool OwnershipCases() {
    bool ok=true;
    for(int f1=0;f1<2;++f1) for(int f2=0;f2<2;++f2) {
        World(); nativePresses=nativeUpdates=0;
        g_Enabled=f1!=0; g_RepeatRequested.store(f2!=0); g_CooldownTicks=0;
        orig_BattleExecute=&OriginalBattle;
        At<int>(task+0x14)=0x20; At<uintptr_t>(task+N::kClearTaskOffset)=image+N::kClearTaskVtableRva;
        At<int>(task+N::kClearTaskOffset+0x14)=2; At<int>(task+N::kClearTaskOffset+0x2C)=1;
        Hooked_BattleMainTask_Execute(reinterpret_cast<void*>(task),4);
        ok &= Check(nativeUpdates==1 && nativePresses==((f1||f2)?1:0) && g_Enabled==(f1!=0),
                    "full battle hook has one input owner for each F1/F2 combination and preserves F1");
        if(f2) ok &= Check(At<uint8_t>(image+RVA::ADDR_KeyJustPressed+VK_SPACE)==0 &&
            At<int>(image+RVA::ADDR_InputDeviceMode)==2, "F2 result input restores original key/device state");
    }
    // Use the actual selector hook, followed by an observed native state6.
    At<int>(scene+4)=9; At<uintptr_t>(selector)=image+R::kSelectorVtable;
    At<int>(selector+0x14)=5; At<int>(selector+0x2C)=1; At<int16_t>(selector+0x978)=1;
    At<int16_t>(selector+0x950)=141; orig_SelectorExecute=&OriginalSelector;
    nativePresses=nativeUpdates=0;
    Hooked_SelectorExecute(reinterpret_cast<void*>(selector),4);
    ok &= Check(nativePresses==1 && nativeUpdates==1 && At<int>(selector+0x14)==6,
                "actual selector hook confirms the recorded stage via native input");
    Hooked_SelectorExecute(reinterpret_cast<void*>(selector),4);
    ok &= Check(nativePresses==1 && R::controller.state()==P::State::WaitingForBattle,
                "state6 is observed as acceptance without duplicate confirmation");
    g_RepeatRequested.store(false);
    Hooked_SelectorExecute(reinterpret_cast<void*>(selector),4);
    ok &= Check(!R::controller.enabled() && nativePresses==1, "turning F2 off immediately prevents new selector input");
    return ok;
}
uint32_t __fastcall OriginalEntryChoice(void* p) {
    ++nativeUpdates;
    const uintptr_t object=reinterpret_cast<uintptr_t>(p);
    if (At<uint8_t>(image+RVA::ADDR_KeyJustPressed+VK_SPACE)) {
        ++nativePresses; At<int>(queue+0x408)=0; At<int>(object+0x600)=2;
    }
    return 0;
}
bool EntryChoiceCases() {
    World(); g_Enabled=false; g_RepeatRequested.store(true); R::controller.Toggle();
    P::Observation b; R::ReadBattle(image,task,b); R::Tick(b);
    At<int>(task+0x14)=0x20; At<uintptr_t>(task+N::kClearTaskOffset)=image+N::kClearTaskVtableRva;
    R::ReadBattle(image,task,b); R::Tick(b);
    At<int>(scene+4)=9; At<uintptr_t>(selector)=image+R::kSelectorVtable;
    At<int>(selector+0x14)=5; At<int>(selector+0x2C)=1; At<int16_t>(selector+0x978)=1;
    At<int16_t>(selector+0x950)=141;
    R::Selector s; R::ReadSelector(image,selector,{2101,0},s); R::Tick(s.observation);
    // Now the native Enter Stage prompt occupies this exact pending stage action.
    At<int>(queue+0x200)=0; At<int>(queue+0x408)=-1;
    At<uint8_t>(selector+0x163E)=1;
    At<uintptr_t>(notice)=image+R::kEntryChoiceVtable; At<int>(notice+0x0C)=513;
    At<int>(notice+0x18)=0;
    At<uintptr_t>(notice+0x10)=ui; At<int>(ui+0x10)=3;
    At<int>(notice+0x600)=1; At<int>(notice+0x62C)=0; At<uintptr_t>(notice+0x630)=queue+0x408;
    bool ok=Check(R::ReadSelector(image,selector,{2101,0},s) && s.observation.screen==P::Screen::StartMenu && s.observation.ready,
        "live entry choice flags513 and gate3 use actual layout kind and phase readiness");
    orig_EntryChoiceUpdate=&OriginalEntryChoice; nativePresses=nativeUpdates=0;
    g_EntryChoiceSentObject=0; g_EntryChoiceSentAttempt=0;
    Hooked_EntryChoiceUpdate(reinterpret_cast<void*>(notice));
    ok &= Check(nativePresses==1 && nativeUpdates==1 && At<int>(queue+0x408)==0 && !g_Enabled,
        "F2 alone confirms expected Yes through original native callback exactly once");
    Hooked_EntryChoiceUpdate(reinterpret_cast<void*>(notice));
    ok &= Check(nativePresses==1 && nativeUpdates==2, "native prompt closing cannot receive a duplicate confirm");
    At<int>(notice+0x600)=1; At<int>(notice+0x62C)=1; At<int>(queue+0x408)=-1;
    ok &= Check(R::ReadSelector(image,selector,{2101,0},s) && s.observation.screen==P::Screen::Unknown,
        "manual No selection is not overwritten by the repeat driver");
    At<int>(notice+0x62C)=0; At<uint8_t>(selector+0x163E)=0;
    ok &= Check(R::ReadSelector(image,selector,{2101,0},s) && s.observation.screen==P::Screen::Unknown,
        "unrelated choice of the same native class remains manual");
    At<int>(task+0x14)=0;
    ok &= Check(R::ReadBattle(image,task,b) && b.screen==P::Screen::Loading, "new task startup is loading, not an already-entered repeat battle");
    return ok;
}
}
int main() {
    if (tmpfile_s(&g_LogFile) || !g_LogFile) return 2;
    image=reinterpret_cast<uintptr_t>(VirtualAlloc(nullptr,kSize,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(!image)return 2;
    const bool ok=BattleCases() & SelectorCases() & OwnershipCases() & EntryChoiceCases();
    VirtualFree(reinterpret_cast<void*>(image),0,MEM_RELEASE); fclose(g_LogFile); g_LogFile=nullptr;
    return ok?0:1;
}
