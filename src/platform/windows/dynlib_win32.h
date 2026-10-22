// platform/windows/dynlib_win32.h — dynlib.h 的 Windows 实现（LoadLibraryA/GetProcAddress）
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>

namespace rta::platform {

inline SharedLib::~SharedLib() { close(); }

inline SharedLib::SharedLib(SharedLib&& o) noexcept
    : handle_(o.handle_), err_(std::move(o.err_)) { o.handle_ = nullptr; }

inline SharedLib& SharedLib::operator=(SharedLib&& o) noexcept {
    if (this != &o) {
        close();
        handle_ = o.handle_;
        err_ = std::move(o.err_);
        o.handle_ = nullptr;
    }
    return *this;
}

inline bool SharedLib::open(const char* filename) {
    close();
    HMODULE h = ::LoadLibraryA(filename);
    if (!h) {
        handle_ = nullptr;
        err_ = "LoadLibrary 失败";
        return false;
    }
    handle_ = (void*)h;
    err_.clear();
    return true;
}

inline void SharedLib::close() {
    if (handle_) { ::FreeLibrary((HMODULE)handle_); handle_ = nullptr; }
}

inline bool SharedLib::is_open() const { return handle_ != nullptr; }

inline void* SharedLib::symbol(const char* name) const {
    if (!handle_) return nullptr;
    return (void*)::GetProcAddress((HMODULE)handle_, name);
}

inline void* SharedLib::detach() { void* h = handle_; handle_ = nullptr; return h; }

inline const std::string& SharedLib::last_error() const { return err_; }

} // namespace rta::platform
