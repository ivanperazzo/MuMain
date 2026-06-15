///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "Render/Textures/ZzzOpenglUtil.h"
#include "Engine/Object/ZzzInfomation.h" 
#include "ZzzBMD.h"
#include "Engine/Object/ZzzObject.h"
#include "Engine/Object/ZzzCharacter.h"
#include "Render/Terrain/ZzzLodTerrain.h"
#include "Render/Textures/ZzzTexture.h"
#include "Engine/AI/ZzzAI.h"
#include "SMD.h"
#include "Render/Effects/ZzzEffect.h"

#include "UI/Legacy/UIMng.h"
#include "Camera/CameraMove.h"
#include "Engine/Physics/PhysicsManager.h"
#include "UI/NewUI/NewUISystem.h"

#include "Render/Models/BmdGpuCache.h"   // P-bmd-gpu: GPU skinning path
#include "Render/Models/BmdInstanceBatch.h"  // P-bmd-instance: Characters batching
#include "Render/Models/ShadowInstanceBatch.h"  // P-bmd-shadow: instanced GPU shadows
#include "Render/GL/BmdShader.h"
#include "Render/GL/GLLoader.h"
#include "Core/Utilities/FrameProfiler.h"   // bottleneck profiling (Anim slot)
#include "Render/Build/WorkerArena.h"   // Task 2: per-vertex skin scratch moved per-worker (accessor macros)
#include "Render/Build/BmdRenderContext.h"   // Etapa 3b 6.2: placement state moved to per-worker ctx
#include "Render/Build/BuildEmitMode.h"       // Etapa 3b 6.8b: mesh-emission suppress on the EffectsOnly replay
#include "Render/GL/GLLog.h"             // Etapa 3b 6.9: [jobs] / GL-on-worker warn log
#include "Core/Jobs/ThreadPool.h"       // Etapa 3b 6.9: GL-on-worker guard (CurrentWorkerIndex / JobsEnabled)
#include <cassert>

BMD* Models;
BMD* ModelsDump;

short  BoundingVertices[MAX_BONES];
vec3_t BoundingMin[MAX_BONES];
vec3_t BoundingMax[MAX_BONES];

// Task 3: the file-global BoneTransform scratch was renamed to g_BoneTransformScratch
// (macro -> per-worker Render::Build::WorkerArena::boneScratch, WorkerArena.h). It was a
// transient hierarchy-concat buffer, used only when an entity has no per-entity
// OBJECT::BoneTransform; an object-like macro named BoneTransform is impossible because
// OBJECT::BoneTransform is a struct member. Per-bone chrome caches go to the arena too.
#define g_chromeage     (Render::Build::CurrentArena().chromeAge)
#define g_chromeup      (Render::Build::CurrentArena().chromeUp)
#define g_chromeright   (Render::Build::CurrentArena().chromeRight)

// Task 3 (review follow-up): BoneQuaternion (per-bone slerp scratch in BMD::Animation) is
// ZzzBMD.cpp-only, so its macro is file-local like the chrome caches. vec4_t == float[4]
// (Core/Globals/_types.h), layout-identical to arena.boneQuaternion[][4]. ParentMatrix is
// cross-TU and macro-mapped in WorkerArena.h.
static_assert(sizeof(vec4_t) == sizeof(float[4]), "BoneQuaternion arena layout drifted from vec4_t");
#define BoneQuaternion  (Render::Build::CurrentArena().boneQuaternion)

// VertexTransform/NormalTransform/IntensityTransform/LightTransform/g_chrome moved
// to the per-worker Render::Build::WorkerArena (Task 2); accessor macros in
// WorkerArena.h keep every call site unchanged.

// Etapa 3b 6.2: the placement state (BodyScale/BodyOrigin/BodyHeight) was per-render
// mutable state living on the shared Models[type] BMD; it now lives in the per-worker
// Render::Build::BmdRenderContext, reached by CurrentRenderCtx(). The file-global
// BoneScale (per-entity edge/select scale, set in ZzzObject/ZzzCharacter, read in
// SkinMesh + instanced gates here) joins it. Bare references inside BMD methods here
// (and the b->/pModel-> setters in other TUs) are repointed onto the ctx slot;
// signatures are unchanged. The set->use sequence is contiguous within a worker, so
// the per-worker single slot reproduces the old shared-member semantics byte-for-byte.
#define BodyScale   (Render::Build::CurrentRenderCtx().bodyScale)
#define BodyOrigin  (Render::Build::CurrentRenderCtx().bodyOrigin)
#define BodyHeight  (Render::Build::CurrentRenderCtx().bodyHeight)
#define BoneScale   (Render::Build::CurrentRenderCtx().boneScale)

// Etapa 3b 6.3: the lighting state (LightEnable / BodyLight / ShadowAngle) was per-render
// mutable state living on the shared Models[type] BMD; it now lives in the per-worker
// Render::Build::BmdRenderContext (CurrentRenderCtx()). Bare references inside BMD methods
// here (and the b->/pModel-> setters in other TUs) are repointed onto the ctx slot;
// signatures are unchanged. The set->use sequence is contiguous within a worker, so the
// per-worker single slot reproduces the old shared-member semantics byte-for-byte.
// These object-like macros are FILE-LOCAL to ZzzBMD.cpp; the free function BodyLight(OBJECT*,
// BMD*) and identifiers like s_lastLightEnable / AmbientShadowAngle / CopyShadowAngle are
// distinct whole tokens the preprocessor will not rewrite. (ContrastEnable has no bare use
// here, so it gets no macro — only its cross-TU setters move to ctx.contrastEnable.)
#define BodyLight    (Render::Build::CurrentRenderCtx().bodyLight)
#define LightEnable  (Render::Build::CurrentRenderCtx().lightEnable)
#define ShadowAngle  (Render::Build::CurrentRenderCtx().shadowAngle)

// Etapa 3b 6.4: the animation state (BodyAngle / CurrentAction / CurrentAnimation /
// CurrentAnimationFrame) was per-render mutable state living on the shared Models[type] BMD;
// it now lives in the per-worker Render::Build::BmdRenderContext (CurrentRenderCtx()). Bare
// references inside BMD methods here (and the b->/pModel-> setters in other TUs) are repointed
// onto the ctx slot; signatures are unchanged. The set->use sequence is contiguous within a
// worker, so the per-worker single slot reproduces the old shared-member semantics byte-for-byte.
// NOTE: PriorAction is DELIBERATELY NOT macro'd — both BMD::Animation and BMD::PlayAnimation take
// a `PriorAction` PARAMETER that shadows the (migrated) BMD member; a macro would rewrite the
// parameter name and break those methods. The BMD member PriorAction has NO bare-token read in
// this file (every use here is the param or an OBJECT member), and its cross-TU setters (if any)
// move to ctx.priorAction explicitly. PlayAnimation's `*PriorAction = CurrentAction;` writes the
// caller's OBJECT field through the param pointer (the durable source) and reads ctx.currentAction.
#define BodyAngle             (Render::Build::CurrentRenderCtx().bodyAngle)
#define CurrentAction         (Render::Build::CurrentRenderCtx().currentAction)
#define CurrentAnimation      (Render::Build::CurrentRenderCtx().currentAnimation)
#define CurrentAnimationFrame (Render::Build::CurrentRenderCtx().currentAnimationFrame)

// Etapa 3b 6.5: the mesh-selection state (StreamMesh / Skin / HideSkin) and the bounding-size
// output (fTransformedSize) were per-render mutable state living on the shared Models[type] BMD;
// they now live in the per-worker Render::Build::BmdRenderContext (CurrentRenderCtx()). Bare
// references inside BMD methods here (and the b->/pModel->/Models[].  setters in other TUs) are
// repointed onto the ctx slot; signatures are unchanged. The set->use sequence is contiguous
// within a worker, so the per-worker single slot reproduces the old shared-member semantics.
// Macro safety: each of these is a WHOLE TOKEN the preprocessor rewrites exactly. SkinMesh /
// EnsureMeshSkinned / MarkMeshSkinned / s_meshSkinned / IsSkin / BITMAP_SKIN / getStreamMesh are
// DISTINCT tokens (the macros never touch them). There is no `x->StreamMesh`/`x.Skin`/etc. and no
// local/param named StreamMesh/Skin/HideSkin/fTransformedSize in this file (the `this->StreamMesh`
// site was rewritten to bare `StreamMesh`; the loop locals are lowercase `streamMesh`), so the
// object-like macros are safe.
#define StreamMesh       (Render::Build::CurrentRenderCtx().streamMesh)
#define Skin             (Render::Build::CurrentRenderCtx().skin)
#define HideSkin         (Render::Build::CurrentRenderCtx().hideSkin)
#define fTransformedSize (Render::Build::CurrentRenderCtx().fTransformedSize)

vec3_t RenderArrayVertices[MAX_VERTICES * 3];
vec4_t RenderArrayColors[MAX_VERTICES * 3];
vec2_t RenderArrayTexCoords[MAX_VERTICES * 3];

bool  StopMotion = false;
// Task 3 (review follow-up): file-global `float ParentMatrix[3][4];` removed — now a
// per-worker arena member via the ParentMatrix macro (WorkerArena.h). Transient root-bone
// concat scratch; was never read across calls. Same TUs that used the old extern now
// include WorkerArena.h.

static vec3_t LightVector = { 0.f, -0.1f, -0.8f };
static vec3_t LightVector2 = { 0.f, -0.5f, -0.8f };

void BMD::Animation(float(*BoneMatrix)[3][4], float AnimationFrame, float PriorFrame, unsigned short PriorAction, vec3_t Angle, vec3_t HeadAngle, bool Parent, bool Translate)
{
    FRAME_PROFILE(Anim);
    if (NumActions <= 0) return;

    if (PriorAction >= NumActions) PriorAction = 0;
    if (CurrentAction >= NumActions)CurrentAction = 0;
    VectorCopy(Angle, BodyAngle);

    CurrentAnimation = AnimationFrame;
    CurrentAnimationFrame = (int)AnimationFrame;
    float s1 = (CurrentAnimation - CurrentAnimationFrame);
    float s2 = 1.f - s1;
    auto PriorAnimationFrame = (int)PriorFrame;
    if (NumActions > 0)
    {
        if (PriorAnimationFrame < 0)
            PriorAnimationFrame = 0;
        if (CurrentAnimationFrame < 0)
            CurrentAnimationFrame = 0;
        if (PriorAnimationFrame >= Actions[PriorAction].NumAnimationKeys)
            PriorAnimationFrame = 0;
        if (CurrentAnimationFrame >= Actions[CurrentAction].NumAnimationKeys)
            CurrentAnimationFrame = 0;
    }

    // bones
    for (int i = 0; i < NumBones; i++)
    {
        Bone_t* b = &Bones[i];
        if (b->Dummy)
        {
            continue;
        }
        BoneMatrix_t* bm1 = &b->BoneMatrixes[PriorAction];
        BoneMatrix_t* bm2 = &b->BoneMatrixes[CurrentAction];
        vec4_t q1, q2;

        if (i == BoneHead)
        {
            vec3_t Angle1, Angle2;
            VectorCopy(bm1->Rotation[PriorAnimationFrame], Angle1);
            VectorCopy(bm2->Rotation[CurrentAnimationFrame], Angle2);

            float HeadAngleX = HeadAngle[0] / (180.f / Q_PI);
            float HeadAngleY = HeadAngle[1] / (180.f / Q_PI);
            Angle1[0] -= HeadAngleX;
            Angle2[0] -= HeadAngleX;
            Angle1[2] -= HeadAngleY;
            Angle2[2] -= HeadAngleY;
            AngleQuaternion(Angle1, q1);
            AngleQuaternion(Angle2, q2);
        }
        else
        {
            QuaternionCopy(bm1->Quaternion[PriorAnimationFrame], q1);
            QuaternionCopy(bm2->Quaternion[CurrentAnimationFrame], q2);
        }
        if (!QuaternionCompare(q1, q2))
        {
            QuaternionSlerp(q1, q2, s1, BoneQuaternion[i]);
        }
        else
        {
            QuaternionCopy(q1, BoneQuaternion[i]);
        }

        float Matrix[3][4];
        QuaternionMatrix(BoneQuaternion[i], Matrix);
        float* Position1 = bm1->Position[PriorAnimationFrame];
        float* Position2 = bm2->Position[CurrentAnimationFrame];

        if (i == 0 && (Actions[PriorAction].LockPositions || Actions[CurrentAction].LockPositions))
        {
            Matrix[0][3] = bm2->Position[0][0];
            Matrix[1][3] = bm2->Position[0][1];
            Matrix[2][3] = Position1[2] * s2 + Position2[2] * s1 + BodyHeight;
        }
        else
        {
            Matrix[0][3] = Position1[0] * s2 + Position2[0] * s1;
            Matrix[1][3] = Position1[1] * s2 + Position2[1] * s1;
            Matrix[2][3] = Position1[2] * s2 + Position2[2] * s1;
        }

        if (b->Parent == -1)
        {
            if (!Parent)
            {
                AngleMatrix(BodyAngle, ParentMatrix);
                if (Translate)
                {
                    for (auto & y : ParentMatrix)
                    {
                        for (int x = 0; x < 3; ++x)
                        {
                            y[x] *= BodyScale;
                        }
                    }

                    ParentMatrix[0][3] = BodyOrigin[0];
                    ParentMatrix[1][3] = BodyOrigin[1];
                    ParentMatrix[2][3] = BodyOrigin[2];
                }
            }
            R_ConcatTransforms(ParentMatrix, Matrix, BoneMatrix[i]);
        }
        else
        {
            R_ConcatTransforms(BoneMatrix[b->Parent], Matrix, BoneMatrix[i]);
        }
    }
}

extern EGameScene SceneFlag;
extern int EditFlag;

bool HighLight = true;
// Etapa 3b 6.2: the file-global BoneScale moved to Render::Build::BmdRenderContext::boneScale
// (per-worker). Bare references below are macro-mapped to CurrentRenderCtx().boneScale.

// Etapa 3b 6.6: the per-Transform correlation context (P-bmd-gpu / P-bmd-skinskip /
// P-bmd-instance) used to be file-scope statics here. They encode the "Transform precedes
// RenderMesh/InstAdd for the SAME object, no intervening Transform" invariant; under
// parallel Phase B two workers would race on them. They now live in the per-worker
// Render::Build::BmdRenderContext (CurrentRenderCtx()), so the set->use correlation holds
// per-worker. These are FILE-STATICS, NOT BMD members, so there is NO sizeof(BMD) concern
// and NO reserved padding (pure static->per-worker relocation, like Tasks 2-3).
//
// File-local object-like macros keep every call site unchanged. The s_*-prefixed tokens
// below are distinct whole tokens with no conflicting local/param/member in this TU, so the
// preprocessor rewrites them exactly. Initial values match the originals (skinnedSerial /
// instPaletteLastSerial = 0xFFFFFFFF via the struct default-member-init; others 0/false/null).
//
//   P-bmd-gpu: context of the last BMD::Transform call, consumed by the next RenderMesh's GPU path.
#define s_lastBoneMatrix         (Render::Build::CurrentRenderCtx().lastBoneMatrix)
#define s_lastTransformTranslate (Render::Build::CurrentRenderCtx().lastTransformTranslate)
#define s_lastTransformScale     (Render::Build::CurrentRenderCtx().lastTransformScale)
//   P-bmd-instance: monotonic id bumped per Transform (one palette appended per character-part).
#define s_transformSerial        (Render::Build::CurrentRenderCtx().transformSerial)
//   P-bmd-skinskip: context recorded by Transform so a deferred mesh can be skinned on demand later.
#define s_lastLightPosition      (Render::Build::CurrentRenderCtx().lastLightPosition)
#define s_lastLightEnable        (Render::Build::CurrentRenderCtx().lastLightEnable)
//   "skinned this mesh for this serial" set; reset when the serial changes.
#define s_skinnedSerial          (Render::Build::CurrentRenderCtx().skinnedSerial)
#define s_meshSkinned            (Render::Build::CurrentRenderCtx().meshSkinned)
// DEAD-AND-DROPPED: s_skinScratchMin/s_skinScratchMax were a "bounding sink for lazy skin
// (unused in gameplay)" — SkinMesh only WRITES them (under EditFlag==2) and nothing reads
// the result downstream. They are now a per-worker throwaway scratch on EnsureMeshSkinned's
// call to SkinMesh (see below), not a shared static. (No sizeof concern, just removing dead
// shared mutable state for an exhaustive race audit.)

void BMD::SkinMesh(int meshIndex, float(*BoneMatrix)[3][4], bool Translate, float _Scale,
                   const float* LightPosition, float* BoundingMin, float* BoundingMax) const
{
    Mesh_t* m = &Meshs[meshIndex];
    const int i = meshIndex;
    for (int j = 0; j < m->NumVertices; j++)
    {
        Vertex_t* v = &m->Vertices[j];
        float* vp = VertexTransform[i][j];

        if (BoneScale == 1.f)
        {
            if (_Scale)
            {
                vec3_t Position;
                VectorCopy(v->Position, Position);
                VectorScale(Position, _Scale, Position);
                VectorTransform(Position, BoneMatrix[v->Node], vp);
            }
            else
                VectorTransform(v->Position, BoneMatrix[v->Node], vp);
            if (Translate)
                VectorScale(vp, BodyScale, vp);
        }
        else
        {
            VectorRotate(v->Position, BoneMatrix[v->Node], vp);
            vp[0] = vp[0] * BoneScale + BoneMatrix[v->Node][0][3];
            vp[1] = vp[1] * BoneScale + BoneMatrix[v->Node][1][3];
            vp[2] = vp[2] * BoneScale + BoneMatrix[v->Node][2][3];
            if (Translate)
                VectorScale(vp, BodyScale, vp);
        }
#ifdef _DEBUG
#else
        if (EditFlag == 2)
#endif
        {
            for (int k = 0; k < 3; k++)
            {
                if (vp[k] < BoundingMin[k]) BoundingMin[k] = vp[k];
                if (vp[k] > BoundingMax[k]) BoundingMax[k] = vp[k];
            }
        }
        if (Translate)
            VectorAdd(vp, BodyOrigin, vp);
    }

    for (int j = 0; j < m->NumNormals; j++)
    {
        Normal_t* sn = &m->Normals[j];
        float* tn = NormalTransform[i][j];
        VectorRotate(sn->Normal, BoneMatrix[sn->Node], tn);
        if (LightEnable && LightPosition)
        {
            float Luminosity = DotProduct(tn, LightPosition) * 0.8f + 0.4f;
            if (Luminosity < 0.2f) Luminosity = 0.2f;
            IntensityTransform[i][j] = Luminosity;
        }
    }
}

void BMD::EnsureMeshSkinned(int meshIndex) const
{
    if (s_skinnedSerial != s_transformSerial)
    {
        for (bool& b : s_meshSkinned) b = false;
        s_skinnedSerial = s_transformSerial;
    }
    if (meshIndex < 0 || meshIndex >= NumMeshs || meshIndex >= MAX_MESH)
        return;
    if (s_meshSkinned[meshIndex] || s_lastBoneMatrix == nullptr)
        return;
    s_meshSkinned[meshIndex] = true;
    // s_skinScratchMin/Max dropped (dead output): SkinMesh's bounding write under EditFlag==2
    // is discarded downstream, so feed a local throwaway sink instead of a shared static.
    vec3_t skinScratchMin, skinScratchMax;
    SkinMesh(meshIndex, s_lastBoneMatrix, s_lastTransformTranslate, s_lastTransformScale,
             s_lastLightEnable ? s_lastLightPosition : nullptr, skinScratchMin, skinScratchMax);
}

void BMD::MarkMeshSkinned(int meshIndex) const
{
    if (s_skinnedSerial != s_transformSerial)
    {
        for (bool& b : s_meshSkinned) b = false;
        s_skinnedSerial = s_transformSerial;
    }
    if (meshIndex >= 0 && meshIndex < MAX_MESH)
        s_meshSkinned[meshIndex] = true;
}

