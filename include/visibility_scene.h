#pragma once

#include "mem_struct.h"
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct VisibilityBounds {
    Vec3 Min{};
    Vec3 Max{};
};

struct VisibilityMeshData {
    uint64_t Key = 0;
    uint64_t Resource = 0;
    uint64_t Shape = 0;
    uint32_t GeometryType = 0;
    VisibilityBounds Bounds{};
    std::vector<Vec3> Vertices;
    std::vector<uint32_t> Indices;
};

struct VisibilityRaycastHit {
    bool Hit = false;
    float Distance = 0.0f;
    uint64_t Key = 0;
    uint64_t Resource = 0;
    uint64_t Shape = 0;
    uint32_t GeometryType = 0;
};

struct VisibilityRaycastQuery {
    Vec3 Origin{};
    Vec3 Target{};
    Vec3 Direction{};
    VisibilityBounds SegmentBounds{};
    float RayLength = 0.0f;
    bool Valid = false;
};

class VisibilityScene {
public:
    // Internal layout types are kept visible for the translation unit implementation.
    void UpdateMeshes(const std::vector<VisibilityMeshData>& adds,
                      const std::vector<uint64_t>& removes);
    void Clear();
    static VisibilityRaycastQuery BuildRaycastQuery(const Vec3& origin, const Vec3& target);
    VisibilityRaycastHit Raycast(const Vec3& origin, const Vec3& target) const;
    VisibilityRaycastHit Raycast(const VisibilityRaycastQuery& query) const;
    bool RaycastAny(const VisibilityRaycastQuery& query) const;
    size_t Size() const;

    struct TriangleBlock {
        std::vector<float> V0X;
        std::vector<float> V0Y;
        std::vector<float> V0Z;
        std::vector<float> Edge1X;
        std::vector<float> Edge1Y;
        std::vector<float> Edge1Z;
        std::vector<float> Edge2X;
        std::vector<float> Edge2Y;
        std::vector<float> Edge2Z;
        std::vector<float> MinX;
        std::vector<float> MinY;
        std::vector<float> MinZ;
        std::vector<float> MaxX;
        std::vector<float> MaxY;
        std::vector<float> MaxZ;
    };

    struct ChunkRange {
        uint32_t TriangleOffset = 0;
        uint32_t TriangleCount = 0;
    };

    struct SceneMesh {
        uint64_t Key = 0;
        uint64_t Resource = 0;
        uint64_t Shape = 0;
        uint32_t GeometryType = 0;
        VisibilityBounds Bounds{};
        TriangleBlock Triangles;
        std::vector<ChunkRange> Chunks;
        std::vector<float> ChunkMinX;
        std::vector<float> ChunkMinY;
        std::vector<float> ChunkMinZ;
        std::vector<float> ChunkMaxX;
        std::vector<float> ChunkMaxY;
        std::vector<float> ChunkMaxZ;
    };

    struct SceneSnapshot {
        std::vector<std::shared_ptr<const SceneMesh>> Meshes;
        std::vector<float> MeshMinX;
        std::vector<float> MeshMinY;
        std::vector<float> MeshMinZ;
        std::vector<float> MeshMaxX;
        std::vector<float> MeshMaxY;
        std::vector<float> MeshMaxZ;
    };

    static SceneMesh BuildSceneMesh(const VisibilityMeshData& mesh);
    static std::shared_ptr<SceneSnapshot> BuildSnapshot(const std::unordered_map<uint64_t, std::shared_ptr<SceneMesh>>& meshes);

    std::shared_ptr<SceneSnapshot> AcquireSnapshot() const;
    bool RaycastAnyWithSnapshot(const VisibilityRaycastQuery& query,
                                const std::shared_ptr<SceneSnapshot>& snapshot) const;

    struct PendingMeshRetry {
        VisibilityMeshData MeshData;
        int RetryCount = 0;
        double LastAttemptTime = 0.0;
    };

private:
    static constexpr int kMaxRetriesPerMesh = 10;
    mutable std::shared_mutex Mutex_;
    std::unordered_map<uint64_t, std::shared_ptr<SceneMesh>> Meshes_;
    std::shared_ptr<SceneSnapshot> Snapshot_;
    std::vector<PendingMeshRetry> PendingRetries_;
    std::unordered_set<uint64_t> PendingKeys_;
};
