#include "visibility_scene.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <shared_mutex>
#include <utility>

#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace {

constexpr size_t kTrianglesPerChunk = 32;
constexpr float kBoundsInflate = 1.0f;
constexpr float kRayEpsilon = 1e-4f;

static VisibilityBounds MakeEmptyBounds() {
    return {
        {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()},
        {-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()}
    };
}

static bool IsBoundsValid(const VisibilityBounds& bounds) {
    return bounds.Min.X <= bounds.Max.X &&
           bounds.Min.Y <= bounds.Max.Y &&
           bounds.Min.Z <= bounds.Max.Z;
}

static VisibilityBounds MakeSegmentBounds(const Vec3& start, const Vec3& end) {
    return {
        {
            std::min(start.X, end.X) - kBoundsInflate,
            std::min(start.Y, end.Y) - kBoundsInflate,
            std::min(start.Z, end.Z) - kBoundsInflate,
        },
        {
            std::max(start.X, end.X) + kBoundsInflate,
            std::max(start.Y, end.Y) + kBoundsInflate,
            std::max(start.Z, end.Z) + kBoundsInflate,
        }
    };
}

static bool BoundsOverlapScalar(const VisibilityBounds& a, const VisibilityBounds& b) {
    return a.Min.X <= b.Max.X && a.Max.X >= b.Min.X &&
           a.Min.Y <= b.Max.Y && a.Max.Y >= b.Min.Y &&
           a.Min.Z <= b.Max.Z && a.Max.Z >= b.Min.Z;
}

static bool BoundsLaneOverlaps(const VisibilityRaycastQuery& query,
                               const float* minX, const float* minY, const float* minZ,
                               const float* maxX, const float* maxY, const float* maxZ,
                               size_t index) {
    return minX[index] <= query.SegmentBounds.Max.X && maxX[index] >= query.SegmentBounds.Min.X &&
           minY[index] <= query.SegmentBounds.Max.Y && maxY[index] >= query.SegmentBounds.Min.Y &&
           minZ[index] <= query.SegmentBounds.Max.Z && maxZ[index] >= query.SegmentBounds.Min.Z;
}

#if defined(__ARM_NEON) || defined(__aarch64__)
static uint32_t BoundsOverlapMask4(const VisibilityRaycastQuery& query,
                                   const float* minX, const float* minY, const float* minZ,
                                   const float* maxX, const float* maxY, const float* maxZ) {
    const float32x4_t queryMinX = vdupq_n_f32(query.SegmentBounds.Min.X);
    const float32x4_t queryMinY = vdupq_n_f32(query.SegmentBounds.Min.Y);
    const float32x4_t queryMinZ = vdupq_n_f32(query.SegmentBounds.Min.Z);
    const float32x4_t queryMaxX = vdupq_n_f32(query.SegmentBounds.Max.X);
    const float32x4_t queryMaxY = vdupq_n_f32(query.SegmentBounds.Max.Y);
    const float32x4_t queryMaxZ = vdupq_n_f32(query.SegmentBounds.Max.Z);

    uint32x4_t overlap = vcleq_f32(vld1q_f32(minX), queryMaxX);
    overlap = vandq_u32(overlap, vcgeq_f32(vld1q_f32(maxX), queryMinX));
    overlap = vandq_u32(overlap, vcleq_f32(vld1q_f32(minY), queryMaxY));
    overlap = vandq_u32(overlap, vcgeq_f32(vld1q_f32(maxY), queryMinY));
    overlap = vandq_u32(overlap, vcleq_f32(vld1q_f32(minZ), queryMaxZ));
    overlap = vandq_u32(overlap, vcgeq_f32(vld1q_f32(maxZ), queryMinZ));

    return ((vgetq_lane_u32(overlap, 0) != 0u) ? 1u : 0u) |
           ((vgetq_lane_u32(overlap, 1) != 0u) ? 2u : 0u) |
           ((vgetq_lane_u32(overlap, 2) != 0u) ? 4u : 0u) |
           ((vgetq_lane_u32(overlap, 3) != 0u) ? 8u : 0u);
}
#endif

static Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {
        a.Y * b.Z - a.Z * b.Y,
        a.Z * b.X - a.X * b.Z,
        a.X * b.Y - a.Y * b.X
    };
}

static float Dot(const Vec3& a, const Vec3& b) {
    return Vec3::Dot(a, b);
}

static bool IntersectTriangle(const VisibilityRaycastQuery& query,
                              float v0x, float v0y, float v0z,
                              float e1x, float e1y, float e1z,
                              float e2x, float e2y, float e2z,
                              float& outDistance) {
    const Vec3 v0{v0x, v0y, v0z};
    const Vec3 edge1{e1x, e1y, e1z};
    const Vec3 edge2{e2x, e2y, e2z};

    const Vec3 pvec = Cross(query.Direction, edge2);
    const float det = Dot(edge1, pvec);
    if (std::fabs(det) < kRayEpsilon) return false;

    const float invDet = 1.0f / det;
    const Vec3 tvec = query.Origin - v0;
    const float u = Dot(tvec, pvec) * invDet;
    if (u < 0.0f || u > 1.0f) return false;

    const Vec3 qvec = Cross(tvec, edge1);
    const float v = Dot(query.Direction, qvec) * invDet;
    if (v < 0.0f || (u + v) > 1.0f) return false;

    const float t = Dot(edge2, qvec) * invDet;
    if (t < 0.0f || t > 1.0f) return false;

    outDistance = t * query.RayLength;
    return true;
}

static void ReserveTriangleBlock(VisibilityScene::TriangleBlock& block, size_t count) {
    block.V0X.reserve(count);
    block.V0Y.reserve(count);
    block.V0Z.reserve(count);
    block.Edge1X.reserve(count);
    block.Edge1Y.reserve(count);
    block.Edge1Z.reserve(count);
    block.Edge2X.reserve(count);
    block.Edge2Y.reserve(count);
    block.Edge2Z.reserve(count);
    block.MinX.reserve(count);
    block.MinY.reserve(count);
    block.MinZ.reserve(count);
    block.MaxX.reserve(count);
    block.MaxY.reserve(count);
    block.MaxZ.reserve(count);
}

static void AppendTriangle(VisibilityScene::SceneMesh& mesh,
                           const Vec3& a, const Vec3& b, const Vec3& c) {
    const Vec3 edge1 = b - a;
    const Vec3 edge2 = c - a;

    mesh.Triangles.V0X.push_back(a.X);
    mesh.Triangles.V0Y.push_back(a.Y);
    mesh.Triangles.V0Z.push_back(a.Z);
    mesh.Triangles.Edge1X.push_back(edge1.X);
    mesh.Triangles.Edge1Y.push_back(edge1.Y);
    mesh.Triangles.Edge1Z.push_back(edge1.Z);
    mesh.Triangles.Edge2X.push_back(edge2.X);
    mesh.Triangles.Edge2Y.push_back(edge2.Y);
    mesh.Triangles.Edge2Z.push_back(edge2.Z);

    const float minX = std::min({a.X, b.X, c.X}) - kBoundsInflate;
    const float minY = std::min({a.Y, b.Y, c.Y}) - kBoundsInflate;
    const float minZ = std::min({a.Z, b.Z, c.Z}) - kBoundsInflate;
    const float maxX = std::max({a.X, b.X, c.X}) + kBoundsInflate;
    const float maxY = std::max({a.Y, b.Y, c.Y}) + kBoundsInflate;
    const float maxZ = std::max({a.Z, b.Z, c.Z}) + kBoundsInflate;

    mesh.Triangles.MinX.push_back(minX);
    mesh.Triangles.MinY.push_back(minY);
    mesh.Triangles.MinZ.push_back(minZ);
    mesh.Triangles.MaxX.push_back(maxX);
    mesh.Triangles.MaxY.push_back(maxY);
    mesh.Triangles.MaxZ.push_back(maxZ);

    if (!IsBoundsValid(mesh.Bounds)) {
        mesh.Bounds = {{minX, minY, minZ}, {maxX, maxY, maxZ}};
        return;
    }

    mesh.Bounds.Min.X = std::min(mesh.Bounds.Min.X, minX);
    mesh.Bounds.Min.Y = std::min(mesh.Bounds.Min.Y, minY);
    mesh.Bounds.Min.Z = std::min(mesh.Bounds.Min.Z, minZ);
    mesh.Bounds.Max.X = std::max(mesh.Bounds.Max.X, maxX);
    mesh.Bounds.Max.Y = std::max(mesh.Bounds.Max.Y, maxY);
    mesh.Bounds.Max.Z = std::max(mesh.Bounds.Max.Z, maxZ);
}

