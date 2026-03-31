#pragma once
#include <cmath>
#include <cstdint>

struct Vec2
{
    float x;
    float y;
    Vec2()
    {
        this->x = 0;
        this->y = 0;
    }
    Vec2(float x, float y)
    {
        this->x = x;
        this->y = y;
    }
    Vec2 operator+(float v) const
    {
        return Vec2(x + v, y + v);
    }
    Vec2 operator-(float v) const
    {
        return Vec2(x - v, y - v);
    }
    Vec2 operator*(float v) const
    {
        return Vec2(x * v, y * v);
    }
    Vec2 operator/(float v) const
    {
        return Vec2(x / v, y / v);
    }
    Vec2 &operator+=(float v)
    {
        x += v;
        y += v;
        return *this;
    }
    Vec2 &operator-=(float v)
    {
        x -= v;
        y -= v;
        return *this;
    }
    Vec2 &operator*=(float v)
    {
        x *= v;
        y *= v;
        return *this;
    }
    Vec2 &operator/=(float v)
    {
        x /= v;
        y /= v;
        return *this;
    }
    Vec2 operator+(const Vec2 &v) const
    {
        return Vec2(x + v.x, y + v.y);
    }
    Vec2 operator-(const Vec2 &v) const
    {
        return Vec2(x - v.x, y - v.y);
    }
    Vec2 operator*(const Vec2 &v) const
    {
        return Vec2(x * v.x, y * v.y);
    }
    Vec2 operator/(const Vec2 &v) const
    {
        return Vec2(x / v.x, y / v.y);
    }
    Vec2 &operator+=(const Vec2 &v)
    {
        x += v.x;
        y += v.y;
        return *this;
    }
    Vec2 &operator-=(const Vec2 &v)
    {
        x -= v.x;
        y -= v.y;
        return *this;
    }
    Vec2 &operator*=(const Vec2 &v)
    {
        x *= v.x;
        y *= v.y;
        return *this;
    }
    Vec2 &operator/=(const Vec2 &v)
    {
        x /= v.x;
        y /= v.y;
        return *this;
    }
};

struct Vec3
{
    float X;
    float Y;
    float Z;

    Vec3()
    {
        X = Y = Z = 0.0f;
    }

    Vec3(float _x, float _y, float _z)
    {
        X = _x;
        Y = _y;
        Z = _z;
    }

    Vec3 operator+(const Vec3 &v) const
    {
        return {X + v.X, Y + v.Y, Z + v.Z};
    }

    Vec3 operator-(const Vec3 &v) const
    {
        return {X - v.X, Y - v.Y, Z - v.Z};
    }

    bool operator==(const Vec3 &v)
    {
        return X == v.X && Y == v.Y && Z == v.Z;
    }

    bool operator!=(const Vec3 &v)
    {
        return !(X == v.X && Y == v.Y && Z == v.Z);
    }

    static Vec3 Zero()
    {
        return {0.0f, 0.0f, 0.0f};
    }

    static float Dot(Vec3 lhs, Vec3 rhs)
    {
        return (((lhs.X * rhs.X) + (lhs.Y * rhs.Y)) + (lhs.Z * rhs.Z));
    }

    static float Distance(Vec3 a, Vec3 b)
    {
        Vec3 vector = Vec3(a.X - b.X, a.Y - b.Y, a.Z - b.Z);
        return sqrt(((vector.X * vector.X) + (vector.Y * vector.Y)) + (vector.Z * vector.Z));
    }
};



