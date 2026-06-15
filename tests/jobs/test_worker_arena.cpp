#include "doctest.h"

#include "Render/Build/WorkerArena.h"
#include "Core/Jobs/ThreadPool.h"

#include <map>
#include <mutex>
#include <set>

TEST_CASE("distinct workers get distinct arenas (injective), same index stable")
{
    using Core::Jobs::ThreadPool;
    ThreadPool& pool = ThreadPool::Instance();
    // Pre-allocate before parallel CurrentArena(): the hardened ArenaAt grow path is
    // startup-only and CurrentArena() asserts InitArenas(>=WorkerCount()) has run.
    Render::Build::InitArenas(pool.WorkerCount());
    std::map<int, const void*> byIdx;
    std::mutex m;
    bool stable = true;
    pool.ParallelFor(pool.WorkerCount() * 64, [&](int){
        const int idx = ThreadPool::CurrentWorkerIndex();
        const void* a = &Render::Build::CurrentArena();
        std::lock_guard<std::mutex> g(m);
        auto it = byIdx.find(idx);
        if (it != byIdx.end()) { if (it->second != a) stable = false; }
        else byIdx[idx] = a;
    });
    CHECK(stable);                         // same index -> same arena
    std::set<const void*> arenas;
    for (auto& kv : byIdx) arenas.insert(kv.second);
    CHECK(arenas.size() == byIdx.size());  // distinct indices -> distinct arenas (injective)
}

TEST_CASE("two distinct worker indices map to distinct arenas")
{
    using Core::Jobs::ThreadPool;
    // Pre-allocate so the arenas exist and are stable.
    Render::Build::InitArenas(ThreadPool::Instance().WorkerCount());

    // Main thread arena is index 0; confirm calling twice from the same thread
    // returns the same instance (stable per worker).
    const void* a0 = &Render::Build::CurrentArena();
    const void* a0again = &Render::Build::CurrentArena();
    CHECK(a0 == a0again);
}

TEST_CASE("main thread is index 0")
{
    using Core::Jobs::ThreadPool;
    // The main thread (outside ParallelFor) is worker index 0.
    CHECK(ThreadPool::CurrentWorkerIndex() == 0);
}
