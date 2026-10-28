#pragma once
#include "party_settings.h"
#include <windows.h>
#include <bcrypt.h>
#include <atomic>
#include <mutex>
#pragma comment(lib, "bcrypt.lib")

namespace AutoPlay::PartyIO {
struct Snapshot {
    bool ready = false;
    bool scopeReady = false;
    uint64_t scopeRevision = 0;
    uint64_t completedSave = 0;
    bool saveSucceeded = false;
    Party::PartySettings settings;
    std::string error;
};

// Only this worker performs file I/O and hashing. It receives copied paths and
// settings, never game pointers, and never invokes engine functions.
class Service {
    std::mutex mutex_;
    HANDLE event_ = nullptr;
    std::wstring path_, requestedScopePath_;
    uint64_t requestedScope_ = 0, handledScope_ = 0;
    uint64_t requestedSave_ = 0, handledSave_ = 0;
    Party::PartySettings pendingSave_;
    Party::Profiles profiles_;
    Snapshot published_;
    std::atomic<bool> started_{false};
    std::string fileError_;
    static DWORD WINAPI Worker(void* self) { static_cast<Service*>(self)->Run(); return 0; }

    static bool ReadFileText(const std::wstring& path, std::string& text, bool& absent) {
        absent = false; text.clear();
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) { absent = GetLastError() == ERROR_FILE_NOT_FOUND; return absent; }
        LARGE_INTEGER size{}; DWORD read = 0;
        const bool valid = GetFileSizeEx(file,&size) && size.QuadPart >= 0 && size.QuadPart <= 128*1024;
        bool okay = false;
        if (valid) {
            text.resize(static_cast<size_t>(size.QuadPart));
            okay = text.empty() || (ReadFile(file,text.data(),static_cast<DWORD>(text.size()),&read,nullptr) && read == text.size());
        }
        CloseHandle(file); return okay;
    }
    static bool WriteAtomic(const std::wstring& path, const std::string& text) {
        const std::wstring temporary = path+L".tmp", backup = path+L".bak";
        HANDLE file = CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        DWORD written=0;
        const bool okay = WriteFile(file,text.data(),static_cast<DWORD>(text.size()),&written,nullptr) &&
            written == text.size() && FlushFileBuffers(file);
        CloseHandle(file);
        if (!okay) return false;
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
            return MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_WRITE_THROUGH) != FALSE;
        return ReplaceFileW(path.c_str(),temporary.c_str(),backup.c_str(),0,nullptr,nullptr) != FALSE;
    }
    static bool Scope(const std::wstring& path, std::string& scope) {
        HANDLE file=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                                nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file == INVALID_HANDLE_VALUE) return false;
        BY_HANDLE_FILE_INFORMATION info{};
        wchar_t normalized[2048]{};
        const DWORD length=GetFinalPathNameByHandleW(file,normalized,2048,FILE_NAME_NORMALIZED);
        const bool okay=GetFileInformationByHandle(file,&info) && length && length<2048;
        CloseHandle(file);
        if(!okay) return false;
        std::wstring name(normalized,length);
        for(wchar_t& ch:name) if(ch>=L'A' && ch<=L'Z') ch+=L'a'-L'A';
        BCRYPT_ALG_HANDLE algorithm=nullptr; BCRYPT_HASH_HANDLE hash=nullptr;
        if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) return false;
        bool result=false;
        if(BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)>=0) {
            const DWORD identity[]={info.dwVolumeSerialNumber,info.nFileIndexHigh,info.nFileIndexLow,
                info.ftCreationTime.dwHighDateTime,info.ftCreationTime.dwLowDateTime};
            BYTE bytes[32]{};
            result=BCryptHashData(hash,reinterpret_cast<PUCHAR>(name.data()),static_cast<ULONG>(name.size()*sizeof(wchar_t)),0)>=0 &&
                BCryptHashData(hash,reinterpret_cast<PUCHAR>(const_cast<DWORD*>(identity)),sizeof(identity),0)>=0 &&
                BCryptFinishHash(hash,bytes,sizeof(bytes),0)>=0;
            if(result) { static const char hex[]="0123456789abcdef"; scope.clear(); for(BYTE b:bytes){scope+=hex[b>>4];scope+=hex[b&15];} }
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(algorithm,0); return result;
    }
    void Run() {
        std::string text,error; bool absent=false;
        const bool read=ReadFileText(path_,text,absent);
        Party::Profiles profiles;
        if(!read) error="Could not read autoplay_party.ini; open F3 to configure again";
        else if(!absent && !Party::Decode(text,profiles,error)) profiles.clear();
        { std::lock_guard<std::mutex> lock(mutex_); profiles_=std::move(profiles); published_.ready=true; published_.error=error;fileError_=error; }
        for(;;) {
            WaitForSingleObject(event_,INFINITE);
            std::wstring scopePath; uint64_t scopeRevision=0,saveRevision=0;
            Party::PartySettings save; Party::Profiles toWrite;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if(requestedScope_ != handledScope_) { scopePath=requestedScopePath_; scopeRevision=requestedScope_; }
                else if(requestedSave_ != handledSave_) { save=pendingSave_;saveRevision=requestedSave_;toWrite=profiles_; }
            }
            if(scopeRevision) {
                std::string key; const bool okay=Scope(scopePath,key);
                std::lock_guard<std::mutex> lock(mutex_);
                handledScope_=scopeRevision;
                if(scopeRevision == requestedScope_) {
                    published_.scopeRevision=scopeRevision; published_.scopeReady=okay;
                    published_.settings={}; published_.settings.saveNamespace=key;
                    if(okay) { auto item=profiles_.find(key); if(item!=profiles_.end()) published_.settings=item->second;published_.error=fileError_; }
                    else published_.error="Loaded save identity could not be verified; Custom party is OFF";
                }
            } else if(saveRevision) {
                const bool valid=Party::ValidNamespace(save.saveNamespace) && !Party::Validate(save) &&
                    (toWrite.count(save.saveNamespace) || toWrite.size()<128);
                if(valid) toWrite[save.saveNamespace]=save;
                const bool okay=valid && WriteAtomic(path_,Party::Encode(toWrite));
                std::lock_guard<std::mutex> lock(mutex_);
                handledSave_=saveRevision; published_.completedSave=saveRevision; published_.saveSucceeded=okay;
                if(okay) {
                    profiles_=std::move(toWrite);
                    if(published_.settings.saveNamespace == save.saveNamespace) published_.settings=save;
                    published_.error.clear();
                    fileError_.clear();
                } else published_.error="Could not save autoplay_party.ini; previous settings remain active";
            }
            { std::lock_guard<std::mutex> lock(mutex_); if(requestedScope_!=handledScope_ || requestedSave_!=handledSave_) SetEvent(event_); }
        }
    }