class FRotator
{
public:
    FRotator() :Pitch(0.f), Yaw(0.f), Roll(0.f) {

    }
    FRotator(float _Pitch, float _Yaw, float _Roll) : Pitch(_Pitch), Yaw(_Yaw), Roll(_Roll)
    {

    }
    ~FRotator()
    {

    }
    float Pitch;
    float Yaw;
    float Roll;
    inline FRotator Clamp()
    {

        if (Pitch > 180)
        {
            Pitch -= 360;
        }
        else
        {
            if (Pitch < -180)
            {
                Pitch += 360;
            }
        }
        if (Yaw > 180)
        {
            Yaw -= 360;
        }
        else {
            if (Yaw < -180)
            {
                Yaw += 360;
            }
        }
        if (Pitch > 89)
        {
            Pitch = 89;
        }
        if (Pitch < -89)
        {
            Pitch = -89;
        }
        while (Yaw < 180)
        {
            Yaw += 360;
        }
        while (Yaw > 180)
        {
            Yaw -= 360;
        }
        Roll = 0;
        return FRotator(Pitch, Yaw, Roll);
    }
    inline float Length()
    {
        return sqrtf(Pitch * Pitch + Yaw * Yaw + Roll * Roll);
    }
    FRotator operator+(FRotator v) {
        return FRotator(Pitch + v.Pitch, Yaw + v.Yaw, Roll + v.Roll);
    }
    FRotator operator-(FRotator v) {
        return FRotator(Pitch - v.Pitch, Yaw - v.Yaw, Roll - v.Roll);
    }
};


struct FMatrix {
    float M[4][4];
};

struct D2DVector {
    float X;
    float Y;
    D2DVector() {
        this->X = 0;
        this->Y = 0;
    }
    D2DVector(float x, float y) {
        this->X = x;
        this->Y = y;
    }
};

struct D3DVector {
    float X;
    float Y;
    float Z;
    D3DVector() {
        this->X = 0;
        this->Y = 0;
        this->Z = 0;
    }
    D3DVector(float x, float y, float z) {
        this->X = x;
        this->Y = y;
        this->Z = z;
    }
};

struct D4DVector {
    float X;
    float Y;
    float Z;
    float W;
    D4DVector() {
        this->X = 0;
        this->Y = 0;
        this->Z = 0;
        this->W = 0;
    }
    D4DVector(float x, float y, float z, float w) {
        this->X = x;
        this->Y = y;
        this->Z = z;
        this->W = w;
    }
};
struct FTransform {
    D4DVector Rotation;
    Vec3 Translation;
    float chunk;
    Vec3 Scale3D;
    float chunk1;
};

inline FMatrix BuildViewMatrix(Vec3 Location, FRotator Rotation) {
    float RadPitch = Rotation.Pitch * (M_PI / 180.0f);
    float RadYaw   = Rotation.Yaw   * (M_PI / 180.0f);
    float RadRoll  = Rotation.Roll  * (M_PI / 180.0f);

    float SP = sinf(RadPitch), CP = cosf(RadPitch);
    float SY = sinf(RadYaw),   CY = cosf(RadYaw);
    float SR = sinf(RadRoll),  CR = cosf(RadRoll);

    Vec3 Forward = { CP * CY,                CP * SY,                SP       };
    Vec3 Right   = { SR*SP*CY - CR*SY,       SR*SP*SY + CR*CY,      -SR * CP  };
    Vec3 Up = { -(CR*SP*CY + SR*SY),  -(CR*SP*SY - SR*CY),  CR * CP };
    FMatrix View = {};
    View.M[0][0] = Right.X;   View.M[0][1] = Up.X;   View.M[0][2] = Forward.X;  View.M[0][3] = 0;
    View.M[1][0] = Right.Y;   View.M[1][1] = Up.Y;   View.M[1][2] = Forward.Y;  View.M[1][3] = 0;
    View.M[2][0] = Right.Z;   View.M[2][1] = Up.Z;   View.M[2][2] = Forward.Z;  View.M[2][3] = 0;
    View.M[3][0] = -Right.Dot(Right,Location);
    View.M[3][1] = -Up.Dot(Up,Location);
    View.M[3][2] = -Forward.Dot(Forward,Location);
    View.M[3][3] = 1.0f;
    return View;
}