void BMD::Transform(float(*BoneMatrix)[3][4], vec3_t BoundingBoxMin, vec3_t BoundingBoxMax, OBB_t* OBB, bool Translate, float _Scale)
{
    FRAME_PROFILE(Anim);
    // P-bmd-gpu: record the context this CPU transform ran with so a following
    // RenderMesh can reproduce it exactly on the GPU (A/B identical).
    s_lastBoneMatrix        = BoneMatrix;
    s_lastTransformTranslate = Translate;
    s_lastTransformScale     = _Scale;
    ++s_transformSerial;

    vec3_t LightPosition;

    if (LightEnable)
    {
        vec3_t Position;

        float Matrix[3][4];
        if (HighLight)
        {
            Vector(1.3f, 0.f, 2.f, Position);
        }
        else if (gMapManager.InBattleCastle())
        {
            Vector(0.5f, -1.f, 1.f, Position);
            Vector(0.f, 0.f, -45.f, ShadowAngle);
        }
        else
        {
            Vector(0.f, -1.5f, 0.f, Position);
        }

        AngleMatrix(ShadowAngle, Matrix);
        VectorIRotate(Position, Matrix, LightPosition);
    }
    vec3_t BoundingMin;
    vec3_t BoundingMax;
#ifdef _DEBUG
#else
    if (EditFlag == 2)
#endif
    {
        Vector(999999.f, 999999.f, 999999.f, BoundingMin);
        Vector(-999999.f, -999999.f, -999999.f, BoundingMax);
    }
    // P-bmd-skinskip: record context for lazy skin + reset the per-serial skinned set.
    s_lastLightEnable = LightEnable;
    if (LightEnable)
        VectorCopy(LightPosition, s_lastLightPosition);
    for (bool& b : s_meshSkinned) b = false;
    s_skinnedSerial = s_transformSerial;

    // measureSkip (MU_SKINSKIP): raw skip, breaks visuals -- skinning-ceiling measurement.
    // deferActive (MU_GPUSKIN): skip CPU skin in the instanced character pass; consumers
    // that still read VertexTransform/NormalTransform (legacy draw, CPU shadow fallback,
    // effects, side-hair, cloth) force-skin lazily via EnsureMeshSkinned. Requires GPU
    // shadows ON (else the CPU shadow would read unskinned vertices for every mesh).
    const bool measureSkip = Render::Models::SkinSkip();
    const bool deferActive = Render::Models::GpuSkinDeferEnabled()
        && Render::Models::GpuCharsPass() && Render::Models::GpuBmdEnabled()
        && Render::Models::GpuInstEnabled() && Render::Models::GpuShadowEnabled();

    for (int i = 0; i < NumMeshs; i++)
    {
        if (measureSkip) continue;
        if (deferActive) continue;   // leave unskinned; EnsureMeshSkinned does it on read
        SkinMesh(i, BoneMatrix, Translate, _Scale,
                 LightEnable ? LightPosition : nullptr, BoundingMin, BoundingMax);
        if (i < MAX_MESH) s_meshSkinned[i] = true;
    }
    if (EditFlag == 2)
    {
        VectorCopy(BoundingMin, OBB->StartPos);
        OBB->XAxis[0] = (BoundingMax[0] - BoundingMin[0]);
        OBB->YAxis[1] = (BoundingMax[1] - BoundingMin[1]);
        OBB->ZAxis[2] = (BoundingMax[2] - BoundingMin[2]);
    }
    else
    {
        VectorCopy(BoundingBoxMin, OBB->StartPos);
        OBB->XAxis[0] = (BoundingBoxMax[0] - BoundingBoxMin[0]);
        OBB->YAxis[1] = (BoundingBoxMax[1] - BoundingBoxMin[1]);
        OBB->ZAxis[2] = (BoundingBoxMax[2] - BoundingBoxMin[2]);
    }
    fTransformedSize = std::max<float>(std::max<float>(BoundingMax[0] - BoundingMin[0], BoundingMax[1] - BoundingMin[1]),
        BoundingMax[2] - BoundingMin[2]);
    VectorAdd(OBB->StartPos, BodyOrigin, OBB->StartPos);
    OBB->XAxis[1] = 0.f;
    OBB->XAxis[2] = 0.f;
    OBB->YAxis[0] = 0.f;
    OBB->YAxis[2] = 0.f;
    OBB->ZAxis[0] = 0.f;
    OBB->ZAxis[1] = 0.f;
}

void BMD::TransformByObjectBone(vec3_t vResultPosition, OBJECT* pObject, int iBoneNumber, vec3_t vRelativePosition)
{
    if (iBoneNumber < 0 || iBoneNumber >= NumBones)
    {
        assert(!"Bone number error");
        return;
    }
    if (pObject == nullptr)
    {
        assert(!"Empty Bone");
        return;
    }

    float(*TransformMatrix)[4];
    if (pObject->BoneTransform != nullptr)
    {
        TransformMatrix = pObject->BoneTransform[iBoneNumber];
    }
    else
    {
        TransformMatrix = g_BoneTransformScratch[iBoneNumber];
    }

    vec3_t vTemp;
    if (vRelativePosition == nullptr)
    {
        vTemp[0] = TransformMatrix[0][3];
        vTemp[1] = TransformMatrix[1][3];
        vTemp[2] = TransformMatrix[2][3];
    }
    else
    {
        VectorTransform(vRelativePosition, TransformMatrix, vTemp);
    }
    VectorScale(vTemp, BodyScale, vTemp);
    VectorAdd(vTemp, pObject->Position, vResultPosition);
}

void BMD::TransformByBoneMatrix(vec3_t vResultPosition, float(*BoneMatrix)[4], vec3_t vWorldPosition, vec3_t vRelativePosition)
{
    if (BoneMatrix == nullptr)
    {
        assert(!"Empty Matrix");
        return;
    }

    vec3_t vTemp;
    if (vRelativePosition == nullptr)
    {
        vTemp[0] = BoneMatrix[0][3];
        vTemp[1] = BoneMatrix[1][3];
        vTemp[2] = BoneMatrix[2][3];
    }
    else
    {
        VectorTransform(vRelativePosition, BoneMatrix, vTemp);
    }
    if (vWorldPosition != nullptr)
    {
        VectorScale(vTemp, BodyScale, vTemp);
        VectorAdd(vTemp, vWorldPosition, vResultPosition);
    }
    else
    {
        VectorScale(vTemp, BodyScale, vResultPosition);
    }
}

void BMD::TransformPosition(float(*Matrix)[4], vec3_t Position, vec3_t WorldPosition, bool Translate)
{
    if (Translate)
    {
        vec3_t p;
        VectorTransform(Position, Matrix, p);
        VectorScale(p, BodyScale, p);
        VectorAdd(p, BodyOrigin, WorldPosition);
    }
    else
        VectorTransform(Position, Matrix, WorldPosition);
}

void BMD::RotationPosition(float(*Matrix)[4], vec3_t Position, vec3_t WorldPosition)
{
    vec3_t p;
    VectorRotate(Position, Matrix, p);
    VectorScale(p, BodyScale, WorldPosition);
    for (int i = 0; i < 3; i++)
    {
        for (int j = 0; j < 4; j++)
        {
            ParentMatrix[i][j] = Matrix[i][j];
        }
    }
}

bool BMD::PlayAnimation(float* AnimationFrame, float* PriorAnimationFrame, unsigned short* PriorAction, float Speed, vec3_t Origin, vec3_t Angle)
{
    bool Loop = true;

    if (AnimationFrame == nullptr || PriorAnimationFrame == nullptr || PriorAction == nullptr || Actions == nullptr || (NumActions > 0 && CurrentAction >= NumActions))
    {
        return Loop;
    }

    if (NumActions == 0 || Actions[CurrentAction].NumAnimationKeys <= 1)
    {
        return Loop;
    }

    const auto priorAnimationFrame = (int)*AnimationFrame;
    *AnimationFrame += Speed * FPS_ANIMATION_FACTOR;
    if (priorAnimationFrame != (int)*AnimationFrame)
    {
        *PriorAction = CurrentAction;
        *PriorAnimationFrame = (float)priorAnimationFrame;
    }
    if (*AnimationFrame <= 0.f)
    {
        *AnimationFrame += (float)Actions[CurrentAction].NumAnimationKeys - 1.f;
    }

    if (Actions[CurrentAction].Loop)
    {
        if (*AnimationFrame >= (float)Actions[CurrentAction].NumAnimationKeys)
        {
            *AnimationFrame = (float)Actions[CurrentAction].NumAnimationKeys - 0.01f;
            Loop = false;
        }
    }
    else
    {
        int Key;
        if (Actions[CurrentAction].LockPositions)
            Key = Actions[CurrentAction].NumAnimationKeys - 1;
        else
            Key = Actions[CurrentAction].NumAnimationKeys;

        float fTemp;

        if (SceneFlag == CHARACTER_SCENE)
        {
            fTemp = *AnimationFrame + 2;
        }
        else if (gMapManager.WorldActive == WD_39KANTURU_3RD && CurrentAction == MONSTER01_APEAR)
        {
            fTemp = *AnimationFrame + 1;
        }
        else
        {
            fTemp = *AnimationFrame;
        }

        if (fTemp >= (int)Key)
        {
            auto Frame = (int)*AnimationFrame;
            *AnimationFrame = (float)(Frame % (Key)) + (*AnimationFrame - (float)Frame);
            Loop = false;
        }
    }
    CurrentAnimation = *AnimationFrame;
    CurrentAnimationFrame = (int)maxf(0, CurrentAnimation);

    return Loop;
}
void BMD::AnimationTransformWithAttachHighModel_usingGlobalTM(OBJECT* oHighHierarchyModel, BMD* bmdHighHierarchyModel, int iBoneNumberHighHierarchyModel, vec3_t& vOutPosHighHiearachyModelBone, vec3_t* arrOutSetfAllBonePositions, bool bApplyTMtoVertices)
{
    if (NumBones < 1) return;
    if (NumBones > MAX_BONES) return;

    vec34_t* arrBonesTMLocal;

    vec34_t		tmBoneHierarchicalObject;

    vec3_t		Temp, v3Position;
    OBB_t		OBB;

    arrBonesTMLocal = new vec34_t[NumBones];
    Vector(0.0f, 0.0f, 0.0f, Temp);

    memset(arrBonesTMLocal, 0, sizeof(vec34_t) * NumBones);
    memset(tmBoneHierarchicalObject, 0, sizeof(vec34_t));

    memcpy(tmBoneHierarchicalObject, oHighHierarchyModel->BoneTransform[iBoneNumberHighHierarchyModel], sizeof(vec34_t));
    BodyScale = oHighHierarchyModel->Scale;

    tmBoneHierarchicalObject[0][3] = tmBoneHierarchicalObject[0][3] * BodyScale;
    tmBoneHierarchicalObject[1][3] = tmBoneHierarchicalObject[1][3] * BodyScale;
    tmBoneHierarchicalObject[2][3] = tmBoneHierarchicalObject[2][3] * BodyScale;

    if (nullptr != vOutPosHighHiearachyModelBone)
    {
        Vector(tmBoneHierarchicalObject[0][3], tmBoneHierarchicalObject[1][3], tmBoneHierarchicalObject[2][3],
            vOutPosHighHiearachyModelBone);
    }

    VectorCopy(oHighHierarchyModel->Position, v3Position);

    Animation(arrBonesTMLocal, 0, 0, 0, Temp, Temp, false, false);

    for (int i_ = 0; i_ < NumBones; ++i_)
    {
        R_ConcatTransforms(tmBoneHierarchicalObject, arrBonesTMLocal[i_], g_BoneTransformScratch[i_]);
        g_BoneTransformScratch[i_][0][3] = g_BoneTransformScratch[i_][0][3] + v3Position[0];
        g_BoneTransformScratch[i_][1][3] = g_BoneTransformScratch[i_][1][3] + v3Position[1];
        g_BoneTransformScratch[i_][2][3] = g_BoneTransformScratch[i_][2][3] + v3Position[2];

        Vector(g_BoneTransformScratch[i_][0][3],
            g_BoneTransformScratch[i_][1][3],
            g_BoneTransformScratch[i_][2][3],
            arrOutSetfAllBonePositions[i_]);
    }

    if (true == bApplyTMtoVertices)
    {
        Transform(g_BoneTransformScratch, Temp, Temp, &OBB, false);
    }

    delete[] arrBonesTMLocal;
}

void BMD::AnimationTransformWithAttachHighModel(OBJECT* oHighHierarchyModel, BMD* bmdHighHierarchyModel, int iBoneNumberHighHierarchyModel, vec3_t& vOutPosHighHiearachyModelBone, vec3_t* arrOutSetfAllBonePositions)
{
    if (NumBones < 1) return;
    if (NumBones > MAX_BONES) return;

    vec34_t* arrBonesTMLocal;
    vec34_t* arrBonesTMLocalResult;
    vec34_t		tmBoneHierarchicalObject;
    vec3_t		Temp, v3Position;

    arrBonesTMLocal = new vec34_t[NumBones];
    Vector(0.0f, 0.0f, 0.0f, Temp);

    arrBonesTMLocalResult = new vec34_t[NumBones];

    memset(arrBonesTMLocalResult, 0, sizeof(vec34_t) * NumBones);
    memset(arrBonesTMLocal, 0, sizeof(vec34_t) * NumBones);

    memset(tmBoneHierarchicalObject, 0, sizeof(vec34_t));

    memcpy(tmBoneHierarchicalObject, oHighHierarchyModel->BoneTransform[iBoneNumberHighHierarchyModel], sizeof(vec34_t));

    BodyScale = oHighHierarchyModel->Scale;

    tmBoneHierarchicalObject[0][3] = tmBoneHierarchicalObject[0][3] * BodyScale;
    tmBoneHierarchicalObject[1][3] = tmBoneHierarchicalObject[1][3] * BodyScale;
    tmBoneHierarchicalObject[2][3] = tmBoneHierarchicalObject[2][3] * BodyScale;

    if (nullptr != vOutPosHighHiearachyModelBone)
    {
        Vector(tmBoneHierarchicalObject[0][3], tmBoneHierarchicalObject[1][3], tmBoneHierarchicalObject[2][3],
            vOutPosHighHiearachyModelBone);
    }

    VectorCopy(oHighHierarchyModel->Position, v3Position);

    Animation(arrBonesTMLocal, 0, 0, 0, Temp, Temp, false, false);
    for (int i_ = 0; i_ < NumBones; ++i_)
    {
        R_ConcatTransforms(tmBoneHierarchicalObject, arrBonesTMLocal[i_], arrBonesTMLocalResult[i_]);
        arrBonesTMLocalResult[i_][0][3] = arrBonesTMLocalResult[i_][0][3] + v3Position[0];
        arrBonesTMLocalResult[i_][1][3] = arrBonesTMLocalResult[i_][1][3] + v3Position[1];
        arrBonesTMLocalResult[i_][2][3] = arrBonesTMLocalResult[i_][2][3] + v3Position[2];

        Vector(arrBonesTMLocalResult[i_][0][3], arrBonesTMLocalResult[i_][1][3], arrBonesTMLocalResult[i_][2][3], arrOutSetfAllBonePositions[i_]);
    }

    delete[] arrBonesTMLocalResult;
    delete[] arrBonesTMLocal;
}

void BMD::AnimationTransformOnlySelf(vec3_t* arrOutSetfAllBonePositions, const OBJECT* oSelf)
{
    if (NumBones < 1) return;
    if (NumBones > MAX_BONES) return;

    vec34_t* arrBonesTMLocal;

    vec3_t		Temp;

    arrBonesTMLocal = new vec34_t[NumBones];
    Vector(0.0f, 0.0f, 0.0f, Temp);

    memset(arrBonesTMLocal, 0, sizeof(vec34_t) * NumBones);

    Animation(arrBonesTMLocal, oSelf->AnimationFrame, oSelf->PriorAnimationFrame, oSelf->PriorAction, (const_cast<OBJECT*>(oSelf))->Angle, Temp, false, true);

    for (int i_ = 0; i_ < NumBones; ++i_)
    {
        Vector(arrBonesTMLocal[i_][0][3], arrBonesTMLocal[i_][1][3], arrBonesTMLocal[i_][2][3], arrOutSetfAllBonePositions[i_]);
    }
    delete[] arrBonesTMLocal;
}

void BMD::AnimationTransformOnlySelf(vec3_t* arrOutSetfAllBonePositions,
    const vec3_t& v3Angle,
    const vec3_t& v3Position,
    const float& fScale,
    OBJECT* oRefAnimation,
    const float fFrameArea,
    const float fWeight)
{
    if (NumBones < 1) return;
    if (NumBones > MAX_BONES) return;

    vec34_t* arrBonesTMLocal;
    vec3_t		v3RootAngle, v3RootPosition;
    float		fRootScale;
    vec3_t		Temp;

    fRootScale = const_cast<float&>(fScale);

    v3RootAngle[0] = v3Angle[0];
    v3RootAngle[1] = v3Angle[1];
    v3RootAngle[2] = v3Angle[2];

    v3RootPosition[0] = v3Position[0];
    v3RootPosition[1] = v3Position[1];
    v3RootPosition[2] = v3Position[2];

    arrBonesTMLocal = new vec34_t[NumBones];
    Vector(0.0f, 0.0f, 0.0f, Temp);

    memset(arrBonesTMLocal, 0, sizeof(vec34_t) * NumBones);

    if (nullptr == oRefAnimation)
    {
        Animation(arrBonesTMLocal, 0, 0, 0, v3RootAngle, Temp, false, true);
    }
    else
    {
        float			fAnimationFrame = oRefAnimation->AnimationFrame,
            fPiriorAnimationFrame = oRefAnimation->PriorAnimationFrame;
        unsigned short	iPiriorAction = oRefAnimation->PriorAction;

        if (fWeight >= 0.0f && fFrameArea > 0.0f)
        {
            float fAnimationFrameStart = fAnimationFrame - fFrameArea;
            float fAnimationFrameEnd = fAnimationFrame;
            LInterpolationF(fAnimationFrame, fAnimationFrameStart, fAnimationFrameEnd, fWeight);
        }

        Animation(arrBonesTMLocal,
            fAnimationFrame,
            fPiriorAnimationFrame,
            iPiriorAction,
            v3RootAngle, Temp, false, true);
    }

    vec3_t	v3RelatePos;
    Vector(1.0f, 1.0f, 1.0f, v3RelatePos);
    for (int i_ = 0; i_ < NumBones; ++i_)
    {
        Vector(arrBonesTMLocal[i_][0][3],
            arrBonesTMLocal[i_][1][3],
            arrBonesTMLocal[i_][2][3],
            arrOutSetfAllBonePositions[i_]);
    }

    delete[] arrBonesTMLocal;
}

// Task 3 (review follow-up): the file-global `vec3_t g_vright;` was written to the invariant
// constant (0,0,1) at the top of BMD::Chrome and read only within that same function — a
// benign write-then-read of a constant, but still a shared mutable global. Made a function-
// local in BMD::Chrome (below) so it is no longer a global at all. Used nowhere else.
// Task 3: g_smodels_total is a read-only "frame id" cookie. It is NEVER incremented
// anywhere in the tree (grep: one def + one read), so it is effectively a constant 1 and
// the original chrome-cache invalidation it keyed is dead. Kept read-only; the per-entity
// build path must never write it (Task 6 race-audit relies on this).
const int	g_smodels_total = 1;				// cookie (read-only; never bumped)
// g_chrome / g_chromeage / g_chromeup / g_chromeright moved to Render::Build::WorkerArena
// (Tasks 2-3; macros above + in WorkerArena.h).

void BMD::Chrome(float* pchrome, int bone, vec3_t normal)
{
    vec3_t g_vright;	// viewer's right; invariant (0,0,1) here, set per-call (was a file-global)
    Vector(0.f, 0.f, 1.f, g_vright);

    float n;

    {
        vec3_t chromeupvec;		
        vec3_t chromerightvec;
        vec3_t tmp;			
        VectorScale(BodyOrigin, -1, tmp);
        VectorNormalize(tmp);
        CrossProduct(tmp, g_vright, chromeupvec);
        VectorNormalize(chromeupvec);
        CrossProduct(tmp, chromeupvec, chromerightvec);
        VectorNormalize(chromerightvec);

        // Vestigial: g_smodels_total is const 1 and NOTHING reads g_chromeage to branch
        // (grep: this is its only access). Retained for serial-identical safety; the original
        // chrome-cache invalidation it keyed is dead. Do not reintroduce a reader (Task 6).
        g_chromeage[bone] = g_smodels_total;
    }

    n = DotProduct(normal, g_chromeright[bone]);
    pchrome[0] = (n + 1.f); // FIX: make this a float

    n = DotProduct(normal, g_chromeup[bone]);
    pchrome[1] = (n + 1.f); // FIX: make this a float
}

void BMD::Lighting(float* pLight, Light_t* lp, vec3_t Position, vec3_t Normal)
{
    vec3_t Light;
    VectorSubtract(lp->Position, Position, Light);
    float Length = sqrtf(Light[0] * Light[0] + Light[1] * Light[1] + Light[2] * Light[2]);

    float LightCos = (DotProduct(Normal, Light) / Length) * 0.8f + 0.3f;
    if (Length > lp->Range) LightCos -= (Length - lp->Range) * 0.01f;
    if (LightCos < 0.f) LightCos = 0.f;
    pLight[0] += LightCos * lp->Color[0];
    pLight[1] += LightCos * lp->Color[1];
    pLight[2] += LightCos * lp->Color[2];
}

///////////////////////////////////////////////////////////////////////////////
// light map
///////////////////////////////////////////////////////////////////////////////

#define AXIS_X  0
#define AXIS_Y  1
#define AXIS_Z  2

float SubPixel = 16.f;

