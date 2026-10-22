// clock_loop.h — 时钟漂移闭环补偿
//
// 问题：Round 14 只做了【检测】。只检测不补偿的话，端点缓冲迟早会溢出（丢样）或耗尽（断音）。
//
// 闭环结构（双环）：
//   内环（快）：用 ppm 漂移估计直接给出速率修正 —— 消除持续漂移
//   外环（慢）：用缓冲水位与目标值的偏差做微调 —— 消除累积误差与估计偏置
//   合计修正经【限幅】与【变化率限制】，避免可闻的音高抖动
//
// 补偿执行方式（发送端）：按修正后的比率做异步重采样。本文件只负责算出修正量，
// 具体施加由发送端的重采样器完成（音质影响最小；丢/插样本的做法不采用）。
#pragma once
#include <algorithm>
#include <cmath>

namespace rta::net {

struct ClockLoopConfig {
    double targetBufferMs   = 200.0;   // 目标缓冲水位
    double maxAdjustPpm     = 500.0;   // 修正上限（异步重采样的合理范围）
    double slewPpmPerSec    = 20.0;    // 变化率上限，避免可闻音高抖动
    double outerKp          = 2.0;     // 外环比例增益（ppm / ms）
    double outerKiPerSec    = 1.0;     // 外环积分增益（ppm / (ms·秒)）—— 消除稳态偏差
    double maxIntegralPpm   = 200.0;   // 积分抗饱和
    double innerGain        = 1.0;     // 内环增益（扣除自身修正后可安全取 1.0）
};

class ClockLoop {
public:
    explicit ClockLoop(const ClockLoopConfig& c = {}) : cfg_(c) {}

    // driftPpm : 检测到的漂移（Round 14 的 DriftEstimator::drift_ppm）
    // bufferMs : 当前缓冲水位；<0 表示未知（只用内环）
    // dtSec    : 距上次调用的时间
    // 返回：应施加的速率修正（ppm）
    double update(double driftPpm, double bufferMs, double dtSec) {
        double want = 0.0;
        if (std::isfinite(driftPpm)) {
            // ★ 关键：估计到的是【相对】漂移，其中已经包含了我们上一拍施加的修正。
            //   若直接按它补偿就形成正反馈（实测增益取 1.0 时修正量在 ±337 ppm 间来回甩）。
            //   先扣除自身修正，剩下的才是端点相对发送端的真实漂移。
            const double external = driftPpm - applied_;
            want += cfg_.innerGain * external;
        }
        if (bufferMs >= 0.0) {
            const double errMs = bufferMs - cfg_.targetBufferMs;
            // 外环 PI：P 快速纠偏，I 消除稳态偏差（纯 P 会留下与漂移成正比的残余）
            integral_ += cfg_.outerKiPerSec * errMs * std::max(dtSec, 1e-6);
            integral_ = std::clamp(integral_, -cfg_.maxIntegralPpm, cfg_.maxIntegralPpm);
            want -= cfg_.outerKp * errMs + integral_;
        }
        want = std::clamp(want, -cfg_.maxAdjustPpm, cfg_.maxAdjustPpm);

        // 变化率限制
        const double maxStep = cfg_.slewPpmPerSec * std::max(dtSec, 1e-6);
        const double delta = std::clamp(want - applied_, -maxStep, maxStep);
        applied_ += delta;
        applied_ = std::clamp(applied_, -cfg_.maxAdjustPpm, cfg_.maxAdjustPpm);
        lastWant_ = want;
        return applied_;
    }

    double applied_ppm() const { return applied_; }
    double wanted_ppm() const { return lastWant_; }
    void reset() { applied_ = 0.0; lastWant_ = 0.0; integral_ = 0.0; }
    double integral_ppm() const { return integral_; }

private:
    ClockLoopConfig cfg_;
    double applied_ = 0.0;
    double lastWant_ = 0.0;
    double integral_ = 0.0;
};

} // namespace rta::net
