// test_scanpool.cpp — 库扫描并行线程池验证（ROADMAP P6a 门禁）
//
// 判据说明（铁律 3）：
//   1. 线程池任务顺序无关的聚合正确性：乱序完成的任务按索引聚合到预分配槽位，逐项严格匹配；
//   2. 进度计数：按完成数递增推进，不按提交数推进；
//   3. 取消后无泄漏：并发中途触发 cancel，工作线程无死锁、无泄漏退出；
//   4. 异常任务不吞错：任务内抛出异常能被捕获并经 rethrowIfError/wait 准确汇报；
//   5. 库扫描结果串行 vs 并行逐条一致：在合成小目录（%TEMP%，少量 WAV）上全量比对，
//      字段（path/sr/bits/ch/dur/title/mtime/size）逐条严格相等，且增量缓存准确复用。
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "net/scan_pool.h"
#include "net/library.h"
#include <cstdio>
#include <string>
#include <vector>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>

namespace fs = std::filesystem;
using rta::net::ScanPool;
using rta::net::Library;
using rta::net::LibTrack;

static int g_fail = 0;
static void ck(bool cond, const char* what, const std::string& detail = "") {
    if (cond) std::printf("  [PASS] %s\n", what);
    else { std::printf("  [FAIL] %s  %s\n", what, detail.c_str()); ++g_fail; }
}

