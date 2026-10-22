// convolution.h — 均匀分块 FFT 卷积（uniformly-partitioned overlap-add）
//
// 用途：卷积引擎（房间校正 / 任意 FIR EQ）与 Matrix 的 WAV 卷积插件。
// 百万抽头的室内校正 IR 用直接卷积不可实时，必须分块 FFT。
//
// 结构：把 IR 切成 P 段、每段 B 个样本；每来一个 B 样本的输入块，
//   1) 补零到 2B 后 FFT
//   2) 存入输入谱环形缓冲
//   3) 累加 Σ_p H[p] · ring[head − p]
//   4) IFFT 得 2B 个样本；前 B 个与上一块的溢出相加后输出，后 B 个存为溢出
//
// 关键性质：**零延迟**。H[0] 与"当前"输入块相乘，其贡献立刻出现在本块输出的
// 前 B 个样本里，尾巴进溢出缓冲。不需要额外的一整块缓冲延迟。
#pragma once
#include "fft.h"
#include <vector>
#include <cmath>
#include <cstddef>
#include <algorithm>

namespace rta::dsp {

class PartitionedConvolver {
public:
    // ir 可为任意长度；block 必须是 2 的幂（推荐 256~4096）
    PartitionedConvolver(const std::vector<double>& ir, size_t block = 512) : B_(block) {
        if (B_ == 0 || (B_ & (B_ - 1)) != 0) B_ = 512;      // 强制 2 的幂
        fftN_ = B_ * 2;
        P_ = (ir.size() + B_ - 1) / B_;
        if (P_ == 0) P_ = 1;

        H_.assign(P_, std::vector<cplx>(fftN_, cplx(0.0, 0.0)));
        for (size_t p = 0; p < P_; ++p) {
            for (size_t i = 0; i < B_; ++i) {
                const size_t k = p * B_ + i;
                if (k < ir.size()) H_[p][i] = cplx(ir[k], 0.0);
            }
            fft(H_[p]);
        }
        ring_.assign(P_, std::vector<cplx>(fftN_, cplx(0.0, 0.0)));
        acc_.assign(fftN_, cplx(0.0, 0.0));
        ola_.assign(B_, 0.0);
        head_ = 0;
    }

    size_t block_size() const { return B_; }
    size_t partitions() const { return P_; }
    size_t ir_length() const { return irLen_; }
    void set_ir_length(size_t n) { irLen_ = n; }
    size_t latency() const { return 0; }   // 均匀分块 overlap-add 为零延迟

    void reset() {
        for (auto& r : ring_) std::fill(r.begin(), r.end(), cplx(0.0, 0.0));
        std::fill(ola_.begin(), ola_.end(), 0.0);
        head_ = 0;
    }

    // 处理一个输入块（恰好 B_ 个样本），输出 B_ 个样本
    void process_block(const double* in, double* out) {
        std::vector<cplx> X(fftN_, cplx(0.0, 0.0));
        for (size_t i = 0; i < B_; ++i) X[i] = cplx(in[i], 0.0);
        fft(X);
        ring_[head_] = std::move(X);

        std::fill(acc_.begin(), acc_.end(), cplx(0.0, 0.0));
        for (size_t p = 0; p < P_; ++p) {
            const size_t idx = (head_ + P_ - (p % P_)) % P_;
            const auto& Xp = ring_[idx];
            const auto& Hp = H_[p];
            for (size_t k = 0; k < fftN_; ++k) acc_[k] += Xp[k] * Hp[k];
        }
        fft(acc_, /*inverse=*/true);

        for (size_t i = 0; i < B_; ++i) {
            out[i] = acc_[i].real() + ola_[i];
            ola_[i] = acc_[B_ + i].real();
        }
        head_ = (head_ + 1) % P_;
    }

private:
    size_t B_ = 512, fftN_ = 1024, P_ = 1, head_ = 0, irLen_ = 0;
    std::vector<std::vector<cplx>> H_, ring_;
    std::vector<cplx> acc_;
    std::vector<double> ola_;
};

// ---- 流式卷积器：接受任意长度输入，输出同长度样本 ----
//
// 均匀分块 FFT 卷积要求【固定块长】，无法直接接受任意分块。做法：
//   * 输入进 FIFO，攒满 B 个才交一块给 PartitionedConvolver
//   * 输出 FIFO 预置 B 个零 —— 这代表 B 帧的算法延迟
//   * 每次调用弹出与输入等量的输出
// 于是"整块喂"与"分块喂"产生【完全相同】的输出序列（同样延迟 B 帧）。
// 代价：B 帧延迟（1024 帧 @44.1k ≈ 23 ms，对房间校正动辄数十万抽头可忽略）。
class StreamingConvolver {
public:
    explicit StreamingConvolver(const std::vector<double>& ir, size_t block = 1024)
        : pc_(ir, (block == 0 || (block & (block - 1)) != 0) ? size_t(1024) : block),
          B_((block == 0 || (block & (block - 1)) != 0) ? size_t(1024) : block) {
        inFifo_.reserve(B_ * 2);
        outFifo_.assign(B_, 0.0);        // ★ 预置 B 帧延迟，保证 FIFO 永不为空
    }

    size_t latency() const { return B_; }
    size_t max_ir() const { return pc_.partitions() * B_; }

    void reset(const std::vector<double>& ir) {
        pc_ = PartitionedConvolver(ir, B_);
        inFifo_.clear();
        outFifo_.assign(B_, 0.0);
    }

    void process(const double* in, double* out, size_t n) {
        inFifo_.insert(inFifo_.end(), in, in + n);
        size_t pos = 0;
        std::vector<double> blk(B_), oblk(B_);
        while (inFifo_.size() - pos >= B_) {
            for (size_t i = 0; i < B_; ++i) blk[i] = inFifo_[pos + i];
            pc_.process_block(blk.data(), oblk.data());
            outFifo_.insert(outFifo_.end(), oblk.begin(), oblk.end());
            pos += B_;
        }
        if (pos) inFifo_.erase(inFifo_.begin(), inFifo_.begin() + (ptrdiff_t)pos);
        // 取出 n 个；预置的延迟保证 FIFO 不会空
        for (size_t i = 0; i < n; ++i) {
            out[i] = (i < outFifo_.size()) ? outFifo_[i] : 0.0;
        }
        if (n <= outFifo_.size()) outFifo_.erase(outFifo_.begin(), outFifo_.begin() + (ptrdiff_t)n);
        else outFifo_.clear();
    }

private:
    PartitionedConvolver pc_;
    size_t B_;
    std::vector<double> inFifo_, outFifo_;
};

// 直接线性卷积（仅用于验证，O(N·M)）
inline std::vector<double> direct_convolve(const std::vector<double>& x, const std::vector<double>& h) {
    std::vector<double> y(x.size() + h.size(), 0.0);
    for (size_t i = 0; i < x.size(); ++i) {
        if (x[i] == 0.0) continue;
        for (size_t j = 0; j < h.size(); ++j) y[i + j] += x[i] * h[j];
    }
    y.resize(x.size());
    return y;
}

} // namespace rta::dsp
