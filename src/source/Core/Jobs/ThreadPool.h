#pragma once

#include <cstddef>
#include <functional>

namespace Core::Jobs
{
    // Minimal persistent fork-join pool. ParallelFor splits [0,count) across the
    // workers + the calling thread and blocks until every index has run. One pool
    // per process. No engine deps -> unit-testable.
    class ThreadPool
    {
    public:
        static ThreadPool& Instance();

        // Run fn(i) for each i in [0,count). Blocks until all complete. An
        // exception from any fn is captured and rethrown after the batch drains.
        void ParallelFor(int count, const std::function<void(int)>& fn);

        int WorkerCount() const { return m_workerCount; }

        // Stable [0,WorkerCount()) id for the calling thread inside ParallelFor;
        // 0 on the main thread outside it. Used to index per-worker arenas/buckets.
        static int CurrentWorkerIndex();

    private:
        ThreadPool();
        ~ThreadPool();
        ThreadPool(const ThreadPool&) = delete;
        ThreadPool& operator=(const ThreadPool&) = delete;

        int m_workerCount = 1;
        struct Impl;
        Impl* m_impl = nullptr;
    };

    // MU_JOBS env flag (default OFF). Gates the parallel Phase B (Task 7).
    bool JobsEnabled();
    void SetJobsEnabled(bool on);
}