void SmoothBitmap(int Width, int Height, unsigned char* Buffer)
{
    int RowStride = Width * 3;
    for (int i = 1; i < Height - 1; i++)
    {
        for (int j = 1; j < Width - 1; j++)
        {
            int Index = (i * Width + j) * 3;
            for (int k = 0; k < 3; k++)
            {
                Buffer[Index] = (Buffer[Index - RowStride - 3] + Buffer[Index - RowStride] + Buffer[Index - RowStride + 3] +
                    Buffer[Index - 3] + Buffer[Index + 3] +
                    Buffer[Index + RowStride - 3] + Buffer[Index + RowStride] + Buffer[Index + RowStride + 3]) / 8;
                Index++;
            }
        }
    }
}

bool BMD::CollisionDetectLineToMesh(vec3_t Position, vec3_t Target, bool Collision, int Mesh, int Triangle)
{
    int i, j;
    for (i = 0; i < NumMeshs; i++)
    {
        Mesh_t* m = &Meshs[i];

        for (j = 0; j < m->NumTriangles; j++)
        {
            if (i == Mesh && j == Triangle) continue;
            Triangle_t* tp = &m->Triangles[j];
            float* vp1 = VertexTransform[i][tp->VertexIndex[0]];
            float* vp2 = VertexTransform[i][tp->VertexIndex[1]];
            float* vp3 = VertexTransform[i][tp->VertexIndex[2]];
            float* vp4 = VertexTransform[i][tp->VertexIndex[3]];

            vec3_t Normal;
            FaceNormalize(vp1, vp2, vp3, Normal);
            bool success = CollisionDetectLineToFace(Position, Target, tp->Polygon, vp1, vp2, vp3, vp4, Normal, Collision);
            if (success == true) return true;
        }
    }
    return false;
}

void BMD::CreateLightMapSurface(Light_t* lp, Mesh_t* m, int i, int j, int MapWidth, int MapHeight, int MapWidthMax, int MapHeightMax, vec3_t BoundingMin, vec3_t BoundingMax, int Axis)
{
    int k, l;
    Triangle_t* tp = &m->Triangles[j];
    float* np = NormalTransform[i][tp->NormalIndex[0]];
    float* vp = VertexTransform[i][tp->VertexIndex[0]];
    float d = -DotProduct(vp, np);

    Bitmap_t* lmp = &LightMaps[NumLightMaps];
    if (lmp->Buffer == nullptr)
    {
        lmp->Width = MapWidthMax;
        lmp->Height = MapHeightMax;
        int BufferBytes = lmp->Width * lmp->Height * 3;
        lmp->Buffer = new unsigned char[BufferBytes];
        memset(lmp->Buffer, 0, BufferBytes);
    }

    for (k = 0; k < MapHeight; k++)
    {
        for (l = 0; l < MapWidth; l++)
        {
            vec3_t p;
            Vector(0.f, 0.f, 0.f, p);
            switch (Axis)
            {
            case AXIS_Z:
                p[0] = BoundingMin[0] + l * SubPixel;
                p[1] = BoundingMin[1] + k * SubPixel;
                if (p[0] >= BoundingMax[0]) p[0] = BoundingMax[0];
                if (p[1] >= BoundingMax[1]) p[1] = BoundingMax[1];
                p[2] = (np[0] * p[0] + np[1] * p[1] + d) / -np[2];
                break;
            case AXIS_Y:
                p[0] = BoundingMin[0] + (float)l * SubPixel;
                p[2] = BoundingMin[2] + (float)k * SubPixel;
                if (p[0] >= BoundingMax[0]) p[0] = BoundingMax[0];
                if (p[2] >= BoundingMax[2]) p[2] = BoundingMax[2];
                p[1] = (np[0] * p[0] + np[2] * p[2] + d) / -np[1];
                break;
            case AXIS_X:
                p[2] = BoundingMin[2] + l * SubPixel;
                p[1] = BoundingMin[1] + k * SubPixel;
                if (p[2] >= BoundingMax[2]) p[2] = BoundingMax[2];
                if (p[1] >= BoundingMax[1]) p[1] = BoundingMax[1];
                p[0] = (np[2] * p[2] + np[1] * p[1] + d) / -np[0];
                break;
            }
            vec3_t Direction;
            VectorSubtract(p, lp->Position, Direction);
            VectorNormalize(Direction);
            VectorSubtract(p, Direction, p);
            bool success = CollisionDetectLineToMesh(lp->Position, p, true, i, j);

            if (success == false)
            {
                unsigned char* Bitmap = &lmp->Buffer[(k * MapWidthMax + l) * 3];
                vec3_t Light;
                Vector(0.f, 0.f, 0.f, Light);
                Lighting(Light, lp, p, np);
                for (int c = 0; c < 3; c++)
                {
                    int color = Bitmap[c];
                    color += (unsigned char)(Light[c] * 255.f);
                    if (color > 255) color = 255;
                    Bitmap[c] = color;
                }
            }
        }
    }
}

void BMD::CreateLightMaps()
{
}

void BMD::BindLightMaps()
{
    if (LightMapEnable == true) return;

    for (int i = 0; i < NumLightMaps; i++)
    {
        Bitmap_t* lmp = &LightMaps[i];
        if (lmp->Buffer != nullptr)
        {
            SmoothBitmap(lmp->Width, lmp->Height, lmp->Buffer);
            SmoothBitmap(lmp->Width, lmp->Height, lmp->Buffer);

            glBindTexture(GL_TEXTURE_2D, i + IndexLightMap);
            glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, lmp->Width, lmp->Height, 0, GL_RGB, GL_UNSIGNED_BYTE, lmp->Buffer);
        }
    }
    LightMapEnable = true;
}

void BMD::ReleaseLightMaps()
{
    if (LightMapEnable == false) return;
    for (int i = 0; i < NumLightMaps; i++)
    {
        Bitmap_t* lmp = &LightMaps[i];
        if (lmp->Buffer != nullptr)
        {
            delete lmp->Buffer;
            lmp->Buffer = nullptr;
        }
    }
    LightMapEnable = false;
}

void BMD::BeginRender(float Alpha)
{
    glPushMatrix();
}

void BMD::EndRender()
{
    glPopMatrix();
}

extern double WorldTime;
extern int WaterTextureNumber;

void BMD::BeginRenderCoinHeap()
{
    constexpr int meshIndex = 0;
    Mesh_t* m = &Meshs[meshIndex];
    const auto textureIndex = IndexTexture[m->Texture];

    BindTexture(textureIndex);
    DisableAlphaBlend();

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
}

int BMD::AddToCoinHeap(int coinIndex, int target_vertex_index)
{
    const auto vertices = RenderArrayVertices;
    const auto colors = RenderArrayColors;
    const auto texCoords = RenderArrayTexCoords;

    constexpr auto alpha = 1.0f;
    constexpr int meshIndex = 0;

    Mesh_t* m = &Meshs[meshIndex];

    for (int j = 0; j < m->NumTriangles; j++)
    {
        const auto triangle = &m->Triangles[j];
        for (int k = 0; k < triangle->Polygon; k++)
        {
            const int source_vertex_index = triangle->VertexIndex[k];
            target_vertex_index++;

            VectorCopy(VertexTransform[meshIndex][source_vertex_index], vertices[target_vertex_index]);

            Vector4(BodyLight[0], BodyLight[1], BodyLight[2], alpha, colors[target_vertex_index]);

            auto texco = m->TexCoords[triangle->TexCoordIndex[k]];
            texCoords[target_vertex_index][0] = texco.TexCoordU;
            texCoords[target_vertex_index][1] = texco.TexCoordV;
        }
    }

    return target_vertex_index;
}

void BMD::EndRenderCoinHeap(int coinCount)
{
    const auto vertices = RenderArrayVertices;
    const auto colors = RenderArrayColors;
    const auto texCoords = RenderArrayTexCoords;

    glVertexPointer(3, GL_FLOAT, 0, vertices);
    glColorPointer(4, GL_FLOAT, 0, colors);
    glTexCoordPointer(2, GL_FLOAT, 0, texCoords);

    constexpr int meshIndex = 0;
    Mesh_t* m = &Meshs[meshIndex];
    glDrawArrays(GL_TRIANGLES, 0, m->NumTriangles * 3 * coinCount);

    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
}

// P-bmd-gpu: draw one mesh via the GPU skinning shader + cached VBO, reproducing
// BMD::Transform's math (bones + BodyScale/Origin + per-normal lighting). Called
// from RenderMesh AFTER its texture/blend setup, so it reuses that state and only
// replaces the CPU per-vertex rebuild + draw. Returns false (state untouched) if
// the shader isn't ready -> caller falls back to legacy.
bool BMD::RenderMeshGpu(int meshIndex, const Render::Models::MeshGpu* gpu, float alpha, bool lit)
{
    using namespace Render::GL;
    BmdShader& sh = GetBmdShader();
    if (!sh.Ensure() || gpu == nullptr || !gpu->eligible || s_lastBoneMatrix == nullptr)
        return false;

    // Bone matrices: row-major 3x4 affine -> column-major mat4 (bottom row 0,0,0,1)
    // for the shader's `mat4 * vec4(pos, 1)`.
    int boneCount = NumBones;
    if (boneCount < 1) boneCount = 1;
    if (boneCount > BmdShader::kMaxBones) boneCount = BmdShader::kMaxBones;
    float bones[BmdShader::kMaxBones * 16];
    for (int b = 0; b < boneCount; ++b)
    {
        const float(*M)[4] = s_lastBoneMatrix[b];   // [3][4]
        float* d = &bones[b * 16];
        for (int c = 0; c < 4; ++c)
        {
            d[c * 4 + 0] = M[0][c];
            d[c * 4 + 1] = M[1][c];
            d[c * 4 + 2] = M[2][c];
            d[c * 4 + 3] = (c == 3) ? 1.f : 0.f;
        }
    }

    // Colour/lighting. Two modes (matches the legacy RenderMesh):
    //  - lit (props): per-normal lighting, base colour = BodyLight, alpha = alpha.
    //  - flat (characters, enableLight==false): a single flat colour = the current
    //    glColor the caller set (read once), no per-vertex lighting.
    vec3_t lightPos  = { 0.f, 0.f, 0.f };
    vec3_t baseColor;
    float  baseAlpha = alpha;
    if (lit)
    {
        VectorCopy(BodyLight, baseColor);
        // Light position: identical recompute to BMD::Transform's lit branch.
        vec3_t Position;
        float Matrix[3][4];
        if (HighLight)
        {
            Vector(1.3f, 0.f, 2.f, Position);
        }
        else if (gMapManager.InBattleCastle())
        {
            Vector(0.5f, -1.f, 1.f, Position);
            Vector(0.f, 0.f, -45.f, ShadowAngle);
        }
        else
        {
            Vector(0.f, -1.5f, 0.f, Position);
        }
        AngleMatrix(ShadowAngle, Matrix);
        VectorIRotate(Position, Matrix, lightPos);
    }
    else
    {
        float cur[4] = { 1.f, 1.f, 1.f, 1.f };
        // sub-task 6.7: GL read kept HERE only. RenderMeshGpu is the per-mesh GPU path that
        // issues GL immediately and is GL-thread-only (Phase B keeps per-mesh-GPU + legacy
        // draws serial; only the instanced-collect path at RenderMesh runs on workers). NOT
        // migrated; the instanced flat branch's glGetFloatv was the one moved to ctx.flatColor.
        glGetFloatv(GL_CURRENT_COLOR, cur);   // the flat colour the caller set
        baseColor[0] = cur[0]; baseColor[1] = cur[1]; baseColor[2] = cur[2];
        baseAlpha = cur[3];
    }

    const bool  translate = s_lastTransformTranslate;
    const float bodyScale = translate ? BodyScale : 1.f;
    vec3_t bodyOrigin = { 0.f, 0.f, 0.f };
    if (translate) { VectorCopy(BodyOrigin, bodyOrigin); }

    sh.Use();
    sh.SetBones(bones, boneCount);
    sh.SetBody(bodyScale, bodyOrigin);
    sh.SetLight(lightPos, baseColor, baseAlpha);
    sh.SetLit(lit ? 1.f : 0.f);
    sh.SetTextureUnit(0);
    ActiveTexture(GL_TEXTURE0);

    gpu->vbo.Bind(GL_ARRAY_BUFFER);
    namespace Lay = Render::Models::GpuVtxLayout;
    const GLint aPos = sh.AttrPos(), aVB = sh.AttrVBone(), aN = sh.AttrNormal(),
                aNB = sh.AttrNBone(), aUV = sh.AttrUV();
    if (aPos >= 0) { EnableVertexAttribArray(aPos); VertexAttribPointer(aPos, 3, GL_FLOAT, GL_FALSE, Lay::kStride, (const GLvoid*)(size_t)Lay::kOffPos); }
    if (aVB  >= 0) { EnableVertexAttribArray(aVB);  VertexAttribPointer(aVB,  1, GL_FLOAT, GL_FALSE, Lay::kStride, (const GLvoid*)(size_t)Lay::kOffVBone); }
    if (aN   >= 0) { EnableVertexAttribArray(aN);   VertexAttribPointer(aN,   3, GL_FLOAT, GL_FALSE, Lay::kStride, (const GLvoid*)(size_t)Lay::kOffNormal); }
    if (aNB  >= 0) { EnableVertexAttribArray(aNB);  VertexAttribPointer(aNB,  1, GL_FLOAT, GL_FALSE, Lay::kStride, (const GLvoid*)(size_t)Lay::kOffNBone); }
    if (aUV  >= 0) { EnableVertexAttribArray(aUV);  VertexAttribPointer(aUV,  2, GL_FLOAT, GL_FALSE, Lay::kStride, (const GLvoid*)(size_t)Lay::kOffUV); }

    glDrawArrays(GL_TRIANGLES, 0, gpu->vertexCount);

    if (aPos >= 0) DisableVertexAttribArray(aPos);
    if (aVB  >= 0) DisableVertexAttribArray(aVB);
    if (aN   >= 0) DisableVertexAttribArray(aN);
    if (aNB  >= 0) DisableVertexAttribArray(aNB);
    if (aUV  >= 0) DisableVertexAttribArray(aUV);
    BindBuffer(GL_ARRAY_BUFFER, 0);   // critical: legacy gl*Pointer would read offsets otherwise
    UseProgram(0);
    return true;
}

// P-bmd-instance: append the current character-part's bone palette to the TBO
// once (subsequent meshes of the same part reuse the base, detected via the
// Transform serial). Returns the palette base index for InstanceRec.
static int InstPaletteBaseForCurrentPart(int numBones)
{
    // Etapa 3b 6.6: these per-Transform palette-base cache statics moved to the per-worker
    // BmdRenderContext (instPaletteLastSerial / instPaletteLastBase). As shared function-local
    // statics, parallel workers would return each other's palette base; per-worker preserves
    // the "one palette per character-part, reused by its meshes" correlation per worker.
    // Initial values match the originals (lastSerial = 0xFFFFFFFF, lastBase = 0).
    auto& ctx = Render::Build::CurrentRenderCtx();
    if (s_transformSerial != ctx.instPaletteLastSerial)
    {
        int bc = numBones;
        if (bc < 1) bc = 1;
        if (bc > Render::GL::BmdShader::kMaxBones) bc = Render::GL::BmdShader::kMaxBones;
        ctx.instPaletteLastBase   = Render::Models::InstAppendPalette(s_lastBoneMatrix, bc);
        ctx.instPaletteLastSerial = s_transformSerial;
    }
    return ctx.instPaletteLastBase;
}

// P-bmd-instance: global light direction for LIT instanced meshes. HighLight is a
// constant 'true' in this client, so RenderMeshGpu's lit branch always uses Position
// (1.3,0,2). Normal chars have ShadowAngle = (0,0,AmbientShadowAngle) (a global), so
// the result is identical for every char -> one uLightPos per flush. We compute from
// AmbientShadowAngle directly (not this->ShadowAngle) so a rare pet with a custom
// shadow angle can't poison the shared uniform for the whole batch.
void BMD::ComputeInstLitLight(vec3_t out)
{
    vec3_t Position, angle;
    float  Matrix[3][4];
    Vector(1.3f, 0.f, 2.f, Position);
    Vector(0.f, 0.f, AmbientShadowAngle, angle);
    AngleMatrix(angle, Matrix);
    VectorIRotate(Position, Matrix, out);
}

// ===========================================================================
// Etapa 3b 3b-diag: MU_JOBSDIAG-gated, thread-safe RenderMesh collect tracer.
// All counters are std::atomic so concurrent workers don't corrupt them; the
// dump runs serially (LogAndResetGpuStats cadence). Localises WHERE a chars-pass
// RenderMesh call exits when the [bmd_cov] inst count jitters under MU_JOBS.
// Removed/gated before the fix commit.
// ===========================================================================
namespace
{
    bool JobsDiagEnabled()
    {
        static const bool s_on = [] {
            char b[8] = {}; size_t n = 0;
            return getenv_s(&n, b, sizeof(b), "MU_JOBSDIAG") == 0 && n > 0 && b[0] != '0';
        }();
        return s_on;
    }
    // One bucket per early-return / branch in the chars pass.
    enum DiagSlot {
        DIAG_ENTRY = 0,      // entered RenderMesh on the chars pass (post suppress)
        DIAG_RET_BOUNDS,     // meshIndex OOB
        DIAG_RET_NOTRI,      // NumTriangles == 0
        DIAG_RET_BITMAPHIDE, // textureIndex == BITMAP_HIDE
        DIAG_RET_SKINHAIR,   // IsSkin/IsHair && HideSkin
        DIAG_RET_NONEBLEND,  // chrome NoneBlendMesh
        DIAG_REACH_GATE,     // reached the GPU/instancing outer gate
        DIAG_GATE_GPUNULL,   // outer gate true but gpu==null/ineligible (worker-defer etc.)
        DIAG_INSTADD,        // reached InstAdd (cls=0)
        DIAG_CLASSIFY,       // reached the classify block (line 1777)
        DIAG_N
    };
    std::atomic<long long> g_diag[DIAG_N];
    inline void DiagHit(int slot) { if (JobsDiagEnabled()) g_diag[slot].fetch_add(1, std::memory_order_relaxed); }
    // Per-meshIndex entry vs instadd histogram (which body mesh slot loses InstAdds).
    std::atomic<long long> g_diagEntryByMesh[MAX_MESH];
    std::atomic<long long> g_diagInstByMesh[MAX_MESH];
    inline void DiagEntryMesh(int mi) { if (JobsDiagEnabled() && mi >= 0 && mi < MAX_MESH) g_diagEntryByMesh[mi].fetch_add(1, std::memory_order_relaxed); }
    inline void DiagInstMesh(int mi)  { if (JobsDiagEnabled() && mi >= 0 && mi < MAX_MESH) g_diagInstByMesh[mi].fetch_add(1, std::memory_order_relaxed); }
}
void JobsDiagDumpAndReset();   // fwd; called from LogAndResetGpuStats-adjacent site

void JobsDiagDumpAndReset()
{
    if (!JobsDiagEnabled()) return;
    long long v[DIAG_N];
    for (int i = 0; i < DIAG_N; ++i) v[i] = g_diag[i].exchange(0, std::memory_order_relaxed);
    Render::GL::Log("[jobsdiag] entry=%lld | ret: bounds=%lld notri=%lld bmphide=%lld skinhair=%lld noneblend=%lld "
                    "| gate=%lld gpunull=%lld instadd=%lld classify=%lld | dropped(entry-classify-instadd)=%lld",
                    v[DIAG_ENTRY], v[DIAG_RET_BOUNDS], v[DIAG_RET_NOTRI], v[DIAG_RET_BITMAPHIDE],
                    v[DIAG_RET_SKINHAIR], v[DIAG_RET_NONEBLEND], v[DIAG_REACH_GATE], v[DIAG_GATE_GPUNULL],
                    v[DIAG_INSTADD], v[DIAG_CLASSIFY],
                    v[DIAG_ENTRY] - v[DIAG_CLASSIFY] - (v[DIAG_RET_BOUNDS]+v[DIAG_RET_NOTRI]+v[DIAG_RET_BITMAPHIDE]+v[DIAG_RET_SKINHAIR]+v[DIAG_RET_NONEBLEND]));
    char buf[1024]; int off = 0;
    off += snprintf(buf+off, sizeof(buf)-off, "[jobsdiag] perMesh entry/inst (mi:e,i):");
    for (int mi = 0; mi < MAX_MESH; ++mi)
    {
        long long e = g_diagEntryByMesh[mi].exchange(0, std::memory_order_relaxed);
        long long ia = g_diagInstByMesh[mi].exchange(0, std::memory_order_relaxed);
        if (e != 0 || ia != 0)
            off += snprintf(buf+off, sizeof(buf)-off, " %d:%lld,%lld", mi, e, ia);
        if (off > (int)sizeof(buf) - 32) break;
    }
    Render::GL::Log("%s", buf);
}