inline FMatrix BuildProjectionMatrix(float FOVDegrees, float AspectRatio) {
    float T = tanf(FOVDegrees * (M_PI / 360.0f));  // 水平半角

    FMatrix Proj = {};
    Proj.M[0][0] = 1.0f / T;             // 水平
    Proj.M[1][1] = 1.0f / T * AspectRatio;   // 垂直
    Proj.M[2][2] = 0.0f;
    Proj.M[2][3] = 1.0f;
    Proj.M[3][2] = 0.01f;
    return Proj;
}

inline FMatrix MatrixMultiply(const FMatrix& A, const FMatrix& B) {
    FMatrix R = {};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            for (int k = 0; k < 4; k++)
                R.M[i][j] += A.M[i][k] * B.M[k][j];
    return R;
}

inline bool WorldToScreen(Vec3 World, const FMatrix& VP, float SW, float SH, Vec2& Out) {
    float W = World.X * VP.M[0][3] + World.Y * VP.M[1][3] + World.Z * VP.M[2][3] + VP.M[3][3];
    if (W <= 0.0f) return false;  // 只过滤真正在相机背后的点
    float X = World.X * VP.M[0][0] + World.Y * VP.M[1][0] + World.Z * VP.M[2][0] + VP.M[3][0];
    float Y = World.X * VP.M[0][1] + World.Y * VP.M[1][1] + World.Z * VP.M[2][1] + VP.M[3][1];
    Out.x = (SW / 2.0f) + (X / W) * (SW / 2.0f);
    Out.y = (SH / 2.0f) - (Y / W) * (SH / 2.0f);
    return true;
}

// 扩展版：同时输出 NDC 深度分量 Z/W，用于 compute shader 深度缓冲比较
inline bool WorldToScreenDepth(Vec3 World, const FMatrix& VP, float SW, float SH, Vec2& Out, float& outDepth) {
    float W = World.X * VP.M[0][3] + World.Y * VP.M[1][3] + World.Z * VP.M[2][3] + VP.M[3][3];
    if (W <= 0.0f) { outDepth = -1.0f; return false; }
    float X = World.X * VP.M[0][0] + World.Y * VP.M[1][0] + World.Z * VP.M[2][0] + VP.M[3][0];
    float Y = World.X * VP.M[0][1] + World.Y * VP.M[1][1] + World.Z * VP.M[2][1] + VP.M[3][1];
    float Z = World.X * VP.M[0][2] + World.Y * VP.M[1][2] + World.Z * VP.M[2][2] + VP.M[3][2];
    Out.x = (SW / 2.0f) + (X / W) * (SW / 2.0f);
    Out.y = (SH / 2.0f) - (Y / W) * (SH / 2.0f);
    outDepth = Z / W;  // NDC depth，与 compute shader 深度缓冲一致
    return true;
}

inline Vec3 QuatRotateVector(const D4DVector& q, const Vec3& v)
{
    float tx = 2.0f * (q.Y * v.Z - q.Z * v.Y);
    float ty = 2.0f * (q.Z * v.X - q.X * v.Z);
    float tz = 2.0f * (q.X * v.Y - q.Y * v.X);

    return Vec3{
        v.X + q.W * tx + (q.Y * tz - q.Z * ty),
        v.Y + q.W * ty + (q.Z * tx - q.X * tz),
        v.Z + q.W * tz + (q.X * ty - q.Y * tx)
    };
}

inline Vec3 TransformPosition(const FTransform& T, const Vec3& LocalPos)
{
    // 1. Scale
    Vec3 Scaled = {
        LocalPos.X * T.Scale3D.X,
        LocalPos.Y * T.Scale3D.Y,
        LocalPos.Z * T.Scale3D.Z
    };

    // 2. Rotate
    Vec3 Rotated = QuatRotateVector(T.Rotation, Scaled);

    // 3. Translate
    return Vec3{
        Rotated.X + T.Translation.X,
        Rotated.Y + T.Translation.Y,
        Rotated.Z + T.Translation.Z
    };
}

