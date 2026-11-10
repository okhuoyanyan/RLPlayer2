// SPDX-License-Identifier: GPL-3.0-or-later
// Windows implementation of the artwork transaction contract.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
namespace rta::platform::artwork_io {
namespace fs=std::filesystem;
inline bool safe(const fs::path&p) {
    std::error_code ec;auto absolute=fs::absolute(p,ec).lexically_normal();if(ec)return false;
    fs::path part;
    for(const auto&c:absolute) {
        part/=c;const DWORD attr=GetFileAttributesW(part.c_str());
        if(attr==INVALID_FILE_ATTRIBUTES){const DWORD e=GetLastError();if(e==ERROR_FILE_NOT_FOUND||e==ERROR_PATH_NOT_FOUND)continue;return false;}
        if(attr&FILE_ATTRIBUTE_REPARSE_POINT)return false;
    }
    return true;
}
inline bool present(const fs::path&p) {
    const DWORD attr=GetFileAttributesW(p.c_str());
    // Denial is a declaration too: never treat an unreadable current path as absent.
    return attr!=INVALID_FILE_ATTRIBUTES || (GetLastError()!=ERROR_FILE_NOT_FOUND&&GetLastError()!=ERROR_PATH_NOT_FOUND);
}
inline bool read(const fs::path&p,size_t limit,std::string&out) {
    out.clear();if(!safe(p))return false;
    HANDLE h=CreateFileW(p.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(h==INVALID_HANDLE_VALUE)return false;
    BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER n{};
    bool ok=GetFileInformationByHandle(h,&info)&&!(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))&&
        GetFileSizeEx(h,&n)&&n.QuadPart>0&&(uint64_t)n.QuadPart<=limit;
    if(ok){out.resize((size_t)n.QuadPart);DWORD got=0;ok=ReadFile(h,out.data(),(DWORD)out.size(),&got,nullptr)&&got==out.size();}
    CloseHandle(h);if(!ok||!safe(p)){out.clear();return false;}return true;
}

inline bool publish(const fs::path&p,const std::string&bytes) {
    if(!safe(p)||present(p))return false;
    std::error_code ec;fs::create_directories(p.parent_path(),ec);if(ec||!safe(p))return false;
    static std::atomic<unsigned long long> serial{0};
    auto tmp=p;tmp+=L".migrate-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(serial.fetch_add(1));
    HANDLE h=CreateFileW(tmp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE)return false;DWORD done=0;
    bool ok=WriteFile(h,bytes.data(),(DWORD)bytes.size(),&done,nullptr)&&done==bytes.size()&&FlushFileBuffers(h);
    CloseHandle(h);
    if(ok&&safe(p))ok=MoveFileExW(tmp.c_str(),p.c_str(),MOVEFILE_WRITE_THROUGH)!=0;
    else ok=false;
    if(!ok)DeleteFileW(tmp.c_str());return ok;
}

inline bool capture_stamp(const fs::path&p,FileStamp&stamp) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if(!safe(p)||!GetFileAttributesExW(p.c_str(),GetFileExInfoStandard,&a)||
       (a.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))return false;
    stamp={a.dwFileAttributes,((uint64_t)a.nFileSizeHigh<<32)|a.nFileSizeLow,
        ((uint64_t)a.ftCreationTime.dwHighDateTime<<32)|a.ftCreationTime.dwLowDateTime,
        ((uint64_t)a.ftLastWriteTime.dwHighDateTime<<32)|a.ftLastWriteTime.dwLowDateTime};
    return true;
}
} // namespace rta::platform::artwork_io