void BMD::RenderMesh(int meshIndex, int renderFlags, float alpha, int blendMeshIndex, float blendMeshAlpha, float blendMeshTextureCoordU, float blendMeshTextureCoordV, int explicitTextureIndex)
{
    // Etapa 3b 6.8b: the EffectsOnly serial replay re-walks RenderCharacter ONLY to
    // re-issue the suppressed effect side effects in entity order. The mesh instances /
    // [bmd_cov] counters were already emitted by the parallel MeshOnly pass, so skip the
    // mesh emission here to avoid double-instancing/double-counting. (The replay only ever
    // renders characters, never props.)
    if (Render::Build::BuildSuppressMesh())
        return;

    const bool diagCharsPass = JobsDiagEnabled() && Render::Models::GpuCharsPass();
    if (diagCharsPass) { DiagHit(DIAG_ENTRY); DiagEntryMesh(meshIndex); }

    if (meshIndex >= NumMeshs || meshIndex < 0) { if (diagCharsPass) DiagHit(DIAG_RET_BOUNDS); return; }

    Mesh_t* m = &Meshs[meshIndex];
    if (m->NumTriangles == 0) { if (diagCharsPass) DiagHit(DIAG_RET_NOTRI); return; }

    float wave = static_cast<long>(WorldTime) % 10000 * 0.0001f;

    int textureIndex = IndexTexture[m->Texture];
    if (textureIndex == BITMAP_HIDE)
    {
        if (diagCharsPass) DiagHit(DIAG_RET_BITMAPHIDE);
        return;
    }

    if (textureIndex == BITMAP_WATER)
    {
        textureIndex = BITMAP_WATER + WaterTextureNumber;
    }

    if (explicitTextureIndex != -1)
    {
        textureIndex = explicitTextureIndex;
    }

    const auto texture = Bitmaps.GetTexture(textureIndex);
    if (texture->IsSkin && HideSkin)
    {
        if (diagCharsPass) DiagHit(DIAG_RET_SKINHAIR);
        return;
    }

    if (texture->IsHair && HideSkin)
    {
        if (diagCharsPass) DiagHit(DIAG_RET_SKINHAIR);
        return;
    }

    bool EnableWave = false;
    int streamMesh = static_cast<u_char>(StreamMesh);
    if (m->m_csTScript != nullptr)
    {
        if (m->m_csTScript->getStreamMesh())
        {
            streamMesh = meshIndex;
        }
    }

    if ((meshIndex == blendMeshIndex || meshIndex == streamMesh)
        && (blendMeshTextureCoordU != 0.f || blendMeshTextureCoordV != 0.f))
    {
        EnableWave = true;
    }

    bool enableLight = LightEnable;
    if (meshIndex == StreamMesh)
    {
        glColor3fv(BodyLight);
        // sub-task 6.7: mirror the flat colour into ctx.flatColor so the instanced flat
        // branch reads it (per-worker) instead of glGetFloatv(GL_CURRENT_COLOR). StreamMesh
        // forces flat here (enableLight=false), so this is one of the flat-branch sources.
        Render::Build::CurrentRenderCtx().flatColor[0] = BodyLight[0];
        Render::Build::CurrentRenderCtx().flatColor[1] = BodyLight[1];
        Render::Build::CurrentRenderCtx().flatColor[2] = BodyLight[2];
        Render::Build::CurrentRenderCtx().flatColor[3] = 1.f;
        enableLight = false;
    }
    // The per-normal LightTransform[] = BodyLight * IntensityTransform[] build is
    // relocated past the instancing early-return + EnsureMeshSkinned (see below):
    // the instanced lit shader does lighting from palette normals, so the loop is
    // pure waste for collected meshes, and on the legacy path it must read freshly
    // skinned IntensityTransform under deferred skinning.

    // True ALPHA-BLEND (translucent) meshes -- e.g. the "blend mesh" branch below that
    // calls EnableAlphaBlend (wing membranes like Wings of Spirits). These must NOT be
    // collected into the instanced batch: that flush is alpha-test/opaque only, so an
    // instanced blend mesh renders flat/opaque (lost translucency). Flag it here and skip
    // the whole GPU/instancing block so it falls through to the legacy alpha-blended draw.
    bool meshAlphaBlended = false;

    int finalRenderFlags = renderFlags;
    if ((renderFlags & RENDER_COLOR) == RENDER_COLOR)
    {
        finalRenderFlags = RENDER_COLOR;
        if ((renderFlags & RENDER_BRIGHT) == RENDER_BRIGHT)
        {
            EnableAlphaBlend();
        }
        else if ((renderFlags & RENDER_DARK) == RENDER_DARK)
        {
            EnableAlphaBlendMinus();
        }
        else
        {
            DisableAlphaBlend();
        }

        if ((renderFlags & RENDER_NODEPTH) == RENDER_NODEPTH)
        {
            DisableDepthTest();
        }

        DisableTexture();
        if (alpha >= 0.99f)
        {
            glColor3fv(BodyLight);
        }
        else
        {
            EnableAlphaTest();
            glColor4f(BodyLight[0], BodyLight[1], BodyLight[2], alpha);
        }
    }
    else if ((renderFlags & RENDER_CHROME) == RENDER_CHROME ||
        (renderFlags & RENDER_CHROME2) == RENDER_CHROME2 ||
        (renderFlags & RENDER_CHROME3) == RENDER_CHROME3 ||
        (renderFlags & RENDER_CHROME4) == RENDER_CHROME4 ||
        (renderFlags & RENDER_CHROME5) == RENDER_CHROME5 ||
        (renderFlags & RENDER_CHROME6) == RENDER_CHROME6 ||
        (renderFlags & RENDER_CHROME7) == RENDER_CHROME7 ||
        (renderFlags & RENDER_METAL) == RENDER_METAL ||
        (renderFlags & RENDER_OIL) == RENDER_OIL
        )
    {
        if (m->m_csTScript != nullptr)
        {
            if (m->m_csTScript->getNoneBlendMesh()) { if (diagCharsPass) DiagHit(DIAG_RET_NONEBLEND); return; }
        }

        if (m->NoneBlendMesh)
            { if (diagCharsPass) DiagHit(DIAG_RET_NONEBLEND); return; }

        finalRenderFlags = RENDER_CHROME;
        if ((renderFlags & RENDER_CHROME4) == RENDER_CHROME4)
        {
            finalRenderFlags = RENDER_CHROME4;
        }
        if ((renderFlags & RENDER_OIL) == RENDER_OIL)
        {
            finalRenderFlags = RENDER_OIL;
        }

        // g_chrome[] sphere-map texcoords are computed AFTER the GPU/instancing
        // decision + skinning (see below, gated on finalRenderFlags). When the mesh
        // instances, the shader builds the UV and this per-normal loop is skipped
        // entirely; on the legacy path it runs against freshly-skinned normals.

        if ((renderFlags & RENDER_CHROME3) == RENDER_CHROME3
            || (renderFlags & RENDER_CHROME4) == RENDER_CHROME4
            || (renderFlags & RENDER_CHROME5) == RENDER_CHROME5
            || (renderFlags & RENDER_CHROME7) == RENDER_CHROME7
            || (renderFlags & RENDER_BRIGHT) == RENDER_BRIGHT
            )
        {
            if (alpha < 0.99f)
            {
                BodyLight[0] *= alpha;
                BodyLight[1] *= alpha;
                BodyLight[2] *= alpha;
            }

            EnableAlphaBlend();
        }
        else if ((renderFlags & RENDER_DARK) == RENDER_DARK)
            EnableAlphaBlendMinus();
        else if ((renderFlags & RENDER_LIGHTMAP) == RENDER_LIGHTMAP)
            EnableLightMap();
        else if (alpha >= 0.99f)
        {
            DisableAlphaBlend();
        }
        else
        {
            EnableAlphaTest();
        }

        if ((renderFlags & RENDER_NODEPTH) == RENDER_NODEPTH)
        {
            DisableDepthTest();
        }

        if (explicitTextureIndex == -1)
        {
            if ((renderFlags & RENDER_CHROME2) == RENDER_CHROME2)
            {
                BindTexture(BITMAP_CHROME2);
            }
            else if ((renderFlags & RENDER_CHROME3) == RENDER_CHROME3)
            {
                BindTexture(BITMAP_CHROME2);
            }
            else if ((renderFlags & RENDER_CHROME4) == RENDER_CHROME4)
            {
                BindTexture(BITMAP_CHROME2);
            }
            else if ((renderFlags & RENDER_CHROME6) == RENDER_CHROME6)
            {
                BindTexture(BITMAP_CHROME6);
            }
            else if ((renderFlags & RENDER_CHROME) == RENDER_CHROME)
            {
                BindTexture(BITMAP_CHROME);
            }
            else if ((renderFlags & RENDER_METAL) == RENDER_METAL)
            {
                BindTexture(BITMAP_SHINY);
            }
        }
        else
        {
            BindTexture(textureIndex);
        }
    }
    else if (blendMeshIndex <= -2 || m->Texture == blendMeshIndex)
    {
        finalRenderFlags = RENDER_TEXTURE;
        meshAlphaBlended = true;   // translucent blend mesh -> keep off the instanced (opaque) batch
        BindTexture(textureIndex);
        if ((renderFlags & RENDER_DARK) == RENDER_DARK)
            EnableAlphaBlendMinus();
        else
            EnableAlphaBlend();

        if ((renderFlags & RENDER_NODEPTH) == RENDER_NODEPTH)
        {
            DisableDepthTest();
        }

        glColor3f(BodyLight[0] * blendMeshAlpha, 
            BodyLight[1] * blendMeshAlpha,
            BodyLight[2] * blendMeshAlpha);
        //glColor3f(BlendMeshLight,BlendMeshLight,BlendMeshLight);
        enableLight = false;
    }
    else if ((renderFlags & RENDER_TEXTURE) == RENDER_TEXTURE)
    {
        finalRenderFlags = RENDER_TEXTURE;
        BindTexture(textureIndex);
        if ((renderFlags & RENDER_BRIGHT) == RENDER_BRIGHT)
        {
            EnableAlphaBlend();
        }
        else if ((renderFlags & RENDER_DARK) == RENDER_DARK)
        {
            EnableAlphaBlendMinus();
        }
        else if (alpha < 0.99f || texture->Components == 4)
        {
            EnableAlphaTest();
        }
        else
        {
            DisableAlphaBlend();
        }

        if ((renderFlags & RENDER_NODEPTH) == RENDER_NODEPTH)
        {
            DisableDepthTest();
        }
    }
    else if ((renderFlags & RENDER_BRIGHT) == RENDER_BRIGHT)
    {
        if (texture->Components == 4 || m->Texture == blendMeshIndex)
        {
            return;
        }

        finalRenderFlags = RENDER_BRIGHT;
        EnableAlphaBlend();
        DisableTexture();
        DisableDepthMask();

        if ((renderFlags & RENDER_NODEPTH) == RENDER_NODEPTH)
        {
            DisableDepthTest();
        }
    }
    else
    {
        finalRenderFlags = RENDER_TEXTURE;
    }
    if (renderFlags & RENDER_DOPPELGANGER)
    {
        if (texture->Components != 4)
        {
            EnableCullFace();
            EnableDepthMask();
        }
    }

    bool enableColor = (enableLight && finalRenderFlags == RENDER_TEXTURE)
        || finalRenderFlags == RENDER_CHROME
        || finalRenderFlags == RENDER_CHROME4
        || finalRenderFlags == RENDER_OIL;

    // P-bmd-gpu: GPU skinning path for lit, plain-textured meshes in the Objects
    // (props) or Characters (players/mobs + parts) pass, $gpubmd on. Reuses the
    // texture/blend state set above; replaces the CPU per-vertex rebuild + client-
    // side draw below. Falls through to legacy otherwise.
    // P-bmd-instance (chrome): PLAIN RENDER_CHROME only — not CHROME2..7/METAL/OIL (those
    // share finalRenderFlags==RENDER_CHROME but use different formulas/textures), not
    // DARK/LIGHTMAP/NODEPTH, standard chrome texture. Two sub-cases: OPAQUE (no BRIGHT,
    // alpha>=0.99 -> alpha-test bucket) and ADDITIVE (RENDER_BRIGHT -> GL_ONE/ONE bucket,
    // order-independent). Char equipment chrome is overwhelmingly CHROME|BRIGHT (additive).
    // The instanced shader reproduces the plain-chrome sphere-map UV (normal.z*0.5+wave,
    // normal.y*0.5+wave*2).
    const bool chromeBase =
        finalRenderFlags == RENDER_CHROME
        && (renderFlags & RENDER_CHROME) != 0
        && (renderFlags & (RENDER_CHROME2 | RENDER_CHROME3 | RENDER_CHROME4 | RENDER_CHROME5
                           | RENDER_CHROME6 | RENDER_CHROME7 | RENDER_METAL | RENDER_OIL
                           | RENDER_DARK | RENDER_LIGHTMAP | RENDER_NODEPTH)) == 0
        && explicitTextureIndex == -1;
    const bool chromeAdditive = chromeBase && (renderFlags & RENDER_BRIGHT) != 0;
    const bool chromeOpaque   = chromeBase && (renderFlags & RENDER_BRIGHT) == 0 && alpha >= 0.99f;
    const bool plainChrome    = chromeOpaque || chromeAdditive;

    // P-bmd-instance (chrome variants): CHROME2/3/4/6 + METAL also instance, each with its
    // own sphere-map formula (shader uChromeMode) + texture. Excludes DARK/LIGHTMAP/NODEPTH
    // (unsupported blend) and explicit-texture/NoneBlend. CHROME5/7/OIL stay legacy (no
    // texture bind in the legacy path / rare). Additive when the legacy path would
    // EnableAlphaBlend (CHROME3/4 or BRIGHT); else opaque.
    int  chromeVarMode = 0;     // 0 = not an instanceable variant; else shader uChromeMode
    int  chromeVarTex  = 0;
    if (explicitTextureIndex == -1
        && (renderFlags & (RENDER_DARK | RENDER_LIGHTMAP | RENDER_NODEPTH)) == 0
        && !m->NoneBlendMesh
        && !(m->m_csTScript != nullptr && m->m_csTScript->getNoneBlendMesh()))
    {
        if      ((renderFlags & RENDER_CHROME2) != 0) { chromeVarMode = 2; chromeVarTex = BITMAP_CHROME2; }
        else if ((renderFlags & RENDER_CHROME3) != 0) { chromeVarMode = 3; chromeVarTex = BITMAP_CHROME2; }
        else if ((renderFlags & RENDER_CHROME4) != 0) { chromeVarMode = 4; chromeVarTex = BITMAP_CHROME2; }
        else if ((renderFlags & RENDER_CHROME6) != 0) { chromeVarMode = 6; chromeVarTex = BITMAP_CHROME6; }
        else if ((renderFlags & RENDER_METAL)   != 0) { chromeVarMode = 8; chromeVarTex = BITMAP_SHINY; }
    }
    const bool chromeVarOk       = chromeVarMode != 0;
    const bool chromeVarAdditive = chromeVarOk
        && (renderFlags & (RENDER_CHROME3 | RENDER_CHROME4 | RENDER_BRIGHT)) != 0;

    // Etapa 1.4a: an ADDITIVE translucent blend mesh (wings — set meshAlphaBlended above,
    // textured, EnableAlphaBlend = GL_ONE/ONE; NOT DARK/AlphaBlendMinus) can join the
    // instanced additive bucket (order-independent) instead of the per-mesh GPU path.
    const bool blendMeshAdditive = meshAlphaBlended
        && (renderFlags & RENDER_DARK) == 0
        && Render::Models::GpuBlendInstEnabled();

    // Etapa 1.4b: a textured BRIGHT mesh that animates its texcoords (EnableWave UV-scroll;
    // NOT RENDER_WAVE vertex displacement, NOT DARK, not a blend mesh) can also join the
    // additive bucket — the instanced shader applies the per-bucket UV offset below.
    const bool waveAdditive = EnableWave
        && finalRenderFlags == RENDER_TEXTURE
        && (renderFlags & RENDER_BRIGHT) != 0
        && (renderFlags & RENDER_DARK) == 0
        && !meshAlphaBlended
        && Render::Models::GpuWaveInstEnabled();

    bool wentGpu = false;
    bool instanced = false;
    if ((Render::Models::GpuObjectsPass() || Render::Models::GpuCharsPass()) && Render::Models::GpuBmdEnabled()
        && Render::GL::IsLoaded()
        && (finalRenderFlags == RENDER_TEXTURE || plainChrome || chromeVarOk) && (!EnableWave || waveAdditive)
        && (!meshAlphaBlended || Render::Models::GpuBlendMeshEnabled() || blendMeshAdditive)   // blend mesh: legacy unless MU_GPUBLENDMESH (per-mesh GPU) or MU_GPUBLENDINST (instanced additive)
        && (renderFlags & (RENDER_SHADOWMAP | RENDER_WAVE)) == 0
        && BoneScale == 1.f && s_lastTransformScale == 0.f && s_lastBoneMatrix != nullptr)
    {
        if (diagCharsPass) DiagHit(DIAG_REACH_GATE);
        // enableLight true -> per-normal lit (props); false -> flat glColor (chars).
        const auto* gpu = Render::Models::GetOrBuildMeshGpu(this, meshIndex, Render::GL::BmdShader::kMaxBones);
        if (diagCharsPass && (gpu == nullptr || !gpu->eligible)) DiagHit(DIAG_GATE_GPUNULL);
        if (gpu != nullptr && gpu->eligible)
        {
            // P-bmd-instance: in the Characters pass, COLLECT world-baked
            // (Translate==true) plain-textured meshes into the instanced batch instead
            // of drawing now; InstFlush() at end of pass collapses identical (model,
            // mesh, texture) into one glDrawArraysInstanced. Both flat (enableLight
            // false -> flat glColor) and lit (per-normal, light is map-global) qualify.
            // Excluded: alpha-blended meshes (the flush is alpha-test only); those keep
            // the per-mesh GPU path. (HighLight is a constant 'true' here, not a per-
            // object highlight, so it is not an exclusion.)
            // Additive chrome (CHROME|BRIGHT) is allowed despite being "blended" because the
            // instanced flush has a dedicated additive (GL_ONE/ONE) pass. Other blended
            // (textured BRIGHT/DARK) meshes still keep the legacy/per-mesh path.
            const bool blended = (renderFlags & (RENDER_BRIGHT | RENDER_DARK)) != 0;
            if (Render::Models::GpuCharsPass() && Render::Models::GpuInstEnabled()
                && s_lastTransformTranslate
                && (!meshAlphaBlended || blendMeshAdditive)   // opaque batch excludes blend meshes; additive blend meshes (wings) go to the additive bucket below
                && (!blended || chromeAdditive || chromeVarAdditive || blendMeshAdditive || waveAdditive))
            {
                Render::Models::InstanceRec rec;
                rec.paletteBase   = (float)InstPaletteBaseForCurrentPart(NumBones);
                rec.bodyScale     = BodyScale;
                rec.bodyOrigin[0] = BodyOrigin[0];
                rec.bodyOrigin[1] = BodyOrigin[1];
                rec.bodyOrigin[2] = BodyOrigin[2];
                int instMode  = 0;           // 0 textured / 1 chrome sphere-map
                int instBlend = 0;           // 0 opaque (alpha-test) / 1 additive (GL_ONE/ONE)
                int instTex   = textureIndex;
                if (plainChrome)
                {
                    // Chrome: flat BodyLight colour (legacy chrome ignores per-normal
                    // lighting; BRIGHT already scaled BodyLight by alpha above); the shader
                    // builds the sphere-map UV from the skinned normal. Binds the shared
                    // chrome texture; wave drives the scroll. Additive when RENDER_BRIGHT.
                    rec.color[0] = BodyLight[0]; rec.color[1] = BodyLight[1]; rec.color[2] = BodyLight[2];
                    rec.color[3] = alpha;
                    rec.lit = 0.f;
                    instMode  = 1;
                    instBlend = chromeAdditive ? 1 : 0;
                    instTex   = BITMAP_CHROME;
                    rec.instWave = wave;   // per-bucket chrome reflection scroll (6.7)
                }
                else if (chromeVarOk)
                {
                    // Chrome variant (CHROME2/3/4/6/METAL): flat BodyLight colour; the
                    // shader builds the per-variant sphere-map UV (uChromeMode) from the
                    // skinned normal + frame globals. BodyLight was already scaled by alpha
                    // above for the additive cases (CHROME3/4/BRIGHT). Mirrors the legacy
                    // Wave2/L/LightVector inputs exactly so the result is A/B identical.
                    rec.color[0] = BodyLight[0]; rec.color[1] = BodyLight[1]; rec.color[2] = BodyLight[2];
                    rec.color[3] = alpha;
                    rec.lit = 0.f;
                    instMode  = chromeVarMode;
                    instBlend = chromeVarAdditive ? 1 : 0;
                    instTex   = chromeVarTex;
                    rec.instWave = wave;   // per-bucket chrome reflection scroll (6.7)
                    const float Wave2 = (int)WorldTime % 5000 * 0.00024f - 0.4f;
                    const float L[3] = { (float)(cos(WorldTime * 0.001f)),
                                         (float)(sin(WorldTime * 0.002f)), 1.f };
                    // CHROME2/3/4/6 extra inputs, now carried per-bucket (6.7).
                    rec.chromeWave2 = Wave2;
                    rec.chromeL[0] = L[0]; rec.chromeL[1] = L[1]; rec.chromeL[2] = L[2];
                    rec.chromeLightVec[0] = LightVector[0]; rec.chromeLightVec[1] = LightVector[1]; rec.chromeLightVec[2] = LightVector[2];
                }
                else if (waveAdditive)
                {
                    // Textured BRIGHT + UV-scroll (wave): additive, per-normal lit like the
                    // legacy path (LightTransform = BodyLight * intensity). BodyLight was
                    // already scaled by alpha above for BRIGHT. The shader adds the per-bucket
                    // UV offset (BlendMeshTexCoordU/V), reproducing legacy "texCoords += scroll".
                    rec.color[0] = BodyLight[0]; rec.color[1] = BodyLight[1]; rec.color[2] = BodyLight[2];
                    rec.color[3] = alpha;
                    rec.lit = enableLight ? 1.f : 0.f;
                    rec.uvScroll[0] = blendMeshTextureCoordU;
                    rec.uvScroll[1] = blendMeshTextureCoordV;
                    instBlend = 1;
                    if (enableLight)
                    {
                        vec3_t lp; ComputeInstLitLight(lp);
                        rec.instLight[0] = lp[0]; rec.instLight[1] = lp[1]; rec.instLight[2] = lp[2];   // per-bucket lit dir (6.7)
                    }
                }
                else if (enableLight)
                {
                    // Lit: base colour = BodyLight, per-normal lum in the shader.
                    rec.color[0] = BodyLight[0]; rec.color[1] = BodyLight[1]; rec.color[2] = BodyLight[2];
                    rec.color[3] = alpha;
                    rec.lit = 1.f;
                    vec3_t lp; ComputeInstLitLight(lp);
                    rec.instLight[0] = lp[0]; rec.instLight[1] = lp[1]; rec.instLight[2] = lp[2];   // per-bucket lit dir (6.7)
                }
                else
                {
                    // Flat: single colour the caller set, no per-vertex lighting. sub-task 6.7:
                    // read the precomputed ctx.flatColor (mirrored at the glColor*(BodyLight)
                    // set sites in RenderMesh/RenderBody/RenderBodyTranslate) instead of
                    // glGetFloatv(GL_CURRENT_COLOR) — the latter is a GL read, illegal on the
                    // parallel build (worker) thread that has no GL context.
                    const float* cur = Render::Build::CurrentRenderCtx().flatColor;
                    rec.color[0] = cur[0]; rec.color[1] = cur[1]; rec.color[2] = cur[2]; rec.color[3] = cur[3];
                    rec.lit = 0.f;
                }
                // Additive blend mesh (wings): the flat 'else' above already captured
                // glColor (BodyLight * blendMeshAlpha) as rec.color; route to the additive
                // bucket so InstFlush pass 2 draws it GL_ONE/ONE, matching the legacy path.
                if (blendMeshAdditive)
                    instBlend = 1;
                if (diagCharsPass) { DiagHit(DIAG_INSTADD); DiagInstMesh(meshIndex); }
                Render::Models::InstAdd(this, meshIndex, instTex, rec, instMode, instBlend);
                wentGpu = true;
                instanced = true;
            }
            else if (!plainChrome && !chromeVarOk && RenderMeshGpu(meshIndex, gpu, alpha, enableLight))
            {
                // Per-mesh GPU path is textured-only; chrome (plain or variant) that did
                // not instance (instancing off / objects pass) falls through to the legacy
                // path so its sphere-map UV/blend are computed correctly.
                wentGpu = true;
            }
        }
    }
    if (Render::Models::GpuCharsPass())
    {
        if (diagCharsPass) DiagHit(DIAG_CLASSIFY);
        int cls;
        if (wentGpu)
            cls = instanced ? 0 : 1;                                   // instanced / per-mesh GPU
        else if (finalRenderFlags != RENDER_TEXTURE)
            cls = 2;                                                   // chrome/color/etc
        else if ((renderFlags & (RENDER_BRIGHT | RENDER_DARK)) != 0)
            cls = 3;                                                   // alpha-blended
        else if (EnableWave || (renderFlags & (RENDER_SHADOWMAP | RENDER_WAVE)) != 0)
            cls = 4;                                                   // wave/shadowmap
        else if (BoneScale != 1.f || s_lastTransformScale != 0.f)
            cls = 5;                                                   // bone/body scale
        else
            cls = 6;                                                   // eligible gate ok -> ineligible geometry/bones
        Render::Models::NoteCharMeshClass(cls);
        Render::Models::NoteCharMeshDraw(wentGpu);
    }
    if (wentGpu)
        return;

    // Etapa 3b 6.9: GL-on-worker safety. Everything below is the LEGACY immediate-draw
    // path (glDrawArrays / glColorPointer / chrome state) and issues GL right now. Under
    // MU_JOBS the per-entity build runs on worker threads with NO GL context, so reaching
    // this path off the main thread (CurrentWorkerIndex() != 0) is a misconfiguration:
    // the full-GPU-instanced config the parallel build targets keeps legacy=0 / permeshGPU=0
    // (verified by [bmd_cov]). Assert in debug + log once; on a worker we MUST NOT issue GL.
    if (Core::Jobs::JobsEnabled() && Core::Jobs::ThreadPool::CurrentWorkerIndex() != 0)
    {
        static bool s_warnedGlOnWorker = false;
        if (!s_warnedGlOnWorker)
        {
            s_warnedGlOnWorker = true;
            Render::GL::Log("[jobs] WARN: legacy GL draw path reached on worker %d (mesh %d) — "
                            "non-instanceable mesh under MU_JOBS; skipping GL (config must keep legacy=0)",
                            Core::Jobs::ThreadPool::CurrentWorkerIndex(), meshIndex);
        }
        assert(!"BMD::RenderMesh legacy GL path on a job worker thread (no GL context)");
        return;   // never issue GL off the GL thread
    }

    // P-bmd-skinskip: legacy CPU draw reads VertexTransform/LightTransform — force-skin
    // this mesh now if Transform deferred it (no-op if already skinned this frame).
    EnsureMeshSkinned(meshIndex);

    // Per-normal lighting for the legacy draw, relocated from the top of RenderMesh:
    // only reached when the mesh did NOT instance, and after EnsureMeshSkinned so
    // IntensityTransform is current under deferred skinning.
    if (enableLight)
    {
        for (int j = 0; j < m->NumNormals; j++)
        {
            VectorScale(BodyLight, IntensityTransform[meshIndex][j], LightTransform[meshIndex][j]);
        }
    }

    // Chrome sphere-map texcoords for the legacy draw, relocated here from the chrome
    // state block: only reached when the mesh did NOT instance (wentGpu returned above),
    // and after EnsureMeshSkinned so NormalTransform is current under deferred skinning.
    if (finalRenderFlags == RENDER_CHROME || finalRenderFlags == RENDER_CHROME4
        || finalRenderFlags == RENDER_OIL)
    {
        const float Wave2 = (int)WorldTime % 5000 * 0.00024f - 0.4f;
        vec3_t L = { (float)(cos(WorldTime * 0.001f)), (float)(sin(WorldTime * 0.002f)), 1.f };
        for (int j = 0; j < m->NumNormals; j++)
        {
            if (j > MAX_VERTICES) break;
            const auto normal = NormalTransform[meshIndex][j];

            if ((renderFlags & RENDER_CHROME2) == RENDER_CHROME2)
            {
                g_chrome[j][0] = (normal[2] + normal[0]) * 0.8f + Wave2 * 2.f;
                g_chrome[j][1] = (normal[1] + normal[0]) * 1.0f + Wave2 * 3.f;
            }
            else if ((renderFlags & RENDER_CHROME3) == RENDER_CHROME3)
            {
                g_chrome[j][0] = DotProduct(normal, LightVector);
                g_chrome[j][1] = 1.f - DotProduct(normal, LightVector);
            }
            else if ((renderFlags & RENDER_CHROME4) == RENDER_CHROME4)
            {
                g_chrome[j][0] = DotProduct(normal, L);
                g_chrome[j][1] = 1.f - DotProduct(normal, L);
                g_chrome[j][1] -= normal[2] * 0.5f + wave * 3.f;
                g_chrome[j][0] += normal[1] * 0.5f + L[1] * 3.f;
            }
            else if ((renderFlags & RENDER_CHROME5) == RENDER_CHROME5)
            {
                g_chrome[j][0] = DotProduct(normal, L);
                g_chrome[j][1] = 1.f - DotProduct(normal, L);
                g_chrome[j][1] -= normal[2] * 2.5f + wave * 1.f;
                g_chrome[j][0] += normal[1] * 3.f + L[1] * 5.f;
            }
            else if ((renderFlags & RENDER_CHROME6) == RENDER_CHROME6)
            {
                g_chrome[j][0] = (normal[2] + normal[0]) * 0.8f + Wave2 * 2.f;
                g_chrome[j][1] = (normal[2] + normal[0]) * 0.8f + Wave2 * 2.f;
            }
            else if ((renderFlags & RENDER_CHROME7) == RENDER_CHROME7)
            {
                g_chrome[j][0] = (normal[2] + normal[0]) * 0.8f + static_cast<float>(WorldTime) * 0.00006f;
                g_chrome[j][1] = (normal[2] + normal[0]) * 0.8f + static_cast<float>(WorldTime) * 0.00006f;
            }
            else if ((renderFlags & RENDER_OIL) == RENDER_OIL)
            {
                g_chrome[j][0] = normal[0];
                g_chrome[j][1] = normal[1];
            }
            else if ((renderFlags & RENDER_CHROME) == RENDER_CHROME)
            {
                g_chrome[j][0] = normal[2] * 0.5f + wave;
                g_chrome[j][1] = normal[1] * 0.5f + wave * 2.f;
            }
            else
            {
                g_chrome[j][0] = normal[2] * 0.5f + 0.2f;
                g_chrome[j][1] = normal[1] * 0.5f + 0.5f;
            }
        }
    }

    glEnableClientState(GL_VERTEX_ARRAY);
    if (enableColor) glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);

    auto vertices = RenderArrayVertices;
    auto colors = RenderArrayColors;
    auto texCoords = RenderArrayTexCoords;

    int target_vertex_index = -1;
    for (int j = 0; j < m->NumTriangles; j++)
    {
        const auto triangle = &m->Triangles[j];
        for (int k = 0; k < triangle->Polygon; k++)
        {
            const int source_vertex_index = triangle->VertexIndex[k];
            target_vertex_index++;

            VectorCopy(VertexTransform[meshIndex][source_vertex_index], vertices[target_vertex_index]);

            Vector4(BodyLight[0], BodyLight[1], BodyLight[2], alpha, colors[target_vertex_index]);

            auto texco = m->TexCoords[triangle->TexCoordIndex[k]];
            texCoords[target_vertex_index][0] = texco.TexCoordU;
            texCoords[target_vertex_index][1] = texco.TexCoordV;

            int normalIndex = triangle->NormalIndex[k];
            switch (finalRenderFlags)
            {
                case RENDER_TEXTURE:
                {
                    if (EnableWave)
                    {
                        texCoords[target_vertex_index][0] += blendMeshTextureCoordU;
                        texCoords[target_vertex_index][1] += blendMeshTextureCoordV;
                    }

                    if (enableLight)
                    {
                        auto light = LightTransform[meshIndex][normalIndex];
                        Vector4(light[0], light[1], light[2], alpha, colors[target_vertex_index]);
                    }

                    break;
                }
                case RENDER_CHROME:
                {
                    texCoords[target_vertex_index][0] = g_chrome[normalIndex][0];
                    texCoords[target_vertex_index][1] = g_chrome[normalIndex][1];
                    break;
                }
                case RENDER_CHROME4:
                {
                    texCoords[target_vertex_index][0] = g_chrome[normalIndex][0] + blendMeshTextureCoordU;
                    texCoords[target_vertex_index][1] = g_chrome[normalIndex][1] + blendMeshTextureCoordV;
                    break;
                }
                case RENDER_OIL:
                {
                    texCoords[target_vertex_index][0] = g_chrome[normalIndex][0] * texCoords[target_vertex_index][0] + blendMeshTextureCoordU;
                    texCoords[target_vertex_index][1] = g_chrome[normalIndex][1] * texCoords[target_vertex_index][1] + blendMeshTextureCoordV;
                    break;
                }
            }

            if ((renderFlags & RENDER_SHADOWMAP) == RENDER_SHADOWMAP)
            {
                vec3_t pos;
                VectorSubtract(vertices[target_vertex_index], BodyOrigin, pos);

                pos[0] += pos[2] * (pos[0] + 2000.f) / (pos[2] - 4000.f);
                pos[2] = 5.f;

                VectorAdd(pos, BodyOrigin, pos);
            }
            else if ((renderFlags & RENDER_WAVE) == RENDER_WAVE)
            {
                float time_sin = sinf((float)((int)WorldTime + source_vertex_index * 931) * 0.007f) * 28.0f;
                float* normal = NormalTransform[meshIndex][normalIndex];
                for (int iCoord = 0; iCoord < 3; ++iCoord)
                {
                    vertices[target_vertex_index][iCoord] += normal[iCoord] * time_sin;
                }
            }
        }
    }

    glVertexPointer(3, GL_FLOAT, 0, vertices);
    if (enableColor) glColorPointer(4, GL_FLOAT, 0, colors);
    glTexCoordPointer(2, GL_FLOAT, 0, texCoords);

    glDrawArrays(GL_TRIANGLES, 0, m->NumTriangles * 3);

    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    if (enableColor) glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
}

