// drift.h — 发送端与端点端的时钟漂移检测
//
// 问题：DSP 机与端点机是两个独立晶振。哪怕只差 10 ppm，1 小时也会累积 36 ms 的偏差，
//       端点缓冲迟早会溢出（丢样）或耗尽（断音）。NAA 用 position 回传做流控，
//       我们做得更明确：直接估计 ppm 并给出速率补偿建议。
//
// 方法：对最近一个滑动窗口内的 (本地时间 t, 流时间戳 ts) 做最小二乘直线拟合，
//       斜率 slope 表示"每本地秒对应多少流样本秒"。
//         slope == 1        ⟹ 无漂移
//         slope >  1        ⟹ 端点消费得比发送慢 ⟹ 缓冲在涨
//         slope <  1        ⟹ 端点消费得比发送快 ⟹ 缓冲在掉
//       漂移(ppm) = (slope − 1) × 1e6
#pragma once
#include <cstdint>
#include <deque>
#include <cmath>
#include <algorithm>
#include <cstddef>

namespace rta::net {

class DriftEstimator {
public:
    // windowSec：拟合窗口长度（越大越稳、越小越快跟上）
    explicit DriftEstimator(double windowSec = 10.0) : windowSec_(windowSec) {}

    // 每收到一包调用一次
    //   streamTs   : 该包第一个样本的流内时间戳（样本号）
    //   localSec   : 本机单调时钟（秒）
    //   rate       : 流采样率
    void observe(uint64_t streamTs, double localSec, uint32_t rate) {
        pts_.push_back({localSec, (double)streamTs / (double)rate});
        // 淘汰窗口外的点
        while (pts_.size() > 2 && (localSec - pts_.front().t) > windowSec_) pts_.pop_front();
        dirty_ = true;
    }

    bool ready() const { return pts_.size() >= 8; }
    size_t samples() const { return pts_.size(); }

    // 解决"设备位置计数被量化"的问题：直接用时间戳/本地时间拟合，不依赖设备位置
    double slope() const {
        if (!ready()) return 1.0;
        if (!dirty_) return cachedSlope_;
        const size_t n = pts_.size();
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (const auto& p : pts_) { sx += p.t; sy += p.ts; sxx += p.t * p.t; sxy += p.t * p.ts; }
        const double den = (double)n * sxx - sx * sx;
        cachedSlope_ = (std::fabs(den) < 1e-12) ? 1.0 : ((double)n * sxy - sx * sy) / den;
        dirty_ = false;
        return cachedSlope_;
    }

    // 漂移，单位 ppm
    double drift_ppm() const { return (slope() - 1.0) * 1e6; }

    // 建议的速率补偿（ppm）：发送端按此微调输出速率即可长期锁住缓冲水位。
    // 端点消费得快（slope<1，缓冲在掉）⟹ 发送要加快 ⟹ 建议值为正。
    double suggested_rate_adjust_ppm() const { return (1.0 - slope()) * 1e6; }

    // 缓冲水位（毫秒）：已发送但尚未播放的量。由调用方喂入设备已播帧数。
    void observe_played(uint64_t streamTs, uint64_t playedFrames, uint32_t rate) {
        bufferMs_ = ((double)streamTs - (double)playedFrames) * 1000.0 / (double)rate;
    }
    double buffer_ms() const { return bufferMs_; }

    void reset() { pts_.clear(); dirty_ = true; bufferMs_ = 0.0; }

private:
    struct Pt { double t; double ts; };
    std::deque<Pt> pts_;
    double windowSec_;
    double bufferMs_ = 0.0;
    mutable bool dirty_ = true;
    mutable double cachedSlope_ = 1.0;
};

} // namespace rta::net
