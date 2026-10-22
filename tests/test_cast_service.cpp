// test_cast_service.cpp — UPnP 媒体投放后台服务与编排单元测试 (ROADMAP P5b)
#include "net/cast_service.h"
#include "audio/wav.h"
#include <cstdio>
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>

#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int);
#endif

using namespace rta::net;
namespace fs = std::filesystem;

static int g_fail = 0;
static void ck(bool cond, const char* what, const std::string& detail = "") {
    if (cond) {
        std::printf("  [PASS] %s\n", what);
    } else {
        std::printf("  [FAIL] %s  %s\n", what, detail.c_str());
        ++g_fail;
    }
}

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(65001);
#endif
    std::printf("=== test_cast_service: UPnP 投放后台服务与编排测试 ===\n\n");

    auto& mgr = CastManager::instance();

    // ================= 1) 初始状态契约 =================
    std::printf("-- 1. 初始状态与字段契约 --\n");
    {
        CastState st = mgr.get_state();
        ck(!st.running, "初始 running 必须为 false");
        ck(st.phase == "stopped", "初始 phase 必须为 stopped", st.phase);
        ck(st.positionSec == 0.0, "初始 positionSec 必须为 0", std::to_string(st.positionSec));
        ck(st.transportState == "STOPPED", "初始 transportState 必须为 STOPPED", st.transportState);
        ck(st.servedBytes == 0, "初始 servedBytes 必须为 0", std::to_string(st.servedBytes));
        ck(st.requestCount == 0, "初始 requestCount 必须为 0", std::to_string(st.requestCount));
        ck(st.rateMatch == "unknown", "初始 rateMatch 必须为 unknown", st.rateMatch);
        ck(st.warning.empty(), "初始 warning 为空");
        ck(st.error.empty(), "初始 error 为空");
    }

    // ================= 2) 参数校验与硬安全红线 =================
    std::printf("\n-- 2. 参数校验与安全门禁 --\n");
    {
        // 2.1 空文件路径
        CastStartParams p1;
        p1.file = "";
        auto r1 = mgr.start(p1);
        ck(!r1.ok, "空文件路径被拒绝");
        ck(r1.error.find("未指定") != std::string::npos, "返回明确提示：未指定音频文件路径", r1.error);

        // 2.2 不存在的文件
        CastStartParams p2;
        p2.file = "C:\\nonexistent_test_audio_file_12345.wav";
        auto r2 = mgr.start(p2);
        ck(!r2.ok, "不存在的文件被拒绝");
        ck(r2.error.find("不存在") != std::string::npos, "返回明确提示：文件不存在", r2.error);

        // 2.3 创建临时虚拟 DSD 文件，测试未开启 DoP 时的拒绝（铁律 3）
        std::string dsfPath = "build_test_temp_cast.dsf";
        {
            std::ofstream f(dsfPath, std::ios::binary);
            f.write("DSD ", 4);
        }
        CastStartParams p3;
        p3.file = dsfPath;
        p3.dop = false;
        auto r3 = mgr.start(p3);
        ck(!r3.ok, "DSD 源文件未开启 DoP 必须被拒绝（铁律 3：绝不擅自降质重采样）");
        ck(r3.error.find("DoP") != std::string::npos && r3.error.find("DSD") != std::string::npos,
           "DSD 错误提示明确包含 DSD 与 DoP 要求", r3.error);

        // 同样测试 .dff 后缀
        std::string dffPath = "build_test_temp_cast.dff";
        {
            std::ofstream f(dffPath, std::ios::binary);
            f.write("FRM8", 4);
        }
        CastStartParams p3b;
        p3b.file = dffPath;
        p3b.dop = false;
        auto r3b = mgr.start(p3b);
        ck(!r3b.ok, ".dff 文件未开启 DoP 同样必须被拒绝");
        ck(r3b.error.find("DoP") != std::string::npos, "dff 错误提示包含 DoP", r3b.error);

        std::remove(dsfPath.c_str());
        std::remove(dffPath.c_str());
    }

    // ================= 3) 设备列表只读查询与接口健壮性 =================
    std::printf("\n-- 3. 设备列表查询接口 --\n");
    {
        // forceRescan = false，不触发阻塞式全网扫描
        auto devs = mgr.get_devices(false);
        ck(true, "get_devices 接口调用正常且不崩潰");
    }

    // ================= 4) 停止与回位 =================
    std::printf("\n-- 4. 停止与回位状态一致性 --\n");
    {
        bool stopped = mgr.stop();
        ck(stopped, "stop 正常返回");
        CastState st = mgr.get_state();
        ck(!st.running, "stop 后 running 状态为 false");
        ck(st.phase == "stopped", "stop 后 phase 状态为 stopped", st.phase);
    }

    std::printf("\n=== 测试总结: %s (失败: %d) ===\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
