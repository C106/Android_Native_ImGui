#include "physx_bvh_export.h"
#include "read_mem.h"
#include <algorithm>
#include <cstring>
#include <fstream>

namespace PhysXBVH {

// PhysX 3.4 内存偏移（基于 IDA 分析）
struct PxTriangleMeshOffsets {
    static constexpr size_t vtable = 0x0;
    static constexpr size_t mNbVertices = 0x1C;
    static constexpr size_t mNbTriangles = 0x20;
    static constexpr size_t mVertices = 0x28;
    static constexpr size_t mTriangles = 0x30;
    static constexpr size_t mFlags = 0x5C;
    static constexpr size_t mBVH = 0xC0; // physx::Gu::RTreeTriangleMesh::mRTree
};

struct RTreeOffsets {
    static constexpr size_t mBoundsMin = 0x00;
    static constexpr size_t mBoundsMax = 0x10;
    static constexpr size_t mInvDiagonal = 0x20;
    static constexpr size_t mDiagonalScaler = 0x30;
    static constexpr size_t mPageSize = 0x40;
    static constexpr size_t mNumRootPages = 0x44;
    static constexpr size_t mNumLevels = 0x48;
    static constexpr size_t mTotalNodes = 0x4C;
    static constexpr size_t mTotalPages = 0x50;
    static constexpr size_t mFlags = 0x54;
    static constexpr size_t mPages = 0x58;
};

static void ResetExportedMesh(ExportedBVHMesh& outMesh) {
    outMesh.vertices.clear();
    outMesh.indices.clear();
    outMesh.rtree.pages.clear();
    std::memset(outMesh.rtree.boundsMin, 0, sizeof(outMesh.rtree.boundsMin));
    std::memset(outMesh.rtree.boundsMax, 0, sizeof(outMesh.rtree.boundsMax));
    std::memset(outMesh.rtree.invDiagonal, 0, sizeof(outMesh.rtree.invDiagonal));
    std::memset(outMesh.rtree.diagonalScaler, 0, sizeof(outMesh.rtree.diagonalScaler));
    outMesh.rtree.pageSize = 0;
    outMesh.rtree.numRootPages = 0;
    outMesh.rtree.numLevels = 0;
    outMesh.rtree.totalNodes = 0;
    outMesh.rtree.totalPages = 0;
    outMesh.rtree.flags = 0;
    outMesh.rtree.pagesPtr = 0;
    outMesh.rtree.valid = false;
    outMesh.vertexCount = 0;
    outMesh.triangleCount = 0;
    outMesh.meshFlags = 0;
    outMesh.uses16BitIndices = false;
    outMesh.valid = false;
}

static bool ReadArray(uint64_t addr, void* dst, size_t size) {
    return addr != 0 && dst != nullptr && size != 0 && Paradise_hook->read(addr, dst, size);
}

bool ExportBVHMeshFromMemory(uint64_t pxTriangleMeshAddr, ExportedBVHMesh& outMesh) {
    ResetExportedMesh(outMesh);
    outMesh.sourceAddr = pxTriangleMeshAddr;

    if (pxTriangleMeshAddr == 0) return false;

    // 读取顶点和三角形数量
    uint32_t nbVertices = Paradise_hook->read<uint32_t>(pxTriangleMeshAddr + PxTriangleMeshOffsets::mNbVertices);
    uint32_t nbTriangles = Paradise_hook->read<uint32_t>(pxTriangleMeshAddr + PxTriangleMeshOffsets::mNbTriangles);
    outMesh.vertexCount = nbVertices;
    outMesh.triangleCount = nbTriangles;

    if (nbVertices == 0 || nbTriangles == 0 || nbVertices > 1000000 || nbTriangles > 1000000) {
        return false;
    }

    // 读取顶点数组指针
    uint64_t verticesPtr = Paradise_hook->read<uint64_t>(pxTriangleMeshAddr + PxTriangleMeshOffsets::mVertices);
    if (verticesPtr == 0) return false;

    // 读取顶点数据
    outMesh.vertices.resize(nbVertices);
    for (uint32_t i = 0; i < nbVertices; ++i) {
        float x = Paradise_hook->read<float>(verticesPtr + i * 12 + 0);
        float y = Paradise_hook->read<float>(verticesPtr + i * 12 + 4);
        float z = Paradise_hook->read<float>(verticesPtr + i * 12 + 8);
        outMesh.vertices[i] = Vec3{x, y, z};
    }

    // 读取三角形索引
    uint64_t trianglesPtr = Paradise_hook->read<uint64_t>(pxTriangleMeshAddr + PxTriangleMeshOffsets::mTriangles);
    if (trianglesPtr == 0) return false;

    outMesh.indices.resize(nbTriangles * 3);

    // 检测索引格式（16位或32位）
    uint8_t flags = Paradise_hook->read<uint8_t>(pxTriangleMeshAddr + PxTriangleMeshOffsets::mFlags);
    const bool has16BitIndices = (flags & 0x2u) != 0;
    outMesh.meshFlags = flags;
    outMesh.uses16BitIndices = has16BitIndices;

    if (has16BitIndices) {
        for (uint32_t i = 0; i < nbTriangles * 3; ++i) {
            outMesh.indices[i] = Paradise_hook->read<uint16_t>(trianglesPtr + i * 2);
        }
    } else {
        for (uint32_t i = 0; i < nbTriangles * 3; ++i) {
            outMesh.indices[i] = Paradise_hook->read<uint32_t>(trianglesPtr + i * 4);
        }
    }

    const uint64_t rtreeAddr = pxTriangleMeshAddr + PxTriangleMeshOffsets::mBVH;
    ReadArray(rtreeAddr + RTreeOffsets::mBoundsMin, outMesh.rtree.boundsMin, sizeof(outMesh.rtree.boundsMin));
    ReadArray(rtreeAddr + RTreeOffsets::mBoundsMax, outMesh.rtree.boundsMax, sizeof(outMesh.rtree.boundsMax));
    ReadArray(rtreeAddr + RTreeOffsets::mInvDiagonal, outMesh.rtree.invDiagonal, sizeof(outMesh.rtree.invDiagonal));
    ReadArray(rtreeAddr + RTreeOffsets::mDiagonalScaler, outMesh.rtree.diagonalScaler, sizeof(outMesh.rtree.diagonalScaler));
    outMesh.rtree.pageSize = Paradise_hook->read<uint32_t>(rtreeAddr + RTreeOffsets::mPageSize);
    outMesh.rtree.numRootPages = Paradise_hook->read<uint32_t>(rtreeAddr + RTreeOffsets::mNumRootPages);
    outMesh.rtree.numLevels = Paradise_hook->read<uint32_t>(rtreeAddr + RTreeOffsets::mNumLevels);
    outMesh.rtree.totalNodes = Paradise_hook->read<uint32_t>(rtreeAddr + RTreeOffsets::mTotalNodes);
    outMesh.rtree.totalPages = Paradise_hook->read<uint32_t>(rtreeAddr + RTreeOffsets::mTotalPages);
    outMesh.rtree.flags = Paradise_hook->read<uint32_t>(rtreeAddr + RTreeOffsets::mFlags);
    outMesh.rtree.pagesPtr = Paradise_hook->read<uint64_t>(rtreeAddr + RTreeOffsets::mPages);

    constexpr uint32_t kExpectedPageSize = 4;
    constexpr uint32_t kMaxExportPages = 1u << 16;
    if (outMesh.rtree.pagesPtr != 0 &&
        outMesh.rtree.totalPages > 0 &&
        outMesh.rtree.totalPages <= kMaxExportPages &&
        outMesh.rtree.pageSize == kExpectedPageSize) {
        outMesh.rtree.pages.resize(outMesh.rtree.totalPages);
        const size_t bytes = static_cast<size_t>(outMesh.rtree.totalPages) * sizeof(RTreePage);
        if (ReadArray(outMesh.rtree.pagesPtr, outMesh.rtree.pages.data(), bytes)) {
            outMesh.rtree.valid = true;
        } else {
            outMesh.rtree.pages.clear();
        }
    }

    outMesh.valid = true;
    return true;
}

bool ExportBVHTextFile(const std::filesystem::path& path, const ExportedBVHMesh& mesh) {
    if (!mesh.valid) return false;

    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;

    out << "# physx triangle mesh + rtree export\n";
    out << "# mesh_space raw_triangle_mesh_local_space\n";
    out << "# mesh_scale_note runtime must apply PhysX PxMeshScale as R^-1 * S * R\n";
    out << "source_addr 0x" << std::hex << mesh.sourceAddr << std::dec << "\n";
    out << "vertex_count " << mesh.vertexCount << "\n";
    out << "triangle_count " << mesh.triangleCount << "\n";
    out << "mesh_flags " << static_cast<uint32_t>(mesh.meshFlags) << "\n";
    out << "index_format " << (mesh.uses16BitIndices ? "u16" : "u32") << "\n";
    out << "rtree_valid " << (mesh.rtree.valid ? 1 : 0) << "\n";
    out << "rtree_page_size " << mesh.rtree.pageSize << "\n";
    out << "rtree_num_root_pages " << mesh.rtree.numRootPages << "\n";
    out << "rtree_num_levels " << mesh.rtree.numLevels << "\n";
    out << "rtree_total_nodes " << mesh.rtree.totalNodes << "\n";
    out << "rtree_total_pages " << mesh.rtree.totalPages << "\n";
    out << "rtree_flags " << mesh.rtree.flags << "\n";
    out << "rtree_pages_ptr 0x" << std::hex << mesh.rtree.pagesPtr << std::dec << "\n";

    auto dump4 = [&](const char* name, const float (&v)[4]) {
        out << name << ' ' << v[0] << ' ' << v[1] << ' ' << v[2] << ' ' << v[3] << "\n";
    };
    dump4("bounds_min", mesh.rtree.boundsMin);
    dump4("bounds_max", mesh.rtree.boundsMax);
    dump4("inv_diagonal", mesh.rtree.invDiagonal);
    dump4("diagonal_scaler", mesh.rtree.diagonalScaler);

    const size_t pageCount = mesh.rtree.pages.size();
    out << "pages_dumped " << pageCount << "\n";
    for (size_t i = 0; i < pageCount; ++i) {
        const RTreePage& page = mesh.rtree.pages[i];
        out << "page " << i << "\n";
        auto dump_page4 = [&](const char* name, const float* v) {
            out << ' ' << name << ' ' << v[0] << ' ' << v[1] << ' ' << v[2] << ' ' << v[3] << "\n";
        };
        dump_page4("minx", page.minx);
        dump_page4("miny", page.miny);
        dump_page4("minz", page.minz);
        dump_page4("maxx", page.maxx);
        dump_page4("maxy", page.maxy);
        dump_page4("maxz", page.maxz);
        out << " ptrs "
            << page.ptrs[0] << ' '
            << page.ptrs[1] << ' '
            << page.ptrs[2] << ' '
            << page.ptrs[3] << "\n";
    }

    return static_cast<bool>(out);
}

} // namespace PhysXBVH
