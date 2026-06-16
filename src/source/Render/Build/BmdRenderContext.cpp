#include "Render/Build/BmdRenderContext.h"

#include "Core/Jobs/ThreadPool.h"

#include <cassert>
#include <memory>
#include <vector>

namespace Render::Build
{
    namespace
    {
        // Indexed by worker id. Heap so we mirror WorkerArena's layout exactly.
        std::vector<std::unique_ptr<BmdRenderContext>> s_ctxs;

        BmdRenderContext& CtxAt(int idx)
        {
            if ((int)s_ctxs.size() <= idx) s_ctxs.resize(idx + 1);
            if (!s_ctxs[idx]) s_ctxs[idx] = std::make_unique<BmdRenderContext>();
            return *s_ctxs[idx];
        }
    }

    void InitRenderCtxs(int count)
    {
        for (int i = 0; i < count; ++i) (void)CtxAt(i);
    }

    BmdRenderContext& CurrentRenderCtx()
    {
        const int idx = Core::Jobs::ThreadPool::CurrentWorkerIndex();
        // Thread-safe to call concurrently from distinct workers ONLY after
        // InitRenderCtxs(>=WorkerCount()) has run (startup). The grow path in CtxAt
        // is not concurrency-safe and must never run during a parallel ParallelFor.
        assert(idx >= 0 && idx < (int)s_ctxs.size() && s_ctxs[idx] &&
               "InitRenderCtxs(WorkerCount()) must run before parallel CurrentRenderCtx()");
        return CtxAt(idx);
    }
}