void BMD::RenderMeshAlternative(int iRndExtFlag, int iParam, int i, int RenderFlag, float Alpha, int BlendMesh, float BlendMeshLight, float BlendMeshTexCoordU, float BlendMeshTexCoordV, int MeshTexture)
{
    if (i >= NumMeshs || i < 0) return;

    Mesh_t* m = &Meshs[i];
    if (m->NumTriangles == 0) return;
    EnsureMeshSkinned(i);   // P-bmd-skinskip: CPU draw reads VertexTransform
    float Wave = (int)WorldTime % 10000 * 0.0001f;

    int Texture = IndexTexture[m->Texture];
    if (Texture == BITMAP_HIDE)
        return;
    if (MeshTexture != -1)
        Texture = MeshTexture;

    BITMAP_t* pBitmap = Bitmaps.GetTexture(Texture);

    bool EnableWave = false;
    int streamMesh = StreamMesh;
    if (m->m_csTScript != nullptr)
    {
        if (m->m_csTScript->getStreamMesh())
        {
            streamMesh = i;
        }
    }
    if ((i == BlendMesh || i == streamMesh) && (BlendMeshTexCoordU != 0.f || BlendMeshTexCoordV != 0.f))
        EnableWave = true;

    bool EnableLight = LightEnable;
    if (i == StreamMesh)
    {
        //vec3_t Light;
        //Vector(1.f,1.f,1.f,Light);
        glColor3fv(BodyLight);
        EnableLight = false;
    }
    else if (EnableLight)
    {
        for (int j = 0; j < m->NumNormals; j++)
        {
            VectorScale(BodyLight, IntensityTransform[i][j], LightTransform[i][j]);
        }
    }

    int Render = RenderFlag;
    if ((RenderFlag & RENDER_COLOR) == RENDER_COLOR)
    {
        Render = RENDER_COLOR;
        if ((RenderFlag & RENDER_BRIGHT) == RENDER_BRIGHT)
            EnableAlphaBlend();
        else if ((RenderFlag & RENDER_DARK) == RENDER_DARK)
            EnableAlphaBlendMinus();
        else
            DisableAlphaBlend();

        if ((RenderFlag & RENDER_NODEPTH) == RENDER_NODEPTH)
        {
            DisableDepthTest();
        }

        DisableTexture();
        if (Alpha >= 0.99f)
        {
            glColor3fv(BodyLight);
        }
        else
        {
            EnableAlphaTest();
            glColor4f(BodyLight[0], BodyLight[1], BodyLight[2], Alpha);
        }
    }
    else if ((RenderFlag & RENDER_CHROME) == RENDER_CHROME ||
        (RenderFlag & RENDER_CHROME2) == RENDER_CHROME2 ||
        (RenderFlag & RENDER_CHROME3) == RENDER_CHROME3 ||
        (RenderFlag & RENDER_CHROME4) == RENDER_CHROME4 ||
        (RenderFlag & RENDER_CHROME5) == RENDER_CHROME5 ||
        (RenderFlag & RENDER_CHROME7) == RENDER_CHROME7 ||
        (RenderFlag & RENDER_METAL) == RENDER_METAL ||
        (RenderFlag & RENDER_OIL) == RENDER_OIL
        )
    {
        if (m->m_csTScript != nullptr)
        {
            if (m->m_csTScript->getNoneBlendMesh()) return;
        }
        if (m->NoneBlendMesh)
            return;
        Render = RENDER_CHROME;
        if ((RenderFlag & RENDER_CHROME4) == RENDER_CHROME4)
        {
            Render = RENDER_CHROME4;
        }
        float Wave2 = (int)WorldTime % 5000 * 0.00024f - 0.4f;

        vec3_t L = { (float)(cos(WorldTime * 0.001f)), (float)(sin(WorldTime * 0.002f)), 1.f };
        for (int j = 0; j < m->NumNormals; j++)
        {
            if (j > MAX_VERTICES) break;
            float* Normal = NormalTransform[i][j];

            if ((RenderFlag & RENDER_CHROME2) == RENDER_CHROME2)
            {
                g_chrome[j][0] = (Normal[2] + Normal[0]) * 0.8f + Wave2 * 2.f;
                g_chrome[j][1] = (Normal[1] + Normal[0]) * 1.0f + Wave2 * 3.f;
            }
            else if ((RenderFlag & RENDER_CHROME3) == RENDER_CHROME3)
            {
                g_chrome[j][0] = DotProduct(Normal, LightVector);
                g_chrome[j][1] = 1.f - DotProduct(Normal, LightVector);
            }
            else if ((RenderFlag & RENDER_CHROME4) == RENDER_CHROME4)
            {
                g_chrome[j][0] = DotProduct(Normal, L);
                g_chrome[j][1] = 1.f - DotProduct(Normal, L);
                g_chrome[j][1] -= Normal[2] * 0.5f + Wave * 3.f;
                g_chrome[j][0] += Normal[1] * 0.5f + L[1] * 3.f;
            }
            else if ((RenderFlag & RENDER_CHROME5) == RENDER_CHROME5)
            {
                Vector(0.1f, -0.23f, 0.22f, LightVector2);

                g_chrome[j][0] = (DotProduct(Normal, LightVector2) /*+ Normal[1] + LightVector2[1]*3.f */) / 1.08f;
                g_chrome[j][1] = (1.f - DotProduct(Normal, LightVector2) /*- Normal[2]*0.5f + 3.f */) / 1.08f;
            }
            else if ((RenderFlag & RENDER_CHROME6) == RENDER_CHROME6)
            {
                g_chrome[j][0] = (Normal[2] + Normal[0]) * 0.8f + Wave2 * 2.f;
                g_chrome[j][1] = (Normal[1] + Normal[0]) * 1.0f + Wave2 * 3.f;
            }
            else if ((RenderFlag & RENDER_CHROME7) == RENDER_CHROME7)
            {
                Vector(0.1f, -0.23f, 0.22f, LightVector2);

                g_chrome[j][0] = (DotProduct(Normal, LightVector2)) / 1.08f;
                g_chrome[j][1] = (1.f - DotProduct(Normal, LightVector2)) / 1.08f;
            }
            else if ((RenderFlag & RENDER_CHROME) == RENDER_CHROME)
            {
                g_chrome[j][0] = Normal[2] * 0.5f + Wave;
                g_chrome[j][1] = Normal[1] * 0.5f + Wave * 2.f;
            }
            else
            {
                g_chrome[j][0] = Normal[2] * 0.5f + 0.2f;
                g_chrome[j][1] = Normal[1] * 0.5f + 0.5f;
            }
        }

        if ((RenderFlag & RENDER_CHROME3) == RENDER_CHROME3
            || (RenderFlag & RENDER_CHROME4) == RENDER_CHROME4
            || (RenderFlag & RENDER_CHROME5) == RENDER_CHROME5
            || (RenderFlag & RENDER_CHROME7) == RENDER_CHROME7
            )
        {
            if (Alpha < 0.99f)
            {
                BodyLight[0] *= Alpha; BodyLight[1] *= Alpha; BodyLight[2] *= Alpha;
            }
            EnableAlphaBlend();
        }
        else if ((RenderFlag & RENDER_BRIGHT) == RENDER_BRIGHT)
        {
            if (Alpha < 0.99f)
            {
                BodyLight[0] *= Alpha; BodyLight[1] *= Alpha; BodyLight[2] *= Alpha;
            }
            EnableAlphaBlend();
        }
        else if ((RenderFlag & RENDER_DARK) == RENDER_DARK)
            EnableAlphaBlendMinus();
        else if ((RenderFlag & RENDER_LIGHTMAP) == RENDER_LIGHTMAP)
            EnableLightMap();
        else if (Alpha >= 0.99f)
        {
            DisableAlphaBlend();
        }
        else
        {
            EnableAlphaTest();
        }

        if ((RenderFlag & RENDER_NODEPTH) == RENDER_NODEPTH)
        {
            DisableDepthTest();
        }

        if ((RenderFlag & RENDER_CHROME2) == RENDER_CHROME2 && MeshTexture == -1)
        {
            BindTexture(BITMAP_CHROME2);
        }
        else if ((RenderFlag & RENDER_CHROME3) == RENDER_CHROME3 && MeshTexture == -1)
        {
            BindTexture(BITMAP_CHROME2);
        }
        else if ((RenderFlag & RENDER_CHROME4) == RENDER_CHROME4 && MeshTexture == -1)
        {
            BindTexture(BITMAP_CHROME2);
        }
        else if ((RenderFlag & RENDER_CHROME) == RENDER_CHROME && MeshTexture == -1)
            BindTexture(BITMAP_CHROME);
        else if ((RenderFlag & RENDER_METAL) == RENDER_METAL && MeshTexture == -1)
            BindTexture(BITMAP_SHINY);
        else
            BindTexture(Texture);
    }
    else if (BlendMesh <= -2 || m->Texture == BlendMesh)
    {
        Render = RENDER_TEXTURE;
        BindTexture(Texture);
        if ((RenderFlag & RENDER_DARK) == RENDER_DARK)
            EnableAlphaBlendMinus();
        else
            EnableAlphaBlend();

        if ((RenderFlag & RENDER_NODEPTH) == RENDER_NODEPTH)
        {
            DisableDepthTest();
        }

        glColor3f(BodyLight[0] * BlendMeshLight, BodyLight[1] * BlendMeshLight, BodyLight[2] * BlendMeshLight);
        //glColor3f(BlendMeshLight,BlendMeshLight,BlendMeshLight);
        EnableLight = false;
    }
    else if ((RenderFlag & RENDER_TEXTURE) == RENDER_TEXTURE)
    {
        Render = RENDER_TEXTURE;
        BindTexture(Texture);
        if ((RenderFlag & RENDER_BRIGHT) == RENDER_BRIGHT)
        {
            EnableAlphaBlend();
        }
        else if ((RenderFlag & RENDER_DARK) == RENDER_DARK)
        {
            EnableAlphaBlendMinus();
        }
        else if (Alpha < 0.99f || pBitmap->Components == 4)
        {
            EnableAlphaTest();
        }
        else
        {
            DisableAlphaBlend();
        }

        if ((RenderFlag & RENDER_NODEPTH) == RENDER_NODEPTH)
        {
            DisableDepthTest();
        }
    }
    else if ((RenderFlag & RENDER_BRIGHT) == RENDER_BRIGHT)
    {
        if (pBitmap->Components == 4 || m->Texture == BlendMesh)
        {
            return;
        }
        Render = RENDER_BRIGHT;
        EnableAlphaBlend();
        DisableTexture();
        DisableDepthMask();

        if ((RenderFlag & RENDER_NODEPTH) == RENDER_NODEPTH)
        {
            DisableDepthTest();
        }
    }
    else
    {
        Render = RENDER_TEXTURE;
    }

    // ver 1.0 (triangle)
    glBegin(GL_TRIANGLES);
    for (int j = 0; j < m->NumTriangles; j++)
    {
        Triangle_t* tp = &m->Triangles[j];
        for (int k = 0; k < tp->Polygon; k++)
        {
            int vi = tp->VertexIndex[k];
            switch (Render)
            {
            case RENDER_TEXTURE:
            {
                TexCoord_t* texp = &m->TexCoords[tp->TexCoordIndex[k]];
                if (EnableWave)
                    glTexCoord2f(texp->TexCoordU + BlendMeshTexCoordU, texp->TexCoordV + BlendMeshTexCoordV);
                else
                    glTexCoord2f(texp->TexCoordU, texp->TexCoordV);
                if (EnableLight)
                {
                    int ni = tp->NormalIndex[k];
                    if (Alpha >= 0.99f)
                    {
                        glColor3fv(LightTransform[i][ni]);
                    }
                    else
                    {
                        float* Light = LightTransform[i][ni];
                        glColor4f(Light[0], Light[1], Light[2], Alpha);
                    }
                }
                break;
            }
            case RENDER_CHROME:
            {
                if (Alpha >= 0.99f)
                    glColor3fv(BodyLight);
                else
                    glColor4f(BodyLight[0], BodyLight[1], BodyLight[2], Alpha);
                int ni = tp->NormalIndex[k];
                glTexCoord2f(g_chrome[ni][0], g_chrome[ni][1]);
                break;
            }
            }
            if ((iRndExtFlag & RNDEXT_WAVE))
            {
                float vPos[3];
                float fParam = (float)((int)WorldTime + vi * 931) * 0.007f;
                float fSin = sinf(fParam);
                int ni = tp->NormalIndex[k];
                float* Normal = NormalTransform[i][ni];
                for (int iCoord = 0; iCoord < 3; ++iCoord)
                {
                    vPos[iCoord] = VertexTransform[i][vi][iCoord] + Normal[iCoord] * fSin * 28.0f;
                }
                glVertex3fv(vPos);
            }
            else
            {
                glVertex3fv(VertexTransform[i][vi]);
            }
        }
    }
    glEnd();
}

