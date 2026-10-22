// gpu_pref.h — GPU 后端偏好（AMD / NVIDIA / 自动 / 关闭）
//
// 为什么需要它：AMD 走 HIP（rtagpu.dll），NVIDIA 需要 CUDA 版后端（rtagpu_cuda.dll）。
// 两者的导出符号完全一样，所以"选哪张卡"只需要换加载的 DLL 名字。
// 注意：选了一个没有对应后端的厂商，不会报错崩掉，而是如实退回 CPU 并在信号链里写清楚。
#pragma once
#include <string>
#include <vector>

namespace rta::dsp {

inline std::string& gpu_vendor_ref() {
    static std::string v = "auto";
    return v;
}
inline void set_gpu_vendor(const std::string& v) { gpu_vendor_ref() = v; }
inline const std::string& gpu_vendor() { return gpu_vendor_ref(); }

// 按偏好给出要尝试的 DLL 名字（按顺序）
inline std::vector<const char*> gpu_dll_candidates() {
    const std::string& v = gpu_vendor();
    if (v == "off") return {};
    // 顺序：HIP（AMD）→ D3D12（AMD/NVIDIA/Intel 通用）→ CUDA（可选）
    if (v == "amd")    return { "rtagpu.dll", "rtagpu_dx12.dll" };
    if (v == "nvidia") return { "rtagpu_cuda.dll", "rtagpu_dx12.dll" };
    return { "rtagpu.dll", "rtagpu_dx12.dll", "rtagpu_cuda.dll" };   // auto
}

} // namespace rta::dsp