static void BuildChunkBounds(VisibilityScene::SceneMesh& mesh) {
    const size_t triangleCount = mesh.Triangles.V0X.size();
    if (triangleCount == 0) return;

    const size_t chunkCount = (triangleCount + kTrianglesPerChunk - 1) / kTrianglesPerChunk;
    mesh.Chunks.reserve(chunkCount);
    mesh.ChunkMinX.reserve(chunkCount);
    mesh.ChunkMinY.reserve(chunkCount);
    mesh.ChunkMinZ.reserve(chunkCount);
    mesh.ChunkMaxX.reserve(chunkCount);
    mesh.ChunkMaxY.reserve(chunkCount);
    mesh.ChunkMaxZ.reserve(chunkCount);

    for (size_t chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex) {
        const size_t begin = chunkIndex * kTrianglesPerChunk;
        const size_t end = std::min(begin + kTrianglesPerChunk, triangleCount);
        float minX = mesh.Triangles.MinX[begin];
        float minY = mesh.Triangles.MinY[begin];
        float minZ = mesh.Triangles.MinZ[begin];
        float maxX = mesh.Triangles.MaxX[begin];
        float maxY = mesh.Triangles.MaxY[begin];
        float maxZ = mesh.Triangles.MaxZ[begin];

        for (size_t i = begin + 1; i < end; ++i) {
            minX = std::min(minX, mesh.Triangles.MinX[i]);
            minY = std::min(minY, mesh.Triangles.MinY[i]);
            minZ = std::min(minZ, mesh.Triangles.MinZ[i]);
            maxX = std::max(maxX, mesh.Triangles.MaxX[i]);
            maxY = std::max(maxY, mesh.Triangles.MaxY[i]);
            maxZ = std::max(maxZ, mesh.Triangles.MaxZ[i]);
        }

        mesh.Chunks.push_back({
            static_cast<uint32_t>(begin),
            static_cast<uint32_t>(end - begin)
        });
        mesh.ChunkMinX.push_back(minX);
        mesh.ChunkMinY.push_back(minY);
        mesh.ChunkMinZ.push_back(minZ);
        mesh.ChunkMaxX.push_back(maxX);
        mesh.ChunkMaxY.push_back(maxY);
        mesh.ChunkMaxZ.push_back(maxZ);
    }
}

}  // namespace

VisibilityScene::SceneMesh VisibilityScene::BuildSceneMesh(const VisibilityMeshData& mesh) {
    SceneMesh sceneMesh{};
    sceneMesh.Key = mesh.Key;
    sceneMesh.Resource = mesh.Resource;
    sceneMesh.Shape = mesh.Shape;
    sceneMesh.GeometryType = mesh.GeometryType;
    sceneMesh.Bounds = MakeEmptyBounds();

    const size_t triangleCount = mesh.Indices.size() / 3;
    ReserveTriangleBlock(sceneMesh.Triangles, triangleCount);

    for (size_t i = 0; i + 2 < mesh.Indices.size(); i += 3) {
        const uint32_t ia = mesh.Indices[i];
        const uint32_t ib = mesh.Indices[i + 1];
        const uint32_t ic = mesh.Indices[i + 2];
        if (ia >= mesh.Vertices.size() || ib >= mesh.Vertices.size() || ic >= mesh.Vertices.size()) continue;
        AppendTriangle(sceneMesh, mesh.Vertices[ia], mesh.Vertices[ib], mesh.Vertices[ic]);
    }

    if (sceneMesh.Triangles.V0X.empty()) {
        sceneMesh.Bounds = {};
        return sceneMesh;
    }

    BuildChunkBounds(sceneMesh);
    return sceneMesh;
}

