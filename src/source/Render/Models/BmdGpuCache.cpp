#include "stdafx.h"

#include "Render/Models/BmdGpuCache.h"

#include "Render/Models/ZzzBMD.h"
#include "Render/GL/GLLog.h"

#include <unordered_map>
#include <vector>

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
        bool s_skinSkip       = false;   // $skinskip: Transform skips CPU skinning

        int  s_charMeshTotal  = 0;       // chars-pass mesh draws this frame
        int  s_charMeshGpu    = 0;       // of those, took the GPU path
        int  s_visibleChars   = 0;       // visible characters this frame
        int  s_statFrameCtr   = 0;

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

    void SetGpuBmdEnabled(bool on) { s_gpuBmdEnabled = on; }
    bool GpuBmdEnabled()           { return s_gpuBmdEnabled; }
    void SetGpuObjectsPass(bool on) { s_gpuObjectsPass = on; }
    bool GpuObjectsPass()           { return s_gpuObjectsPass; }
    void SetGpuCharsPass(bool on)   { s_gpuCharsPass = on; }
    bool GpuCharsPass()             { return s_gpuCharsPass; }

    void SetSkinSkip(bool on) { s_skinSkip = on; }
    bool SkinSkip()           { return s_skinSkip; }

    void NoteCharMeshDraw(bool wentGpu)
    {
        ++s_charMeshTotal;
        if (wentGpu) ++s_charMeshGpu;
    }

    void NoteVisibleChar() { ++s_visibleChars; }

    void LogAndResetGpuStats()
    {
        if (++s_statFrameCtr >= 120)   // ~ every 2-4s depending on FPS
        {
            Render::GL::Log("[bmd_gpu] %d visible chars, %d mesh draws/frame (%d/char), %d via GPU (%d%%) | skinskip=%d gpubmd=%d",
                s_visibleChars, s_charMeshTotal,
                s_visibleChars ? (s_charMeshTotal / s_visibleChars) : 0,
                s_charMeshGpu,
                s_charMeshTotal ? (s_charMeshGpu * 100 / s_charMeshTotal) : 0,
                (int)s_skinSkip, (int)s_gpuBmdEnabled);
            s_statFrameCtr = 0;
        }
        s_charMeshTotal = 0;
        s_charMeshGpu = 0;
        s_visibleChars = 0;
    }
}