void BMD::RenderMeshEffect(int i, int iType, int iSubType, vec3_t Angle, VOID* obj)
{
    if (i >= NumMeshs || i < 0) return;

    Mesh_t* m = &Meshs[i];
    if (m->NumTriangles <= 0) return;
    EnsureMeshSkinned(i);   // P-bmd-skinskip: spawns effects at VertexTransform positions

    vec3_t angle, Light;
    int iEffectCount = 0;

    Vector(0.f, 0.f, 0.f, angle);
    Vector(1.f, 1.f, 1.f, Light);
    for (int j = 0; j < m->NumTriangles; j++)
    {
        Triangle_t* tp = &m->Triangles[j];
        for (int k = 0; k < tp->Polygon; k++)
        {
            int vi = tp->VertexIndex[k];

            switch (iType)
            {
            case MODEL_STONE_COFFIN:
                if (iSubType == 0)
                {
                    if (rand_fps_check(2))
                    {
                        CreateEffect(MODEL_STONE_COFFIN + 1, VertexTransform[i][vi], angle, Light);
                    }
                    if (rand_fps_check(10))
                    {
                        CreateEffect(MODEL_STONE_COFFIN, VertexTransform[i][vi], angle, Light);
                    }
                }
                else if (iSubType == 1)
                {
                    CreateEffect(MODEL_STONE_COFFIN + 1, VertexTransform[i][vi], angle, Light, 2);
                }
                else if (iSubType == 2)
                {
                    CreateEffect(MODEL_STONE_COFFIN + 1, VertexTransform[i][vi], angle, Light, 3);
                }
                else if (iSubType == 3)
                {
                    CreateEffect(MODEL_STONE_COFFIN + rand() % 2, VertexTransform[i][vi], angle, Light, 4);
                }
                break;
            case MODEL_GATE:
                if (iSubType == 1)
                {
                    Vector(0.2f, 0.2f, 0.2f, Light);
                    if (rand_fps_check(5))
                    {
                        CreateEffect(MODEL_GATE + 1, VertexTransform[i][vi], angle, Light, 2);
                    }
                    if (rand_fps_check(10))
                    {
                        CreateEffect(MODEL_GATE, VertexTransform[i][vi], angle, Light, 2);
                    }
                }
                else if (iSubType == 0)
                {
                    Vector(0.2f, 0.2f, 0.2f, Light);
                    if (rand_fps_check(12))
                    {
                        CreateEffect(MODEL_GATE + 1, VertexTransform[i][vi], angle, Light);
                    }
                    if (rand_fps_check(50))
                    {
                        CreateEffect(MODEL_GATE, VertexTransform[i][vi], angle, Light);
                    }
                }
                break;
            case MODEL_BIG_STONE_PART1:
                if (rand_fps_check(3))
                {
                    CreateEffect(MODEL_BIG_STONE_PART1 + rand() % 2, VertexTransform[i][vi], angle, Light, 1);
                }
                break;

            case MODEL_BIG_STONE_PART2:
                if (rand_fps_check(3))
                {
                    CreateEffect(MODEL_BIG_STONE_PART1 + rand() % 2, VertexTransform[i][vi], angle, Light);
                }
                break;

            case MODEL_WALL_PART1:
                if (rand_fps_check(3))
                {
                    CreateEffect(MODEL_WALL_PART1 + rand() % 2, VertexTransform[i][vi], angle, Light);
                }
                break;

            case MODEL_GATE_PART1:
                Vector(0.2f, 0.2f, 0.2f, Light);
                if (rand_fps_check(12))
                {
                    CreateEffect(MODEL_GATE_PART1 + 1, VertexTransform[i][vi], angle, Light);
                }
                if (rand_fps_check(40))
                {
                    CreateEffect(MODEL_GATE_PART1, VertexTransform[i][vi], angle, Light);
                }
                if (rand_fps_check(40))
                {
                    CreateEffect(MODEL_GATE_PART1 + 2, VertexTransform[i][vi], angle, Light);
                }
                break;
            case MODEL_GOLEM_STONE:
                if (rand_fps_check(45) && iEffectCount < 20)
                {
                    if (iSubType == 0) {	//. 불골렘
                        CreateEffect(MODEL_GOLEM_STONE, VertexTransform[i][vi], angle, Light);
                    }
                    else if (iSubType == 1) {	//. 독골렘
                        CreateEffect(MODEL_BIG_STONE_PART1, VertexTransform[i][vi], angle, Light, 2);
                        CreateEffect(MODEL_BIG_STONE_PART2, VertexTransform[i][vi], angle, Light, 2);
                    }
                    iEffectCount++;
                }
                break;
            case MODEL_SKIN_SHELL:
                if (rand_fps_check(8))
                {
                    CreateEffect(MODEL_SKIN_SHELL, VertexTransform[i][vi], angle, Light, iSubType);
                }
                break;
            case BITMAP_LIGHT:
                Vector(0.08f, 0.08f, 0.08f, Light);
                if (iSubType == 0)
                {
                    CreateSprite(BITMAP_LIGHT, VertexTransform[i][vi], BodyScale, Light, nullptr);
                }
                else if (iSubType == 1)
                {
                    Vector(1.f, 0.8f, 0.2f, Light);
                    if ((j % 22) == 0)
                    {
                        auto* o = (OBJECT*)obj;

                        angle[0] = -(float)(rand() % 90);
                        angle[1] = 0.f;
                        angle[2] = Angle[2] + (float)(rand() % 120 - 60);
                        CreateJoint(BITMAP_JOINT_SPIRIT, VertexTransform[i][vi], o->Position, angle, 13, o, 20.f, 0, 0);
                    }
                }
                break;
            case BITMAP_BUBBLE:
                Vector(1.f, 1.f, 1.f, Light);
                if (rand_fps_check(30))
                {
                    CreateParticle(BITMAP_BUBBLE, VertexTransform[i][vi], angle, Light, 2);
                }
                break;
            }
        }
    }
}

void BMD::RenderBody(int Flag, float Alpha, int BlendMesh, float BlendMeshLight, float BlendMeshTexCoordU, float BlendMeshTexCoordV, int HiddenMesh, int Texture)
{
    if (NumMeshs == 0) return;

    int iBlendMesh = BlendMesh;
    BeginRender(Alpha);
    if (!LightEnable)
    {
        if (Alpha >= 0.99f)
            glColor3fv(BodyLight);
        else
            glColor4f(BodyLight[0], BodyLight[1], BodyLight[2], Alpha);
        // sub-task 6.7: mirror the flat body colour into ctx.flatColor so the instanced flat
        // branch reads it (per-worker) instead of glGetFloatv(GL_CURRENT_COLOR).
        Render::Build::CurrentRenderCtx().flatColor[0] = BodyLight[0];
        Render::Build::CurrentRenderCtx().flatColor[1] = BodyLight[1];
        Render::Build::CurrentRenderCtx().flatColor[2] = BodyLight[2];
        Render::Build::CurrentRenderCtx().flatColor[3] = (Alpha >= 0.99f) ? 1.f : Alpha;
    }
    for (int i = 0; i < NumMeshs; i++)
    {
        iBlendMesh = BlendMesh;

        Mesh_t* m = &Meshs[i];
        if (m->m_csTScript != nullptr)
        {
            if (m->m_csTScript->getHiddenMesh() == false && i != HiddenMesh)
            {
                if (m->m_csTScript->getBright())
                {
                    iBlendMesh = i;
                }
                RenderMesh(i, Flag, Alpha, iBlendMesh, BlendMeshLight, BlendMeshTexCoordU, BlendMeshTexCoordV, Texture);

                BYTE shadowType = m->m_csTScript->getShadowMesh();
                if (shadowType == SHADOW_RENDER_COLOR)
                {
                    DisableAlphaBlend();
                    if (Alpha >= 0.99f)
                        glColor3f(0.f, 0.f, 0.f);
                    else
                        glColor4f(0.f, 0.f, 0.f, Alpha);

                    RenderMesh(i, RENDER_COLOR | RENDER_SHADOWMAP, Alpha, iBlendMesh, BlendMeshLight, BlendMeshTexCoordU, BlendMeshTexCoordV);
                    glColor3f(1.f, 1.f, 1.f);
                }
                else if (shadowType == SHADOW_RENDER_TEXTURE)
                {
                    DisableAlphaBlend();
                    if (Alpha >= 0.99f)
                        glColor3f(0.f, 0.f, 0.f);
                    else
                        glColor4f(0.f, 0.f, 0.f, Alpha);

                    RenderMesh(i, RENDER_TEXTURE | RENDER_SHADOWMAP, Alpha, iBlendMesh, BlendMeshLight, BlendMeshTexCoordU, BlendMeshTexCoordV);
                    glColor3f(1.f, 1.f, 1.f);
                }
            }
        }
        else
        {
            if (i != HiddenMesh)
            {
                RenderMesh(i, Flag, Alpha, iBlendMesh, BlendMeshLight, BlendMeshTexCoordU, BlendMeshTexCoordV, Texture);
            }
        }
    }
    EndRender();
}

void BMD::RenderBodyAlternative(int iRndExtFlag, int iParam, int Flag, float Alpha, int BlendMesh, float BlendMeshLight, float BlendMeshTexCoordU, float BlendMeshTexCoordV, int HiddenMesh, int Texture)
{
    if (NumMeshs == 0) return;

    BeginRender(Alpha);
    if (!LightEnable)
    {
        if (Alpha >= 0.99f)
            glColor3fv(BodyLight);
        else
            glColor4f(BodyLight[0], BodyLight[1], BodyLight[2], Alpha);
    }
    for (int i = 0; i < NumMeshs; i++)
    {
        if (i != HiddenMesh)
        {
            RenderMeshAlternative(iRndExtFlag, iParam, i, Flag, Alpha, BlendMesh, BlendMeshLight, BlendMeshTexCoordU, BlendMeshTexCoordV, Texture);
        }
    }
    EndRender();
}

void BMD::RenderMeshTranslate(int i, int RenderFlag, float Alpha, int BlendMesh, float BlendMeshLight, float BlendMeshTexCoordU, float BlendMeshTexCoordV, int MeshTexture)
{
    if (i >= NumMeshs || i < 0) return;

    Mesh_t* m = &Meshs[i];
    if (m->NumTriangles == 0) return;
    EnsureMeshSkinned(i);   // P-bmd-skinskip: CPU draw reads VertexTransform
    float Wave = (int)WorldTime % 10000 * 0.0001f;

    int Texture = IndexTexture[m->Texture];
    if (Texture == BITMAP_HIDE)
        return;
    if (Texture == BITMAP_SKIN)
    {
        if (HideSkin) return;
        Texture = BITMAP_SKIN + Skin;
    }
    else if (Texture == BITMAP_WATER)
    {
        Texture = BITMAP_WATER + WaterTextureNumber;
    }
    if (MeshTexture != -1)
        Texture = MeshTexture;

    BITMAP_t* pBitmap = Bitmaps.GetTexture(Texture);

    bool EnableWave = false;
    int streamMesh = StreamMesh;
    if (m->m_csTScript != nullptr)
    {
        if (m->m_csTScript->getStreamMesh())
        {
            streamMesh = i;
        }
    }
    if ((i == BlendMesh || i == streamMesh) && (BlendMeshTexCoordU != 0.f || BlendMeshTexCoordV != 0.f))
        EnableWave = true;

    bool EnableLight = LightEnable;
    if (i == StreamMesh)
    {
        //vec3_t Light;
        //Vector(1.f,1.f,1.f,Light);
        glColor3fv(BodyLight);
        EnableLight = false;
    }
    else if (EnableLight)
    {
        for (int j = 0; j < m->NumNormals; j++)
        {
            VectorScale(BodyLight, IntensityTransform[i][j], LightTransform[i][j]);
        }
    }

    int Render = RenderFlag;
    if ((RenderFlag & RENDER_COLOR) == RENDER_COLOR)
    {
        Render = RENDER_COLOR;
        if ((RenderFlag & RENDER_BRIGHT) == RENDER_BRIGHT)
            EnableAlphaBlend();
        else if ((RenderFlag & RENDER_DARK) == RENDER_DARK)
            EnableAlphaBlendMinus();
        else
            DisableAlphaBlend();
        DisableTexture();
        glColor3fv(BodyLight);
    }
    else if ((RenderFlag & RENDER_CHROME) == RENDER_CHROME
        || (RenderFlag & RENDER_METAL) == RENDER_METAL
        || (RenderFlag & RENDER_CHROME2) == RENDER_CHROME2
        || (RenderFlag & RENDER_CHROME6) == RENDER_CHROME6
        )
    {
        if (m->m_csTScript != nullptr)
        {
            if (m->m_csTScript->getNoneBlendMesh()) return;
        }
        if (m->NoneBlendMesh)
            return;
        Render = RENDER_CHROME;

        float Wave2 = (int)WorldTime % 5000 * 0.00024f - 0.4f;

        for (int j = 0; j < m->NumNormals; j++)
        {
            //			Normal_t *np = &m->Normals[j];
            if (j > MAX_VERTICES) break;
            float* Normal = NormalTransform[i][j];

            if ((RenderFlag & RENDER_CHROME2) == RENDER_CHROME2)
            {
                g_chrome[j][0] = (Normal[2] + Normal[0]) * 0.8f + Wave2 * 2.f;
                g_chrome[j][1] = (Normal[1] + Normal[0]) * 1.0f + Wave2 * 3.f;
            }
            else if ((RenderFlag & RENDER_CHROME) == RENDER_CHROME)
            {
                g_chrome[j][0] = Normal[2] * 0.5f + Wave;
                g_chrome[j][1] = Normal[1] * 0.5f + Wave * 2.f;
            }
            else if ((RenderFlag & RENDER_CHROME6) == RENDER_CHROME6)
            {
                g_chrome[j][0] = (Normal[2] + Normal[0]) * 0.8f + Wave2 * 2.f;
                g_chrome[j][1] = (Normal[1] + Normal[0]) * 1.0f + Wave2 * 3.f;
            }
            else
            {
                g_chrome[j][0] = Normal[2] * 0.5f + 0.2f;
                g_chrome[j][1] = Normal[1] * 0.5f + 0.5f;
            }
        }

        if ((RenderFlag & RENDER_BRIGHT) == RENDER_BRIGHT)
            EnableAlphaBlend();
        else if ((RenderFlag & RENDER_DARK) == RENDER_DARK)
            EnableAlphaBlendMinus();
        else if ((RenderFlag & RENDER_LIGHTMAP) == RENDER_LIGHTMAP)
            EnableLightMap();
        else
            DisableAlphaBlend();

        if ((RenderFlag & RENDER_CHROME2) == RENDER_CHROME2 && MeshTexture == -1)
        {
            BindTexture(BITMAP_CHROME2);
        }
        else if ((RenderFlag & RENDER_CHROME) == RENDER_CHROME && MeshTexture == -1)
            BindTexture(BITMAP_CHROME);
        else if ((RenderFlag & RENDER_METAL) == RENDER_METAL && MeshTexture == -1)
            BindTexture(BITMAP_SHINY);
        else
            BindTexture(Texture);
    }
    else if (BlendMesh <= -2 || m->Texture == BlendMesh)
    {
        Render = RENDER_TEXTURE;
        BindTexture(Texture);
        if ((RenderFlag & RENDER_DARK) == RENDER_DARK)
            EnableAlphaBlendMinus();
        else
            EnableAlphaBlend();
        glColor3f(BodyLight[0] * BlendMeshLight, BodyLight[1] * BlendMeshLight, BodyLight[2] * BlendMeshLight);
        //glColor3f(BlendMeshLight,BlendMeshLight,BlendMeshLight);
        EnableLight = false;
    }
    else if ((RenderFlag & RENDER_TEXTURE) == RENDER_TEXTURE)
    {
        Render = RENDER_TEXTURE;
        BindTexture(Texture);
        if ((RenderFlag & RENDER_BRIGHT) == RENDER_BRIGHT)
        {
            EnableAlphaBlend();
        }
        else if ((RenderFlag & RENDER_DARK) == RENDER_DARK)
        {
            EnableAlphaBlendMinus();
        }
        else if (Alpha < 0.99f || pBitmap->Components == 4)
        {
            EnableAlphaTest();
        }
        else
        {
            DisableAlphaBlend();
        }
    }
    else if ((RenderFlag & RENDER_BRIGHT) == RENDER_BRIGHT)
    {
        if (pBitmap->Components == 4 || m->Texture == BlendMesh)
        {
            return;
        }
        Render = RENDER_BRIGHT;
        EnableAlphaBlend();
        DisableTexture();
        DisableDepthMask();
    }
    else
    {
        Render = RENDER_TEXTURE;
    }

    glBegin(GL_TRIANGLES);
    for (int j = 0; j < m->NumTriangles; j++)
    {
        vec3_t  pos;
        Triangle_t* tp = &m->Triangles[j];
        for (int k = 0; k < tp->Polygon; k++)
        {
            int vi = tp->VertexIndex[k];
            switch (Render)
            {
            case RENDER_TEXTURE:
            {
                TexCoord_t* texp = &m->TexCoords[tp->TexCoordIndex[k]];
                if (EnableWave)
                    glTexCoord2f(texp->TexCoordU + BlendMeshTexCoordU, texp->TexCoordV + BlendMeshTexCoordV);
                else
                    glTexCoord2f(texp->TexCoordU, texp->TexCoordV);
                if (EnableLight)
                {
                    int ni = tp->NormalIndex[k];
                    if (Alpha >= 0.99f)
                    {
                        glColor3fv(LightTransform[i][ni]);
                    }
                    else
                    {
                        float* Light = LightTransform[i][ni];
                        glColor4f(Light[0], Light[1], Light[2], Alpha);
                    }
                }
                break;
            }
            case RENDER_CHROME:
            {
                if (Alpha >= 0.99f)
                    glColor3fv(BodyLight);
                else
                    glColor4f(BodyLight[0], BodyLight[1], BodyLight[2], Alpha);
                int ni = tp->NormalIndex[k];
                glTexCoord2f(g_chrome[ni][0], g_chrome[ni][1]);
                break;
            }
            }
            {
                VectorAdd(VertexTransform[i][vi], BodyOrigin, pos);
                glVertex3fv(pos);
            }
        }
    }
    glEnd();
}