public:
    void Start(HMODULE module) {
        wchar_t path[2048]{}; const DWORD count=GetModuleFileNameW(module,path,2048);
        if(!count || count>=2048) return;
        std::wstring settingsPath(path,count); const auto slash=settingsPath.find_last_of(L"/\\");
        if(slash==std::wstring::npos) return;
        settingsPath.resize(slash+1);settingsPath+=L"autoplay_party.ini";
        StartAtPath(std::move(settingsPath));
    }
    void StartAtPath(std::wstring settingsPath) {
        if(started_.exchange(true)) return;
        path_=std::move(settingsPath);
        event_=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        // Start is called from DLL initialization. A std::thread constructor can
        // wait for a loader-blocked startup handshake; raw CreateThread does not.
        if(event_) { HANDLE worker=CreateThread(nullptr,0,&Worker,this,0,nullptr); if(worker) CloseHandle(worker);else{CloseHandle(event_);event_=nullptr;} }
        if(!event_){std::lock_guard<std::mutex> lock(mutex_);published_.ready=true;published_.error="Party settings worker could not start";}
    }
    Snapshot Read() { std::lock_guard<std::mutex> lock(mutex_); return published_; }
    void RequestScope(std::wstring copiedPath,uint64_t revision) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!event_){published_.scopeRevision=revision;published_.scopeReady=false;return;}
        requestedScopePath_=std::move(copiedPath);requestedScope_=revision;published_.scopeReady=false;
        SetEvent(event_);
    }
    uint64_t Save(const Party::PartySettings& settings) {
        if(!event_) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        pendingSave_=settings;const auto revision=++requestedSave_;SetEvent(event_);return revision;
    }
};
inline Service service;
} // namespace AutoPlay::PartyIO
