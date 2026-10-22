// test_md5.cpp — MD5 对 RFC 1321 标准测试向量
#include "net/md5.h"
#include <cstdio>
#include <string>

using rta::net::md5_hex;

static int fails = 0;
static void ck(const std::string& in, const std::string& want) {
    const std::string got = md5_hex(in);
    const bool ok = (got == want);
    if (!ok) ++fails;
    printf("  [%s] \"%s\" -> %s\n", ok ? "PASS" : "FAIL", in.substr(0, 28).c_str(), got.c_str());
    if (!ok) printf("         期望 %s\n", want.c_str());
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("=== RFC 1321 标准向量 ===\n");
    ck("", "d41d8cd98f00b204e9800998ecf8427e");
    ck("a", "0cc175b9c0f1b6a831c399e269772661");
    ck("abc", "900150983cd24fb0d6963f7d28e17f72");
    ck("message digest", "f96b697d7cb7938d525a2f31aaf161d0");
    ck("abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b");
    ck("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
       "d174ab98d277d9f5a5611c2c9f419d9f");
    ck("12345678901234567890123456789012345678901234567890123456789012345678901234567890",
       "57edf4a22be3c955ac49da2e2107b67a");
    printf("\n%s (失败 %d)\n", fails == 0 ? "全部通过" : "存在失败", fails);
    return fails == 0 ? 0 : 1;
}
