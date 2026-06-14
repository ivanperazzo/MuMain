#include "stdafx.h"

#include "Render/Models/BmdGpuCache.h"

#include "Render/Models/ZzzBMD.h"
#include "Render/Models/BmdInstanceBatch.h"
#include "Render/Models/ShadowInstanceBatch.h"
#include "Render/GL/GLLog.h"

#include <unordered_map>
#include <vector>
#include <cstdlib>

namespace
{
    // Autonomous/headless smoke-tests can't type "$gpubmd on", so honour an env
    // var once: MU_GPUBMD=1 / MU_GPUINST=1 enable the paths at startup.
    bool EnvFlag(const char* name)
    {
        char buf[8] = {};
        size_t n = 0;
        return getenv_s(&n, buf, sizeof(buf), name) == 0 && n > 0 && buf[0] == '1';
    }
}

namespace Render::Models
{
    namespace
    {
        // One BMD* -> its per-mesh GPU buffers. The vector is sized to NumMeshs the
        // first time the model is touched and never resized again, so MeshGpu
        // addresses stay stable (callers hold a const MeshGpu*). unordered_map node
        // storage also keeps existing entries stable across rehash.
        std::unordered_map<const BMD*, std::vector<MeshGpu>> s_cache;

        bool s_gpuBmdEnabled  = false;   // $gpubmd master switch (default off)
        bool s_gpuObjectsPass = false;   // true only during the Objects render pass
        bool s_gpuCharsPass   = false;   // true only during the Characters render pass
        bool s_gpuInstEnabled = false;   // $gpuinst: instanced Characters batching
        bool s_skinSkip       = false;   // $skinskip: Transform skips CPU skinning

        int  s_charMeshTotal  = 0;       // chars-pass mesh draws this frame
        int  s_charMeshGpu    = 0;       // of those, took the GPU path
        int  s_visibleChars   = 0;       // visible characters this frame
        int  s_statFrameCtr   = 0;

        // Coverage breakdown: why each char mesh did/didn't reach the GPU/instanced
        // path. Index = MeshCoverClass. Tells us which legacy bucket to attack next.
        int  s_charClass[8]   = {0};

        // Pack the expanded (non-indexed) triangle stream into the interleaved layout
        // BmdShader expects. Mirrors the legacy RenderMesh expansion exactly: for each
        // triangle, for each corner, emit pos/bone/normal/bone/uv in MODEL space.
        // Skinning + lighting move to the shader, so this is built once and resident.
        void BuildMesh(const BMD* model, int meshIndex, int maxBones, MeshGpu& out)
        {
            out.built       = true;
            out.eligible    = false;
            out.vertexCount = 0;

            const Mesh_t* m = &model->Meshs[meshIndex];
            if (m->NumTriangles <= 0 || m->Triangles == nullptr || m->Vertices == nullptr
                || m->Normals == nullptr || m->TexCoords == nullptr)
            {
                return;
            }

            const int expected = m->NumTriangles * 3;
            std::vector<float> verts;
            verts.reserve(static_cast<size_t>(expected) * GpuVtxLayout::kFloats);

            bool boneOk = true;
            for (int j = 0; j < m->NumTriangles; j++)
            {
                const Triangle_t* tri = &m->Triangles[j];
                for (int k = 0; k < tri->Polygon; k++)
                {
                    const int vi = tri->VertexIndex[k];
                    const int ni = tri->NormalIndex[k];
                    const int ti = tri->TexCoordIndex[k];
                    if (vi < 0 || vi >= m->NumVertices
                        || ni < 0 || ni >= m->NumNormals
                        || ti < 0 || ti >= m->NumTexCoords)
                    {
                        return;   // malformed indices -> legacy path
                    }

                    const Vertex_t&   v  = m->Vertices[vi];
                    const Normal_t&   n  = m->Normals[ni];
                    const TexCoord_t& tc = m->TexCoords[ti];

                    if (v.Node < 0 || v.Node >= maxBones || n.Node < 0 || n.Node >= maxBones)
                        boneOk = false;

                    verts.push_back(v.Position[0]);
                    verts.push_back(v.Position[1]);
                    verts.push_back(v.Position[2]);
                    verts.push_back(static_cast<float>(v.Node));
                    verts.push_back(n.Normal[0]);
                    verts.push_back(n.Normal[1]);
                    verts.push_back(n.Normal[2]);
                    verts.push_back(static_cast<float>(n.Node));
                    verts.push_back(tc.TexCoordU);
                    verts.push_back(tc.TexCoordV);
                }
            }

            const int total = static_cast<int>(verts.size() / GpuVtxLayout::kFloats);
            // Only pure triangle meshes (Polygon == 3 everywhere) map 1:1 to a single
            // glDrawArrays of NumTriangles*3; any quad (Polygon == 4) breaks that count
            // -> legacy path. Bone index out of the shader's uBones[] range likewise.
            if (total != expected || !boneOk)
            {
                Render::GL::Log("[bmd_gpu] mesh %p#%d ineligible (total=%d expected=%d boneOk=%d)",
                    static_cast<const void*>(model), meshIndex, total, expected, static_cast<int>(boneOk));
                return;
            }

            out.vbo.Upload(GL_ARRAY_BUFFER, verts.data(),
                static_cast<GLsizeiptr>(verts.size() * sizeof(float)), GL_STATIC_DRAW);
            if (!out.vbo.Valid())
            {
                Render::GL::Log("[bmd_gpu] mesh %p#%d VBO upload failed",
                    static_cast<const void*>(model), meshIndex);
                return;
            }

            out.vertexCount = static_cast<GLsizei>(total);
            out.eligible    = true;
        }
    }

