// aidecode.cpp — 把任意音频格式经 read_any 解码并写成 WAV，用于验证解码无损
#include "audio/audio_loader.h"
#include <cstdio>
#include <string>
#include <windows.h>

using namespace rta::audio;

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) { printf("用法: aidecode <输入> <输出.wav>\n"); return 2; }
    try {
        LoadNote note;
        AudioBuffer b = read_any(argv[1], &note);
        printf("输入: %s\n", argv[1]);
        printf("  解码: %s\n", note.text.empty() ? "(原生 WAV)" : note.text.c_str());
        printf("  格式: %u Hz, %d ch, %d bit, %zu 帧 (%.3f 秒)\n",
               b.rate, b.channels, b.bits, b.frames(), (double)b.frames() / (double)b.rate);
        write_wav(argv[2], b);
        printf("  已写出: %s\n", argv[2]);
        return 0;
    } catch (const std::exception& e) {
        printf("!! 失败: %s\n", e.what());
        return 1;
    }
}
