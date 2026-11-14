// platform/windows/process_win32.h — process.h 的 Windows 实现
//
// run_hidden 逐行搬自 audio/audio_loader.h（迁移前唯一实现），唯一差别：
// 命令行以 UTF-8 传入，本层用 MultiByteToWideChar(CP_UTF8) 转宽 —— 与调用方
// 原先自己 widen() 的结果逐字节相同。
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <algorithm>

#include "platform/windows/win_text.h"

// request_launcher_exit 用 FindWindowW/PostMessageW（user32）。
// 迁移前这条 pragma 写在 net/feedback.h / net/update_apply.h 里；实现收进平台层后跟过来。
#pragma comment(lib, "user32.lib")

namespace rta::platform {

// 不弹窗地跑一个命令，返回退出码（-1 = 启动失败）+ 合并输出
inline ProcessResult run_hidden(const std::string& command_line_utf8, bool capture_output) {
    ProcessResult out;                       // exit_code = -1, started = false
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (capture_output && !::CreatePipe(&rd, &wr, &sa, 0)) return out;
    if (wr) ::SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (capture_output) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdOutput = wr;
        si.hStdError  = wr;
        si.hStdInput  = ::GetStdHandle(STD_INPUT_HANDLE);
    }
    PROCESS_INFORMATION pi{};
    std::wstring tmp = detail::widen(command_line_utf8);   // CreateProcessW 需要可写缓冲
    if (!::CreateProcessW(nullptr, tmp.data(), nullptr, nullptr, capture_output ? TRUE : FALSE,
                          CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        if (capture_output) { ::CloseHandle(rd); ::CloseHandle(wr); }
        return out;
    }
    out.started = true;
    if (capture_output) ::CloseHandle(wr);
    if (capture_output) {
        char buf[4096];
        DWORD got = 0;
        while (::ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got > 0)
            out.output.append(buf, got);
        ::CloseHandle(rd);
    }
    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    out.exit_code = (int)code;
    return out;
}

// Drain two independent pipes so stderr can never corrupt binary PCM or fill
// a blocking pipe. No process name or process tree is used for cancellation.
inline StreamProcessResult run_hidden_stream(const std::string& command_line_utf8,
    const std::function<bool(const void*, size_t)>& on_stdout,
    const std::function<bool()>& cancelled) {
    StreamProcessResult out;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd=nullptr,wr=nullptr,errRd=nullptr,errWr=nullptr,input=nullptr;
    auto close=[&](HANDLE& h){if(h&&h!=INVALID_HANDLE_VALUE)::CloseHandle(h);h=nullptr;};
    auto cleanup=[&](){close(rd);close(wr);close(errRd);close(errWr);close(input);};
    if(!::CreatePipe(&rd,&wr,&sa,65536)||!::CreatePipe(&errRd,&errWr,&sa,4096)){cleanup();return out;}
    ::SetHandleInformation(rd,HANDLE_FLAG_INHERIT,0);::SetHandleInformation(errRd,HANDLE_FLAG_INHERIT,0);
    input=::CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    STARTUPINFOW si{};si.cb=sizeof(si);si.dwFlags=STARTF_USESHOWWINDOW|STARTF_USESTDHANDLES;si.wShowWindow=SW_HIDE;
    si.hStdOutput=wr;si.hStdError=errWr;si.hStdInput=input;
    PROCESS_INFORMATION pi{};std::wstring command=detail::widen(command_line_utf8);
    if(!::CreateProcessW(nullptr,command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi)){cleanup();return out;}
    out.started=true;close(wr);close(errWr);close(input);::CloseHandle(pi.hThread);
    bool stopping=false,callbackFailed=false;char buffer[65536];
    auto stopOwnChild=[&](){if(!stopping){stopping=true;out.cancelled=true;::TerminateProcess(pi.hProcess,ERROR_CANCELLED);}};
    for(;;){
        if(cancelled&&cancelled())stopOwnChild();
        bool received=false;
        for(int stream=0;stream<2;++stream){
            HANDLE pipe=stream?errRd:rd;DWORD available=0;
            if(!::PeekNamedPipe(pipe,nullptr,0,nullptr,&available,nullptr)||!available)continue;
            DWORD got=0;DWORD want=std::min<DWORD>(available,sizeof(buffer));
            if(!::ReadFile(pipe,buffer,want,&got,nullptr)||!got)continue;
            received=true;
            if(stream){if(out.output.size()<65536)out.output.append(buffer,std::min<size_t>(got,65536-out.output.size()));}
            else if(!stopping){
                try{if(on_stdout&&!on_stdout(buffer,got))stopOwnChild();}
                catch(...){callbackFailed=true;stopOwnChild();}
            }
        }
        DWORD wait=::WaitForSingleObject(pi.hProcess,0);
        if(wait==WAIT_OBJECT_0){
            DWORD a=0,b=0;::PeekNamedPipe(rd,nullptr,0,nullptr,&a,nullptr);::PeekNamedPipe(errRd,nullptr,0,nullptr,&b,nullptr);
            if(!a&&!b)break;
        }
        if(!received)::Sleep(2);
    }
    ::WaitForSingleObject(pi.hProcess,INFINITE);DWORD code=1;::GetExitCodeProcess(pi.hProcess,&code);out.exit_code=int(code);
    ::CloseHandle(pi.hProcess);cleanup();
    if(callbackFailed&&out.output.empty())out.output="stream callback failed";
    return out;
}

// 语义 = 发一条启动器本来就认的 WM_COMMAND（ID_TRAY_EXIT = 1003，见 tools/launcher.cpp）。
// 先按"类名 + 任意标题"找，再退到"类名 + 同名标题"（与迁移前 net/update_apply.h 一致）。
inline bool request_launcher_exit() {
    HWND h = ::FindWindowW(L"RLPlayerTray", nullptr);
    if (!h) h = ::FindWindowW(L"RLPlayerTray", L"RLPlayerTray");
    if (!h) return false;
    ::PostMessageW(h, WM_COMMAND, (WPARAM)1003, 0);
    return true;
}

inline std::string find_executable(const char* name) {
    const std::string dir = module_dir_utf8();
    if (!dir.empty()) {
        const std::wstring local = detail::widen(join(dir, std::string(name) + ".exe"));
        if (::GetFileAttributesW(local.c_str()) != INVALID_FILE_ATTRIBUTES)
            return detail::narrow(local);
    }
    wchar_t buf[4096];
    const std::wstring w = detail::widen(name);
    const DWORD n = ::SearchPathW(nullptr, w.c_str(), L".exe", 4096, buf, nullptr);
    if (n == 0 || n >= 4096) return "";
    return detail::narrow(std::wstring(buf, n));
}

} // namespace rta::platform
