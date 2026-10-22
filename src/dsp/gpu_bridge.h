// gpu_bridge.h — GPU 卷积的动态加载桥接
//
// 设计：管线【不】在链接期依赖 HIP。运行时尝试加载 rtagpu.dll：
//   * 找不到 DLL  ⟹ 返回不可用，管线自动退回 CPU 卷积（普通构建照常工作）
//   * 找到但 kernel 跑不起来（如架构不匹配）⟹ rta_gpu_available() 返回 0，同样退回
// 这样 GPU 是纯粹的"锦上添花"，不会让整个项目变得必须有显卡才能跑。
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "gpu_pref.h"

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #include <windows.h>
#else
  #include <dlfcn.h>
#endif

namespace rta::dsp {

class GpuBridge {
public:
    static GpuBridge& instance() { static GpuBridge b; return b; }

    bool available() const { return available_; }
    const std::string& status() const { return status_; }
    // 换 GPU 后端（AMD/NVIDIA/关闭）后重新探测；下次播放生效
    void reload() { available_ = false; status_.clear(); load(); }

    // 返回 0 = 成功；非 0 = 失败（调用方应退回 CPU）
    int convolve(const double* in, size_t n, const double* ir, size_t irLen,
                 double* out, int block) const {
        if (!available_ || !fnConv_) return -1;
        return fnConv_(in, (long long)n, ir, (long long)irLen, out, block);
    }

private:
    GpuBridge() { load(); }
    GpuBridge(const GpuBridge&) = delete;
    GpuBridge& operator=(const GpuBridge&) = delete;

    void load() {
#ifdef _WIN32
        const auto cands = gpu_dll_candidates();
        if (cands.empty()) { status_ = "GPU 已关闭（使用 CPU 卷积）"; return; }
        // 逐个候选尝试；某个 DLL 存在但 kernel 跑不起来（例如 NVIDIA 机器上
        // 残留的 HIP 版 rtagpu.dll）时必须继续试下一个，而不是直接退回 CPU。
        std::string tried;
        for (const char* nm : cands) {
            HMODULE h = ::LoadLibraryA(nm);
            if (!h) { if (!tried.empty()) tried += " / "; tried += nm; continue; }
            auto avail = (int (*)())::GetProcAddress(h, "rta_gpu_available");
            auto conv  = (int (*)(const double*, long long, const double*, long long, double*, int))
                         ::GetProcAddress(h, "rta_gpu_convolve");
            if (!avail || !conv || avail() != 1) {
                ::FreeLibrary(h);
                if (!tried.empty()) tried += " / ";
                tried += std::string(nm) + "(不可用)";
                continue;
            }
            fnAvail_ = avail; fnConv_ = conv;
            available_ = true;
            status_ = std::string("GPU 卷积可用（") + nm + "）";
            return;
        }
        status_ = "未找到可执行的 GPU 后端（" + tried + "），使用 CPU 卷积";
        return;
#else
        void* h = ::dlopen("librtagpu.so", RTLD_NOW);
        if (!h) { status_ = "未找到 librtagpu.so（使用 CPU 卷积）"; return; }
        fnAvail_ = (int (*)())::dlsym(h, "rta_gpu_available");
        fnConv_  = (int (*)(const double*, long long, const double*, long long, double*, int))
                   ::dlsym(h, "rta_gpu_convolve");
#endif
        if (!fnAvail_ || !fnConv_) {
            status_ = "GPU 后端缺少导出符号 rta_gpu_available / rta_gpu_convolve（使用 CPU 卷积）";
            return;
        }
        // 必须真正验证 kernel 能执行：架构不匹配时 hipDeviceSynchronize 会"成功"但输出全零
        if (fnAvail_() != 1) { status_ = "rtagpu.dll 存在但 kernel 无法执行（架构不匹配？）——使用 CPU 卷积"; return; }
        available_ = true;
        status_ = "GPU 卷积可用";
    }

    bool available_ = false;
    std::string status_;
    int (*fnAvail_)() = nullptr;
    int (*fnConv_)(const double*, long long, const double*, long long, double*, int) = nullptr;
};

} // namespace rta::dsp
