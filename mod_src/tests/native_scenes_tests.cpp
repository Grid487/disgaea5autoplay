#include "../native_scenes.h"
#include <cstdio>
namespace AutoPlay { void LogMsg(const char*, ...) {} }
using namespace AutoPlay;
namespace S = NativeScenes;
bool Check(bool ok, const char* label) { printf("%s: %s\n", ok ? "PASS" : "FAIL", label); return ok; }
uint64_t __fastcall Replacement() { return 0; }
int main() {
    const size_t size = 16ull * 1024 * 1024;
    const uintptr_t base = reinterpret_cast<uintptr_t>(VirtualAlloc(nullptr,size,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if (!base) return 2;
    const uintptr_t manager = base + 0x10000;
    *reinterpret_cast<uintptr_t*>(base + S::kManagerPointerRva) = manager;
    *reinterpret_cast<int*>(manager) = -1;
    *reinterpret_cast<int*>(manager + 4) = 9;
    *reinterpret_cast<int*>(manager + 8) = 2;
    auto* rows = reinterpret_cast<S::Row*>(manager + 0x10);
    rows[0] = {5,0,base+0x4AEE70,base+S::kBattleUpdateRva,base+0x4AF010,base+0x4AF030};
    rows[1] = {9,0,base+0x647260,base+0x647320,base+0x647350,base+0x647370};
    S::Registry registry;
    bool ok = Check(S::ReadRegistry(base,registry) && registry.current == 9 && registry.count == 2,
                    "literal native scene registry and callback layout decoded");
    const auto address = reinterpret_cast<uintptr_t>(&rows[1].update);
    ok &= Check(S::FindUpdateSlot(base,9,0x647320,registry) == address,
                "verified selector update is distinct from post/draw and cleanup");
    ok &= Check(S::FindUpdateSlot(base,9,0x647350,registry) == 0,
                "post/draw callback cannot be mistaken for owning update thread");
    auto original = static_cast<S::Update>(nullptr);
    ok &= Check(S::InstallUpdate(base,9,0x647320,&Replacement,original) &&
        reinterpret_cast<uintptr_t>(original) == base+0x647320 &&
        rows[1].update == reinterpret_cast<uintptr_t>(&Replacement) && rows[1].post == base+0x647350,
        "hook replaces only exact update callback and retains original");
    ok &= Check(!S::InstallUpdate(base,9,0x647320,&Replacement,original), "duplicate hook installation is rejected");
    rows[1].update = base+0x647320; rows[0] = rows[1];
    ok &= Check(S::FindUpdateSlot(base,9,0x647320,registry) == 0, "ambiguous duplicate scene IDs rejected");
    *reinterpret_cast<int*>(manager+8) = 65;
    ok &= Check(!S::ReadRegistry(base,registry), "invalid scene counts fail closed");
    *reinterpret_cast<uintptr_t*>(base + S::kManagerPointerRva) = 1;
    ok &= Check(!S::ReadRegistry(base,registry), "unreadable registry rejected without native calls");
    VirtualFree(reinterpret_cast<void*>(base),0,MEM_RELEASE);
    return ok ? 0 : 1;
}
