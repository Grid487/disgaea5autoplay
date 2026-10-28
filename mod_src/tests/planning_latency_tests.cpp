#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
uint64_t queryCount=0, queryNow=0, heightReads=0;
SIZE_T WINAPI CountedQuery(LPCVOID,PMEMORY_BASIC_INFORMATION,SIZE_T);
#define VirtualQuery CountedQuery
#define main OriginalGoalFixtureMain
#include "native_action_positions_tests.cpp"
#undef main
#undef VirtualQuery
#include "reference_v541_action_positions.h"
namespace R=AutoPlay::ReferencePositions;
SIZE_T WINAPI CountedQuery(LPCVOID p,PMEMORY_BASIC_INFORMATION info,SIZE_T n) {
    ++queryCount;
    if (AutoPlay::Planning::active) queryNow+=250000; // observed costly OS queries
    return ::VirtualQuery(p,info,n);
}
uint64_t QueryClock(){return queryNow;}
bool RefFootprint(int i,std::vector<R::FootprintCell>& out){
    out.clear(); for(const auto& c:g_Footprints[i])out.push_back({c.x,c.z});
    if(out.empty())out.push_back({0,0}); return true;
}
bool CountHeight(int x,int z,int16_t& h){++heightReads;queryNow+=500;return StubHeight(x,z,h);}
bool CacheSafety(){
    using namespace AutoPlay;
    auto* block=static_cast<uint8_t*>(VirtualAlloc(nullptr,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(!block)return false; bool ok=true; DWORD protection=0;
    {
        Planning::PlanningBudget b(QueryClock);Planning::Scope scope(b);
        const auto before=queryCount;
        for(int i=0;i<1000;++i)ok &= NativeGeometry::IsReadableRange(reinterpret_cast<uintptr_t>(block)+i,1,true);
        ok &= Expect(queryCount==before+1,"immutable asset range needs one OS query per update, not one per byte read");
        VirtualProtect(block,4096,PAGE_NOACCESS,&protection);
        ok &= Expect(!NativeGeometry::IsReadableRange(reinterpret_cast<uintptr_t>(block),1),
                     "asset cache never authorizes ordinary heap-object access after protection changes");
    }
    {
        Planning::PlanningBudget b(QueryClock);Planning::Scope scope(b);
        ok &= Expect(!NativeGeometry::IsReadableRange(reinterpret_cast<uintptr_t>(block),1,true),
                     "a new update revalidates asset protection and rejects stale readability");
    }
    VirtualProtect(block,4096,PAGE_READWRITE,&protection);
    VirtualProtect(block+4096,4096,PAGE_NOACCESS,&protection);
    {
        Planning::PlanningBudget b(QueryClock);Planning::Scope scope(b);
        ok &= NativeGeometry::IsReadableRange(reinterpret_cast<uintptr_t>(block),1,true);
        ok &= Expect(!NativeGeometry::IsReadableRange(reinterpret_cast<uintptr_t>(block)+4094,4,true),
                     "cached range cannot authorize access across a guard boundary");
        const auto image=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const auto before=queryCount;
        for(int i=0;i<1000;++i)ok &= NativeGeometry::IsReadableRange(image,4);
        ok &= Expect(queryCount==before+1,"native image bounds are checked once within the planning update");
        ok &= !NativeGeometry::IsReadableRange(UINTPTR_MAX-1,8);
    }
    VirtualFree(block,0,MEM_RELEASE);return ok;
}
template<class Job> bool Finish(Job& j,size_t batch,int& frames){
    frames=0;AutoPlay::Planning::Telemetry timing;
    while(!j.IsComplete() && frames++<200000){
        AutoPlay::Planning::PlanningBudget b(QueryClock);AutoPlay::Planning::Scope scope(b);
        AutoPlay::Planning::Run("offline-latency",timing,[&]{return j.IsComplete();},[&]{j.Step(batch);});
        if(b.used.work>8192 || b.used.previews>8 || b.used.effects>32 || b.used.paths>4)return false;
    }
    return j.Succeeded();
}
bool GoalParity(){
    using namespace AutoPlay;
    InstallReaders();g_HeightReaderOverride=&CountHeight;
    R::g_UnitFootprintReaderOverride=&RefFootprint;R::g_BasicRangeReaderOverride=&StubBasicRange;
    R::g_BasicReachReaderOverride=&StubBasicReach;R::g_LineWeaponReaderOverride=&StubLineWeapon;
    R::g_BasicVerticalReaderOverride=&StubBasicVertical;R::g_ConfiguredShapeReaderOverride=&StubConfiguredShape;
    R::g_HeightReaderOverride=&CountHeight;R::g_HeightLimitsReaderOverride=&StubHeightLimits;
    uint64_t oldReads=0,newReads=0; int totalOldFrames=0,totalNewFrames=0;
    for(int scenario=0;scenario<24;++scenario){
        ResetImage();g_BasicRange=1+scenario%3;g_LineWeapon=scenario%4==0;
        SetShapeTable({{1,0,5,0}},{{0,0,1},{1,0,1},{-1,0,1},{0,1,1},{0,-1,1}});
        Combat::BattleSnapshot state{};
        const int x=scenario%3==0?0:12;
        const int z=scenario%4==0?2:scenario%4==1?63:scenario%4==2?125:14;
        SetUnit(state,1,Combat::Faction::Player,x,z);
        SetUnit(state,2,Combat::Faction::Player,14,std::min(127,z+1),scenario%2?20:100);
        for(int i=0;i<4;++i)SetUnit(state,20+i,Combat::Faction::Enemy,17+i,std::min(127,z+i%2));
        if(scenario%3==1)for(int z=0;z<128;++z)for(int cx=0;cx<128;++cx)
            g_Heights[TacticalPath::CellIndex(cx,z)]=static_cast<int16_t>((cx*3+z)%13);
        std::vector<NativeSkills::Entry> skills;
        const int range=scenario==0?0:scenario>=22?127:7;
        for(int i=0;i<(range==127?2:12);++i)skills.push_back(MakeSkill(i+1,1,range,scenario%2!=0,i%3==0?1:0));
        R::GoalJob original;GoalJob fast;int oldFrames=0,newFrames=0;
        heightReads=0;if(!original.Begin(1,state,skills)||!Finish(original,13,oldFrames))return false;oldReads+=heightReads;
        heightReads=0;if(!fast.Begin(1,state,skills)||!Finish(fast,37,newFrames))return false;newReads+=heightReads;
        totalOldFrames+=oldFrames;totalNewFrames+=newFrames;
        if(!Expect(original.Goals()==fast.Goals(),"complete goal mask matches archived v5.4.1 across height, healing, range and batch variations"))return false;
    }
    printf("LATENCY heightQueriesBefore=%llu heightQueriesAfter=%llu framesBefore=%d framesAfter=%d\n",oldReads,newReads,totalOldFrames,totalNewFrames);
    return Expect(newReads*5<oldReads,"proven-tile reuse removes over 80 percent of duplicate height work without dropping goals") &&
        Expect(totalNewFrames*5<totalOldFrames,"costly-read deadline model reduces planning frames by over 80 percent with identical masks");
}
int main(){
    const bool ok=CacheSafety() && GoalParity();
    if(g_Image)VirtualFree(reinterpret_cast<void*>(g_Image),0,MEM_RELEASE);
    return ok?0:1;
}
