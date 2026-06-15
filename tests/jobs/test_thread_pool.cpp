#include "doctest.h"

#include "Core/Jobs/ThreadPool.h"

#include <atomic>
#include <mutex>
#include <set>
#include <stdexcept>
#include <vector>

using Core::Jobs::ThreadPool;

TEST_CASE("ParallelFor visits every index exactly once")
{
    ThreadPool& pool = ThreadPool::Instance();
    const int n = 1000;
    std::vector<std::atomic<int>> hits(n);
    for (auto& h : hits) h.store(0);

    pool.ParallelFor(n, [&](int i) { hits[i].fetch_add(1); });

    for (int i = 0; i < n; ++i)
        CHECK(hits[i].load() == 1);
}

TEST_CASE("ParallelFor with n=0 is a no-op")
{
    ThreadPool& pool = ThreadPool::Instance();
    std::atomic<int> calls{0};
    pool.ParallelFor(0, [&](int) { calls.fetch_add(1); });
    CHECK(calls.load() == 0);
}

TEST_CASE("ParallelFor blocks until all work is done")
{
    ThreadPool& pool = ThreadPool::Instance();
    const int n = 64;
    std::atomic<int> done{0};
    pool.ParallelFor(n, [&](int) { done.fetch_add(1); });
    CHECK(done.load() == n);   // already complete when ParallelFor returns
}

TEST_CASE("ParallelFor rethrows a worker exception")
{
    ThreadPool& pool = ThreadPool::Instance();
    bool threw = false;
    try {
        pool.ParallelFor(32, [](int i) { if (i == 7) throw std::runtime_error("boom"); });
    } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    // Pool still usable afterward.
    std::atomic<int> c{0};
    pool.ParallelFor(8, [&](int) { c.fetch_add(1); });
    CHECK(c.load() == 8);
}

TEST_CASE("worker count is sane")
{
    CHECK(Core::Jobs::ThreadPool::Instance().WorkerCount() >= 1);
    CHECK(Core::Jobs::ThreadPool::Instance().WorkerCount() <= 16);
}

TEST_CASE("CurrentWorkerIndex is 0 on main and in-range inside ParallelFor")
{
    using Core::Jobs::ThreadPool;
    ThreadPool& pool = ThreadPool::Instance();
    CHECK(ThreadPool::CurrentWorkerIndex() == 0);   // main thread outside ParallelFor

    std::mutex m;
    std::set<int> indices;
    bool inRange = true;
    pool.ParallelFor(pool.WorkerCount() * 64, [&](int) {
        int w = ThreadPool::CurrentWorkerIndex();
        std::lock_guard<std::mutex> g(m);
        if (w < 0 || w >= pool.WorkerCount()) inRange = false;
        indices.insert(w);
    });
    CHECK(inRange);
    CHECK(!indices.empty());
    CHECK(ThreadPool::CurrentWorkerIndex() == 0);    // back to 0 on main afterward
}
