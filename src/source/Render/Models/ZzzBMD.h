#ifndef __ZZZBMD_H__
#define __ZZZBMD_H__

#include "Render/Sprites/TextureScript.h"

#define MAX_BONES    200
#define MAX_MESH     50
#define MAX_VERTICES 15000

#define RENDER_COLOR        0x00000001
#define RENDER_TEXTURE      0x00000002
#define RENDER_CHROME       0x00000004
#define RENDER_METAL        0x00000008
#define RENDER_LIGHTMAP     0x00000010
#define RENDER_SHADOWMAP    0x00000020
#define RENDER_BRIGHT       0x00000040
#define RENDER_DARK         0x00000080
#define RENDER_EXTRA        0x00000100
#define RENDER_CHROME2      0x00000200
#define RENDER_WAVE			0x00000400 
#define RENDER_CHROME3      0x00000800
#define RENDER_CHROME4      0x00001000
#define RENDER_NODEPTH      0x00002000
#define RENDER_CHROME5      0x00004000
#define RENDER_OIL          0x00008000
#define RENDER_CHROME6      0x00010000
#define RENDER_CHROME7      0x00020000
#define RENDER_DOPPELGANGER 0x00040000
#define RENDER_WAVE_EXT		0x10000000
#define RENDER_BYSCRIPT		0x80000000
#define RNDEXT_WAVE			1
#define RNDEXT_OIL          2
#define RNDEXT_RISE			4

#define MAX_MONSTER_SOUND   10

namespace Render::Models { struct MeshGpu; }   // P-bmd-gpu (see BmdGpuCache.h)

typedef struct
{
    vec3_t Position;
    vec3_t Color;
    float  Range;
} Light_t;

typedef struct
{
    vec3_t* Position;
    vec3_t* Rotation;
    vec4_t* Quaternion;
} BoneMatrix_t;

typedef struct
{
    char         Name[32];
    short        Parent;
    char         Dummy;
    BoneMatrix_t* BoneMatrixes;
    char         BoundingBox;
    vec3_t       BoundingVertices[8];
} Bone_t;

typedef struct
{
    char FileName[32];
} Texture_t;

typedef struct
{
    unsigned char Width;
    unsigned char Height;
    unsigned char* Buffer;
} Bitmap_t;

typedef struct
{
    short  Node;
    vec3_t Position;
} Vertex_t;

typedef struct
{
    short  Node;
    vec3_t Normal;
    short  BindVertex;
} Normal_t;

typedef struct
{
    float TexCoordU;
    float TexCoordV;
} TexCoord_t;

typedef struct
{
    BYTE m_Colors[3];	//0~255 RGB
} VertexColor_t;

typedef struct
{
    char       Polygon;
    short      VertexIndex[4];
    short      NormalIndex[4];
    short      TexCoordIndex[4];
    short      EdgeTriangleIndex[4];
    bool       Front;
} Triangle_t;

typedef struct
{
    char       Polygon;
    short      VertexIndex[4];
    short      NormalIndex[4];
    short      TexCoordIndex[4];
    TexCoord_t LightMapCoord[4]; //ver1.2
    short      LightMapIndexes; //ver1.2
} Triangle_t2;

typedef struct
{
    bool          Loop;
    float         PlaySpeed;
    short         NumAnimationKeys;
    bool          LockPositions;
    vec3_t* Positions;
} Action_t;

typedef struct _Triangle_t3 : public Triangle_t
{
    short	   m_ivIndexAdditional[4];
} Triangle_t3;

typedef struct _Mesh_t
{
    bool          NoneBlendMesh;
    short         Texture;
    short         NumVertices;
    short         NumNormals;
    short         NumTexCoords;
    short		  NumVertexColors;	//ver1.3
    short         NumTriangles;
    int           NumCommandBytes; //ver1.1
    Vertex_t* Vertices;
    Normal_t* Normals;
    TexCoord_t* TexCoords;
    VertexColor_t* VertexColors;	//ver1.3
    Triangle_t* Triangles;
    unsigned char* Commands; //ver1.1

    TextureScript* m_csTScript;

    _Mesh_t()
    {
        Vertices = NULL;
        Normals = NULL;
        Triangles = NULL;
        Commands = NULL;
        m_csTScript = NULL;

        NumVertices = NumNormals = NumTexCoords =
            NumVertexColors = NumTriangles = 0;
    }
} Mesh_t;

