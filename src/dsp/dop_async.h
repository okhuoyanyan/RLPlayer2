// One outstanding PCM->DoP job, permitting next-block producer work to overlap.
// No device access. The producer alone calls this API; mods must outlive us and
// must not be used elsewhere while a result is pending. No detached threads.
#pragma once
#include "dop_stream.h"
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>

namespace rta::dsp {
class DopAsyncStream {
public:
    struct Result {
        std::vector<int32_t> words;
        double wallSeconds=0; // process only, excluding queue/wait/result retrieval
        size_t pendingBitsAfter=0;
    };
    // Factory must return one joinable thread that invokes the supplied entry
    // exactly once. Optional seam for resource-failure and CV ordering tests.
    using ThreadFactory=std::function<std::thread(std::function<void()>)>;
    explicit DopAsyncStream(const std::vector<std::unique_ptr<SdmModulator>>& mods,
                            ThreadFactory factory={}) : mods_(mods),factory_(std::move(factory)) {}
    DopAsyncStream(const DopAsyncStream&)=delete;
    DopAsyncStream& operator=(const DopAsyncStream&)=delete;
    ~DopAsyncStream() noexcept {
        {std::lock_guard<std::mutex> lock(mutex_);stopping_=true;}
        work_.notify_one();
        // worker consumes an outstanding job before observing shutdown.
        if(worker_.joinable())worker_.join();
    }
    void submit(std::vector<std::vector<double>> planes,size_t drop,bool finish) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(pending_)throw std::logic_error("DoP result must be taken before another submit");
        if(poisoned_)throw std::logic_error("DoP internal failure requires reset_for_seek");
        // A failed factory leaves no pending job and may be retried.
        if(!worker_.joinable()) {
            if(factory_)worker_=factory_([this]{run();});
            else worker_=std::thread([this]{run();});
            if(!worker_.joinable())throw std::logic_error("DoP thread factory returned no worker");
        }
        planes_=std::move(planes);drop_=drop;finish_=finish;
        error_=nullptr;result_=Result{};completed_=false;pending_=ready_=true;
        work_.notify_one();
    }
    bool pending() const {std::lock_guard<std::mutex> lock(mutex_);return pending_;}
    Result take() {
        std::unique_lock<std::mutex> lock(mutex_);
        if(!pending_)throw std::logic_error("No pending DoP result");
        done_.wait(lock,[this]{return completed_;});
        auto error=error_;error_=nullptr;pending_=false;completed_=false;
        Result result=std::move(result_);
        lock.unlock();
        if(error)std::rethrow_exception(error);
        return result;
    }
    // Same failure propagation as take; destructor alone suppresses failures.
    void discard(){(void)take();}
    size_t pending_bits() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if(pending_)throw std::logic_error("Cannot read DoP bit state with a pending job");
        if(poisoned_)throw std::logic_error("DoP internal failure requires reset_for_seek");
        return stream_.pending_bits();
    }
    void reset_for_seek() {
        std::lock_guard<std::mutex> lock(mutex_);
        if(pending_)throw std::logic_error("Take or discard DoP before seek");
        // An internal process exception may have partially advanced channels.
        // Rebuilding that aborted stream resets marker phase too; the caller must
        // re-establish the receiver/transport on this exceptional recovery path.
        if(poisoned_)stream_=DopModulationStream{};
        else stream_.discard_pending(); // preserve receiver marker phase
        for(const auto& m:mods_)if(m)m->reset();
        poisoned_=false;
    }
private:
    void run() noexcept {
        std::unique_lock<std::mutex> lock(mutex_);
        for(;;) {
            work_.wait(lock,[this]{return stopping_||ready_;});
            if(!ready_)return;
            auto planes=std::move(planes_);const size_t drop=drop_;const bool finish=finish_;
            ready_=false;lock.unlock();
            Result result;std::exception_ptr error;bool poisoned=false;
            try {
                const auto begin=std::chrono::steady_clock::now();std::string why;
                const bool ok=stream_.process(planes,mods_,drop,finish,result.words,&why);
                result.wallSeconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
                if(!ok)error=std::make_exception_ptr(std::invalid_argument(why));
                else result.pendingBitsAfter=stream_.pending_bits();
            } catch(...) {error=std::current_exception();poisoned=true;}
            lock.lock();result_=std::move(result);error_=error;poisoned_|=poisoned;completed_=true;
            done_.notify_one();
        }
    }
    const std::vector<std::unique_ptr<SdmModulator>>& mods_;
    ThreadFactory factory_;DopModulationStream stream_;std::thread worker_;
    mutable std::mutex mutex_;std::condition_variable work_,done_;
    std::vector<std::vector<double>> planes_;Result result_;std::exception_ptr error_;
    size_t drop_=0;bool finish_=false,pending_=false,ready_=false,completed_=false,stopping_=false,poisoned_=false;
};
} // namespace rta::dsp
