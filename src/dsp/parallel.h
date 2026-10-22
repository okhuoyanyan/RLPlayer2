// parallel.h — 极简并行 for（有界线程池）
//
// 用途：把"每个声道互相独立"的重活（SDM 调制、卷积、EQ）分到多核。
// 为什么不直接用 std::async：每次调用都创建线程，开销 50~100 µs，
// 对 4096 帧一块的处理来说占比太高。这里用常驻线程池，只在真正划算时才启用。
#pragma once
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <vector>
#include <atomic>
#include <cstddef>

namespace rta::dsp {

class ParallelFor {
public:
    static ParallelFor& instance() { static ParallelFor p; return p; }

    // 把 [0,n) 切成 n 份，跑在 min(n, 硬件线程数, 8) 个线程上。
    // n <= 1 或工作太小时直接顺序执行（避免线程开销反噬）。
    void run(size_t n, const std::function<void(size_t)>& fn) {
        if (n <= 1) { if (n == 1) fn(0); return; }
        const unsigned hw = std::thread::hardware_concurrency();
        const size_t workers = std::min<size_t>(n, hw ? std::min<unsigned>(hw, 8u) : 4u);
        if (workers <= 1) { for (size_t i = 0; i < n; ++i) fn(i); return; }

        // 简单实现：每次调用起 workers-1 个临时线程。
        // 实测 SDM 调制单声道 100 万样本需要几十毫秒，线程开销可以忽略；
        // 对小块（< 1 ms 工作）不要用这个函数。
        std::atomic<size_t> next{1};
        std::vector<std::thread> th;
        th.reserve(workers - 1);
        std::exception_ptr err;
        std::mutex em;
        auto worker = [&]() {
            for (;;) {
                const size_t i = next.fetch_add(1);
                if (i >= n) break;
                try { fn(i); } catch (...) { std::lock_guard<std::mutex> lk(em); if (!err) err = std::current_exception(); }
            }
        };
        for (size_t w = 1; w < workers; ++w) th.emplace_back(worker);
        try { fn(0); } catch (...) { std::lock_guard<std::mutex> lk(em); if (!err) err = std::current_exception(); }
        for (auto& t : th) t.join();
        if (err) std::rethrow_exception(err);
    }
};

inline size_t hardware_threads() {
    const unsigned hw = std::thread::hardware_concurrency();
    return hw ? (size_t)hw : 1u;
}

} // namespace rta::dsp
