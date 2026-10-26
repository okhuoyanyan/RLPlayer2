// test_llmtools.cpp — DeepSeek Search/Fetch「工具调用链」往返验证（261026-06 #2）
//
// 判据（铁律 3）：不能只测解析函数。这里起一个**真的本地 HTTP 服务端**（127.0.0.1 随机端口），
// 让 llm_chat_tool_loop 走真实 http_post → winhttp → 本机 socket 的完整链路：
//   ① 请求体里必须带 tools 定义（web_search/web_fetch）与 tool_choice=auto；
//   ② 服务端回 tool_calls → 本地执行 search → 结果以 role="tool" 回填（带 tool_call_id）；
//   ③ 第二轮请求里必须出现 assistant 的 tool_calls 回显 + tool 消息；
//   ④ 最终拿到模型文本。
// 全程离线：不需要真 KEY（用假的 sk-test），不碰用户配置、不写用量到真实路径。
//
// 另测降级：工具端点 500、连接被拒（端口无人监听）、无 key、轮数耗尽 —— 都必须快速失败、
// 不崩、不阻塞（每条都打印实测耗时，判据 < 10s）。
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include "meta/llm.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace rta::meta;
namespace fs = std::filesystem;

static int g_fail = 0;
static void ck(bool cond, const char* what, const std::string& detail = "") {
    if (cond) std::printf("  [PASS] %s\n", what);
    else { std::printf("  [FAIL] %s  %s\n", what, detail.c_str()); ++g_fail; }
}

struct MockReq { std::string method, path, body; };

// 极简 HTTP/1.1 假服务端：一次一个连接，读完请求 → 按路径给固定 JSON → Connection: close
class MockServer {
public:
    bool start() {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        listen_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_ == INVALID_SOCKET) return false;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        if (::bind(listen_, (sockaddr*)&a, sizeof(a)) != 0) return false;
        if (::listen(listen_, 16) != 0) return false;
        int len = (int)sizeof(a);
        ::getsockname(listen_, (sockaddr*)&a, &len);
        port_ = ntohs(a.sin_port);
        th_ = std::thread([this] { loop(); });
        return true;
    }
    void stop() {
        stop_ = true;
        if (listen_ != INVALID_SOCKET) { ::closesocket(listen_); listen_ = INVALID_SOCKET; }
        if (th_.joinable()) th_.join();
        ::WSACleanup();
    }
    uint16_t port() const { return port_; }

    // 脚本：按顺序消费；队列空 → 500
    void push_chat(const std::string& json) { std::lock_guard<std::mutex> lk(m_); chats_.push_back(json); }
    void clear_chats() { std::lock_guard<std::mutex> lk(m_); chats_.clear(); }
    void set_search(int status, const std::string& json) { std::lock_guard<std::mutex> lk(m_); searchStatus_ = status; search_ = json; }
    void set_fetch(int status, const std::string& json) { std::lock_guard<std::mutex> lk(m_); fetchStatus_ = status; fetch_ = json; }
    std::vector<MockReq> reqs() { std::lock_guard<std::mutex> lk(m_); return reqs_; }
    size_t count() { std::lock_guard<std::mutex> lk(m_); return reqs_.size(); }

