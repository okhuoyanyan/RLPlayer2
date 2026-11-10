// Scoped end-to-end network budget. No scope means existing caller behavior.
#pragma once
#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>
namespace rta::net {
using HttpDeadline = std::chrono::steady_clock::time_point;
inline HttpDeadline& http_deadline_slot() { static thread_local HttpDeadline d{}; return d; }
class HttpBudgetScope {
    HttpDeadline previous_;
public:
    explicit HttpBudgetScope(int milliseconds) : previous_(http_deadline_slot()) {
        const auto d = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(0, milliseconds));
        http_deadline_slot() = previous_ == HttpDeadline{} ? d : std::min(previous_, d);
    }
    ~HttpBudgetScope() { http_deadline_slot() = previous_; }
    HttpBudgetScope(const HttpBudgetScope&) = delete;
    HttpBudgetScope& operator=(const HttpBudgetScope&) = delete;
};
inline bool http_budget_active() { return http_deadline_slot() != HttpDeadline{}; }
inline bool http_budget_expired() { return http_budget_active() && std::chrono::steady_clock::now() >= http_deadline_slot(); }
inline int http_budget_timeout(int requested) {
    if (!http_budget_active()) return requested;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(http_deadline_slot() - std::chrono::steady_clock::now()).count();
    return left <= 0 ? 0 : (int)std::min<long long>(std::max(1, requested), left);
}
inline bool http_budget_sleep(std::chrono::steady_clock::duration delay) {
    const auto until = std::chrono::steady_clock::now() + delay;
    std::this_thread::sleep_until(http_budget_active() ? std::min(until, http_deadline_slot()) : until);
    return !http_budget_expired();
}
inline bool http_budget_lock(std::unique_lock<std::mutex>& lock) {
    if (!http_budget_active()) { lock.lock(); return true; }
    while (!http_budget_expired()) {
        if (lock.try_lock()) return true;
        http_budget_sleep(std::chrono::milliseconds(2));
    }
    return false;
}
} // namespace rta::net