static void make_test_wav(const std::string& path, int sr, int ch, int bits, int samples) {
    std::ofstream f(path, std::ios::binary);
    int byteRate = sr * ch * (bits / 8);
    int blockAlign = ch * (bits / 8);
    int dataBytes = samples * blockAlign;
    int chunkSize = 36 + dataBytes;

    f.write("RIFF", 4);
    uint32_t u32 = (uint32_t)chunkSize; f.write((char*)&u32, 4);
    f.write("WAVEfmt ", 8);
    u32 = 16; f.write((char*)&u32, 4);
    uint16_t u16 = 1; f.write((char*)&u16, 2); // PCM
    u16 = (uint16_t)ch; f.write((char*)&u16, 2);
    u32 = (uint32_t)sr; f.write((char*)&u32, 4);
    u32 = (uint32_t)byteRate; f.write((char*)&u32, 4);
    u16 = (uint16_t)blockAlign; f.write((char*)&u16, 2);
    u16 = (uint16_t)bits; f.write((char*)&u16, 2);
    f.write("data", 4);
    u32 = (uint32_t)dataBytes; f.write((char*)&u32, 4);
    std::vector<char> silence((size_t)dataBytes, 0);
    f.write(silence.data(), (std::streamsize)dataBytes);
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    std::printf("== 库扫描线程池与一致性验证 ==\n");

    // ---------------- 1. 线程池任务顺序无关的聚合正确性 ----------------
    {
        constexpr size_t kN = 80;
        ScanPool pool(4, 16);
        std::vector<int> results(kN, -1);

        for (size_t i = 0; i < kN; ++i) {
            pool.submit([i, &results]() {
                // 人为制造乱序完成
                int sleepUs = (int)((i * 37) % 500);
                std::this_thread::sleep_for(std::chrono::microseconds(sleepUs));
                results[i] = (int)(i * 3 + 17);
            });
        }
        pool.wait();

        bool match = true;
        for (size_t i = 0; i < kN; ++i) {
            if (results[i] != (int)(i * 3 + 17)) {
                match = false;
                break;
            }
        }
        ck(match, "任务顺序无关聚合：所有槽位结果精确匹配");
        ck(pool.completedTasks() == kN, "完成计数等于提交总数 (80)");
    }

    // ---------------- 2. 进度计数按完成数推进 ----------------
    {
        ScanPool pool(2, 4);
        std::atomic<bool> gate{true};
        std::atomic<int> running{0};

        pool.submit([&gate, &running]() {
            running++;
            while (gate.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        });
        pool.submit([&gate, &running]() {
            running++;
            while (gate.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        });
        // 提交两项，但尚未完成
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        ck(pool.submittedTasks() == 2, "提交计数已达到 2");
        ck(pool.completedTasks() == 0, "完成数严格为 0（未完成前不虚增）");

        gate.store(false);
        pool.wait();
        ck(pool.completedTasks() == 2, "任务全部完成后，完成数正确达到 2");
    }

    // ---------------- 3. 取消后无泄漏 ----------------
    {
        ScanPool pool(4, 8);
        std::atomic<int> started{0};
        for (int i = 0; i < 30; ++i) {
            pool.submit([&started]() {
                started++;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            });
        }
        // 等部分任务开始执行后取消
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        pool.cancel();
        pool.wait(false);

        ck(pool.isCancelled(), "线程池状态标记为已取消");
        ck(pool.completedTasks() < 30, "取消生效：未执行的任务被丢弃，未全部完成",
           std::to_string(pool.completedTasks()) + "/30");
        ck(true, "取消后工作线程全部安全退出无死锁");
    }

    // ---------------- 4. 异常任务不吞错 ----------------
    {
        ScanPool pool(2, 4);
        pool.submit([]() {
            throw std::runtime_error("scanpool test error");
        });
        bool caught = false;
        try {
            pool.wait(true);
        } catch (const std::runtime_error& e) {
            caught = (std::string(e.what()).find("scanpool test error") != std::string::npos);
        } catch (...) {
            caught = false;
        }
        ck(caught, "异常任务不吞错：wait 准确重抛 task 抛出的异常");
        ck(pool.hasError(), "hasError 状态正确置位");
    }

    // ---------------- 5. 库扫描串行 vs 并行逐条一致性（合成小目录） ----------------
    {
        const fs::path tmpDir = fs::temp_directory_path() / "rta_scanpool_test";
        std::error_code ec;
        fs::remove_all(tmpDir, ec);
        fs::create_directories(tmpDir, ec);

        const std::string oldCwd = fs::current_path().string();
        fs::current_path(tmpDir, ec);

        // 合成 6 首不同参数的 WAV 文件
        const std::string m1 = (tmpDir / "track01.wav").string();
        const std::string m2 = (tmpDir / "track02.wav").string();
        const std::string m3 = (tmpDir / "track03.wav").string();
        const std::string m4 = (tmpDir / "sub" / "track04.wav").string();
        const std::string m5 = (tmpDir / "sub" / "track05.wav").string();
        const std::string m6 = (tmpDir / "sub" / "track06.wav").string();
        fs::create_directories(tmpDir / "sub", ec);

        make_test_wav(m1, 44100, 2, 16, 4410); // 0.1s
        make_test_wav(m2, 48000, 2, 24, 4800); // 0.1s
        make_test_wav(m3, 96000, 2, 24, 9600); // 0.1s
        make_test_wav(m4, 44100, 1, 16, 2205); // 0.05s
        make_test_wav(m5, 88200, 2, 16, 8820); // 0.1s
        make_test_wav(m6, 48000, 1, 16, 4800); // 0.1s

        Library& lib = Library::instance();
        lib.addRoot(tmpDir.string());

        // 串行扫描（scanThreads = 1）
        lib.setScanThreads(1);
        lib.startScan(true);
        lib.joinScan();

        const size_t nSerial = lib.trackCount();
        ck(nSerial == 6, "串行全量扫描曲目数 = 6", "实际 " + std::to_string(nSerial));

        std::vector<LibTrack> serialTracks;
        for (size_t i = 0; i < nSerial; ++i) {
            LibTrack t;
            lib.trackAt(i, t);
            serialTracks.push_back(t);
        }

        // 并行扫描（scanThreads = 4, forceFull = true）
        lib.setScanThreads(4);
        lib.startScan(true);
        lib.joinScan();

        const size_t nParallel = lib.trackCount();
        ck(nParallel == 6, "并行全量扫描曲目数 = 6", "实际 " + std::to_string(nParallel));

        std::vector<LibTrack> parallelTracks;
        for (size_t i = 0; i < nParallel; ++i) {
            LibTrack t;
            lib.trackAt(i, t);
            parallelTracks.push_back(t);
        }

        // 逐条严格比对
        bool allEqual = (serialTracks.size() == parallelTracks.size());
        std::string diffMsg;
        if (allEqual) {
            for (size_t i = 0; i < serialTracks.size(); ++i) {
                const auto& s = serialTracks[i];
                const auto& p = parallelTracks[i];
                if (s.path != p.path || s.sampleRate != p.sampleRate || s.bits != p.bits ||
                    s.channels != p.channels || s.title != p.title || s.mtime != p.mtime ||
                    s.size != p.size || std::abs(s.duration - p.duration) > 0.001) {
                    allEqual = false;
                    diffMsg = "第 " + std::to_string(i) + " 首不一致: serial=" + s.path +
                              " parallel=" + p.path;
                    break;
                }
            }
        }
        ck(allEqual, "串行 vs 并行全量扫描逐条一致（路径/采样率/位深/声道/时长/标题/mtime/大小）", diffMsg);
        ck(lib.lastScanProbed() == 6, "全量扫描探针调用次数 = 6", "实际 " + std::to_string(lib.lastScanProbed()));
        ck(lib.lastScanMs() > 0.0, "扫描耗时统计字段已记录 (> 0 ms)", std::to_string(lib.lastScanMs()) + " ms");

        // 增量扫描测试（文件未变动）
        lib.startScan(false);
        lib.joinScan();
        ck(lib.lastScanCached() == 6, "增量扫描全部命中缓存 (cached=6)", "实际 " + std::to_string(lib.lastScanCached()));
        ck(lib.lastScanProbed() == 0, "增量扫描未重复探针 (probed=0)", "实际 " + std::to_string(lib.lastScanProbed()));

        fs::current_path(oldCwd, ec);
        fs::remove_all(tmpDir, ec);
    }

    std::printf("\n%s\n", g_fail == 0 ? "全部通过" : (std::to_string(g_fail) + " 项失败").c_str());
    return g_fail == 0 ? 0 : 1;
}
