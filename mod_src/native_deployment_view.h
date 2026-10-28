#pragma once
#include "native_geometry.h"

namespace AutoPlay::NativeDeploymentView {
constexpr uintptr_t kVtable=0x986FA0;
constexpr size_t kWidget=0x130,kRecords=0xD8,kStride=0xB8;
constexpr size_t kFilter=0xB8D8,kMap=0xB8E4,kVisible=0xBCE4,kMapped=0xBCED;
using SetCursor=void(__fastcall*)(void*,int);
inline SetCursor setCursor=nullptr;
inline void Initialize(uintptr_t base) {
    // Exact live class and original-file vtable contracts, not a generic list.
    if(*reinterpret_cast<const uintptr_t*>(base+kVtable+0xE0)!=base+0x64F20 ||
       *reinterpret_cast<const uintptr_t*>(base+kVtable+0xC0)!=base+0x652B0 ||
       *reinterpret_cast<const uintptr_t*>(base+kVtable+0xA8)!=base+0x439D80)return;
    setCursor=reinterpret_cast<SetCursor>(base+0x439D80);
}
struct View {
    uintptr_t widget=0;int total=0,visible=0,filter=0;bool mapped=false;
    bool Read(uintptr_t base,uintptr_t task,int expectedCount) {
        *this={};widget=task+kWidget;
        if(!task || !NativeGeometry::IsReadableRange(widget,0xBD00) ||
            *reinterpret_cast<const uintptr_t*>(widget)!=base+kVtable)return false;
        total=*reinterpret_cast<const int*>(widget+0xC);
        mapped=*reinterpret_cast<const uint8_t*>(widget+kMapped)!=0;
        filter=*reinterpret_cast<const int*>(widget+kFilter);
        visible=mapped?*reinterpret_cast<const int*>(widget+kVisible):total;
        return total==expectedCount && total>0 && total<=128 && visible>=0 && visible<=total && filter>=0 && filter<=4;
    }
    uintptr_t Record(int visibleRow) const {
        if(visibleRow<0 || visibleRow>=visible)return 0;
        const int index=mapped?*reinterpret_cast<const int*>(widget+kMap+visibleRow*4):visibleRow;
        return index>=0&&index<total?widget+kRecords+static_cast<size_t>(index)*kStride:0;
    }
    int Find(int sourceRow,uintptr_t chara) const {
        int found=-1;
        for(int row=0;row<visible;++row) {
            const auto record=Record(row);
            if(!record)return -1;
            if(*reinterpret_cast<const int*>(record+0xB0)!=sourceRow)continue;
            if(found>=0 || *reinterpret_cast<const uintptr_t*>(record)!=chara ||
                *reinterpret_cast<const int*>(record+0xB4)!=0)return -1;
            found=row;
        }
        return found;
    }
    bool Matches(int sourceRow,uintptr_t chara,uint64_t serial) const {
        const int cursor=*reinterpret_cast<const int*>(widget+0x90);
        const auto record=Record(cursor);
        return record && *reinterpret_cast<const int*>(record+0xB0)==sourceRow &&
            *reinterpret_cast<const uintptr_t*>(record)==chara &&
            *reinterpret_cast<const int*>(record+0xB4)==0 &&
            (!serial || *reinterpret_cast<const uint64_t*>(chara+0x3060)==serial);
    }
};

// Reveal hidden selections for this confirmation, then restore the transient
// tab/scroll state. Saved groups and native eligibility flags are untouched.
struct Override {
    uintptr_t widget=0,base=0;int cursor=0,top=0,line=0;uint8_t mapped=0;
    void Restore() {
        if(!widget)return;
        if(NativeGeometry::IsReadableRange(widget,0xBD00) &&
           *reinterpret_cast<const uintptr_t*>(widget)==base+kVtable) {
            *reinterpret_cast<uint8_t*>(widget+kMapped)=mapped;
            *reinterpret_cast<int*>(widget+0x90)=cursor;
            *reinterpret_cast<int*>(widget+0x94)=top;
            *reinterpret_cast<int*>(widget+0x8C)=line;
        }
        widget=0;
    }
    bool Select(uintptr_t image,uintptr_t task,int count,int sourceRow,uintptr_t chara,uint64_t serial) {
        Restore();View view;
        if(!setCursor || !view.Read(image,task,count))return false;
        int row=view.Find(sourceRow,chara);
        if(row<0 && view.mapped && view.filter!=0) {
            base=image;widget=view.widget;mapped=*reinterpret_cast<const uint8_t*>(widget+kMapped);
            cursor=*reinterpret_cast<const int*>(widget+0x90);
            top=*reinterpret_cast<const int*>(widget+0x94);line=*reinterpret_cast<const int*>(widget+0x8C);
            // Same scoped bypass used by native4323D0 to access all records.
            // Leave filter+B8D8 unchanged: native54F830 persists that value.
            *reinterpret_cast<uint8_t*>(widget+kMapped)=0;
            if(!view.Read(image,task,count)){Restore();return false;}
            row=view.Find(sourceRow,chara);
        }
        if(row<0){Restore();return false;}
        setCursor(reinterpret_cast<void*>(view.widget),row);
        if(!view.Matches(sourceRow,chara,serial)){Restore();return false;}
        return true;
    }
};
} // namespace AutoPlay::NativeDeploymentView