std::shared_ptr<VisibilityScene::SceneSnapshot> VisibilityScene::BuildSnapshot(
    const std::unordered_map<uint64_t, SceneMesh>& meshes) {
    auto snapshot = std::make_shared<SceneSnapshot>();
    snapshot->Meshes.reserve(meshes.size());
    snapshot->MeshMinX.reserve(meshes.size());
    snapshot->MeshMinY.reserve(meshes.size());
    snapshot->MeshMinZ.reserve(meshes.size());
    snapshot->MeshMaxX.reserve(meshes.size());
    snapshot->MeshMaxY.reserve(meshes.size());
    snapshot->MeshMaxZ.reserve(meshes.size());

    for (const auto& [key, mesh] : meshes) {
        (void)key;
        if (!IsBoundsValid(mesh.Bounds) || mesh.Triangles.V0X.empty()) continue;
        snapshot->MeshMinX.push_back(mesh.Bounds.Min.X);
        snapshot->MeshMinY.push_back(mesh.Bounds.Min.Y);
        snapshot->MeshMinZ.push_back(mesh.Bounds.Min.Z);
        snapshot->MeshMaxX.push_back(mesh.Bounds.Max.X);
        snapshot->MeshMaxY.push_back(mesh.Bounds.Max.Y);
        snapshot->MeshMaxZ.push_back(mesh.Bounds.Max.Z);
        snapshot->Meshes.push_back(mesh);
    }
    return snapshot;
}

void VisibilityScene::UpdateMeshes(const std::vector<VisibilityMeshData>& adds,
                                   const std::vector<uint64_t>& removes) {
    std::vector<SceneMesh> builtAdds;
    builtAdds.reserve(adds.size());
    for (const VisibilityMeshData& mesh : adds) {
        if (mesh.Key == 0 || mesh.Vertices.empty() || mesh.Indices.size() < 3) continue;
        SceneMesh built = BuildSceneMesh(mesh);
        if (IsBoundsValid(built.Bounds) && !built.Triangles.V0X.empty()) {
            builtAdds.push_back(std::move(built));
        }
    }

    std::shared_ptr<SceneSnapshot> nextSnapshot;
    {
        std::unique_lock<std::shared_mutex> lock(Mutex_);
        for (uint64_t key : removes) {
            Meshes_.erase(key);
        }
        for (SceneMesh& mesh : builtAdds) {
            Meshes_[mesh.Key] = std::move(mesh);
        }
        nextSnapshot = BuildSnapshot(Meshes_);
        Snapshot_ = nextSnapshot;
    }
}

void VisibilityScene::Clear() {
    std::unique_lock<std::shared_mutex> lock(Mutex_);
    Meshes_.clear();
    Snapshot_.reset();
}

VisibilityRaycastQuery VisibilityScene::BuildRaycastQuery(const Vec3& origin, const Vec3& target) {
    VisibilityRaycastQuery query{};
    query.Origin = origin;
    query.Target = target;
    query.Direction = target - origin;
    const float rayLengthSq = Vec3::Dot(query.Direction, query.Direction);
    if (rayLengthSq <= kRayEpsilon) return query;
    query.SegmentBounds = MakeSegmentBounds(origin, target);
    query.RayLength = std::sqrt(rayLengthSq);
    query.Valid = true;
    return query;
}

VisibilityRaycastHit VisibilityScene::Raycast(const Vec3& origin, const Vec3& target) const {
    return Raycast(BuildRaycastQuery(origin, target));
}

