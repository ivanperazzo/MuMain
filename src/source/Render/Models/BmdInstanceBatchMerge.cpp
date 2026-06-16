// Pure, GL-free merge of per-worker instance buckets (Etapa 3, Task 4).
//
// Kept in its own translation unit (no stdafx / GL / engine includes) so the merge
// can be unit-tested without an OpenGL context. BmdInstanceBatch.cpp owns the GL-backed
// collection/flush; this file owns only the data transform the test exercises.

#include "Render/Models/BmdInstanceBatch.h"

namespace Render::Models
{
    void MergeBuckets(const InstBucketMap* workerMaps, int workerCount, InstBucketMap& out)
    {
        // Visit workers in index order; for each key, create-on-first-sight (copying the
        // bucket metadata) then concatenate that worker's records. Worker 0 first keeps the
        // serial path byte-identical with the pre-Task-4 single-map behaviour. Order across
        // workers is safe: additive buckets use GL_ONE/ONE (commutative) and opaque buckets
        // are alpha-test + depth-tested, so within-key concat order does not change pixels.
        if (workerMaps == nullptr || workerCount <= 0)
            return;

        for (int w = 0; w < workerCount; ++w)
        {
            const InstBucketMap& wm = workerMaps[w];
            for (const auto& kv : wm)
            {
                const InstBucketData& src = kv.second;
                if (src.recs.empty())
                    continue;   // skip cleared/empty buckets -> no spurious draw keys
                auto res = out.emplace(kv.first, InstBucketData{});
                InstBucketData& dst = res.first->second;
                if (res.second)
                {
                    // First worker contributing this key: copy metadata (uvScroll + the
                    // 6.7 light/wave/chrome params are all frame-global per (model, mesh, mode,
                    // blend), identical across workers/instances, so first-sight is correct).
                    dst.model = src.model; dst.meshIndex = src.meshIndex; dst.texId = src.texId;
                    dst.mode = src.mode; dst.blend = src.blend;
                    dst.uvScroll[0] = src.uvScroll[0]; dst.uvScroll[1] = src.uvScroll[1];
                    dst.instLight[0] = src.instLight[0]; dst.instLight[1] = src.instLight[1]; dst.instLight[2] = src.instLight[2];
                    dst.instWave = src.instWave;
                    dst.chromeWave2 = src.chromeWave2;
                    dst.chromeL[0] = src.chromeL[0]; dst.chromeL[1] = src.chromeL[1]; dst.chromeL[2] = src.chromeL[2];
                    dst.chromeLightVec[0] = src.chromeLightVec[0]; dst.chromeLightVec[1] = src.chromeLightVec[1]; dst.chromeLightVec[2] = src.chromeLightVec[2];
                    dst.recs.reserve(src.recs.size());
                }
                dst.recs.insert(dst.recs.end(), src.recs.begin(), src.recs.end());
            }
        }
    }
}
