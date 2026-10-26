// cda_mci.h —— CD 音轨（.cda）播放（2026-10-27，261026-08 #1）
//
// 为什么用 MCI 而不是 SPTI 抓轨：
//   CDA 抓轨的标准做法是 SCSI Pass-Through + READ CD(0xBE)，但本机实测**会把 USB 光驱搞掉线**
//   （err=121 超时 -> 补发 READ(10) -> err=1167 -> 设备变 CM_PROB_PHANTOM，只能物理拔插）。
//   这一条已固化为 AGENTS.md 铁律 12。MCI 由 Windows 内部处理光驱，我们不发任何原生 SCSI 命令，
//   实测 open/status/seek/play 全部成功且光驱稳定，所以 CD 播放走 MCI。
//
// ★ 诚实标注的限制（UI 必须显示）：
//   MCI 播放的音频由 Windows 直接送给**系统默认输出设备**，**不经过 RLPlayer 的 DSP 管线**
//   ⟹ 音量/EQ/升频/PEQ/房间校正对它全部无效。想要「CD 也走 DSP」必须抓轨，而抓轨有掉线风险。
//
// ★ MCI 语义实测（2026-10-27，别踩）：
//   - `play <alias> from 5 to 5` 里的 5 被当成**秒**，不是轨道号（实测 position 跳到 05:00:00）。
//     正确：先用 `status <alias> position track N` 取该轨起点 MSF，再 `play from <起点> to <终点>`。
//   - `status <alias> position` 是**绝对** MSF（整碟坐标），轨内位置要减掉该轨起点。
//   - `status <alias> length` 返回整碟总长；单轨时长 = 下一轨起点 − 本轨起点。
//
// .cda 是 44 字节索引文件，本身不含音频；真正的音频在 CDDA 轨道里。
// 轨道号从文件名 TrackNN.cda 取（Windows 生成的就是这个格式）。
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <string>
#include <windows.h>
#include <mmsystem.h>

namespace rta::audio {

class CdaMciPlayer {
public:
    ~CdaMciPlayer() { close(); }

    // 从 .cda 路径起动播放。返回空串 = 成功；否则是人话错误。
    std::string play(const std::string& cdaPath) {
        close();
        std::string letter;
        int track = 0;
        if (!parseCdaPath(cdaPath, letter, track))
            return "不是可识别的 .cda 路径（需要形如 H:\\Track01.cda）";
        char buf[512] = {0};
        char cmd[256];
        std::snprintf(cmd, sizeof(cmd), "open %s: type cdaudio alias rlcd", letter.c_str());
        if (!mci(cmd, buf, sizeof(buf))) return std::string("打开 CD 设备失败：") + buf;

        tracks_ = 0;
        if (mci("status rlcd number of tracks", buf, sizeof(buf))) tracks_ = std::atoi(buf);
        if (tracks_ <= 0) {
            mci("close rlcd", nullptr, 0);
            return "读不到轨道数（光驱里没有音频 CD？）";
        }
        if (track > tracks_) track = tracks_;
        char start[64] = {0}, end[64] = {0};
        std::snprintf(cmd, sizeof(cmd), "status rlcd position track %d", track);
        if (!mci(cmd, start, sizeof(start))) { mci("close rlcd", nullptr, 0); return "取轨道起点失败"; }
        if (track < tracks_) {
            std::snprintf(cmd, sizeof(cmd), "status rlcd position track %d", track + 1);
            if (!mci(cmd, end, sizeof(end))) { mci("close rlcd", nullptr, 0); return "取轨道终点失败"; }
        } else {
            if (!mci("status rlcd length", end, sizeof(end))) { mci("close rlcd", nullptr, 0); return "取总长失败"; }
        }
        std::snprintf(cmd, sizeof(cmd), "play rlcd from %s to %s", start, end);
        if (!mci(cmd, buf, sizeof(buf))) {
            mci("close rlcd", nullptr, 0);
            return std::string("播放第 ") + std::to_string(track) + " 轨失败：" + buf;
        }
        open_ = true;
        track_ = track;
        letter_ = letter;
        trackStartMs_ = msfToMs(start);
        trackEndMs_   = msfToMs(end);
        return {};
    }

    void stop()  { if (open_) { char b[64] = {0}; mci("stop rlcd", b, sizeof(b)); } }
    void pause() { if (open_) { char b[64] = {0}; mci("pause rlcd", b, sizeof(b)); } }
    void resume(){ if (open_) { char b[64] = {0}; mci("resume rlcd", b, sizeof(b)); } }
    void close() {
        if (open_) {
            char b[64] = {0};
            mci("stop rlcd", b, sizeof(b));
            mci("close rlcd", b, sizeof(b));
            open_ = false;
        }
    }

    bool playing() const {
        if (!open_) return false;
        char buf[64] = {0};
        if (!mci("status rlcd mode", buf, sizeof(buf))) return false;
        return std::string(buf) == "playing";
    }
    // 诊断用：原样返回 MCI 的 mode / position 字符串（排查播放立即结束等问题）
    std::string rawMode() const {
        if (!open_) return "<closed>";
        char buf[64] = {0};
        if (!mci("status rlcd mode", buf, sizeof(buf))) return std::string("<err>") + buf;
        return std::string(buf);
    }
    std::string rawPosition() const {
        if (!open_) return "<closed>";
        char buf[64] = {0};
        if (!mci("status rlcd position", buf, sizeof(buf))) return std::string("<err>") + buf;
        return std::string(buf);
    }
    // 当前**轨内**播放位置（毫秒）：MCI position 是绝对 MSF，要减掉该轨起点
    long long positionMs() const {
        if (!open_) return 0;
        char buf[64] = {0};
        if (!mci("status rlcd position", buf, sizeof(buf))) return 0;
        const long long p = msfToMs(buf) - (long long)trackStartMs_;
        return p > 0 ? p : 0;
    }
    // 本轨时长（毫秒）
    long long lengthMs() const {
        if (!open_) return 0;
        return (trackEndMs_ > trackStartMs_) ? (long long)(trackEndMs_ - trackStartMs_) : 0;
    }
    int  trackCount() const { return tracks_; }
    int  track() const { return track_; }
    bool opened() const { return open_; }

    // "H:\\Track01.cda" -> letter="H", track=1
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
    // MCI 返回 "mm:ss:ff"（分:秒:帧，帧 = 1/75 秒）
    static long long msfToMs(const char* s) {
        int m = 0, sec = 0, f = 0;
        if (std::sscanf(s, "%d:%d:%d", &m, &sec, &f) != 3) return 0;
        return (long long)(((m * 60LL + sec) * 1000LL) + (f * 1000LL / 75LL));
    }

    bool open_ = false;
    int  track_ = 0;
    int  tracks_ = 0;
    int  trackStartMs_ = 0;
    int  trackEndMs_ = 0;
    std::string letter_;
};

}  // namespace rta::audio
