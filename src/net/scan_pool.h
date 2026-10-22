// scan_pool.h — 库扫描专用固定大小工作线程池（带背压与取消机制）
//
// 设计要点（ROADMAP P6a）：
//   * 固定大小工作线程池，默认 min(hardware_concurrency, 8)
//   * 有界任务队列（默认 capacity = max(threads * 2, 16)），提供背压，避免海量文件占满内存
//   * 支持取消（cancel）：立即清空等待队列并唤醒所有工作线程退出，不残留线程
//   * 进度计数（completedTasks）：严格按任务完成数推进，不按提交数推进
//   * 异常安全：工作任务抛出的异常被捕获记录（lastException / rethrowIfError），不静默吞错
//   * 零 Windows API 依赖，完全基于标准库 <thread> / <mutex> / <condition_variable>
#pragma once

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <exception>
#include <stdexcept>
#include <algorithm>
#include <cstddef>

namespace rta::net {

class ScanPool {
public:
    using Task = std::function<void()>;

    explicit ScanPool(size_t n_threads = 0, size_t queue_capacity = 0) {
        if (n_threads == 0) {
            unsigned int h = std::thread::hardware_concurrency();
            n_threads = std::min(std::max(1u, h), 8u);
        }
        numThreads_ = n_threads;
        if (queue_capacity == 0) {
            queue_capacity = std::max<size_t>(numThreads_ * 2, 16);
        }
        capacity_ = queue_capacity;

        workers_.reserve(numThreads_);
        for (size_t i = 0; i < numThreads_; ++i) {
            workers_.emplace_back([this]() { workerLoop(); });
        }
    }

    ~ScanPool() {
        cancel();
        join();
    }

    ScanPool(const ScanPool&) = delete;
    ScanPool& operator=(const ScanPool&) = delete;

    size_t threadCount() const { return numThreads_; }
    size_t queueCapacity() const { return capacity_; }

    // 提交任务（带背压）：若队列已满则阻塞等待，直到有空位；若池已取消/停止则返回 false
    bool submit(Task task) {
        std::unique_lock<std::mutex> lock(mtx_);
        cvNotFull_.wait(lock, [this]() {
            return queue_.size() < capacity_ || stop_.load() || cancel_.load();
        });
        if (stop_.load() || cancel_.load()) {
            return false;
        }
        queue_.push(std::move(task));
        submittedTasks_.fetch_add(1, std::memory_order_relaxed);
        cvNotEmpty_.notify_one();
        return true;
    }

    // 等待所有已提交任务完成并停止工作线程；rethrow 为 true 时若有任务异常则重抛
    void wait(bool rethrow = true) {
        {
            std::unique_lock<std::mutex> lock(mtx_);
            stop_.store(true);
            cvNotEmpty_.notify_all();
            cvNotFull_.notify_all();
        }
        join();
        if (rethrow) {
            rethrowIfError();
        }
    }

    // 取消扫描：清空待执行队列，唤醒所有阻塞线程，让工作线程尽快退出
    void cancel() {
        cancel_.store(true);
        stop_.store(true);
        {
            std::lock_guard<std::mutex> lock(mtx_);
            while (!queue_.empty()) queue_.pop();
        }
        cvNotFull_.notify_all();
        cvNotEmpty_.notify_all();
    }

    void join() {
        for (auto& th : workers_) {
            if (th.joinable()) {
                th.join();
            }
        }
        workers_.clear();
    }

    bool isCancelled() const { return cancel_.load(); }
    bool isStopped()   const { return stop_.load(); }

    // 进度计数：completedTasks 反映实际执行完成的任务数，不按提交数推进
    size_t completedTasks() const { return completedTasks_.load(std::memory_order_relaxed); }
    size_t submittedTasks() const { return submittedTasks_.load(std::memory_order_relaxed); }

    bool hasError() const {
        std::lock_guard<std::mutex> lock(errMtx_);
        return static_cast<bool>(lastException_);
    }

    void rethrowIfError() {
        std::lock_guard<std::mutex> lock(errMtx_);
        if (lastException_) {
            std::rethrow_exception(lastException_);
        }
    }

    std::exception_ptr lastException() const {
        std::lock_guard<std::mutex> lock(errMtx_);
        return lastException_;
    }

private:
    void workerLoop() {
        while (true) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(mtx_);
                cvNotEmpty_.wait(lock, [this]() {
                    return !queue_.empty() || stop_.load() || cancel_.load();
                });
                if (cancel_.load()) {
                    return;
                }
                if (queue_.empty() && stop_.load()) {
                    return;
                }
                if (queue_.empty()) {
                    continue;
                }
                task = std::move(queue_.front());
                queue_.pop();
                cvNotFull_.notify_one();
            }

            if (task) {
                try {
                    task();
                } catch (...) {
                    std::lock_guard<std::mutex> elock(errMtx_);
                    if (!lastException_) {
                        lastException_ = std::current_exception();
                    }
                }
                completedTasks_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    size_t numThreads_ = 0;
    size_t capacity_ = 0;
    std::vector<std::thread> workers_;
    std::queue<Task> queue_;
    mutable std::mutex mtx_;
    std::condition_variable cvNotEmpty_;
    std::condition_variable cvNotFull_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> cancel_{false};
    std::atomic<size_t> submittedTasks_{0};
    std::atomic<size_t> completedTasks_{0};

    mutable std::mutex errMtx_;
    std::exception_ptr lastException_;
};

} // namespace rta::net
