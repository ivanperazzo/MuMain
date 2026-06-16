#include "Core/Jobs/ThreadPool.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace Core::Jobs
{
    namespace
    {
        thread_local int t_workerIndex = 0;   // main thread = 0

        int ComputeWorkerCount()
        {
            int hc = (int)std::thread::hardware_concurrency();
            int n  = std::max(1, std::min(16, hc - 2));
            return n;
        }
    }

    struct ThreadPool::Impl
    {
        std::vector<std::thread> threads;          // worker count - 1 (main also works)
        std::mutex               mtx;
        std::condition_variable  cvStart;
        std::condition_variable  cvDone;
        const std::function<void(int)>* job = nullptr;
        int   count = 0;
        std::atomic<int> nextIndex{0};
        std::atomic<int> remaining{0};
        int   epoch = 0;                           // bumped per batch to wake workers
        bool  stop = false;
        std::exception_ptr error;                  // first captured worker exception
    };

    ThreadPool& ThreadPool::Instance()
    {
        static ThreadPool s_pool;
        return s_pool;
    }

    ThreadPool::ThreadPool()
    {
        m_workerCount = ComputeWorkerCount();
        m_impl = new Impl();
        // Spawn (m_workerCount - 1) background threads; the calling thread is worker 0.
        for (int w = 1; w < m_workerCount; ++w)
        {
            m_impl->threads.emplace_back([this, w] {
                t_workerIndex = w;
                Impl& im = *m_impl;
                int seenEpoch = 0;
                for (;;)
                {
                    std::unique_lock<std::mutex> lk(im.mtx);
                    im.cvStart.wait(lk, [&] { return im.stop || im.epoch != seenEpoch; });
                    if (im.stop) return;
                    seenEpoch = im.epoch;
                    lk.unlock();

                    for (;;)
                    {
                        int i = im.nextIndex.fetch_add(1);
                        if (i >= im.count) break;
                        try { (*im.job)(i); }
                        catch (...) {
                            std::lock_guard<std::mutex> g(im.mtx);
                            if (!im.error) im.error = std::current_exception();
                        }
                        if (im.remaining.fetch_sub(1) == 1) im.cvDone.notify_all();
                    }
                }
            });
        }
    }

    ThreadPool::~ThreadPool()
    {
        if (m_impl)
        {
            {
                std::lock_guard<std::mutex> g(m_impl->mtx);
                m_impl->stop = true;
            }
            m_impl->cvStart.notify_all();
            for (auto& t : m_impl->threads) if (t.joinable()) t.join();
            delete m_impl;
            m_impl = nullptr;
        }
    }

    int ThreadPool::CurrentWorkerIndex() { return t_workerIndex; }

    void ThreadPool::ParallelFor(int count, const std::function<void(int)>& fn)
    {
        if (count <= 0) return;
        Impl& im = *m_impl;

        if (m_workerCount <= 1 || count == 1)
        {
            // Serial fast path (also keeps the flag-off / single-core build identical).
            for (int i = 0; i < count; ++i) fn(i);
            return;
        }

        {
            std::lock_guard<std::mutex> g(im.mtx);
            im.job = &fn;
            im.count = count;
            im.nextIndex.store(0);
            im.remaining.store(count);
            im.error = nullptr;
            ++im.epoch;
        }
        im.cvStart.notify_all();

        // The calling thread (worker 0) also pulls work.
        for (;;)
        {
            int i = im.nextIndex.fetch_add(1);
            if (i >= count) break;
            try { fn(i); }
            catch (...) {
                std::lock_guard<std::mutex> g(im.mtx);
                if (!im.error) im.error = std::current_exception();
            }
            if (im.remaining.fetch_sub(1) == 1) im.cvDone.notify_all();
        }

        {
            std::unique_lock<std::mutex> lk(im.mtx);
            im.cvDone.wait(lk, [&] { return im.remaining.load() == 0; });
        }

        if (im.error) { auto e = im.error; im.error = nullptr; std::rethrow_exception(e); }
    }

    namespace { bool s_jobsEnabled = false; }

    void SetJobsEnabled(bool on) { s_jobsEnabled = on; }
    bool JobsEnabled()
    {
        static const bool s_envInit = [] {
            char b[8] = {}; size_t n = 0;
            if (getenv_s(&n, b, sizeof(b), "MU_JOBS") == 0 && n > 0)
                s_jobsEnabled = (atoi(b) != 0);
            return true;
        }();
        (void)s_envInit;
        return s_jobsEnabled;
    }
}