void BMD::RenderBodyTranslate(int Flag, float Alpha, int BlendMesh, float BlendMeshLight, float BlendMeshTexCoordU, float BlendMeshTexCoordV, int HiddenMesh, int Texture)
{
    if (NumMeshs == 0) return;

    BeginRender(Alpha);
    if (!LightEnable)
    {
        if (Alpha >= 0.99f)
            glColor3fv(BodyLight);
        else
            glColor4f(BodyLight[0], BodyLight[1], BodyLight[2], Alpha);
    }
    for (int i = 0; i < NumMeshs; i++)
    {
        if (i != HiddenMesh)
        {
            RenderMeshTranslate(i, Flag, Alpha, BlendMesh, BlendMeshLight, BlendMeshTexCoordU, BlendMeshTexCoordV, Texture);
        }
    }
    EndRender();
}

__forceinline void CalcShadowPosition(vec3_t* position, const vec3_t origin, const float sx, const float sy)
{
    vec3_t result;
    VectorCopy(*position, result);

    // Subtract the origin (position of the character) from the current position of the vertex
    // The result is the relative coordinate of the vertex to the origin.
    VectorSubtract(result, origin, result)

    // scale the shadow in the x direction
    result[0] += result[2] * (result[0] + sx) / (result[2] - sy);

    // Add the origin again, to get the absolute coordinate of the vertex again
    VectorAdd(result, origin, result);

    // put it on the ground by adding 5 to the actual ground coordinate.
    result[2] = RequestTerrainHeight(result[0], result[1]) + 5.f;

    // copy to result
    VectorCopy(result, *position);
}

__forceinline void GetClothShadowPosition(vec3_t* target, CPhysicsCloth* pCloth, const int index, const vec3_t origin, const float sx, const float sy)
{
    pCloth->GetPosition(index, target);
    CalcShadowPosition(target, origin, sx, sy);
}

void BMD::AddClothesShadowTriangles(void* pClothes, const int clothesCount, const float sx, const float sy) const
{
    auto vertices = RenderArrayVertices;
    int target_vertex_index = -1;
    
    for (int i = 0; i < clothesCount; i++)
    {
        auto* const pCloth = &static_cast<CPhysicsCloth*>(pClothes)[i];
        auto const columns = pCloth->GetVerticalCount();
        auto const rows = pCloth->GetHorizontalCount();
        
        for (int col = 0; col < columns - 1; ++col)
        {
            for (int row = 0; row < rows - 1; ++row)
            {
                // first we take each point for an square from which we derive
                // a A-Triangle and the V-Triangle.
                int a = rows * col + row;
                int b = rows * (col + 1) + row;
                int c = rows * col + row + 1;
                int d = rows * (col + 1) + row + 1;

                vec3_t posA, posB, posC, posD;

                GetClothShadowPosition(&posA, pCloth, a, BodyOrigin, sx, sy);
                GetClothShadowPosition(&posB, pCloth, b, BodyOrigin, sx, sy);
                GetClothShadowPosition(&posC, pCloth, c, BodyOrigin, sx, sy);
                GetClothShadowPosition(&posD, pCloth, d, BodyOrigin, sx, sy);

                // A-Triangle:
                target_vertex_index++;
                VectorCopy(posA, vertices[target_vertex_index]);
                target_vertex_index++;
                VectorCopy(posB, vertices[target_vertex_index]);
                target_vertex_index++;
                VectorCopy(posC, vertices[target_vertex_index]);

                // V-Triangle:
                target_vertex_index++;
                VectorCopy(posD, vertices[target_vertex_index]);
                target_vertex_index++;
                VectorCopy(posB, vertices[target_vertex_index]);
                target_vertex_index++;
                VectorCopy(posC, vertices[target_vertex_index]);
            }
        }
    }

    if (target_vertex_index < 0)
    {
        return;
    }

    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, vertices);
    glDrawArrays(GL_TRIANGLES, 0, target_vertex_index + 1);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
}

void BMD::AddMeshShadowTriangles(const int blendMesh, const int hiddenMesh, const int startMesh, const int endMesh, const float sx, const float sy) const
{
    auto vertices = RenderArrayVertices;
    int target_vertex_index = -1;

    for (int i = startMesh; i < endMesh; i++)
    {
        if (i == hiddenMesh)
        {
            continue;
        }

        const Mesh_t* mesh = &Meshs[i];
        if (mesh->NumTriangles <= 0 || mesh->Texture == blendMesh)
        {
            continue;
        }

        EnsureMeshSkinned(i);   // P-bmd-skinskip: CPU shadow fallback reads VertexTransform

        for (int j = 0; j < mesh->NumTriangles; j++)
        {
            const auto* tp = &mesh->Triangles[j];
            for (int k = 0; k < tp->Polygon; k++)
            {
                const int source_vertex_index = tp->VertexIndex[k];
                target_vertex_index++;

                VectorCopy(VertexTransform[i][source_vertex_index], vertices[target_vertex_index]);
                
                CalcShadowPosition(&vertices[target_vertex_index], BodyOrigin, sx, sy);
            }
        }
    }

    if (target_vertex_index < 0)
    {
        return;
    }

    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, vertices);
    glDrawArrays(GL_TRIANGLES, 0, target_vertex_index + 1);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
}

void BMD::RenderBodyShadow(const int blendMesh, const int hiddenMesh, const int startMeshNumber, const int endMeshNumber, void* pClothes, const int clothesCount)
{
    // Etapa 3b 6.8b: shadows were already emitted by the parallel MeshOnly pass; the
    // EffectsOnly replay must not re-emit them (see RenderMesh / BuildEmitMode.h).
    if (Render::Build::BuildSuppressMesh())
        return;

    if (!g_pOption->GetRenderAllEffects())
    {
        return;
    }

    // $noshadow: measurement-only toggle. MU_NOSHADOW=1 skips the whole shadow pass
    // so the harness can attribute the per-vertex CalcShadowPosition + immediate-mode
    // draw cost. No-op when unset.
    static const bool s_noShadow = [] {
        char buf[8] = {}; size_t n = 0;
        return getenv_s(&n, buf, sizeof(buf), "MU_NOSHADOW") == 0 && n > 0 && buf[0] == '1';
    }();
    if (s_noShadow)
    {
        return;
    }

    if (NumMeshs == 0 && clothesCount == 0)
    {
        return;
    }

    // P-bmd-shadow: GPU instanced shadow path. The mesh shadow (no cloth) is the
    // single biggest character CPU cost (per-vertex CalcShadowPosition +
    // RequestTerrainHeight + immediate draw, ~16ms at 100 chars). When this part is
    // GPU-skin eligible (same gate as the body instanced path), COLLECT its shadow-
    // casting meshes into the shadow batch (skinned + flattened on the GPU at
    // ShadowFlush) instead of drawing on the CPU. groundZ is sampled once here (vs
    // once per vertex). Cloth capes keep the CPU path. Falls back to CPU if any
    // included mesh is GPU-ineligible (so the part never double-draws).
    if (clothesCount == 0 && Render::Models::GpuShadowEnabled()
        && Render::Models::GpuCharsPass()
        && Render::Models::GpuBmdEnabled() && Render::Models::GpuInstEnabled()
        && Render::GL::IsLoaded()
        && BoneScale == 1.f && s_lastTransformScale == 0.f
        && s_lastTransformTranslate && s_lastBoneMatrix != nullptr)
    {
        const int gsStart = (startMeshNumber != -1) ? startMeshNumber : 0;
        const int gsEnd   = (endMeshNumber   != -1) ? endMeshNumber   : NumMeshs;

        bool allEligible = true;
        for (int i = gsStart; i < gsEnd; i++)
        {
            if (i == hiddenMesh) continue;
            const Mesh_t* mesh = &Meshs[i];
            if (mesh->NumTriangles <= 0 || mesh->Texture == blendMesh) continue;
            const auto* g = Render::Models::GetOrBuildMeshGpu(this, i, Render::GL::BmdShader::kMaxBones);
            if (g == nullptr || !g->eligible) { allEligible = false; break; }
        }

        if (allEligible)
        {
            const float gsSx = gMapManager.InBattleCastle() ? 2500.f : 2000.f;
            const float gsSy = 4000.f;
            const int   gsBase = InstPaletteBaseForCurrentPart(NumBones);
            const float gsGroundZ = RequestTerrainHeight(BodyOrigin[0], BodyOrigin[1]) + 5.f;
            for (int i = gsStart; i < gsEnd; i++)
            {
                if (i == hiddenMesh) continue;
                const Mesh_t* mesh = &Meshs[i];
                if (mesh->NumTriangles <= 0 || mesh->Texture == blendMesh) continue;
                Render::Models::ShadowRec rec;
                rec.paletteBase   = (float)gsBase;
                rec.bodyScale     = BodyScale;
                rec.bodyOrigin[0] = BodyOrigin[0];
                rec.bodyOrigin[1] = BodyOrigin[1];
                rec.bodyOrigin[2] = BodyOrigin[2];
                rec.groundZ       = gsGroundZ;
                Render::Models::ShadowAdd(this, i, rec, gsSx, gsSy);
            }
            return;   // collected to the GPU shadow batch; skip the legacy CPU draw
        }
        // else: fall through to the legacy CPU path for the whole part.
    }

    // Etapa 3b 6.9: GL-on-worker safety for the legacy CPU shadow path (issues GL below).
    // In the full-GPU config every shadow mesh instances (allEligible -> return above). If
    // a worker reaches here (a straggler mesh GetOrBuildMeshGpu deferred), it has no GL
    // context: skip rather than crash. The deferred mesh builds + instances on the next
    // main-thread frame.
    if (Core::Jobs::JobsEnabled() && Core::Jobs::ThreadPool::CurrentWorkerIndex() != 0)
    {
        static bool s_warnedShadowOnWorker = false;
        if (!s_warnedShadowOnWorker)
        {
            s_warnedShadowOnWorker = true;
            Render::GL::Log("[jobs] WARN: legacy CPU shadow path reached on worker %d — "
                            "straggler mesh under MU_JOBS; skipping GL (will instance next frame)",
                            Core::Jobs::ThreadPool::CurrentWorkerIndex());
        }
        return;
    }

    EnableAlphaTest(false);

    glColor4f(0.0f, 0.0f, 0.0f, 0.5f); // 50% opacity for shadows

    DisableTexture();
    DisableDepthMask();
    BeginRender(1.f);

    // enable stencil and continue draw
    glEnable(GL_STENCIL_TEST);
    glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);

    int startMesh = 0;
    int endMesh = NumMeshs;

    if (startMeshNumber != -1)
    {
        startMesh = startMeshNumber;
    }

    if (endMeshNumber != -1)
    {
        endMesh = endMeshNumber;
    }

    const float sx = gMapManager.InBattleCastle() ? 2500.f : 2000.f;
    const float sy = 4000.f;

    if (clothesCount == 0)
    {
        AddMeshShadowTriangles(blendMesh, hiddenMesh, startMesh, endMesh, sx, sy);
    }
    else
    {
        AddClothesShadowTriangles(pClothes, clothesCount, sx, sy);
    }

    EndRender();
    EnableDepthMask();

    glDisable(GL_STENCIL_TEST);
}

void BMD::RenderObjectBoundingBox()
{
    DisableTexture();
    glPushMatrix();
    glTranslatef(BodyOrigin[0], BodyOrigin[1], BodyOrigin[2]);
    glScalef(BodyScale, BodyScale, BodyScale);
    for (int i = 0; i < NumBones; i++)
    {
        Bone_t* b = &Bones[i];
        if (b->BoundingBox)
        {
            vec3_t BoundingVertices[8];
            for (int j = 0; j < 8; j++)
            {
                VectorTransform(b->BoundingVertices[j], g_BoneTransformScratch[i], BoundingVertices[j]);
            }

            glBegin(GL_QUADS);
            glColor3f(0.2f, 0.2f, 0.2f);
            glTexCoord2f(1.0F, 1.0F); glVertex3fv(BoundingVertices[7]);
            glTexCoord2f(1.0F, 0.0F); glVertex3fv(BoundingVertices[6]);
            glTexCoord2f(0.0F, 0.0F); glVertex3fv(BoundingVertices[4]);
            glTexCoord2f(0.0F, 1.0F); glVertex3fv(BoundingVertices[5]);

            glColor3f(0.2f, 0.2f, 0.2f);
            glTexCoord2f(0.0F, 1.0F); glVertex3fv(BoundingVertices[0]);
            glTexCoord2f(1.0F, 1.0F); glVertex3fv(BoundingVertices[2]);
            glTexCoord2f(1.0F, 0.0F); glVertex3fv(BoundingVertices[3]);
            glTexCoord2f(0.0F, 0.0F); glVertex3fv(BoundingVertices[1]);

            glColor3f(0.6f, 0.6f, 0.6f);
            glTexCoord2f(1.0F, 1.0F); glVertex3fv(BoundingVertices[7]);
            glTexCoord2f(1.0F, 0.0F); glVertex3fv(BoundingVertices[3]);
            glTexCoord2f(0.0F, 0.0F); glVertex3fv(BoundingVertices[2]);
            glTexCoord2f(0.0F, 1.0F); glVertex3fv(BoundingVertices[6]);

            glColor3f(0.6f, 0.6f, 0.6f);
            glTexCoord2f(0.0F, 1.0F); glVertex3fv(BoundingVertices[0]);
            glTexCoord2f(1.0F, 1.0F); glVertex3fv(BoundingVertices[1]);
            glTexCoord2f(1.0F, 0.0F); glVertex3fv(BoundingVertices[5]);
            glTexCoord2f(0.0F, 0.0F); glVertex3fv(BoundingVertices[4]);

            glColor3f(0.4f, 0.4f, 0.4f);
            glTexCoord2f(1.0F, 1.0F); glVertex3fv(BoundingVertices[7]);
            glTexCoord2f(1.0F, 0.0F); glVertex3fv(BoundingVertices[5]);
            glTexCoord2f(0.0F, 0.0F); glVertex3fv(BoundingVertices[1]);
            glTexCoord2f(0.0F, 1.0F); glVertex3fv(BoundingVertices[3]);

            glColor3f(0.4f, 0.4f, 0.4f);
            glTexCoord2f(0.0F, 1.0F); glVertex3fv(BoundingVertices[0]);
            glTexCoord2f(1.0F, 1.0F); glVertex3fv(BoundingVertices[4]);
            glTexCoord2f(1.0F, 0.0F); glVertex3fv(BoundingVertices[6]);
            glTexCoord2f(0.0F, 0.0F); glVertex3fv(BoundingVertices[2]);
            glEnd();
        }
    }
    glPopMatrix();
    DisableAlphaBlend();
}

void BMD::RenderBone(float(*BoneMatrix)[3][4])
{
    DisableTexture();
    glDepthFunc(GL_ALWAYS);
    glColor3f(0.8f, 0.8f, 0.2f);
    for (int i = 0; i < NumBones; i++)
    {
        Bone_t* b = &Bones[i];
        if (!b->Dummy)
        {
            BoneMatrix_t* bm = &b->BoneMatrixes[CurrentAction];
            int Parent = b->Parent;
            if (Parent > 0)
            {
                float Scale = 1.f;
                float dx = bm->Position[CurrentAnimationFrame][0];
                float dy = bm->Position[CurrentAnimationFrame][1];
                float dz = bm->Position[CurrentAnimationFrame][2];
                Scale = sqrtf(dx * dx + dy * dy + dz * dz) * 0.05f;
                vec3_t Position[3];
                Vector(0.f, 0.f, -Scale, Position[0]);
                Vector(0.f, 0.f, Scale, Position[1]);
                Vector(0.f, 0.f, 0.f, Position[2]);
                vec3_t BoneVertices[3];
                VectorTransform(Position[0], BoneMatrix[Parent], BoneVertices[0]);
                VectorTransform(Position[1], BoneMatrix[Parent], BoneVertices[1]);
                VectorTransform(Position[2], BoneMatrix[i], BoneVertices[2]);
                for (auto & BoneVertice : BoneVertices)
                {
                    VectorMA(BodyOrigin, BodyScale, BoneVertice, BoneVertice);
                }
                glBegin(GL_LINES);
                glVertex3fv(BoneVertices[0]);
                glVertex3fv(BoneVertices[1]); 
                glVertex3fv(BoneVertices[1]);
                glVertex3fv(BoneVertices[2]);
                glVertex3fv(BoneVertices[2]);
                glVertex3fv(BoneVertices[0]);
                glEnd();
            }
        }
    }
    glDepthFunc(GL_LEQUAL);
}