class BMD
{
public:
    char          Name[64];
    char          Version;
    short         NumBones;
    short         NumMeshs;
    short         NumActions;
    Mesh_t* Meshs;
    Bone_t* Bones;
    Action_t* Actions;
    Texture_t* Textures;
    GLuint* IndexTexture;

    short         NumLightMaps;  //ver1.2
    short         IndexLightMap; //ver1.2
    Bitmap_t* LightMaps;    //ver1.2

    // Etapa 3b: the per-render fields below were MIGRATED to the per-worker
    // Render::Build::BmdRenderContext (CurrentRenderCtx()); render code no longer reads/writes
    // them on the shared BMD. They are RETAINED as RESERVED LAYOUT PADDING because
    // sizeof(BMD)/the member layout MUST stay byte-identical to the original: Models[] is
    // allocated as `ModelsDump + rand()%1024` with ZeroMemory(Models, MAX_MODELS*sizeof(BMD))
    // (ZzzOpenData.cpp:100-102), and a pre-existing heap overflow ("World74\") lands harmlessly
    // only at the original layout — shrinking sizeof(BMD) shifts the array base onto Models[]
    // and corrupts it (crash-login). DO NOT delete or reorder these. Do not read/write them in
    // render code — use CurrentRenderCtx().<field>.
    bool          LightEnable;     // reserved (layout) -> ctx.lightEnable
    bool          ContrastEnable;  // reserved (layout) -> ctx.contrastEnable
    vec3_t        BodyLight;       // reserved (layout) -> ctx.bodyLight
    int           BoneHead;

    int           BoneFoot[4];
    float         BodyScale;       // reserved (layout) -> ctx.bodyScale
    vec3_t        BodyOrigin;      // reserved (layout) -> ctx.bodyOrigin
    vec3_t        BodyAngle;
    float         BodyHeight;      // reserved (layout) -> ctx.bodyHeight
    char          StreamMesh;
    vec3_t        ShadowAngle;     // reserved (layout) -> ctx.shadowAngle
    char          Skin;
    bool          HideSkin;
    float         Velocity;
    unsigned short CurrentAction;
    unsigned short PriorAction;
    float         CurrentAnimation;
    short         CurrentAnimationFrame;
    short         Sounds[MAX_MONSTER_SOUND];
    int           renderCount;
    float		  fTransformedSize;

    unsigned int		m_iBMDSeqID;
    bool				bLightMap;
    bool				bOffLight;
    char				iBillType;

    bool				m_bCompletedAlloc;

    BMD() : NumBones(0), NumActions(0), NumMeshs(0),
        Meshs(NULL), Bones(NULL), Actions(NULL), Textures(NULL), IndexTexture(NULL)
    {
        HideSkin = false;
        bLightMap = false;
        iBillType = -1;
        bOffLight = false;
        m_bCompletedAlloc = false;
    }

    ~BMD();
    //utility
    void Init(bool Dummy);
    bool Open2(const wchar_t* DirName, const wchar_t* FileName, bool bReAlloc = true);
    bool Save2(wchar_t* DirName, wchar_t* FileName);
    void Release();
    void CreateBoundingBox();