// FTransform 转 4x4 矩阵
inline FMatrix TransformToMatrix(const FTransform& T)
{
    const auto& q = T.Rotation;
    const auto& t = T.Translation;
    const auto& s = T.Scale3D;

    // 四元数转旋转矩阵
    float xx = q.X * q.X, yy = q.Y * q.Y, zz = q.Z * q.Z;
    float xy = q.X * q.Y, xz = q.X * q.Z, yz = q.Y * q.Z;
    float wx = q.W * q.X, wy = q.W * q.Y, wz = q.W * q.Z;

    FMatrix M = {};
    M.M[0][0] = (1.0f - 2.0f * (yy + zz)) * s.X;
    M.M[0][1] = (2.0f * (xy + wz)) * s.X;
    M.M[0][2] = (2.0f * (xz - wy)) * s.X;
    M.M[0][3] = 0.0f;

    M.M[1][0] = (2.0f * (xy - wz)) * s.Y;
    M.M[1][1] = (1.0f - 2.0f * (xx + zz)) * s.Y;
    M.M[1][2] = (2.0f * (yz + wx)) * s.Y;
    M.M[1][3] = 0.0f;

    M.M[2][0] = (2.0f * (xz + wy)) * s.Z;
    M.M[2][1] = (2.0f * (yz - wx)) * s.Z;
    M.M[2][2] = (1.0f - 2.0f * (xx + yy)) * s.Z;
    M.M[2][3] = 0.0f;

    M.M[3][0] = t.X;
    M.M[3][1] = t.Y;
    M.M[3][2] = t.Z;
    M.M[3][3] = 1.0f;

    return M;
}

// 矩阵变换点
inline Vec3 MatrixTransformPosition(const FMatrix& M, const Vec3& P)
{
    return Vec3{
        M.M[0][0] * P.X + M.M[1][0] * P.Y + M.M[2][0] * P.Z + M.M[3][0],
        M.M[0][1] * P.X + M.M[1][1] * P.Y + M.M[2][1] * P.Z + M.M[3][1],
        M.M[0][2] * P.X + M.M[1][2] * P.Y + M.M[2][2] * P.Z + M.M[3][2]
    };
}

