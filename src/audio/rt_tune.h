// rt_tune.h — 播放线程的系统级时序调优
//
// 为什么这些「小改动」对音质是实打实的：
//   * MMCSS "Pro Audio"：让系统把该线程当专业音频任务调度，避免被浏览器/杀毒/索引服务抢占。
//     抢占了会怎样？喂数不及时 -> WASAPI 缓冲欠载 -> 该驱动会【永久停止发送事件】
//     （Round 6 实测确认过这个行为），表现为声音突然断掉。
//   * CPU 亲和：绑到单个核。跨核迁移会让线程的 L1/L2 缓存失效，造成偶发的几十微秒抖动。
//   * 定时器精度提到 1ms：Windows 默认 15.6ms，会让 Sleep/等待的粒度变得很粗（Round 6 踩过）。
//   * TIME_CRITICAL：在 MMCSS 之外再兜一层。
//
// 这些都是「不提升解析力、但减少突发劣化」的措施 —— 对听感的贡献是「不出事」。
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <avrt.h>
// 只声明 timeBeginPeriod/timeEndPeriod，不引入 <mmsystem.h>（会与 <complex> 冲突）
extern "C" __declspec(dllimport) unsigned int __stdcall timeBeginPeriod(unsigned int);
extern "C" __declspec(dllimport) unsigned int __stdcall timeEndPeriod(unsigned int);

namespace rta::audio {

struct RtTune {
    HANDLE mmcss = nullptr;
    DWORD  taskIdx = 0;
    DWORD_PTR oldAffinity = 0;
    bool   timerSet = false;
    int    core = -1;
    bool   mmcssOk = false;
};

// 在【播放线程内部】调用。preferCore < 0 表示自动选（默认绑到 2 号核）。
inline RtTune apply_playback_tuning(int preferCore = -1) {
    RtTune t;

    // 1) MMCSS Pro Audio
    t.mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &t.taskIdx);
    if (t.mmcss) {
        AvSetMmThreadPriority(t.mmcss, AVRT_PRIORITY_CRITICAL);
        t.mmcssOk = true;
    }

    // 2) 线程优先级兜底
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    // 3) CPU 亲和
    const int core = (preferCore >= 0) ? preferCore : 2;
    const DWORD_PTR mask = SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << core);
    if (mask) { t.oldAffinity = mask; t.core = core; }

    // 4) 定时器精度
    timeBeginPeriod(1);
    t.timerSet = true;

    return t;
}

inline void release_playback_tuning(RtTune& t) {
    if (t.oldAffinity) SetThreadAffinityMask(GetCurrentThread(), t.oldAffinity);
    if (t.timerSet) timeEndPeriod(1);
    if (t.mmcss) { AvRevertMmThreadCharacteristics(t.mmcss); t.mmcss = nullptr; }
}

} // namespace rta::audio