    bool PlayAnimation(float* AnimationFrame, float* PriorAnimationFrame, unsigned short* PriorAction, float Speed, vec3_t Origin, vec3_t Angle);
    void Animation(float(*BoneTransform)[3][4], float AnimationFrame, float PriorAnimationFrame, unsigned short PriorAction, vec3_t Angle, vec3_t HeadAngle, bool Parent = false, bool Translate = true);
    void InterpolationTrans(float(*Mat1)[4], float(*TransMat2)[4], float _Scale);
    void Transform(float(*BoneMatrix)[3][4], vec3_t BoundingBoxMin, vec3_t BoundingBoxMax, OBB_t* OBB, bool Translate = false, float _Scale = 0.0f);
    // P-bmd-skinskip: per-mesh CPU skin (writes VertexTransform/NormalTransform/
    // IntensityTransform for one mesh). Extracted from Transform so it can run lazily.
    void SkinMesh(int meshIndex, float(*BoneMatrix)[3][4], bool Translate, float _Scale,
                  const float* LightPosition, float* BoundingMin, float* BoundingMax) const;
    // P-bmd-skinskip: skin one mesh on demand if Transform deferred it this frame
    // (using the context Transform recorded). No-op if already skinned for this serial.
    // const: writes only the global skin arrays + file statics, never *this.
    void EnsureMeshSkinned(int meshIndex) const;
    // P-bmd-skinskip: mark a mesh as already skinned this frame so EnsureMeshSkinned
    // won't recompute/overwrite it. Used by cloth, which writes its own simulated
    // positions into VertexTransform[mesh] and must not be re-skinned from bones.
    void MarkMeshSkinned(int meshIndex) const;
    void TransformByObjectBone(vec3_t vResultPosition, OBJECT* pObject, int iBoneNumber, vec3_t vRelativePosition = NULL);
    void TransformByBoneMatrix(vec3_t vResultPosition, float(*BoneMatrix)[4], vec3_t vWorldPosition = NULL, vec3_t vRelativePosition = NULL);
    void TransformPosition(float(*Matrix)[4], vec3_t Position, vec3_t WorldPosition, bool Translate = false);
    void RotationPosition(float(*Matrix)[4], vec3_t Position, vec3_t WorldPosition);

public:
    void AnimationTransformWithAttachHighModel_usingGlobalTM(
        OBJECT* oHighHierarchyModel,
        BMD* bmdHighHierarchyModel,
        int iBoneNumberHighHierarchyModel,
        vec3_t& vOutPosHighHiearachyModelBone,
        vec3_t* arrOutSetfAllBonePositions,
        bool bApplyTMtoVertices);

    void AnimationTransformWithAttachHighModel(
        OBJECT* oHighHierarchyModel,
        BMD* bmdHighHierarchyModel,
        int iBoneNumberHighHierarchyModel,
        vec3_t& vOutPosHighHiearachyModelBone,
        vec3_t* arrOutSetfAllBonePositions);

    void AnimationTransformOnlySelf(vec3_t* arrOutSetfAllBonePositions,
        const OBJECT* oSelf);

    void AnimationTransformOnlySelf(vec3_t* arrOutSetfAllBonePositions,
        const vec3_t& v3Angle,
        const vec3_t& v3Position,
        const float& fScale,
        OBJECT* oRefAnimation = NULL,
        const float fFrameArea = -1.0f,
        const float fWeight = -1.0f);

    void Lighting(float*, Light_t*, vec3_t, vec3_t);
    void Chrome(float*, int, vec3_t);

    //render
    void RenderBone(float(*BoneTransform)[3][4]);
    void RenderObjectBoundingBox();
    void BeginRender(float);
    void EndRender();

    void RenderMeshEffect(int i, int iType, int iSubType = 0, vec3_t Angle = 0, VOID* obj = NULL);

    void RenderMesh(int meshIndex, int renderFlags, float alpha = 1.f, int blendMeshIndex = -1, float blendMeshAlpha = 1.f, float blendMeshTextureCoordU = 0.f, float blendMeshTextureCoordV = 0.f, int textureIndex = -1);
    void BeginRenderCoinHeap();
    int AddToCoinHeap(int coinIndex, int target_vertex_index);
    void EndRenderCoinHeap(int coinCount);

