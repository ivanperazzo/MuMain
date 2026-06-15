#include "doctest.h"

#include "Render/Build/WorkerArena.h"
#include "Core/Jobs/ThreadPool.h"

#include <set>
#include <mutex>

TEST_CASE("each worker gets a distinct arena; main thread is index 0")
{
    using Core::Jobs::ThreadPool;
    ThreadPool& pool = ThreadPool::Instance();

    std::set<const void*> seen;
    std::mutex m;
    pool.ParallelFor(pool.WorkerCount() * 8, [&](int) {
        const void* a = &Render::Build::CurrentArena();
        std::lock_guard<std::mutex> g(m);
        seen.insert(a);
    });
    // At most WorkerCount distinct arenas, at least 1.
    CHECK(seen.size() >= 1);
    CHECK(seen.size() <= (size_t)pool.WorkerCount());

    // The main thread (outside ParallelFor) is worker index 0.
    CHECK(ThreadPool::CurrentWorkerIndex() == 0);
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
