#include "Render/Build/WorkerArena.h"

#include "Core/Jobs/ThreadPool.h"

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
        return ArenaAt(Core::Jobs::ThreadPool::CurrentWorkerIndex());
    }
}
