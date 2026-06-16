#include "Render/Build/WorkerArena.h"

#include "Core/Jobs/ThreadPool.h"

#include <cassert>
#include <memory>
#include <vector>

namespace Render::Build
{
    namespace
    {
        // Indexed by worker id. Heap (each ~30 MB) so we don't blow the stack/BSS.
        std::vector<std::unique_ptr<WorkerArena>> s_arenas;

        WorkerArena& ArenaAt(int idx)
        {
            if ((int)s_arenas.size() <= idx) s_arenas.resize(idx + 1);
            if (!s_arenas[idx]) s_arenas[idx] = std::make_unique<WorkerArena>();
            return *s_arenas[idx];
        }
    }

    void InitArenas(int count)
    {
        for (int i = 0; i < count; ++i) (void)ArenaAt(i);
    }

    WorkerArena& CurrentArena()
    {
        const int idx = Core::Jobs::ThreadPool::CurrentWorkerIndex();
        // Thread-safe to call concurrently from distinct workers ONLY after
        // InitArenas(>=WorkerCount()) has run (startup). The grow path in ArenaAt is
        // not concurrency-safe and must never run during a parallel ParallelFor.
        assert(idx >= 0 && idx < (int)s_arenas.size() && s_arenas[idx] &&
               "InitArenas(WorkerCount()) must run before parallel CurrentArena()");
        return ArenaAt(idx);
    }
}
