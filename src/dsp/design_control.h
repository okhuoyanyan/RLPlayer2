#pragma once
#include <atomic>
#include <stdexcept>

namespace rta::dsp {

// Design-only cooperative cancellation. Playback leaves the thread-local flag
// null. A caller owns the flag until its design worker has joined.
class DesignCancelled : public std::runtime_error {
public:
    DesignCancelled() : std::runtime_error("Filter preparation cancelled") {}
};

inline thread_local const std::atomic_bool* design_cancel_flag = nullptr;

class DesignCancellationScope {
    const std::atomic_bool* previous_;
public:
    explicit DesignCancellationScope(const std::atomic_bool* flag)
        : previous_(design_cancel_flag) {
        if (flag) design_cancel_flag = flag;
    }
    ~DesignCancellationScope() { design_cancel_flag = previous_; }
    DesignCancellationScope(const DesignCancellationScope&) = delete;
    DesignCancellationScope& operator=(const DesignCancellationScope&) = delete;
};

inline void check_design_cancel() {
    if (design_cancel_flag && design_cancel_flag->load(std::memory_order_relaxed))
        throw DesignCancelled();
}

} // namespace rta::dsp
