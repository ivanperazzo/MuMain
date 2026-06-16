#include "doctest.h"

#include "Render/Models/BmdInstanceBatch.h"

#include <cstdint>
#include <vector>

using Render::Models::InstBucketData;
using Render::Models::InstBucketMap;
using Render::Models::MergeBuckets;

namespace
{
    // 10 floats per instance record (must match kInstFloats in BmdInstanceBatch.cpp).
    constexpr int kInstFloats = 10;

    // A synthetic instance record derived from a seed so payloads are distinguishable.
    void PushRec(InstBucketData& b, int seed)
    {
        for (int f = 0; f < kInstFloats; ++f)
            b.recs.push_back(static_cast<float>(seed * 100 + f));
    }

    // Synthesize one record into the worker map under a key, setting metadata on first sight.
    // The per-bucket frame-global shader inputs (instLight/instWave/chrome*, sub-task 6.7) are
    // derived deterministically from `mode` so distinct keys carry distinct values, exercising
    // the first-sight carry in MergeBuckets.
    void Feed(InstBucketMap& wm, uint64_t key, int seed,
              const void* model, int mesh, int tex, int mode, int blend,
              float uv0, float uv1)
    {
        InstBucketData& b = wm[key];
        b.model = reinterpret_cast<const BMD*>(model);
        b.meshIndex = mesh; b.texId = tex; b.mode = mode; b.blend = blend;
        b.uvScroll[0] = uv0; b.uvScroll[1] = uv1;
        b.instLight[0] = (float)mode + 0.1f; b.instLight[1] = (float)mode + 0.2f; b.instLight[2] = (float)mode + 0.3f;
        b.instWave = (float)mode * 0.5f;
        b.chromeWave2 = (float)mode * 0.25f;
        b.chromeL[0] = (float)mode + 1.f; b.chromeL[1] = (float)mode + 2.f; b.chromeL[2] = (float)mode + 3.f;
        b.chromeLightVec[0] = (float)mode + 4.f; b.chromeLightVec[1] = (float)mode + 5.f; b.chromeLightVec[2] = (float)mode + 6.f;
        PushRec(b, seed);
    }
}

TEST_CASE("MergeBuckets: round-robin across W workers == all on worker 0")
{
    const int W = 4;
    const int N = 300;

    // 6 distinct synthetic keys with stable metadata.
    struct KeyMeta { uint64_t key; const void* model; int mesh, tex, mode, blend; float uv0, uv1; };
    const KeyMeta keys[] = {
        { 0x1001, (const void*)0x10, 0, 5, 0, 0, 0.1f, 0.2f },
        { 0x1002, (const void*)0x20, 1, 6, 0, 1, 0.0f, 0.0f },
        { 0x1003, (const void*)0x30, 2, 7, 1, 0, 0.3f, 0.0f },
        { 0x1004, (const void*)0x40, 3, 8, 1, 1, 0.0f, 0.5f },
        { 0x1005, (const void*)0x50, 4, 9, 0, 0, 0.7f, 0.7f },
        { 0x1006, (const void*)0x60, 5, 10, 1, 1, 0.9f, 0.1f },
    };
    const int nKeys = (int)(sizeof(keys) / sizeof(keys[0]));

    // Reference: everything fed to a single worker (worker 0), in record order 0..N-1.
    std::vector<InstBucketMap> single(W);   // workers 1..W-1 stay empty
    // Parallel-style: same records round-robined across all W workers, same record order.
    std::vector<InstBucketMap> split(W);

    for (int i = 0; i < N; ++i)
    {
        const KeyMeta& k = keys[i % nKeys];
        Feed(single[0], k.key, i, k.model, k.mesh, k.tex, k.mode, k.blend, k.uv0, k.uv1);
        Feed(split[i % W], k.key, i, k.model, k.mesh, k.tex, k.mode, k.blend, k.uv0, k.uv1);
    }

    InstBucketMap mergedSingle, mergedSplit;
    MergeBuckets(single.data(), W, mergedSingle);
    MergeBuckets(split.data(), W, mergedSplit);

    // Same set of keys.
    CHECK(mergedSingle.size() == mergedSplit.size());
    CHECK(mergedSingle.size() == (size_t)nKeys);

    for (const auto& kv : mergedSingle)
    {
        auto it = mergedSplit.find(kv.first);
        REQUIRE(it != mergedSplit.end());
        const InstBucketData& a = kv.second;
        const InstBucketData& b = it->second;

        // Metadata preserved & identical.
        CHECK(a.model == b.model);
        CHECK(a.meshIndex == b.meshIndex);
        CHECK(a.texId == b.texId);
        CHECK(a.mode == b.mode);
        CHECK(a.blend == b.blend);
        CHECK(a.uvScroll[0] == b.uvScroll[0]);
        CHECK(a.uvScroll[1] == b.uvScroll[1]);

        // Per-bucket frame-global shader inputs (sub-task 6.7) carried from first-sight.
        CHECK(a.instWave == b.instWave);
        CHECK(a.chromeWave2 == b.chromeWave2);
        for (int k = 0; k < 3; ++k)
        {
            CHECK(a.instLight[k] == b.instLight[k]);
            CHECK(a.chromeL[k] == b.chromeL[k]);
            CHECK(a.chromeLightVec[k] == b.chromeLightVec[k]);
        }
        // And they match the metadata the key was fed with (mode-derived above).
        CHECK(a.instWave == (float)a.mode * 0.5f);
        CHECK(a.chromeWave2 == (float)a.mode * 0.25f);

        // Same instance count.
        CHECK(a.recs.size() == b.recs.size());
        CHECK(a.recs.size() % kInstFloats == 0);
    }
}

TEST_CASE("MergeBuckets: each key's merged array is the concatenation of per-worker arrays")
{
    const int W = 3;

    // Single key, deterministic per-instance payloads so we can compare byte-for-byte.
    const uint64_t key = 0xABCD;
    std::vector<InstBucketMap> workers(W);

    // Round-robin 30 records: worker 0 gets seeds 0,3,6,...; worker 1 gets 1,4,7,...; etc.
    const int N = 30;
    for (int i = 0; i < N; ++i)
        Feed(workers[i % W], key, i, (const void*)0x99, 0, 0, 0, 1, 0.f, 0.f);

    InstBucketMap merged;
    MergeBuckets(workers.data(), W, merged);

    REQUIRE(merged.count(key) == 1);
    const InstBucketData& m = merged.at(key);
    REQUIRE(m.recs.size() == (size_t)(N * kInstFloats));

    // Merge visits workers in index order, so the merged stream is:
    //   worker0 records (in its append order) then worker1 records then worker2 records.
    // Reconstruct that expected concatenation from the worker maps and compare exactly.
    std::vector<float> expected;
    for (int w = 0; w < W; ++w)
        expected.insert(expected.end(), workers[w].at(key).recs.begin(), workers[w].at(key).recs.end());

    CHECK(m.recs == expected);
}

TEST_CASE("MergeBuckets: empty worker records are skipped, no spurious keys")
{
    const int W = 4;
    std::vector<InstBucketMap> workers(W);

    // Worker 1 holds a key with NO records (e.g. cleared at InstBegin); must not appear.
    workers[1][0x55].model = (const BMD*)0x1;   // empty recs
    // Worker 2 holds a real contribution.
    Feed(workers[2], 0x66, 0, (const void*)0x2, 0, 0, 0, 0, 0.f, 0.f);

    InstBucketMap merged;
    MergeBuckets(workers.data(), W, merged);

    CHECK(merged.count(0x55) == 0);
    CHECK(merged.count(0x66) == 1);
    CHECK(merged.size() == 1);
}