    const MeshGpu* GetOrBuildMeshGpu(const BMD* model, int meshIndex, int maxBones)
    {
        if (model == nullptr || model->Meshs == nullptr
            || meshIndex < 0 || meshIndex >= model->NumMeshs)
        {
            return nullptr;
        }

        auto it = s_cache.find(model);
        if (it == s_cache.end())
            it = s_cache.emplace(model, std::vector<MeshGpu>(model->NumMeshs)).first;

        MeshGpu& slot = it->second[meshIndex];
        if (!slot.built)
            BuildMesh(model, meshIndex, maxBones, slot);

        return &slot;
    }

    void ClearGpuCache()
    {
        s_cache.clear();
    }

    void InvalidateGpuModel(const BMD* model)
    {
        if (model == nullptr)
            return;
        s_cache.erase(model);          // MeshGpu dtors free the VBOs (guarded if GL gone)
        DropInstanceBucketsFor(model); // drop any instance buckets keyed by this BMD*
    }

    void SetGpuBmdEnabled(bool on) { s_gpuBmdEnabled = on; }
    bool GpuBmdEnabled()
    {
        static const bool s_envInit = [] { if (EnvFlag("MU_GPUBMD")) s_gpuBmdEnabled = true; return true; }();
        (void)s_envInit;
        return s_gpuBmdEnabled;
    }
    void SetGpuObjectsPass(bool on) { s_gpuObjectsPass = on; }
    bool GpuObjectsPass()           { return s_gpuObjectsPass; }
    void SetGpuCharsPass(bool on)   { s_gpuCharsPass = on; }
    bool GpuCharsPass()             { return s_gpuCharsPass; }
    void SetGpuInstEnabled(bool on) { s_gpuInstEnabled = on; }
    bool GpuInstEnabled()
    {
        static const bool s_envInit = [] { if (EnvFlag("MU_GPUINST")) s_gpuInstEnabled = true; return true; }();
        (void)s_envInit;
        return s_gpuInstEnabled;
    }

    void SetSkinSkip(bool on) { s_skinSkip = on; }
    bool SkinSkip()
    {
        // MU_SKINSKIP=1: skip all CPU skinning (measures the skinning ceiling; legacy
        // non-instanced meshes render wrong, instanced ones are fine — GPU skins from
        // the bone palette, not VertexTransform).
        static const bool s_envInit = [] { if (EnvFlag("MU_SKINSKIP")) s_skinSkip = true; return true; }();
        (void)s_envInit;
        return s_skinSkip;
    }

    void NoteCharMeshDraw(bool wentGpu)
    {
        ++s_charMeshTotal;
        if (wentGpu) ++s_charMeshGpu;
    }

    void NoteCharMeshClass(int cls)
    {
        if (cls >= 0 && cls < 8) ++s_charClass[cls];
    }

    void NoteVisibleChar() { ++s_visibleChars; }

    void LogAndResetGpuStats()
    {
        if (++s_statFrameCtr >= 30)    // ~ every 1-2s depending on FPS
        {
            Render::GL::Log("[bmd_gpu] %d visible chars, %d mesh draws/frame (%d/char), %d via GPU (%d%%) "
                "| inst: %d draws / %d instances | skinskip=%d gpubmd=%d gpuinst=%d",
                s_visibleChars, s_charMeshTotal,
                s_visibleChars ? (s_charMeshTotal / s_visibleChars) : 0,
                s_charMeshGpu,
                s_charMeshTotal ? (s_charMeshGpu * 100 / s_charMeshTotal) : 0,
                InstDrawCount(), InstInstanceCount(),
                (int)s_skinSkip, (int)GpuBmdEnabled(), (int)GpuInstEnabled());
            Render::GL::Log("[bmd_cov] inst=%d permeshGPU=%d | legacy: nontex=%d blend=%d wave=%d scale=%d geom=%d other=%d",
                s_charClass[0], s_charClass[1], s_charClass[2], s_charClass[3],
                s_charClass[4], s_charClass[5], s_charClass[6], s_charClass[7]);
            Render::GL::Log("[bmd_shadow] gpu: %d draws / %d instances (MU_GPUSHADOW=%d)",
                ShadowDrawCount(), ShadowInstanceCount(), (int)GpuShadowEnabled());
            s_statFrameCtr = 0;
        }
        s_charMeshTotal = 0;
        s_charMeshGpu = 0;
        s_visibleChars = 0;
        for (int& c : s_charClass) c = 0;
    }
}
