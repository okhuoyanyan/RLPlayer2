// cda_mci.h —— CD 音轨（.cda）播放（2026-10-27，261026-08 #1）
//
// 为什么用 MCI 而不是 SPTI 抓轨：
//   CDA 抓轨的标准做法是 SCSI Pass-Through + READ CD(0xBE)，但本机实测**会把 USB 光驱搞掉线**
//   （err=121 超时 -> 补发 READ(10) -> err=1167 -> 设备变 CM_PROB_PHANTOM，只能物理拔插）。
//   已固化为 AGENTS.md 铁律 12。MCI 由 Windows 内部处理光驱，我们不发原生 SCSI 命令，实测稳定。
//
// ★ 为什么必须有专用线程（2026-10-27 实测踩到）：
//   MCI 的设备 alias 与**调用线程**绑定（内部依赖该线程的消息队列）。
//   rtaweb 的 HTTP 请求分散在多个工作线程：play 在 A 线程 open 成功，
//   随后另一个请求在 B 线程发 `status rlcd mode` 直接报「指定的设备未打开，或不是 MCI 设备」。
//   所以本类内部起一个**常驻工作线程**，所有 MCI 调用都投递到它执行，调用方同步等结果。
//
// ★ 诚实标注的限制（UI 必须显示）：
//   MCI 播放的音频由 Windows 直接送给**系统默认输出设备**，**不经过 RLPlayer 的 DSP 管线**
//   ⟹ 音量/EQ/升频/PEQ/房间校正对它全部无效。想要「CD 也走 DSP」必须抓轨，而抓轨有掉线风险。
//
// ★ MCI 语义实测（别踩）：
//   - `play <alias> from 5 to 5` 里的 5 被当成**秒**，不是轨道号（实测 position 跳到 05:00:00）。
//     正确：先 `status <alias> position track N` 取该轨起点 MSF，再 `play from <起点> to <终点>`。
//   - `status <alias> position` 是**绝对** MSF，轨内位置要减掉该轨起点。
//   - `status <alias> length` 返回整碟总长；单轨时长 = 下一轨起点 − 本轨起点。
#pragma once
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <windows.h>
#include <mmsystem.h>

namespace rta::audio {

struct CdaState {
    bool        opened = false;
    bool        playing = false;
    long long   posMs = 0;      // 轨内位置
    long long   lenMs = 0;      // 本轨时长
    int         tracks = 0;
    int         track = 0;
    std::string rawMode, rawPos;
};

class CdaMciPlayer {
public:
    CdaMciPlayer() = default;
    ~CdaMciPlayer() { shutdown(); }
    CdaMciPlayer(const CdaMciPlayer&) = delete;
    CdaMciPlayer& operator=(const CdaMciPlayer&) = delete;

    std::string play(const std::string& cdaPath) {
        std::string letter;
        int track = 0;
        if (!parseCdaPath(cdaPath, letter, track))
            return "不是可识别的 .cda 路径（需要形如 H:\\Track01.cda）";
        std::string err;
        call([&] { err = playOnWorker(letter, track); });
        return err;
    }
    void stop()   { call([&] { if (opened_) mci("stop rlcd", nullptr, 0); }); }
    void pause()  { call([&] { if (opened_) mci("pause rlcd", nullptr, 0); }); }
    void resume() { call([&] { if (opened_) mci("resume rlcd", nullptr, 0); }); }
    void close()  {
        call([&] {
            if (opened_) {
                mci("stop rlcd", nullptr, 0);
                mci("close rlcd", nullptr, 0);
                opened_ = false;
            }
        });
    }

    CdaState state() {
        CdaState s;
        call([&] {
            s.opened = opened_;
            s.track  = track_;
            s.tracks = tracks_;
            if (!opened_) return;
            char buf[64] = {0};
            if (mci("status rlcd mode", buf, sizeof(buf))) {
                s.rawMode = buf;
                s.playing = (s.rawMode == "playing");
            }
            char pbuf[64] = {0};
            if (mci("status rlcd position", pbuf, sizeof(pbuf))) {
                s.rawPos = pbuf;
                const long long abs = msfToMs(pbuf);
                s.posMs = abs > (long long)startMs_ ? abs - (long long)startMs_ : 0;
            }
            s.lenMs = (endMs_ > startMs_) ? (long long)(endMs_ - startMs_) : 0;
        });
        return s;
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lk(mx_);
            if (!th_.joinable()) return;
            quit_ = true;
        }
        cv_.notify_all();
        th_.join();
    }