void BMD::Release()
{
    // P-bmd-gpu/instance: this slot's Meshs are about to be freed; drop any GPU
    // geometry/instance buckets cached against this BMD* so an in-place reload
    // (same address, new geometry — e.g. monster slots across maps) can't reuse
    // stale VBOs.
    Render::Models::InvalidateGpuModel(this);

    if (Bones)
    {
        for (int i = 0; i < NumBones; ++i)
        {
            Bone_t* b = &Bones[i];

            if (!b->Dummy && b->BoneMatrixes)
            {
                for (int j = 0; j < NumActions; ++j)
                {
                    BoneMatrix_t* bm = &b->BoneMatrixes[j];
                    if (bm)
                    {
                        if (bm->Position) {
							delete[] bm->Position;
							bm->Position = nullptr;
                        }
                        if (bm->Rotation) {
							delete[] bm->Rotation;
							bm->Rotation = nullptr;
                        }
						if (bm->Quaternion) {
							delete[] bm->Quaternion;
							bm->Quaternion = nullptr;

                        }
                    }
                }
                if (b->BoneMatrixes) { delete[] b->BoneMatrixes; b->BoneMatrixes = nullptr; }
            }
        }
    }

    if (Actions)
    {
        for (int i = 0; i < NumActions; ++i)
        {
            Action_t* a = &Actions[i];
            if (a && a->LockPositions && a->Positions)
            {
                delete[] a->Positions;
                a->Positions = nullptr;
            }
        }
    }

    if (Meshs)
    {
        for (int i = 0; i < NumMeshs; ++i)
        {
            Mesh_t* m = &Meshs[i];

            if (m->Vertices) { delete[] m->Vertices; m->Vertices = nullptr; }
            if (m->Normals) { delete[] m->Normals; m->Normals = nullptr; }
            if (m->TexCoords) { delete[] m->TexCoords; m->TexCoords = nullptr; }
            if (m->Triangles) { delete[] m->Triangles; m->Triangles = nullptr; }

            if (m->m_csTScript)
            {
                delete m->m_csTScript;
                m->m_csTScript = nullptr;
            }

            if (IndexTexture && m->Texture >= 0)
            {
                auto textureIndex = IndexTexture[m->Texture];
                if (textureIndex >= BITMAP_SKIN_BEGIN && textureIndex <= BITMAP_SKIN_END)
                    continue;

                DeleteBitmap(textureIndex);
            }
        }
    }

    if (Meshs) { delete[] Meshs; Meshs = nullptr; }
    if (Bones) { delete[] Bones; Bones = nullptr; }
    if (Actions) { delete[] Actions; Actions = nullptr; }
    if (Textures) { delete[] Textures; Textures = nullptr; }
    if (IndexTexture) { delete[] IndexTexture; IndexTexture = nullptr; }

    NumBones = 0;
    NumActions = 0;
    NumMeshs = 0;

#ifdef LDS_FIX_SETNULLALLOCVALUE_WHEN_BMDRELEASE
    m_bCompletedAlloc = false;
#endif
}

void BMD::FindNearTriangle()
{
    for (int iMesh = 0; iMesh < NumMeshs; iMesh++)
    {
        Mesh_t* m = &Meshs[iMesh];

        Triangle_t* pTriangle = m->Triangles;
        int iNumTriangles = m->NumTriangles;
        for (int iTri = 0; iTri < iNumTriangles; ++iTri)
        {
            for (int i = 0; i < 3; ++i)
            {
                pTriangle[iTri].EdgeTriangleIndex[i] = -1;
            }
        }
        for (int iTri = 0; iTri < iNumTriangles; ++iTri)
        {
            FindTriangleForEdge(iMesh, iTri, 0);
            FindTriangleForEdge(iMesh, iTri, 1);
            FindTriangleForEdge(iMesh, iTri, 2);
        }
    }
}

void BMD::FindTriangleForEdge(int iMesh, int iTri1, int iIndex11)
{
    if (iMesh >= NumMeshs || iMesh < 0) return;

    Mesh_t* m = &Meshs[iMesh];
    Triangle_t* pTriangle = m->Triangles;

    Triangle_t* pTri1 = &pTriangle[iTri1];
    if (pTri1->EdgeTriangleIndex[iIndex11] != -1)
    {
        return;
    }

    int iNumTriangles = m->NumTriangles;
    for (int iTri2 = 0; iTri2 < iNumTriangles; ++iTri2)
    {
        if (iTri1 == iTri2)
        {
            continue;
        }

        Triangle_t* pTri2 = &pTriangle[iTri2];
        int iIndex12 = (iIndex11 + 1) % 3;
        for (int iIndex21 = 0; iIndex21 < 3; ++iIndex21)
        {
            int iIndex22 = (iIndex21 + 1) % 3;
            if (pTri2->EdgeTriangleIndex[iIndex21] == -1 &&
                pTri1->VertexIndex[iIndex11] == pTri2->VertexIndex[iIndex22] &&
                pTri1->VertexIndex[iIndex12] == pTri2->VertexIndex[iIndex21])
            {
                pTri1->EdgeTriangleIndex[iIndex11] = iTri2;
                pTri2->EdgeTriangleIndex[iIndex21] = iTri1;
                return;
            }
        }
    }
}
//#endif //USE_SHADOWVOLUME


class BMDReader {
public:
    BMDReader(unsigned char* data, size_t size) : data(data), size(size), ptr(0) {}

    void Skip(size_t bytes) { ptr += bytes; }

    template <typename T>
    T Read() {
        T value;
        memcpy(&value, data + ptr, sizeof(T));
        ptr += sizeof(T);
        return value;
    }

    void ReadBytes(void* dst, size_t count) {
        memcpy(dst, data + ptr, count);
        ptr += count;
    }

    size_t Tell() const { return ptr; }
    unsigned char* GetPointer() const { return data + ptr; }

private:
    unsigned char* data;
    size_t size;
    size_t ptr;
};


bool BMD::Open2(const wchar_t* DirName, const wchar_t* ModelFileName, bool bReAlloc)
{
    if (m_bCompletedAlloc)
    {
        if (!bReAlloc)
            return true;
        Release();
    }

    wchar_t ModelPath[260] = {};
    _snwprintf(ModelPath, std::size(ModelPath), L"%ls%ls", DirName, ModelFileName);

    FILE* fp = _wfopen(ModelPath, L"rb");
    if (!fp)
    {
        //// wprintf(L"[Open2] ERROR: Unable to open file: %ls\n", ModelPath);
        m_bCompletedAlloc = false;
        return false;
    }

    fseek(fp, 0, SEEK_END);
    int dataSize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    std::unique_ptr<unsigned char[]> fileData(new(std::nothrow) unsigned char[dataSize]);
    if (!fileData)
    {
        fclose(fp);
        m_bCompletedAlloc = false;
        return false;
    }

    fread(fileData.get(), 1, dataSize, fp);
    fclose(fp);

    // *** Check the "BMD" header ***
    if (!(fileData[0] == 'B' && fileData[1] == 'M' && fileData[2] == 'D'))
    {
        wprintf(L"[Open2] ERROR: Invalid file header (expected 'BMD') in file %.64s\n", ModelPath);
        m_bCompletedAlloc = false;
        return false;
    }

    int ptr = 3;
    Version = fileData[ptr++];
    

    std::unique_ptr<unsigned char[]> decryptedData;
    if (Version == 0xC)
    {
        //// wprintf(L"[Open2] Version: %d\n", Version);
        long encSize = *(long*)(fileData.get() + ptr); ptr += sizeof(long);
        unsigned char* encData = fileData.get() + ptr;
        //// wprintf(L"[Open2] Encrypted Size: %ld\n", encSize);

        long decSize = MapFileDecrypt(nullptr, encData, encSize);
        //// wprintf(L"[Open2] Decrypted Size: %ld\n", decSize);

        decryptedData.reset(new(std::nothrow) unsigned char[decSize]);
        if (!decryptedData)
        {
            m_bCompletedAlloc = false;
            return false;
        }

        MapFileDecrypt(decryptedData.get(), encData, encSize);
        ptr = 0;
    }
    else if (Version == 0xE)
    {
        wprintf(L"[Open2] Version: %d\n, not yet supported. File: %.64s\n", Version, ModelPath);
        // FIXME FOR NEW MAPS 
        // DECRYPT KEY: webzen#@!01webzen#@!01webzen#@!0
    }
    else if (Version == 0xA)
    {
        // wprintf(L"[Open2] Version: %d\n", Version);
        ptr = 4;
    }
    else
    {
        wprintf(L"[Open2] Unknown BMD version: %ld\n in %.64s\n", Version, ModelPath);
        m_bCompletedAlloc = false;
        return false;
    }


    unsigned char* data = decryptedData ? decryptedData.get() : fileData.get();

    memcpy(Name, data + ptr, 32); ptr += 32;

    const char* ext = strrchr(Name, '.');
    if (!ext || (_stricmp(ext, ".smd") != 0))
    {
        // wprintf(L"[Open2] WARNING: Invalid file extension: %.64hs in %.64s\n", Name, ModelPath);
    }

    NumMeshs = *(short*)(data + ptr); ptr += sizeof(short);
    NumBones = *(short*)(data + ptr); ptr += sizeof(short);
    NumActions = *(short*)(data + ptr); ptr += sizeof(short);

    assert(NumBones <= MAX_BONES && "Bones 200");
    //// wprintf(L"[Open2] Model: %.32hs | Meshes: %d | Bones: %d | Actions: %d\n", Name, NumMeshs, NumBones, NumActions);

    const int meshCount = NumMeshs > 0 ? NumMeshs : 1;
    const int boneCount = NumBones > 0 ? NumBones : 1;
    const int actionCount = NumActions > 0 ? NumActions : 1;

    Meshs = new(std::nothrow) Mesh_t[meshCount]();
    Bones = new(std::nothrow) Bone_t[boneCount]();
    Actions = new(std::nothrow) Action_t[actionCount]();
    Textures = new(std::nothrow) Texture_t[meshCount]();
    IndexTexture = new(std::nothrow) GLuint[meshCount]();

    if (!Meshs || !Bones || !Actions || !Textures || !IndexTexture)
    {
        Release();
        m_bCompletedAlloc = false;
        return false;
    }

    for (int i = 0; i < NumMeshs; ++i)
    {
        Mesh_t& m = Meshs[i];
        m.NumVertices = *(short*)(data + ptr); ptr += sizeof(short);
        m.NumNormals = *(short*)(data + ptr); ptr += sizeof(short);
        m.NumTexCoords = *(short*)(data + ptr); ptr += sizeof(short);
        m.NumTriangles = *(short*)(data + ptr); ptr += sizeof(short);
        m.Texture = *(short*)(data + ptr); ptr += sizeof(short);
        m.NoneBlendMesh = false;

        //// wprintf(L"[Open2] Mesh[%d] V:%d N:%d T:%d Tri:%d Tex:%d\n", i, m.NumVertices, m.NumNormals, m.NumTexCoords, m.NumTriangles, m.Texture);

        m.Vertices = new Vertex_t[m.NumVertices];
        m.Normals = new Normal_t[m.NumNormals];
        m.TexCoords = new TexCoord_t[m.NumTexCoords];
        m.Triangles = new Triangle_t[m.NumTriangles];

        memcpy(m.Vertices, data + ptr, m.NumVertices * sizeof(Vertex_t));  ptr += m.NumVertices * sizeof(Vertex_t);
        memcpy(m.Normals, data + ptr, m.NumNormals * sizeof(Normal_t));   ptr += m.NumNormals * sizeof(Normal_t);
        memcpy(m.TexCoords, data + ptr, m.NumTexCoords * sizeof(TexCoord_t)); ptr += m.NumTexCoords * sizeof(TexCoord_t);

        for (int j = 0; j < m.NumTriangles; ++j)
        {
            memcpy(&m.Triangles[j], data + ptr, sizeof(Triangle_t));
            ptr += sizeof(Triangle_t2);
        }

        memcpy(Textures[i].FileName, data + ptr, 32); ptr += 32;

        TextureScriptParsing script;
        if (script.parsingTScriptA(Textures[i].FileName))
        {
            m.m_csTScript = new TextureScript;
            m.m_csTScript->setScript(script);
        }
        else
        {
            m.m_csTScript = nullptr;
        }
    }

    for (int i = 0; i < NumActions; ++i)
    {
        Action_t& a = Actions[i];
        a.Loop = false;
        a.NumAnimationKeys = *(short*)(data + ptr); ptr += sizeof(short);
        a.LockPositions = *(bool*)(data + ptr);  ptr += sizeof(bool);

        //// wprintf(L"[Open2] Action[%d] Keys: %d Lock: %d\n", i, a.NumAnimationKeys, a.LockPositions);

        if (a.LockPositions && a.NumAnimationKeys > 0)
        {
            a.Positions = new vec3_t[a.NumAnimationKeys];
            memcpy(a.Positions, data + ptr, sizeof(vec3_t) * a.NumAnimationKeys);
            ptr += sizeof(vec3_t) * a.NumAnimationKeys;
        }
        else
        {
            a.Positions = nullptr;
        }
    }

    for (int i = 0; i < NumBones; ++i)
    {
        Bone_t& b = Bones[i];
        b.Dummy = *(char*)(data + ptr); ptr += sizeof(char);

        if (!b.Dummy)
        {
            memcpy(b.Name, data + ptr, 32); ptr += 32;
            b.Parent = *(short*)(data + ptr); ptr += sizeof(short);

            //// wprintf(L"[Open2] Bone[%d] Name: %.32hs Parent: %d\n", i, b.Name, b.Parent);

            b.BoneMatrixes = new BoneMatrix_t[NumActions]();

            for (int j = 0; j < NumActions; ++j)
            {
                BoneMatrix_t& bm = b.BoneMatrixes[j];
                int numKeys = Actions[j].NumAnimationKeys;

                if (numKeys > 0)
                {
                    bm.Position = new vec3_t[numKeys];
                    bm.Rotation = new vec3_t[numKeys];
                    bm.Quaternion = new vec4_t[numKeys];

                    memcpy(bm.Position, data + ptr, sizeof(vec3_t) * numKeys); ptr += sizeof(vec3_t) * numKeys;
                    memcpy(bm.Rotation, data + ptr, sizeof(vec3_t) * numKeys); ptr += sizeof(vec3_t) * numKeys;

                    for (int k = 0; k < numKeys; ++k)
                        AngleQuaternion(bm.Rotation[k], bm.Quaternion[k]);

                }
                else
                {
                    bm.Position = nullptr;
                    bm.Rotation = nullptr;
                    bm.Quaternion = nullptr;
                }
            }
        }
    }

    Init(false);
    m_bCompletedAlloc = true;
    return true;
}


bool BMD::Save2(wchar_t* DirName, wchar_t* ModelFileName)
{
    wchar_t ModelName[64];
    wcscpy(ModelName, DirName);
    wcscat(ModelName, ModelFileName);
    FILE* fp = _wfopen(ModelName, L"wb");
    if (fp == nullptr) return false;
    putc('B', fp);
    putc('M', fp);
    putc('D', fp);
    Version = 12;
    fwrite(&Version, 1, 1, fp);

    auto* pbyBuffer = new BYTE[1024 * 1024];
    BYTE* pbyCur = pbyBuffer;
    memcpy(pbyCur, Name, 32); pbyCur += 32;
    memcpy(pbyCur, &NumMeshs, 2); pbyCur += 2;
    memcpy(pbyCur, &NumBones, 2); pbyCur += 2;
    memcpy(pbyCur, &NumActions, 2); pbyCur += 2;

    int i;
    for (i = 0; i < NumMeshs; i++)
    {
        Mesh_t* m = &Meshs[i];
        memcpy(pbyCur, &m->NumVertices, 2); pbyCur += 2;
        memcpy(pbyCur, &m->NumNormals, 2); pbyCur += 2;
        memcpy(pbyCur, &m->NumTexCoords, 2); pbyCur += 2;
        memcpy(pbyCur, &m->NumTriangles, 2); pbyCur += 2;
        memcpy(pbyCur, &m->Texture, 2); pbyCur += 2;
        memcpy(pbyCur, m->Vertices, m->NumVertices * sizeof(Vertex_t)); pbyCur += m->NumVertices * sizeof(Vertex_t);
        memcpy(pbyCur, m->Normals, m->NumNormals * sizeof(Normal_t)); pbyCur += m->NumNormals * sizeof(Normal_t);
        memcpy(pbyCur, m->TexCoords, m->NumTexCoords * sizeof(TexCoord_t)); pbyCur += m->NumTexCoords * sizeof(TexCoord_t);
        for (int j = 0; j < m->NumTriangles; j++)
        {
            memcpy(pbyCur, &m->Triangles[j], sizeof(Triangle_t2)); pbyCur += sizeof(Triangle_t2);
        }
        memcpy(pbyCur, Textures[i].FileName, 32); pbyCur += 32;
    }
    for (i = 0; i < NumActions; i++)
    {
        Action_t* a = &Actions[i];
        memcpy(pbyCur, &a->NumAnimationKeys, 2); pbyCur += 2;
        memcpy(pbyCur, &a->LockPositions, 1); pbyCur += 1;
        if (a->LockPositions)
        {
            memcpy(pbyCur, a->Positions, a->NumAnimationKeys * sizeof(vec3_t)); pbyCur += a->NumAnimationKeys * sizeof(vec3_t);
        }
    }
    for (i = 0; i < NumBones; i++)
    {
        Bone_t* b = &Bones[i];
        memcpy(pbyCur, &b->Dummy, 1); pbyCur += 1;
        if (!b->Dummy)
        {
            memcpy(pbyCur, b->Name, 32); pbyCur += 32;
            memcpy(pbyCur, &b->Parent, 2); pbyCur += 2;
            for (int j = 0; j < NumActions; j++)
            {
                BoneMatrix_t* bm = &b->BoneMatrixes[j];
                memcpy(pbyCur, bm->Position, Actions[j].NumAnimationKeys * sizeof(vec3_t)); pbyCur += Actions[j].NumAnimationKeys * sizeof(vec3_t);
                memcpy(pbyCur, bm->Rotation, Actions[j].NumAnimationKeys * sizeof(vec3_t)); pbyCur += Actions[j].NumAnimationKeys * sizeof(vec3_t);
            }
        }
    }
    auto lSize = (long)(pbyCur - pbyBuffer);
    long lEncSize = MapFileEncrypt(nullptr, pbyBuffer, lSize);
    auto* pbyEnc = new BYTE[lEncSize];
    MapFileEncrypt(pbyEnc, pbyBuffer, lSize);
    fwrite(&lEncSize, sizeof(long), 1, fp);
    fwrite(pbyEnc, lEncSize, 1, fp);
    fclose(fp);
    delete[] pbyBuffer;
    delete[] pbyEnc;
    return true;
}

void BMD::Init(bool Dummy)
{
    if (Dummy)
    {
        int i;
        for (i = 0; i < NumBones; i++)
        {
            Bone_t* b = &Bones[i];
            if (b->Name[0] == 'D' && b->Name[1] == 'u')
                b->Dummy = true;
            else
                b->Dummy = false;
        }
    }
    renderCount = 0;
    BoneHead = -1;
    StreamMesh = -1;
    CreateBoundingBox();
}

void BMD::CreateBoundingBox()
{
    for (int i = 0; i < NumBones; i++)
    {
        for (int j = 0; j < 3; j++)
        {
            BoundingMin[i][j] = 9999.0;
            BoundingMax[i][j] = -9999.0;
        }
        BoundingVertices[i] = 0;
    }

    for (int i = 0; i < NumMeshs; i++)
    {
        Mesh_t* m = &Meshs[i];
        for (int j = 0; j < m->NumVertices; j++)
        {
            Vertex_t* v = &m->Vertices[j];
            for (int k = 0; k < 3; k++)
            {
                if (v->Position[k] < BoundingMin[v->Node][k]) BoundingMin[v->Node][k] = v->Position[k];
                if (v->Position[k] > BoundingMax[v->Node][k]) BoundingMax[v->Node][k] = v->Position[k];
            }
            BoundingVertices[v->Node]++;
        }
    }
    for (int i = 0; i < NumBones; i++)
    {
        Bone_t* b = &Bones[i];
        if (BoundingVertices[i])
            b->BoundingBox = true;
        else
            b->BoundingBox = false;
        Vector(BoundingMax[i][0], BoundingMax[i][1], BoundingMax[i][2], b->BoundingVertices[0]);
        Vector(BoundingMax[i][0], BoundingMax[i][1], BoundingMin[i][2], b->BoundingVertices[1]);
        Vector(BoundingMax[i][0], BoundingMin[i][1], BoundingMax[i][2], b->BoundingVertices[2]);
        Vector(BoundingMax[i][0], BoundingMin[i][1], BoundingMin[i][2], b->BoundingVertices[3]);
        Vector(BoundingMin[i][0], BoundingMax[i][1], BoundingMax[i][2], b->BoundingVertices[4]);
        Vector(BoundingMin[i][0], BoundingMax[i][1], BoundingMin[i][2], b->BoundingVertices[5]);
        Vector(BoundingMin[i][0], BoundingMin[i][1], BoundingMax[i][2], b->BoundingVertices[6]);
        Vector(BoundingMin[i][0], BoundingMin[i][1], BoundingMin[i][2], b->BoundingVertices[7]);
    }
}

BMD::~BMD()
{
    Release();
}

void BMD::InterpolationTrans(float(*Mat1)[4], float(*TransMat2)[4], float _Scale)
{
    TransMat2[0][3] = TransMat2[0][3] - (TransMat2[0][3] - Mat1[0][3]) * (1 - _Scale);
    TransMat2[1][3] = TransMat2[1][3] - (TransMat2[1][3] - Mat1[1][3]) * (1 - _Scale);
    TransMat2[2][3] = TransMat2[2][3] - (TransMat2[2][3] - Mat1[2][3]) * (1 - _Scale);
}