VisibilityRaycastHit VisibilityScene::Raycast(const VisibilityRaycastQuery& query) const {
    VisibilityRaycastHit hit{};
    if (!query.Valid) return hit;

    std::shared_ptr<SceneSnapshot> snapshot;
    {
        std::shared_lock<std::shared_mutex> lock(Mutex_);
        snapshot = Snapshot_;
    }
    if (!snapshot || snapshot->Meshes.empty()) return hit;

    float nearestDistance = std::numeric_limits<float>::max();
    const size_t meshCount = snapshot->Meshes.size();
    size_t meshIndex = 0;

#if defined(__ARM_NEON) || defined(__aarch64__)
    for (; meshIndex + 4 <= meshCount; meshIndex += 4) {
        const uint32_t mask = BoundsOverlapMask4(query,
                                                 snapshot->MeshMinX.data() + meshIndex,
                                                 snapshot->MeshMinY.data() + meshIndex,
                                                 snapshot->MeshMinZ.data() + meshIndex,
                                                 snapshot->MeshMaxX.data() + meshIndex,
                                                 snapshot->MeshMaxY.data() + meshIndex,
                                                 snapshot->MeshMaxZ.data() + meshIndex);
        if (mask == 0u) continue;

        for (size_t lane = 0; lane < 4; ++lane) {
            if ((mask & (1u << lane)) == 0u) continue;
            const SceneMesh& mesh = snapshot->Meshes[meshIndex + lane];
#else
    for (; meshIndex < meshCount; ++meshIndex) {
        if (!BoundsLaneOverlaps(query,
                                snapshot->MeshMinX.data(),
                                snapshot->MeshMinY.data(),
                                snapshot->MeshMinZ.data(),
                                snapshot->MeshMaxX.data(),
                                snapshot->MeshMaxY.data(),
                                snapshot->MeshMaxZ.data(),
                                meshIndex)) {
            continue;
        }
        const SceneMesh& mesh = snapshot->Meshes[meshIndex];
#endif
            const size_t chunkCount = mesh.Chunks.size();
            size_t chunkIndex = 0;

#if defined(__ARM_NEON) || defined(__aarch64__)
            for (; chunkIndex + 4 <= chunkCount; chunkIndex += 4) {
                const uint32_t chunkMask = BoundsOverlapMask4(query,
                                                              mesh.ChunkMinX.data() + chunkIndex,
                                                              mesh.ChunkMinY.data() + chunkIndex,
                                                              mesh.ChunkMinZ.data() + chunkIndex,
                                                              mesh.ChunkMaxX.data() + chunkIndex,
                                                              mesh.ChunkMaxY.data() + chunkIndex,
                                                              mesh.ChunkMaxZ.data() + chunkIndex);
                if (chunkMask == 0u) continue;

                for (size_t chunkLane = 0; chunkLane < 4; ++chunkLane) {
                    if ((chunkMask & (1u << chunkLane)) == 0u) continue;
                    const ChunkRange& chunk = mesh.Chunks[chunkIndex + chunkLane];
#else
            for (; chunkIndex < chunkCount; ++chunkIndex) {
                if (!BoundsLaneOverlaps(query,
                                        mesh.ChunkMinX.data(),
                                        mesh.ChunkMinY.data(),
                                        mesh.ChunkMinZ.data(),
                                        mesh.ChunkMaxX.data(),
                                        mesh.ChunkMaxY.data(),
                                        mesh.ChunkMaxZ.data(),
                                        chunkIndex)) {
                    continue;
                }
                const ChunkRange& chunk = mesh.Chunks[chunkIndex];
#endif
                    const size_t triangleEnd = static_cast<size_t>(chunk.TriangleOffset) + chunk.TriangleCount;
                    size_t triangleIndex = chunk.TriangleOffset;

#if defined(__ARM_NEON) || defined(__aarch64__)
                    for (; triangleIndex + 4 <= triangleEnd; triangleIndex += 4) {
                        const uint32_t triangleMask = BoundsOverlapMask4(query,
                                                                         mesh.Triangles.MinX.data() + triangleIndex,
                                                                         mesh.Triangles.MinY.data() + triangleIndex,
                                                                         mesh.Triangles.MinZ.data() + triangleIndex,
                                                                         mesh.Triangles.MaxX.data() + triangleIndex,
                                                                         mesh.Triangles.MaxY.data() + triangleIndex,
                                                                         mesh.Triangles.MaxZ.data() + triangleIndex);
                        if (triangleMask == 0u) continue;

                        for (size_t triangleLane = 0; triangleLane < 4; ++triangleLane) {
                            if ((triangleMask & (1u << triangleLane)) == 0u) continue;
                            const size_t tri = triangleIndex + triangleLane;
#else
                    for (; triangleIndex < triangleEnd; ++triangleIndex) {
                        if (!BoundsLaneOverlaps(query,
                                                mesh.Triangles.MinX.data(),
                                                mesh.Triangles.MinY.data(),
                                                mesh.Triangles.MinZ.data(),
                                                mesh.Triangles.MaxX.data(),
                                                mesh.Triangles.MaxY.data(),
                                                mesh.Triangles.MaxZ.data(),
                                                triangleIndex)) {
                            continue;
                        }
                        const size_t tri = triangleIndex;
#endif
                            float distance = 0.0f;
                            if (!IntersectTriangle(query,
                                                   mesh.Triangles.V0X[tri], mesh.Triangles.V0Y[tri], mesh.Triangles.V0Z[tri],
                                                   mesh.Triangles.Edge1X[tri], mesh.Triangles.Edge1Y[tri], mesh.Triangles.Edge1Z[tri],
                                                   mesh.Triangles.Edge2X[tri], mesh.Triangles.Edge2Y[tri], mesh.Triangles.Edge2Z[tri],
                                                   distance)) {
                                continue;
                            }
                            if (distance >= nearestDistance) continue;

                            nearestDistance = distance;
                            hit.Hit = true;
                            hit.Distance = distance;
                            hit.Key = mesh.Key;
                            hit.Resource = mesh.Resource;
                            hit.Shape = mesh.Shape;
                            hit.GeometryType = mesh.GeometryType;
                        }
                    }

                    for (; triangleIndex < triangleEnd; ++triangleIndex) {
                        if (!BoundsLaneOverlaps(query,
                                                mesh.Triangles.MinX.data(),
                                                mesh.Triangles.MinY.data(),
                                                mesh.Triangles.MinZ.data(),
                                                mesh.Triangles.MaxX.data(),
                                                mesh.Triangles.MaxY.data(),
                                                mesh.Triangles.MaxZ.data(),
                                                triangleIndex)) {
                            continue;
                        }

                        float distance = 0.0f;
                        if (!IntersectTriangle(query,
                                               mesh.Triangles.V0X[triangleIndex], mesh.Triangles.V0Y[triangleIndex], mesh.Triangles.V0Z[triangleIndex],
                                               mesh.Triangles.Edge1X[triangleIndex], mesh.Triangles.Edge1Y[triangleIndex], mesh.Triangles.Edge1Z[triangleIndex],
                                               mesh.Triangles.Edge2X[triangleIndex], mesh.Triangles.Edge2Y[triangleIndex], mesh.Triangles.Edge2Z[triangleIndex],
                                               distance)) {
                            continue;
                        }
                        if (distance >= nearestDistance) continue;

                        nearestDistance = distance;
                        hit.Hit = true;
                        hit.Distance = distance;
                        hit.Key = mesh.Key;
                        hit.Resource = mesh.Resource;
                        hit.Shape = mesh.Shape;
                        hit.GeometryType = mesh.GeometryType;
                    }
                }
            }

            for (; chunkIndex < chunkCount; ++chunkIndex) {
                if (!BoundsLaneOverlaps(query,
                                        mesh.ChunkMinX.data(),
                                        mesh.ChunkMinY.data(),
                                        mesh.ChunkMinZ.data(),
                                        mesh.ChunkMaxX.data(),
                                        mesh.ChunkMaxY.data(),
                                        mesh.ChunkMaxZ.data(),
                                        chunkIndex)) {
                    continue;
                }

                const ChunkRange& chunk = mesh.Chunks[chunkIndex];
                const size_t triangleEnd = static_cast<size_t>(chunk.TriangleOffset) + chunk.TriangleCount;
                for (size_t triangleIndex = chunk.TriangleOffset; triangleIndex < triangleEnd; ++triangleIndex) {
                    if (!BoundsLaneOverlaps(query,
                                            mesh.Triangles.MinX.data(),
                                            mesh.Triangles.MinY.data(),
                                            mesh.Triangles.MinZ.data(),
                                            mesh.Triangles.MaxX.data(),
                                            mesh.Triangles.MaxY.data(),
                                            mesh.Triangles.MaxZ.data(),
                                            triangleIndex)) {
                        continue;
                    }

                    float distance = 0.0f;
                    if (!IntersectTriangle(query,
                                           mesh.Triangles.V0X[triangleIndex], mesh.Triangles.V0Y[triangleIndex], mesh.Triangles.V0Z[triangleIndex],
                                           mesh.Triangles.Edge1X[triangleIndex], mesh.Triangles.Edge1Y[triangleIndex], mesh.Triangles.Edge1Z[triangleIndex],
                                           mesh.Triangles.Edge2X[triangleIndex], mesh.Triangles.Edge2Y[triangleIndex], mesh.Triangles.Edge2Z[triangleIndex],
                                           distance)) {
                        continue;
                    }
                    if (distance >= nearestDistance) continue;

                    nearestDistance = distance;
                    hit.Hit = true;
                    hit.Distance = distance;
                    hit.Key = mesh.Key;
                    hit.Resource = mesh.Resource;
                    hit.Shape = mesh.Shape;
                    hit.GeometryType = mesh.GeometryType;
                }
            }
        }
    }

    for (; meshIndex < meshCount; ++meshIndex) {
        if (!BoundsLaneOverlaps(query,
                                snapshot->MeshMinX.data(),
                                snapshot->MeshMinY.data(),
                                snapshot->MeshMinZ.data(),
                                snapshot->MeshMaxX.data(),
                                snapshot->MeshMaxY.data(),
                                snapshot->MeshMaxZ.data(),
                                meshIndex)) {
            continue;
        }

        const SceneMesh& mesh = snapshot->Meshes[meshIndex];
        for (size_t chunkIndex = 0; chunkIndex < mesh.Chunks.size(); ++chunkIndex) {
            if (!BoundsLaneOverlaps(query,
                                    mesh.ChunkMinX.data(),
                                    mesh.ChunkMinY.data(),
                                    mesh.ChunkMinZ.data(),
                                    mesh.ChunkMaxX.data(),
                                    mesh.ChunkMaxY.data(),
                                    mesh.ChunkMaxZ.data(),
                                    chunkIndex)) {
                continue;
            }

            const ChunkRange& chunk = mesh.Chunks[chunkIndex];
            const size_t triangleEnd = static_cast<size_t>(chunk.TriangleOffset) + chunk.TriangleCount;
            for (size_t triangleIndex = chunk.TriangleOffset; triangleIndex < triangleEnd; ++triangleIndex) {
                if (!BoundsLaneOverlaps(query,
                                        mesh.Triangles.MinX.data(),
                                        mesh.Triangles.MinY.data(),
                                        mesh.Triangles.MinZ.data(),
                                        mesh.Triangles.MaxX.data(),
                                        mesh.Triangles.MaxY.data(),
                                        mesh.Triangles.MaxZ.data(),
                                        triangleIndex)) {
                    continue;
                }

                float distance = 0.0f;
                if (!IntersectTriangle(query,
                                       mesh.Triangles.V0X[triangleIndex], mesh.Triangles.V0Y[triangleIndex], mesh.Triangles.V0Z[triangleIndex],
                                       mesh.Triangles.Edge1X[triangleIndex], mesh.Triangles.Edge1Y[triangleIndex], mesh.Triangles.Edge1Z[triangleIndex],
                                       mesh.Triangles.Edge2X[triangleIndex], mesh.Triangles.Edge2Y[triangleIndex], mesh.Triangles.Edge2Z[triangleIndex],
                                       distance)) {
                    continue;
                }
                if (distance >= nearestDistance) continue;

                nearestDistance = distance;
                hit.Hit = true;
                hit.Distance = distance;
                hit.Key = mesh.Key;
                hit.Resource = mesh.Resource;
                hit.Shape = mesh.Shape;
                hit.GeometryType = mesh.GeometryType;
            }
        }
    }

    return hit;
}

size_t VisibilityScene::Size() const {
    std::shared_lock<std::shared_mutex> lock(Mutex_);
    return Snapshot_ ? Snapshot_->Meshes.size() : 0;
}