    static bool parseCdaPath(const std::string& p, std::string& letter, int& track) {
        if (p.size() < 4) return false;
        const size_t dot = p.find_last_of('.');
        if (dot == std::string::npos) return false;
        std::string ext = p.substr(dot);
        for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext != ".cda") return false;
        letter = std::string(1, (char)std::toupper((unsigned char)p[0]));
        const size_t slash = p.find_last_of("\\/");
        const std::string base = (slash == std::string::npos) ? p : p.substr(slash + 1);
        int n = 0; bool got = false;
        for (char c : base) {
            if (c >= '0' && c <= '9') { n = n * 10 + (c - '0'); got = true; }
            else if (got) break;
        }
        if (!got) return false;
        track = n;
        return track >= 1 && track <= 99;
    }

private:
    void call(const std::function<void()>& fn) {
        ensureThread();
        std::mutex m; std::condition_variable c; bool done = false;
        {
            std::lock_guard<std::mutex> lk(mx_);
            q_.push_back([&] { fn(); { std::lock_guard<std::mutex> l2(m); done = true; } c.notify_one(); });
        }
        cv_.notify_all();
        std::unique_lock<std::mutex> lk(m);
        c.wait(lk, [&] { return done; });
    }
    void ensureThread() {
        std::lock_guard<std::mutex> lk(mx_);
        if (th_.joinable()) return;
        th_ = std::thread([this] {
            MSG msg;
            PeekMessageA(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
            for (;;) {
                std::function<void()> job;
                {
                    std::unique_lock<std::mutex> lk2(mx_);
                    cv_.wait(lk2, [&] { return quit_ || !q_.empty(); });
                    if (quit_ && q_.empty()) return;
                    job = std::move(q_.front());
                    q_.pop_front();
                }
                job();
                while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageA(&msg);
                }
            }
        });
    }

    std::string playOnWorker(const std::string& letter, int track) {
        if (opened_) {
            mci("stop rlcd", nullptr, 0);
            mci("close rlcd", nullptr, 0);
            opened_ = false;
        }
        char buf[512] = {0};
        char cmd[256];
        std::snprintf(cmd, sizeof(cmd), "open %s: type cdaudio alias rlcd", letter.c_str());
        if (!mci(cmd, buf, sizeof(buf))) return std::string("打开 CD 设备失败：") + buf;
        opened_ = true;
        tracks_ = 0;
        if (mci("status rlcd number of tracks", buf, sizeof(buf))) tracks_ = std::atoi(buf);
        if (tracks_ <= 0) { mci("close rlcd", nullptr, 0); opened_ = false; return "读不到轨道数（光驱里没有音频 CD？）"; }
        if (track > tracks_) track = tracks_;
        char start[64] = {0}, end[64] = {0};
        std::snprintf(cmd, sizeof(cmd), "status rlcd position track %d", track);
        if (!mci(cmd, start, sizeof(start))) { mci("close rlcd", nullptr, 0); opened_ = false; return "取轨道起点失败"; }
        if (track < tracks_) {
            std::snprintf(cmd, sizeof(cmd), "status rlcd position track %d", track + 1);
            if (!mci(cmd, end, sizeof(end))) { mci("close rlcd", nullptr, 0); opened_ = false; return "取轨道终点失败"; }
        } else {
            if (!mci("status rlcd length", end, sizeof(end))) { mci("close rlcd", nullptr, 0); opened_ = false; return "取总长失败"; }
        }
        std::snprintf(cmd, sizeof(cmd), "play rlcd from %s to %s", start, end);
        if (!mci(cmd, buf, sizeof(buf))) {
            mci("close rlcd", nullptr, 0); opened_ = false;
            return std::string("播放第 ") + std::to_string(track) + " 轨失败：" + buf;
        }
        track_ = track;
        startMs_ = msfToMs(start);
        endMs_   = msfToMs(end);
        return {};
    }

    static bool mci(const char* cmd, char* out, DWORD outLen) {
        const MCIERROR e = mciSendStringA(cmd, out, outLen, nullptr);
        if (e != 0) {
            if (out && outLen) {
                char why[200] = {0};
                mciGetErrorStringA(e, why, sizeof(why));
                std::snprintf(out, outLen, "%s", why);
            }
            return false;
        }
        return true;
    }
    static long long msfToMs(const char* s) {
        int m = 0, sec = 0, f = 0;
        if (std::sscanf(s, "%d:%d:%d", &m, &sec, &f) != 3) return 0;
        return (long long)(((m * 60LL + sec) * 1000LL) + (f * 1000LL / 75LL));
    }

    std::thread th_;
    std::mutex mx_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
    bool quit_ = false;
    bool opened_ = false;
    int  track_ = 0;
    int  tracks_ = 0;
    int  startMs_ = 0;
    int  endMs_ = 0;
};

}  // namespace rta::audio