private:
    void loop() {
        while (!stop_) {
            SOCKET c = ::accept(listen_, nullptr, nullptr);
            if (c == INVALID_SOCKET) break;
            serve(c);
            ::closesocket(c);
        }
    }
    void serve(SOCKET c) {
        DWORD tv = 3000;
        ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
        std::string buf;
        char tmp[4096];
        size_t hdrEnd = std::string::npos;
        size_t contentLen = 0;
        // 读到 header 完整
        while ((hdrEnd = buf.find("\r\n\r\n")) == std::string::npos) {
            const int n = ::recv(c, tmp, sizeof(tmp), 0);
            if (n <= 0) return;
            buf.append(tmp, (size_t)n);
            if (buf.size() > 4u * 1024 * 1024) return;
        }
        // 解析 request line 与 Content-Length
        MockReq req;
        {
            const std::string head = buf.substr(0, hdrEnd);
            size_t sp1 = head.find(' ');
            size_t sp2 = (sp1 == std::string::npos) ? std::string::npos : head.find(' ', sp1 + 1);
            if (sp1 != std::string::npos && sp2 != std::string::npos) {
                req.method = head.substr(0, sp1);
                req.path = head.substr(sp1 + 1, sp2 - sp1 - 1);
            }
            std::string lower = head;
            for (auto& ch : lower) ch = (char)std::tolower((unsigned char)ch);
            const size_t cl = lower.find("content-length:");
            if (cl != std::string::npos) contentLen = (size_t)atoi(head.c_str() + cl + 15);
        }
        std::string body = buf.substr(hdrEnd + 4);
        while (body.size() < contentLen) {
            const int n = ::recv(c, tmp, sizeof(tmp), 0);
            if (n <= 0) break;
            body.append(tmp, (size_t)n);
        }
        req.body = body;

        int status = 200;
        std::string resp;
        {
            std::lock_guard<std::mutex> lk(m_);
            reqs_.push_back(req);
            if (req.path.find("/chat/completions") != std::string::npos) {
                if (chats_.empty()) { status = 500; resp = "{\"error\":{\"message\":\"mock: no scripted chat left\"}}"; }
                else { resp = chats_.front(); chats_.erase(chats_.begin()); }
            } else if (req.path.find("/v1/tools/search") != std::string::npos) {
                status = searchStatus_;
                resp = search_;
            } else if (req.path.find("/v1/tools/fetch") != std::string::npos) {
                status = fetchStatus_;
                resp = fetch_;
            } else {
                status = 404;
                resp = "{\"error\":{\"message\":\"mock: unknown path\"}}";
            }
        }
        char head[256];
        const char* reason = (status == 200) ? "OK" : (status == 500 ? "Internal Server Error" : "Not Found");
        std::snprintf(head, sizeof(head),
                      "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                      status, reason, resp.size());
        ::send(c, head, (int)std::strlen(head), 0);
        if (!resp.empty()) ::send(c, resp.data(), (int)resp.size(), 0);
    }

    SOCKET listen_ = INVALID_SOCKET;
    uint16_t port_ = 0;
    std::atomic<bool> stop_{ false };
    std::thread th_;
    std::mutex m_;
    std::vector<MockReq> reqs_;
    std::vector<std::string> chats_;
    std::string search_, fetch_;
    int searchStatus_ = 200, fetchStatus_ = 200;
};