    void RenderMeshAlternative(int iRndExtFlag, int iParam, int i, int RenderFlag, float Alpha = 1.f, int BlendMesh = -1, float BlendMeshLight = 1.f, float BlendMeshTexCoordU = 0.f, float BlendMeshTexCoordV = 0.f, int Texture = -1);
    void RenderBody(int RenderFlag, float Alpha = 1.f, int BlendMesh = -1, float BlendMeshLight = 1.f, float BlendMeshTexCoordU = 0.f, float BlendMeshTexCoordV = 0.f, int HiddenMesh = -1, int Texture = -1);
    void RenderBodyAlternative(int iRndExtFlag, int iParam, int RenderFlag, float Alpha = 1.f, int BlendMesh = -1, float BlendMeshLight = 1.f, float BlendMeshTexCoordU = 0.f, float BlendMeshTexCoordV = 0.f, int HiddenMesh = -1, int Texture = -1);
    void RenderMeshTranslate(int i, int RenderFlag, float Alpha = 1.f, int BlendMesh = -1, float BlendMeshLight = 1.f, float BlendMeshTexCoordU = 0.f, float BlendMeshTexCoordV = 0.f, int Texture = -1);
    void RenderBodyTranslate(int RenderFlag, float Alpha = 1.f, int BlendMesh = -1, float BlendMeshLight = 1.f, float BlendMeshTexCoordU = 0.f, float BlendMeshTexCoordV = 0.f, int HiddenMesh = -1, int Texture = -1);
    void RenderBodyShadow(int blendMesh = -1, int hiddenMesh = -1, int startMeshNumber = -1, int endMeshNumber = -1, void* pClothes = nullptr, int clothesCount = 0);

    // Etapa 3b 6.3: SetBodyLight removed — BodyLight is now per-worker ctx.bodyLight; its
    // sole caller (ZzzEffect) writes VectorCopy(o->Light, CurrentRenderCtx().bodyLight) directly.

    bool LightMapEnable;
    bool CollisionDetectLineToMesh(vec3_t, vec3_t, bool Collision = true, int Mesh = -1, int Triangle = -1);
    void CreateLightMapSurface(Light_t*, Mesh_t*, int, int, int, int, int, int, vec3_t, vec3_t, int);
    void CreateLightMaps();
    void BindLightMaps();
    void ReleaseLightMaps();

    //#ifdef USE_SHADOWVOLUME
    void FindNearTriangle(void);

    void FindTriangleForEdge(int iMesh, int iTri, int iIndex11);
    //#endif //USE_SHADOWVOLUME
private:
    BMD(const BMD& b);

    // P-bmd-gpu: GPU skinning path. RenderMeshGpu draws one mesh via the BmdShader
    // + cached VBO, reusing the texture/blend state RenderMesh already set. Returns
    // false (untouched state) if the shader isn't ready, so the caller falls back to
    // legacy. The Transform context it needs (bone matrix ptr, Translate, scale) is
    // kept in file-scope statics in ZzzBMD.cpp -- NOT members -- so this feature does
    // not change sizeof(BMD) (the Models[] array layout must stay byte-identical).
    bool RenderMeshGpu(int meshIndex, const Render::Models::MeshGpu* gpu, float alpha, bool lit);

    // P-bmd-instance: global light dir for LIT instanced meshes (member: reads/writes
    // this->ShadowAngle like RenderMeshGpu's lit branch, minus the HighLight case).
    void ComputeInstLitLight(vec3_t out);

    void AddClothesShadowTriangles(void* pClothes, int clothesCount, float sx, float sy) const;
    void AddMeshShadowTriangles(int blendMesh, int hiddenMesh, int startMesh, int endMesh, float sx, float sy) const;
};

extern BMD* Models;
extern BMD* ModelsDump;
// Task 3: the file-global `BoneTransform[MAX_BONES][3][4]` scratch was renamed to
// `g_BoneTransformScratch` (macro -> per-worker Render::Build::WorkerArena::boneScratch).
// Do NOT confuse with OBJECT::BoneTransform (the per-entity durable palette pointer).
// VertexTransform/NormalTransform/LightTransform/IntensityTransform/g_chrome moved
// to the per-worker Render::Build::WorkerArena (Task 2). The accessor macros (incl.
// g_BoneTransformScratch, Task 3) live in WorkerArena.h, pulled in here AFTER MAX_BONES/
// MAX_MESH/MAX_VERTICES are defined so its drift static_asserts fire. Every TU that uses
// the renamed bare globals already includes ZzzBMD.h, so this single include covers them.
#include "Render/Build/WorkerArena.h"

#endif
