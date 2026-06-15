#include "Render/Build/BuildEmitMode.h"

#include <cstdlib>

namespace Render::Build
{
    namespace
    {
        // The mode is set on the driving thread BEFORE a pass starts and read by all
        // workers DURING the pass; there is a happens-before via the ParallelFor fork/join
        // (ThreadPool dispatch), so a plain global is safe. Kept as a simple file-global to
        // avoid pulling <atomic> into the unit-test link; the value never changes mid-pass.
        EmitMode s_mode = EmitMode::Full;
    }

    void     SetBuildEmitMode(EmitMode m) { s_mode = m; }
    EmitMode GetBuildEmitMode()           { return s_mode; }

    int Rand()
    {
        if (s_mode == EmitMode::MeshOnly)
            return 0;
        return ::rand();
    }
}
