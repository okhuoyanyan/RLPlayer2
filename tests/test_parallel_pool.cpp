#include "dsp/parallel.h"
#include <cstdlib>
#include <chrono>
#include <set>
#include <stdexcept>
#include <cstdio>
#define CHECK(x) do { if(!(x)) { std::fprintf(stderr,"CHECK failed at line %d: %s\n",__LINE__,#x); std::abort(); } } while(false)
using rta::dsp::ParallelFor;
int main() {
    auto& pool=ParallelFor::instance();
    size_t calls=0;pool.run(0,[&](size_t){++calls;});pool.run(1,[&](size_t i){CHECK(i==0);++calls;});CHECK(calls==1);
    // Callback zero waits for a helper, guaranteeing helper observation without
    // assuming every worker wins a task or requiring all workers at a barrier.
    std::set<std::thread::id> prior;
    for(int round=0;round<10;++round) {
        std::mutex m;std::condition_variable cv;bool helperSeen=false;
        const auto caller=std::this_thread::get_id();std::set<std::thread::id> ids;
        pool.run(200,[&](size_t i) {
            std::unique_lock<std::mutex> lock(m);
            if(std::this_thread::get_id()!=caller) {ids.insert(std::this_thread::get_id());helperSeen=true;cv.notify_all();}
            if(i==0 && std::thread::hardware_concurrency()!=1)
                CHECK(cv.wait_for(lock,std::chrono::seconds(5),[&]{return helperSeen;}));
        });
        // A pool exposes its actual retained helper count, not incidental IDs
        // that an OS may recycle immediately after temporary threads exit.
        CHECK(pool.helper_count()>0 || std::thread::hardware_concurrency()==1);
        const auto actual=pool.helper_thread_ids();
        std::set<std::thread::id> retained(actual.begin(),actual.end());
        CHECK(retained.size()<=7);
        for(auto id:ids)CHECK(retained.count(id));
        if(round)CHECK(retained==prior);else prior=retained;
    }
    std::vector<std::atomic<int>> counts(10003);
    pool.run(counts.size(),[&](size_t i){++counts[i];});for(auto& x:counts)CHECK(x==1);
    std::atomic<int> completed=0;bool caught=false;
    try {pool.run(100,[&](size_t i){++completed;if(i==7)throw std::runtime_error("expected");});}
    catch(const std::runtime_error&){caught=true;}CHECK(caught && completed==100);
    std::atomic<int> nested=0;
    pool.run(32,[&](size_t){pool.run(17,[&](size_t){++nested;});});CHECK(nested==544);
    std::atomic<int> external=0;
    std::vector<std::thread> callers;
    for(int t=0;t<6;++t)callers.emplace_back([&]{for(int r=0;r<20;++r)pool.run(79,[&](size_t){++external;});});
    for(auto& t:callers)t.join();CHECK(external==6*20*79);
    // An automatic pool exercises destructor shutdown and joining as well.
    {ParallelFor local;local.run(33,[](size_t){});}
    // Partial creation failure keeps successful helpers owned and reusable;
    // the following run retries growth. Also test failure before any helper.
    for(int failAt: {1,2}) {
        int attempts=0;std::atomic<int> done=0;
        ParallelFor local([&](std::function<void()> work) {
            if(++attempts==failAt)throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
            return std::thread(std::move(work));
        });
        local.run(99,[&](size_t){++done;});CHECK(done==99);
        local.run(101,[&](size_t){++done;});CHECK(done==200);
        if(std::thread::hardware_concurrency()!=1)CHECK(local.helper_count()>0);
    }
    std::atomic<int> nestedCompleted=0;bool nestedCaught=false;
    try{pool.run(19,[&](size_t){pool.run(7,[&](size_t i){++nestedCompleted;if(i==3)throw std::runtime_error("nested");});});}
    catch(const std::runtime_error&){nestedCaught=true;}
    CHECK(nestedCaught && nestedCompleted==133);
    std::printf("PASS retained_helpers=%zu reused_runs=10 tasks=10003 nested=544 concurrent=9480 creation_failures=2 nested_exception_tasks=133\n",pool.helper_count());
}