// ---- 固定报文 ----
static std::string chat_tool_call(const std::string& name, const std::string& argsJson, const std::string& id) {
    return std::string("{\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":null,") +
           "\"tool_calls\":[{\"id\":\"" + id + "\",\"type\":\"function\",\"function\":{\"name\":\"" + name +
           "\",\"arguments\":\"" + llm_esc(argsJson) + "\"}}]},\"finish_reason\":\"tool_calls\"}]," +
           "\"usage\":{\"total_tokens\":42}}";
}
static std::string chat_final(const std::string& text) {
    return std::string("{\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"") +
           llm_esc(text) + "\"},\"finish_reason\":\"stop\"}],\"usage\":{\"total_tokens\":99}}";
}
static std::string search_body() {
    std::string s = "{\"results\":[{";
    s += "\"rerank_score\":1.95,\"title\":\"Winged Step - dizzylab\",";
    s += "\"url\":\"https://www.dizzylab.net/d/WSD0001/\",";
    s += "\"snippet\":\"同人专辑 Winged Step\",";
    s += "\"summary\":\"Winged Step 是同人社团作品，发行于 2019 年。\",";
    s += "\"content\":\"曲目 01 Intro / 02 Winged Step …\",";
    s += "\"site_name\":\"dizzylab\",\"date\":\"2019-05-01T00:00:00+00:00\",\"authority\":2}]}";
    return s;
}
static std::string fetch_body() {
    std::string s = "{\"title\":\"Winged Step | dizzylab\",";
    s += "\"content\":\"# Winged Step\\n发行: 2019-05-01\\n厂牌: 自作自演\\n曲目: 8 首\"}";
    return s;
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    std::printf("== DeepSeek search/fetch 工具调用链（本地假服务端，离线）==\n");
    const fs::path tmp = fs::temp_directory_path() / "rta_llmtools_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);

    MockServer srv;
    if (!srv.start()) { std::printf("  [FAIL] 假服务端启动失败\n"); return 1; }
    char pbuf[128];
    std::snprintf(pbuf, sizeof(pbuf), "(127.0.0.1:%u)", srv.port());
    std::printf("  假服务端 %s\n", pbuf);

    auto make_cfg = [&]() {
        LlmConfig c;
        c.baseUrl = "http://127.0.0.1:" + std::to_string(srv.port());
        c.apiKey = "sk-test-not-a-real-key";
        c.model = "deepseek-flash";
        c.usagePath = (tmp / "usage.json").string();
        return c;
    };

    // ---------------- 1) web_search 往返 ----------------
    std::printf("\n-- 1) 模型要 web_search → 本地执行 → 回填 → 最终回答\n");
    srv.clear_chats();
    srv.set_search(200, search_body());
    srv.push_chat(chat_tool_call("web_search", "{\"query\":\"Winged Step\",\"site\":\"dizzylab.net\",\"count\":5}", "call_1"));
    srv.push_chat(chat_final("Winged Step 由同人社团发行于 2019 年（来源 dizzylab）。"));
    {
        const LlmConfig cfg = make_cfg();
        std::vector<LlmMessage> msgs{ {"user", "Winged Step 是哪一年发行的？"} };
        const auto t0 = std::chrono::steady_clock::now();
        const LlmToolLoopResult r = llm_chat_tool_loop(cfg, msgs, 400, 0.2f, 4, 15000, 8000);
        const long long ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        ck(r.ok, "环路收敛（ok=true）", r.error);
        ck(r.rounds == 2, "2 轮 chat", "rounds=" + std::to_string(r.rounds));
        ck(r.toolCalls == 1, "执行了 1 次工具", "calls=" + std::to_string(r.toolCalls));
        ck(r.content.find("2019") != std::string::npos, "最终回答含工具给出的事实", r.content);
        std::printf("       实测 %lld ms  %s\n", ms, r.diag.c_str());

        const auto reqs = srv.reqs();
        ck(reqs.size() >= 3, "服务端收到 >=3 个请求（chat/search/chat）",
           "n=" + std::to_string(reqs.size()));
        if (reqs.size() >= 3) {
            ck(reqs[0].body.find("\"tools\":[") != std::string::npos, "第 1 轮请求带 tools 定义");
            ck(reqs[0].body.find("\"tool_choice\":\"auto\"") != std::string::npos, "第 1 轮带 tool_choice=auto");
            ck(reqs[0].body.find("web_search") != std::string::npos &&
               reqs[0].body.find("web_fetch") != std::string::npos, "两个工具都在声明里");
            ck(reqs[1].path.find("/v1/tools/search") != std::string::npos, "第 2 个请求打到 /v1/tools/search", reqs[1].path);
            ck(reqs[1].body.find("\"need_content\":true") != std::string::npos, "search 带 need_content=true");
            ck(reqs[1].body.find("site:dizzylab.net") != std::string::npos, "search 使用了 site: 语法", reqs[1].body);
            ck(reqs[2].body.find("\"role\":\"tool\"") != std::string::npos, "第 3 个请求含 role=tool 消息");
            ck(reqs[2].body.find("\"tool_call_id\":\"call_1\"") != std::string::npos, "tool 消息带 tool_call_id");
            ck(reqs[2].body.find("dizzylab.net/d/WSD0001") != std::string::npos, "工具结果（URL）已回填给模型");
            ck(reqs[2].body.find("\"tool_calls\":[") != std::string::npos, "assistant 的 tool_calls 已回显");
        }
    }

    // ---------------- 2) web_fetch 往返 ----------------
    std::printf("\n-- 2) 模型要 web_fetch → 本地抓正文 → 回填 → 最终回答\n");
    srv.clear_chats();
    srv.set_fetch(200, fetch_body());
    srv.push_chat(chat_tool_call("web_fetch", "{\"url\":\"https://www.dizzylab.net/d/WSD0001/\"}", "call_f1"));
    srv.push_chat(chat_final("发行日期 2019-05-01，共 8 首。"));
    {
        const LlmConfig cfg = make_cfg();
        std::vector<LlmMessage> msgs{ {"user", "读一下这个页面"} };
        const size_t before = srv.count();
        const LlmToolLoopResult r = llm_chat_tool_loop(cfg, msgs, 400, 0.2f, 4, 15000, 8000);
        ck(r.ok, "web_fetch 环路收敛", r.error);
        ck(r.toolCalls == 1, "执行了 1 次 web_fetch");
        const auto reqs = srv.reqs();
        ck(reqs.size() >= before + 2, "收到 fetch + 第二轮 chat");
        bool sawFetch = false, sawContent = false;
        for (size_t i = before; i < reqs.size(); ++i) {
            if (reqs[i].path.find("/v1/tools/fetch") != std::string::npos) {
                sawFetch = true;
                ck(reqs[i].body.find("dizzylab.net/d/WSD0001") != std::string::npos, "fetch 请求体带 url", reqs[i].body);
            }
            if (reqs[i].path.find("/chat/completions") != std::string::npos &&
                reqs[i].body.find("2019-05-01") != std::string::npos) sawContent = true;
        }
        ck(sawFetch, "确实调用了 /v1/tools/fetch");
        ck(sawContent, "fetch 的正文已回填进下一轮 chat 请求");
    }

    // ---------------- 3) 工具端点 500：仍要收敛 ----------------
    std::printf("\n-- 3) 降级：search 端点 500 → 工具结果 {\"ok\":false} → 模型仍给最终答案\n");
    srv.clear_chats();
    srv.set_search(500, "{\"error\":{\"message\":\"mock boom\"}}");
    srv.push_chat(chat_tool_call("web_search", "{\"query\":\"找不到的东西\"}", "call_e1"));
    srv.push_chat(chat_final("没查到，资料有限。"));
    {
        const LlmConfig cfg = make_cfg();
        std::vector<LlmMessage> msgs{ {"user", "查一下不存在的东西"} };
        const auto t0 = std::chrono::steady_clock::now();
        const LlmToolLoopResult r = llm_chat_tool_loop(cfg, msgs, 400, 0.2f, 4, 15000, 8000);
        const long long ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        ck(r.ok, "工具失败不阻断（仍 ok）", r.error);
        ck(!r.trace.empty() && !r.trace[0].ok, "trace 记录该次工具失败");
        ck(!r.trace.empty() && r.trace[0].result.find("\"ok\":false") != std::string::npos,
           "回填给模型的是 ok:false 的 JSON", r.trace.empty() ? "" : r.trace[0].result);
        ck(ms < 10000, "耗时 < 10s", std::to_string(ms) + "ms");
        std::printf("       实测 %lld ms\n", ms);
    }

    // ---------------- 4) 无 key：不发任何请求 ----------------
    std::printf("\n-- 4) 降级：无 api_key → 直接失败，不发网络请求\n");
    {
        LlmConfig cfg = make_cfg();
        cfg.apiKey.clear();
        std::vector<LlmMessage> msgs{ {"user", "hi"} };
        const size_t before = srv.count();
        const LlmToolLoopResult r = llm_chat_tool_loop(cfg, msgs, 100, 0.2f, 2, 3000, 3000);
        ck(!r.ok, "无 key 时 ok=false");
        ck(r.error.find("api_key") != std::string::npos, "错误信息点明缺 api_key", r.error);
        ck(r.rounds == 0, "没有发起 chat 轮次");
        ck(srv.count() == before, "服务端没有收到新请求");
    }

    // ---------------- 5) 轮数耗尽 ----------------
    std::printf("\n-- 5) 降级：模型一直要工具（不收敛）→ 轮数上限收手\n");
    srv.clear_chats();
    srv.set_search(200, search_body());
    for (int i = 0; i < 4; ++i) srv.push_chat(chat_tool_call("web_search", "{\"query\":\"loop\"}", "call_l" + std::to_string(i)));
    {
        const LlmConfig cfg = make_cfg();
        std::vector<LlmMessage> msgs{ {"user", "loop"} };
        const auto t0 = std::chrono::steady_clock::now();
        const LlmToolLoopResult r = llm_chat_tool_loop(cfg, msgs, 100, 0.2f, 2, 10000, 5000);
        const long long ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        ck(!r.ok, "不收敛时 ok=false");
        ck(r.rounds == 2, "严格停在 maxRounds=2", "rounds=" + std::to_string(r.rounds));
        ck(!r.error.empty(), "有明确错误信息", r.error);
        ck(ms < 10000, "耗时 < 10s", std::to_string(ms) + "ms");
    }

    // ---------------- 5b) 轮数用尽 → 无工具收尾调用逼出正文 ----------------
    std::printf("\n-- 5b) 轮数用尽：补一次不带工具的收尾调用，仍要给出正文\n");
    srv.clear_chats();
    srv.set_search(200, search_body());
    srv.push_chat(chat_tool_call("web_search", "{\"query\":\"a\"}", "call_x1"));
    srv.push_chat(chat_tool_call("web_search", "{\"query\":\"b\"}", "call_x2"));
    srv.push_chat(chat_final("根据检索：Winged Step 发行于 2019 年。"));
    {
        const LlmConfig cfg = make_cfg();
        std::vector<LlmMessage> msgs{ {"user", "Winged Step 哪年发行"} };
        const size_t before = srv.count();
        const auto t0 = std::chrono::steady_clock::now();
        const LlmToolLoopResult r = llm_chat_tool_loop(cfg, msgs, 200, 0.2f, 2, 15000, 5000);
        const long long ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        ck(r.ok, "收尾调用拿到正文", r.error);
        ck(r.finalized, "标记 finalized=true");
        ck(r.rounds == 2, "工具轮数仍是 2", "rounds=" + std::to_string(r.rounds));
        ck(r.content.find("2019") != std::string::npos, "正文可用", r.content);
        // 最后一次 chat 请求必须不带 tools
        const auto reqs = srv.reqs();
        std::string lastChat;
        for (size_t i = before; i < reqs.size(); ++i)
            if (reqs[i].path.find("/chat/completions") != std::string::npos) lastChat = reqs[i].body;
        ck(!lastChat.empty() && lastChat.find("\"tools\":") == std::string::npos,
           "收尾请求不带 tools（模型只能作答）");
        ck(ms < 10000, "耗时 < 10s", std::to_string(ms) + "ms");
    }

    // ---------------- 6) 连接被拒（网络不可用）----------------
    std::printf("\n-- 6) 降级：端口无人监听（等同断网）→ 快速失败\n");
    {
        LlmConfig cfg = make_cfg();
        cfg.baseUrl = "http://127.0.0.1:1";     // 保留端口，必定连接失败
        std::vector<LlmMessage> msgs{ {"user", "hi"} };
        const auto t0 = std::chrono::steady_clock::now();
        const LlmToolLoopResult r = llm_chat_tool_loop(cfg, msgs, 100, 0.2f, 2, 8000, 3000);
        const long long ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        ck(!r.ok, "连接失败时 ok=false");
        ck(!r.error.empty(), "有明确错误信息", r.error);
        ck(ms < 10000, "耗时 < 10s（不阻塞主流程）", std::to_string(ms) + "ms");
        std::printf("       实测 %lld ms  err=%s\n", ms, r.error.c_str());
    }

    // ---------------- 7) 纯函数：tool_calls 解析与工具声明 ----------------
    std::printf("\n-- 7) 纯函数断言\n");
    {
        const LlmReply r = llm_parse_chat_response(chat_tool_call("web_search", "{\"query\":\"x y\"}", "call_z"));
        ck(r.ok, "含 tool_calls 的响应可解析");
        ck(r.toolCalls.size() == 1, "解析出 1 个 tool_call");
        ck(!r.toolCalls.empty() && r.toolCalls[0].name == "web_search", "工具名正确");
        ck(!r.toolCalls.empty() && r.toolCalls[0].arguments == "{\"query\":\"x y\"}", "参数原样保留",
           r.toolCalls.empty() ? "" : r.toolCalls[0].arguments);
        ck(r.finishReason == "tool_calls", "finish_reason=tool_calls");
        ck(r.content.empty(), "tool_calls 响应的 content 为空");

        const std::string tools = llm_web_tools_json();
        ck(tools.find("\"web_search\"") != std::string::npos && tools.find("\"web_fetch\"") != std::string::npos,
           "工具声明含两个工具");
        ck(tools.find("site:y.qq.com") != std::string::npos, "工具描述里教模型用 site: 语法");

        // 回填 JSON 的转义必须是"字符串套字符串"
        LlmToolCalls calls{ {"call_1", "web_fetch", "{\"url\":\"https://a/b\"}"} };
        const std::string j = llm_tool_calls_to_json(calls);
        ck(j.find("\"arguments\":\"{\\\"url\\\":") != std::string::npos, "arguments 是转义后的 JSON 字符串", j);
    }

    srv.stop();
    std::printf("\n== 结束：%s（失败 %d 项）==\n", g_fail == 0 ? "全部通过" : "有失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
