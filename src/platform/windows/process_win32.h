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

#include "platform/windows/win_text.h"

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
