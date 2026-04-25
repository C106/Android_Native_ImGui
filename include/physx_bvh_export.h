#pragma once
#include "mem_struct.h"
#include <filesystem>
#include <vector>
#include <cstdint>

// PhysX BVH 导出结构（基于 UE4 4.18 / PhysX 3.4）

namespace PhysXBVH {

// PhysX 3.4 的 RTreePage，和 IDA / live 验证布局一致
struct RTreePage {
    float minx[4];
    float miny[4];
    float minz[4];
    float maxx[4];
    float maxy[4];
    float maxz[4];
    uint32_t ptrs[4];
};

struct RTreeData {
    float boundsMin[4];
    float boundsMax[4];
    float invDiagonal[4];
    float diagonalScaler[4];
    uint32_t pageSize;
    uint32_t numRootPages;
    uint32_t numLevels;
    uint32_t totalNodes;
    uint32_t totalPages;
    uint32_t flags;
    uint64_t pagesPtr;
    std::vector<RTreePage> pages;
    bool valid;
};

// BVH Mesh 导出数据
// 注意：这里导出的是 TriangleMesh 原始局部空间数据，不包含 shape 上的 PxMeshScale。
// 运行时若需要还原带旋转的非均匀缩放，必须按 PhysX 语义应用 R^-1 * S * R。
struct ExportedBVHMesh {
    std::vector<Vec3> vertices;
    std::vector<uint32_t> indices;  // 三角形索引（每3个为一个三角形）
    RTreeData rtree;
    uint64_t sourceAddr;  // 原始 PxTriangleMesh 地址
    uint32_t vertexCount;
    uint32_t triangleCount;
    uint8_t meshFlags;
    bool uses16BitIndices;
    bool valid;
};

// 从运行时内存导出 BVH mesh
bool ExportBVHMeshFromMemory(uint64_t pxTriangleMeshAddr, ExportedBVHMesh& outMesh);
bool ExportBVHTextFile(const std::filesystem::path& path, const ExportedBVHMesh& mesh);

} // namespace PhysXBVH