struct Offsets{
    uintptr_t Gworld = 0x14988578;
    uintptr_t Gname = 0x146F9F30;
    uintptr_t GUObject = 0x14706480;
    uintptr_t PersistentLevel = 0xB0;
    uintptr_t TArray = 0xA0;
    uintptr_t NetDriver = 0xb8;
    uintptr_t PhysicsScene = 0xD88; //UWorld -> PhysicsScene (verified live via ceserver)
    uintptr_t PhysSceneSceneCount = 0x4; // FPhysScene -> scene count / upper bound
    uintptr_t PhysSceneSceneIndexArray = 0x56; // FPhysScene -> uint16 SceneIndex[SceneType]
    uintptr_t GPhysXSceneMap = 0x1496E918; // libUE4 + offset -> entry array pointer, 0x18 bytes per entry
    uintptr_t GPhysXSceneMapBuckets = 0x1496E950; // inline hash bucket storage for SceneIndex -> PxScene*
    uintptr_t GPhysXSceneMapBucketPtr = 0x1496E958; // libUE4 + offset -> heap bucket pointer override
    uintptr_t GPhysXSceneMapHashSize = 0x1496E960; // libUE4 + offset -> hash bucket count
    uintptr_t NpSceneQueriesSceneQueryManager = 0x2430; // physx::NpSceneQueries -> Sq::SceneQueryManager
    uintptr_t SceneQueryManagerPrunerExtStride = 0x30; // Sq::SceneQueryManager::mPrunerExt[i] stride
    uintptr_t PrunerExtPruner = 0x0; // Sq::PrunerExt -> Sq::Pruner*
    uintptr_t PrunerExtType = 0x28; // Sq::PrunerExt -> PxPruningStructureType::Enum
    uintptr_t AABBPrunerPool = 0x1F90; // Sq::AABBPruner -> Sq::PruningPool
    uintptr_t BucketPrunerPool = 0x1EA0; // Sq::BucketPruner -> Sq::PruningPool
    uintptr_t PruningPoolNbObjects = 0x0; // Sq::PruningPool -> uint32 active object count
    uintptr_t PruningPoolWorldBoxes = 0x8; // Sq::PruningPool -> PxBounds3* world boxes
    uintptr_t PruningPoolObjects = 0x10; // Sq::PruningPool -> PrunerPayload* objects
    uintptr_t ScbBaseControlState = 0x8; // Scb::Base packed control/state flags, scbType in bits 24..27
    uintptr_t ScbBaseStreamPtr = 0x10; // Scb::Base -> buffered stream pointer
    uintptr_t ScbShapeCore = 0x30; // Scb::Shape -> Sc::ShapeCore
    uintptr_t ScbShapeCoreTransform = 0x40; // Scb::Shape -> inline shape2Actor transform
    uintptr_t ScbShapeCoreShapeFlags = 0x64; // Scb::Shape -> inline PxShapeFlags in Sc::ShapeCore
    uintptr_t ScbShapeCoreGeometry = 0x68; // Scb::Shape -> inline Gu::GeometryUnion in Sc::ShapeCore
    uintptr_t ScbRigidStaticActor2World = 0x30; // Scb::RigidStatic -> inline actor2World
    uintptr_t ScbRigidStaticBufferedActor2World = 0xB0; // buffered Sc::StaticCore -> actor2World
    uintptr_t PxSceneActors = 0x2568; // physx::NpScene -> actor pointer array
    uintptr_t PxSceneActorCount = 0x2570; // physx::NpScene -> actor count for the main actor list
    uintptr_t PxSceneFlags = 0x1124; // physx::NpScene -> inline flags when external scene desc flag is not set
    uintptr_t PxSceneFlagsExternal = 0x2408; // physx::NpScene -> external flags storage selected by bit0x4 at 0x2428
    uintptr_t PxSceneFlagsSelector = 0x2428; // physx::NpScene selector flags, bit0x4 chooses external scene flags
    uintptr_t PxActorType = 0x8; // physx actor header -> type id used by NpScene::getActors
    uintptr_t PxActorShapes = 0x28; // physx PxActor -> inline shape pointer or shape pointer array
    uintptr_t PxActorShapeCount = 0x30; // physx PxActor -> uint16 shape count
    uintptr_t PxShapeFlags = 0x38; // physx::NpShape flags, bit0 selects external geometry storage
    uintptr_t PxShapeCorePtr = 0x40; // physx::NpShape -> shape core pointer when geometry is external
    uintptr_t PxShapeLocalPose = 0x70; // physx::NpShape -> inline local pose
    uintptr_t PxShapeLocalPoseExternal = 0x0; // shape core -> local pose when flags bit0x4 is set
    uintptr_t PxShapeSimulationFilterData = 0x60; // physx::NpShape -> inline simulation filter data when flags bit0x8 is not set
    uintptr_t PxShapeCoreSimulationFilterData = 0x1C; // shape core -> simulation filter data when flags bit0x8 is set
    uintptr_t PxShapeShapeFlags = 0x90; // physx::NpShape -> inline PxShapeFlags when flags bit0x40 is not set
    uintptr_t PxShapeCoreShapeFlags = 0x34; // shape core -> PxShapeFlags when flags bit0x40 is set
    uintptr_t PxShapeGeometryInline = 0x98; // physx::NpShape -> inline geometry block (0x30 bytes)
    uintptr_t PxShapeCoreGeometry = 0x38; // shape core -> geometry block (0x30 bytes)
    uintptr_t PxShapeGeometryType = 0x98; // physx::NpShape::getGeometryType reads the first dword of the geometry block
    uintptr_t PxRigidDynamicBodyToWorld = 0xB0; // physx::NpRigidDynamic -> inline body-to-world pose
    uintptr_t PxRigidDynamicFlags = 0x17C; // physx::NpRigidDynamic flags, bit0x200 selects external pose storage
    uintptr_t PxRigidDynamicCorePtr = 0x70; // physx::NpRigidDynamic -> core pointer when pose is external
    uintptr_t PxRigidDynamicCoreBodyToWorld = 0xD0; // dynamic core -> body-to-world pose
    uintptr_t PxRigidStaticGlobalPose = 0x90; // physx::NpRigidStatic -> inline global pose
    uintptr_t PxRigidStaticFlags = 0x68; // physx::NpRigidStatic flags, bit0x40 selects external pose storage
    uintptr_t PxRigidStaticCorePtr = 0x70; // physx::NpRigidStatic -> core pointer when pose is external
    uintptr_t PxRigidStaticCoreGlobalPose = 0xB0; // static core -> global pose
    uintptr_t ServerConnection = 0x88;
    uintptr_t PxTriangleMeshGeometryTriangleMesh = 0x28; // PxTriangleMeshGeometry -> PxTriangleMesh*
    uintptr_t PxTriangleMeshGeometryScale = 0x4; // PxTriangleMeshGeometry -> PxMeshScale (7 dwords)
    uintptr_t PxSphereGeometryRadius = 0x4; // PxSphereGeometry -> float radius
    uintptr_t PxCapsuleGeometryHalfHeight = 0x4; // PxCapsuleGeometry -> float halfHeight
    uintptr_t PxCapsuleGeometryRadius = 0x8; // PxCapsuleGeometry -> float radius
    uintptr_t PxBoxGeometryHalfExtents = 0x4; // PxBoxGeometry -> PxVec3 halfExtents
    uintptr_t PxTriangleMeshVertices = 0x28; // physx::Gu::TriangleMesh -> PxVec3* vertices
    uintptr_t PxTriangleMeshVertexCount = 0x1C; // physx::Gu::TriangleMesh -> uint32 vertex count
    uintptr_t PxTriangleMeshTriangles = 0x30; // physx::Gu::TriangleMesh -> triangle index buffer
    uintptr_t PxTriangleMeshTriangleCount = 0x20; // physx::Gu::TriangleMesh -> uint32 triangle count
    uintptr_t PxTriangleMeshFlags = 0x5C; // physx::Gu::TriangleMesh -> PxTriangleMeshFlags
    uintptr_t PxConvexMeshGeometryConvexMesh = 0x20; // PxConvexMeshGeometry -> PxConvexMesh*
    uintptr_t PxConvexMeshGeometryScale = 0x4; // PxConvexMeshGeometry -> PxMeshScale (7 dwords)
    uintptr_t PxConvexMeshHullData = 0x48; // physx::Gu::ConvexMesh -> hull data base
    uintptr_t PxConvexMeshEdgeCount = 0x44; // physx::Gu::ConvexMesh -> uint16 edge count (low 15 bits)
    uintptr_t PxConvexMeshVertexCount = 0x46; // physx::Gu::ConvexMesh -> uint8 vertex count
    uintptr_t PxConvexMeshPolygonCount = 0x47; // physx::Gu::ConvexMesh -> uint8 polygon count
    uintptr_t PxConvexMeshVertices = 0x48; // physx::Gu::ConvexMesh::getVertices uses hull data base + 20 * polygonCount
    uintptr_t PxConvexMeshIndexBuffer = 0x48; // physx::Gu::ConvexMesh::getIndexBuffer uses hull data base + derived offsets
    uintptr_t PxConvexMeshPolygonData = 0x48; // physx::Gu::ConvexMesh polygon records start at hull data base
    uintptr_t PxConvexMeshPolygonStride = 0x14; // bytes per polygon record
    uintptr_t PxHeightFieldGeometryHeightField = 0x8; // PxHeightFieldGeometry -> PxHeightField*
    uintptr_t PxHeightFieldGeometryHeightScale = 0x10; // PxHeightFieldGeometry -> float heightScale (verified live)
    uintptr_t PxHeightFieldGeometryRowScale = 0x14; // PxHeightFieldGeometry -> float rowScale (verified live)
    uintptr_t PxHeightFieldGeometryColumnScale = 0x18; // PxHeightFieldGeometry -> float columnScale (verified live)
    uintptr_t PxHeightFieldData = 0x20; // physx::Gu::HeightField -> Gu::HeightFieldData
    uintptr_t PxHeightFieldSampleBuffer = 0x50; // physx::Gu::HeightField -> PxHeightFieldSample*
    uintptr_t PxHeightFieldRows = 0x38; // physx::Gu::HeightField::getNbRows() -> ldr w0, [x0,#56]
    uintptr_t PxHeightFieldColumns = 0x3C; // physx::Gu::HeightField::getNbColumns() -> ldr w0, [x0,#60]
    uintptr_t PxHeightFieldThickness = 0x58; // physx::Gu::HeightField::getThickness() -> ldr s0, [x0,#88]
    uintptr_t PxHeightFieldSampleStride = 0x60; // physx::Gu::HeightField -> uint32_t mSampleStride
    uintptr_t PxHeightFieldSampleCount = 0x64; // physx::Gu::HeightField -> uint32_t mNbSamples
    uintptr_t PxHeightFieldModifyCount = 0x70; // physx::Gu::HeightField::getTimestamp() -> uint32_t mModifyCount
    uintptr_t PlayerController = 0x30;
    uintptr_t AcknowledgedPawn = 0x638;  // PlayerController->AcknowledgedPawn
    uintptr_t PlayerCameraManager = 0x658;
    uintptr_t CameraCache = 0x640;
    uintptr_t POV = 0x10;
    uintptr_t RootComponent = 0x268;
    uintptr_t ComponentToWorld = 0x1F0;
    uintptr_t CanvasMap = 0x14954368;
    uintptr_t SkeletalMeshComponent = 0x650;
    uintptr_t MasterPoseComponent = 0x7f8;  // USkinnedMeshComponent -> USkinnedMeshComponent* (from SDK dump)
    uintptr_t ComponentSpaceTransforms = 0x810;   // ComponentSpaceTransformsArray[0], [1] is at +0x10
    uintptr_t CachedComponentSpaceTransforms = 0xbb8;  // TArray<FTransform> (缓存的组件空间变换，单缓冲，更稳定)
    uintptr_t CurrentReadComponentTransformIndex = 0x830; // int32, 0 or 1 — 需要根据 SDK dump 确认
    uintptr_t BoneSpaceTransforms = 0xba8;  // TArray<FTransform> (局部空间骨骼变换)
    uintptr_t SkeletalMesh = 0x7f0;        // USkinnedMeshComponent -> USkeletalMesh*
    uintptr_t RefBoneInfo = 0x238;          // USkeletalMesh -> FReferenceSkeleton.RawRefBoneInfo
    // UAECharacter (Character.Pawn.Actor.Object)
    uintptr_t PlayerName = 0xae8;           // FString PlayerName
    uintptr_t TeamID = 0xb68;              // int TeamID
    uintptr_t bIsWeaponFiring = 0x2598;    // bool (ByteOffset: 0, ByteMask: 1, FieldMask: 255)
    // FApp::DeltaTime (GOT 表，需要两次解引用)
    uintptr_t FAppDeltaTimeGOT = 0x14012220;  // libUE4 + offset -> ptr -> double*
    uintptr_t GFrameCounterGOT = 0x14018260; //uint64 
};
struct Addresses{
    uintptr_t Uworld,libUE4;
    uintptr_t Gname;
    uintptr_t Matrix;
    uintptr_t ProjectionMat,Ulevel,localplay,oneself,Arrayaddr,Objaddr;
    uintptr_t LocalPlayerActor = 0;  // 本地玩家操控的 actor
};
