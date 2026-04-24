#include "draw_objects.h"
#include "read_mem.h"
#include "auto_aim.h"
#include "ImGuiLayer.h"
#include "VulkanApp.h"
#include "driver_manager.h"
#include "game_frame_reader.h"
#include "game_fps_monitor.h"
#include "visibility_scene.h"
#include "visibility_depth_comp_spv.h"
#include "visibility_depth_query_comp_spv.h"
#include "physx_bvh_export.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <future>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <time.h>

#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#endif

// 骨骼连接定义（用于绘制骨架线条）
static const std::pair<int, int> kBoneConnections[] = {
    // 头部到躯干
    {BONE_HEAD, BONE_NECK},
    {BONE_NECK, BONE_CHEST},
    {BONE_CHEST, BONE_PELVIS},

    // 左臂
    {BONE_CHEST, BONE_L_SHOULDER},
    {BONE_L_SHOULDER, BONE_L_ELBOW},
    {BONE_L_ELBOW, BONE_L_HAND},

    // 右臂
    {BONE_CHEST, BONE_R_SHOULDER},
    {BONE_R_SHOULDER, BONE_R_ELBOW},
    {BONE_R_ELBOW, BONE_R_HAND},

    // 左腿
    {BONE_PELVIS, BONE_L_KNEE},
    {BONE_L_KNEE, BONE_L_FOOT},

    // 右腿
    {BONE_PELVIS, BONE_R_KNEE},
    {BONE_R_KNEE, BONE_R_FOOT},
};

bool gShowObjects = true;
bool gShowAllClassNames = false;
bool gUseBatchBoneRead = true;  // 默认使用批量读取（优化模式）
bool gEnableBoneSmoothing = false;  // 默认关闭，避免骨骼视觉上慢半拍
bool gUseCameraCacheVPMatrix = false;  // 默认仍使用原矩阵地址，按需手动切换
int gBoneCount = 0;
float gMaxSkeletonDistance = 230.0f;  // 默认 230 米，超过不绘制骨骼

// 分类显示开关（默认全部显示）
bool gShowPlayers = true;
bool gShowBots = true;
bool gShowNPCs = true;
bool gShowMonsters = true;
bool gShowTombBoxes = true;
bool gShowOtherBoxes = true;
bool gShowEscapeBoxes = true;
bool gShowContainers = true;
bool gShowVehicles = true;
bool gShowOthers = true;

// 绘制模块开关（默认全部启用）
bool gDrawSkeleton = true;   // 绘制骨骼线条
bool gDrawPredictedAimPoint = false;
bool gDrawDistance = true;   // 绘制距离信息
bool gDrawName = true;       // 绘制名称标签
bool gDrawBox = false;       // 绘制包围盒（预留，默认关闭）
bool gDrawPhysXGeometry = false;

bool gPhysXDrawMeshes = true;
bool gPhysXDrawPrimitives = true;
float gPhysXDrawRadiusMeters = 80.0f;
int gPhysXMaxActorsPerFrame = 256;
int gPhysXMaxShapesPerActor = 16;
int gPhysXMaxTrianglesPerMesh = 4000;
float gPhysXCenterRegionFovDegrees = 20.0f;
bool gPhysXManualSceneIndexEnabled = false;
int gPhysXManualSceneIndex = 0;
bool gPhysXUseLocalModelData = false;
bool gPhysXAutoExport = false;          // 自动导出未缓存的模型到磁盘
int gPhysXMaxPrunerObjectsPerFrame = 20000;
int gPhysXMaxPrunerObjectCount = 200000;

// 骨骼可视性判断
bool gUseDepthBufferVisibility = false;  // 默认关闭，需手动开启
float gDepthBufferBias = 0.002f;         // 深度比较偏移（避免 z-fighting）
float gDepthBufferTolerance = 0.005f;    // 深度容差（骨骼 vs 场景）
int gDepthBufferDownscale = 4;           // 降采样倍率（1/4 分辨率）
bool gDrawDepthBuffer = false;           // 调试：直接绘制深度缓冲

bool gDrawMiniMap = false;
float gMiniMapPosX = 36.0f;
float gMiniMapPosY = 140.0f;
float gMiniMapSizePx = 220.0f;
float gMiniMapZoomMeters = 120.0f;

// 骨骼屏幕坐标缓存（主线程整帧发布只读快照，auto-aim 线程无锁读取）
static BoneScreenCacheSnapshot gBoneScreenCacheSnapshot =
    std::make_shared<BoneScreenCache>();
static std::unordered_map<uint64_t, float> gPredictedAimHoverTimes;
extern VulkanApp gApp;

constexpr float kPredictedAimShowDelaySeconds = 0.18f;
constexpr float kPredictedAimCenterRadiusFraction = 0.10f;

// ── 骨骼插值缓存 ──
// 缓存上一帧每个 actor 的骨骼世界坐标，用于 lerp 平滑
struct BoneWorldCache {
    Vec3 positions[BONE_COUNT];
    bool valid[BONE_COUNT];
    uint64_t lastFrameCounter;  // 上次更新时的引擎帧号
    bool initialized;
};
static std::unordered_map<uint64_t, BoneWorldCache> gBoneWorldCache;

// 上一帧引擎帧号（用于检测引擎是否推进了新帧）
static uint64_t gLastEngineFrame = 0;

static void DrawLabelBackground(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, ImU32 fillColor) {
    drawList->AddRectFilled(min, max, fillColor, 6.0f);
}

static void DrawHealthBar(ImDrawList* drawList, const ImVec2& anchor, float width, float height, float ratio) {
    ratio = std::clamp(ratio, 0.0f, 1.0f);
    ImVec2 min(anchor.x - width * 0.5f, anchor.y);
    ImVec2 max(anchor.x + width * 0.5f, anchor.y + height);

    drawList->AddRectFilled(min, max, IM_COL32(0, 0, 0, 150), 3.0f);

    ImVec2 fillMax(min.x + width * ratio, max.y);
    const int r = static_cast<int>((1.0f - ratio) * 255.0f);
    const int g = static_cast<int>(ratio * 220.0f + 35.0f);
    drawList->AddRectFilled(min, fillMax, IM_COL32(r, g, 40, 220), 3.0f);
    drawList->AddRect(min, max, IM_COL32(255, 255, 255, 120), 3.0f);
}

struct PxTransformData;

struct GpuVisibilityBuffer {
    VkBuffer Buffer = VK_NULL_HANDLE;
    VkDeviceMemory Memory = VK_NULL_HANDLE;
    VkDeviceSize Size = 0;
};

struct GpuSceneTriangle {
    float A[4];
    float B[4];
    float C[4];
};

struct GpuSceneCache {
    std::vector<GpuSceneTriangle> Triangles;
    uint64_t Generation = 0;
    int CameraCellX = INT32_MIN;
    int CameraCellY = INT32_MIN;
    int CameraCellZ = INT32_MIN;
    bool Valid = false;
};

static GpuSceneCache gGpuSceneCache;


// ── 深度缓冲 Compute Shader 资源 ──

struct GpuDepthBufferConfig {
    float VP[16];
    float DepthBufferSize[4];  // width, height, triangleCount, depthBias
};

struct GpuDepthQueryConfig {
    float VP[16];
    float DepthBufferSize[4];  // width, height, queryCount, depthTolerance
};

struct GpuBoneQuery {
    float WorldPos[4];  // xyz = world position, w = per-query tolerance
};

// 光栅化 pipeline（三角形 → 深度缓冲）
struct GpuDepthRasterResources {
    VkDevice Device = VK_NULL_HANDLE;
    VkQueue Queue = VK_NULL_HANDLE;
    uint32_t QueueFamily = UINT32_MAX;
    VkDescriptorSetLayout DescriptorSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout PipelineLayout = VK_NULL_HANDLE;
    VkPipeline Pipeline = VK_NULL_HANDLE;
    VkDescriptorPool DescriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet DescriptorSet = VK_NULL_HANDLE;
    VkCommandPool CommandPool = VK_NULL_HANDLE;
    VkCommandBuffer CommandBuffer = VK_NULL_HANDLE;
    VkFence Fence = VK_NULL_HANDLE;
    GpuVisibilityBuffer TriangleBuffer;
    GpuVisibilityBuffer ConfigBuffer;
    GpuVisibilityBuffer DepthBuffer;
    uint32_t TriangleCapacity = 0;
    uint32_t DepthBufferWidth = 0;
    uint32_t DepthBufferHeight = 0;
    bool Ready = false;
};

// 查询 pipeline（骨骼 → 可视性结果）
struct GpuDepthQueryResources {
    VkDevice Device = VK_NULL_HANDLE;
    VkQueue Queue = VK_NULL_HANDLE;
    uint32_t QueueFamily = UINT32_MAX;
    VkDescriptorSetLayout DescriptorSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout PipelineLayout = VK_NULL_HANDLE;
    VkPipeline Pipeline = VK_NULL_HANDLE;
    VkDescriptorPool DescriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet DescriptorSet = VK_NULL_HANDLE;
    VkCommandPool CommandPool = VK_NULL_HANDLE;
    VkCommandBuffer CommandBuffer = VK_NULL_HANDLE;
    VkFence Fence = VK_NULL_HANDLE;
    GpuVisibilityBuffer QueryBuffer;
    GpuVisibilityBuffer ConfigBuffer;
    // DepthBuffer 共享自 GpuDepthRasterResources（只读）
    GpuVisibilityBuffer ResultBuffer;
    uint32_t QueryCapacity = 0;
    bool Ready = false;
};

static GpuDepthRasterResources gGpuDepthRaster;
static GpuDepthQueryResources gGpuDepthQuery;

// 深度缓冲诊断
struct DepthBufferDiag {
    bool rasterPipelineReady = false;
    bool queryPipelineReady = false;
    bool ranThisFrame = false;
    int depthBufferWidth = 0;
    int depthBufferHeight = 0;
    int triangleCount = 0;
    int queryCount = 0;
    int visibleCount = 0;
    int occludedCount = 0;
    int nonZeroPixels = 0;       // 深度缓冲中非零像素数（光栅 pass 写入的像素）
    float minDepth = 0.0f;       // 非零像素的最小深度（Reversed-Z: 最远）
    float maxDepth = 0.0f;       // 非零像素的最大深度（Reversed-Z: 最近）
    const char* failReason = nullptr;
};
static DepthBufferDiag gDepthDiag;

// Forward declaration needed by depth Reset functions
static void DestroyGpuVisibilityBuffer(VkDevice device, GpuVisibilityBuffer& buffer);

static void ResetGpuDepthRasterResources() {
    if (gGpuDepthRaster.Device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(gGpuDepthRaster.Device);
        DestroyGpuVisibilityBuffer(gGpuDepthRaster.Device, gGpuDepthRaster.TriangleBuffer);
        DestroyGpuVisibilityBuffer(gGpuDepthRaster.Device, gGpuDepthRaster.ConfigBuffer);
        DestroyGpuVisibilityBuffer(gGpuDepthRaster.Device, gGpuDepthRaster.DepthBuffer);
        if (gGpuDepthRaster.Fence != VK_NULL_HANDLE) vkDestroyFence(gGpuDepthRaster.Device, gGpuDepthRaster.Fence, nullptr);
        if (gGpuDepthRaster.CommandPool != VK_NULL_HANDLE) vkDestroyCommandPool(gGpuDepthRaster.Device, gGpuDepthRaster.CommandPool, nullptr);
        if (gGpuDepthRaster.DescriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(gGpuDepthRaster.Device, gGpuDepthRaster.DescriptorPool, nullptr);
        if (gGpuDepthRaster.Pipeline != VK_NULL_HANDLE) vkDestroyPipeline(gGpuDepthRaster.Device, gGpuDepthRaster.Pipeline, nullptr);
        if (gGpuDepthRaster.PipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(gGpuDepthRaster.Device, gGpuDepthRaster.PipelineLayout, nullptr);
        if (gGpuDepthRaster.DescriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(gGpuDepthRaster.Device, gGpuDepthRaster.DescriptorSetLayout, nullptr);
    }
    gGpuDepthRaster = {};
}

static void ResetGpuDepthQueryResources() {
    if (gGpuDepthQuery.Device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(gGpuDepthQuery.Device);
        DestroyGpuVisibilityBuffer(gGpuDepthQuery.Device, gGpuDepthQuery.QueryBuffer);
        DestroyGpuVisibilityBuffer(gGpuDepthQuery.Device, gGpuDepthQuery.ConfigBuffer);
        DestroyGpuVisibilityBuffer(gGpuDepthQuery.Device, gGpuDepthQuery.ResultBuffer);
        if (gGpuDepthQuery.Fence != VK_NULL_HANDLE) vkDestroyFence(gGpuDepthQuery.Device, gGpuDepthQuery.Fence, nullptr);
        if (gGpuDepthQuery.CommandPool != VK_NULL_HANDLE) vkDestroyCommandPool(gGpuDepthQuery.Device, gGpuDepthQuery.CommandPool, nullptr);
        if (gGpuDepthQuery.DescriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(gGpuDepthQuery.Device, gGpuDepthQuery.DescriptorPool, nullptr);
        if (gGpuDepthQuery.Pipeline != VK_NULL_HANDLE) vkDestroyPipeline(gGpuDepthQuery.Device, gGpuDepthQuery.Pipeline, nullptr);
        if (gGpuDepthQuery.PipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(gGpuDepthQuery.Device, gGpuDepthQuery.PipelineLayout, nullptr);
        if (gGpuDepthQuery.DescriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(gGpuDepthQuery.Device, gGpuDepthQuery.DescriptorSetLayout, nullptr);
    }
    gGpuDepthQuery = {};
}

static void DestroyGpuVisibilityBuffer(VkDevice device, GpuVisibilityBuffer& buffer) {
    if (device == VK_NULL_HANDLE) return;
    if (buffer.Buffer != VK_NULL_HANDLE) vkDestroyBuffer(device, buffer.Buffer, nullptr);
    if (buffer.Memory != VK_NULL_HANDLE) vkFreeMemory(device, buffer.Memory, nullptr);
    buffer = {};
}



static uint32_t FindMemoryTypeIndex(VkPhysicalDevice physicalDevice, uint32_t typeBits, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        const bool typeMatch = (typeBits & (1u << i)) != 0;
        const bool propertyMatch = (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties;
        if (typeMatch && propertyMatch) return i;
    }
    return UINT32_MAX;
}

static bool CreateGpuVisibilityBuffer(VkDeviceSize size, VkBufferUsageFlags usage, GpuVisibilityBuffer& outBuffer) {
    DestroyGpuVisibilityBuffer(gApp.device, outBuffer);

    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(gApp.device, &bufferInfo, nullptr, &outBuffer.Buffer) != VK_SUCCESS) return false;

    VkMemoryRequirements memoryRequirements{};
    vkGetBufferMemoryRequirements(gApp.device, outBuffer.Buffer, &memoryRequirements);
    const uint32_t memoryTypeIndex = FindMemoryTypeIndex(
        gApp.physicalDevice,
        memoryRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memoryTypeIndex == UINT32_MAX) return false;

    VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.allocationSize = memoryRequirements.size;
    allocInfo.memoryTypeIndex = memoryTypeIndex;
    if (vkAllocateMemory(gApp.device, &allocInfo, nullptr, &outBuffer.Memory) != VK_SUCCESS) return false;
    if (vkBindBufferMemory(gApp.device, outBuffer.Buffer, outBuffer.Memory, 0) != VK_SUCCESS) return false;
    outBuffer.Size = size;
    return true;
}





// ── 深度缓冲 Raster Pipeline ──

static bool EnsureGpuDepthRasterResources() {
    if (gApp.device == VK_NULL_HANDLE || gApp.graphicsQueue == VK_NULL_HANDLE) return false;
    if (gGpuDepthRaster.Ready &&
        gGpuDepthRaster.Device == gApp.device &&
        gGpuDepthRaster.Queue == gApp.graphicsQueue &&
        gGpuDepthRaster.QueueFamily == gApp.graphicsQueueFamily) {
        return true;
    }

    ResetGpuDepthRasterResources();
    gGpuDepthRaster.Device = gApp.device;
    gGpuDepthRaster.Queue = gApp.graphicsQueue;
    gGpuDepthRaster.QueueFamily = gApp.graphicsQueueFamily;

    VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shaderInfo.codeSize = kVisibilityDepthCompSpv_len;
    shaderInfo.pCode = reinterpret_cast<const uint32_t*>(kVisibilityDepthCompSpv);
    VkShaderModule shaderModule = VK_NULL_HANDLE;
    if (vkCreateShaderModule(gApp.device, &shaderInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        ResetGpuDepthRasterResources();
        return false;
    }

    // 3 bindings: TriangleBuffer(0), ConfigBuffer(1), DepthBuffer(2)
    VkDescriptorSetLayoutBinding bindings[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorCount = 1;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 3;
    layoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(gApp.device, &layoutInfo, nullptr, &gGpuDepthRaster.DescriptorSetLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(gApp.device, shaderModule, nullptr);
        ResetGpuDepthRasterResources();
        return false;
    }

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &gGpuDepthRaster.DescriptorSetLayout;
    if (vkCreatePipelineLayout(gApp.device, &pipelineLayoutInfo, nullptr, &gGpuDepthRaster.PipelineLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(gApp.device, shaderModule, nullptr);
        ResetGpuDepthRasterResources();
        return false;
    }

    VkPipelineShaderStageCreateInfo stageInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stageInfo.module = shaderModule;
    stageInfo.pName = "main";
    VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.stage = stageInfo;
    pipelineInfo.layout = gGpuDepthRaster.PipelineLayout;
    if (vkCreateComputePipelines(gApp.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &gGpuDepthRaster.Pipeline) != VK_SUCCESS) {
        vkDestroyShaderModule(gApp.device, shaderModule, nullptr);
        ResetGpuDepthRasterResources();
        return false;
    }
    vkDestroyShaderModule(gApp.device, shaderModule, nullptr);

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(gApp.device, &poolInfo, nullptr, &gGpuDepthRaster.DescriptorPool) != VK_SUCCESS) {
        ResetGpuDepthRasterResources();
        return false;
    }

    VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.descriptorPool = gGpuDepthRaster.DescriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &gGpuDepthRaster.DescriptorSetLayout;
    if (vkAllocateDescriptorSets(gApp.device, &allocInfo, &gGpuDepthRaster.DescriptorSet) != VK_SUCCESS) {
        ResetGpuDepthRasterResources();
        return false;
    }

    VkCommandPoolCreateInfo commandPoolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    commandPoolInfo.queueFamilyIndex = gApp.graphicsQueueFamily;
    commandPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(gApp.device, &commandPoolInfo, nullptr, &gGpuDepthRaster.CommandPool) != VK_SUCCESS) {
        ResetGpuDepthRasterResources();
        return false;
    }

    VkCommandBufferAllocateInfo commandBufferInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandBufferInfo.commandPool = gGpuDepthRaster.CommandPool;
    commandBufferInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandBufferInfo.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(gApp.device, &commandBufferInfo, &gGpuDepthRaster.CommandBuffer) != VK_SUCCESS) {
        ResetGpuDepthRasterResources();
        return false;
    }

    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(gApp.device, &fenceInfo, nullptr, &gGpuDepthRaster.Fence) != VK_SUCCESS) {
        ResetGpuDepthRasterResources();
        return false;
    }

    gGpuDepthRaster.Ready = true;
    return true;
}

static bool EnsureGpuDepthRasterCapacity(uint32_t triangleCount, uint32_t depthW, uint32_t depthH) {
    bool changed = false;
    const uint32_t newTriangleCapacity = std::max(gGpuDepthRaster.TriangleCapacity, std::max(1024u, triangleCount));
    const uint32_t newDepthPixels = depthW * depthH;

    if (newTriangleCapacity != gGpuDepthRaster.TriangleCapacity) {
        if (!CreateGpuVisibilityBuffer(sizeof(GpuSceneTriangle) * newTriangleCapacity,
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                       gGpuDepthRaster.TriangleBuffer)) return false;
        gGpuDepthRaster.TriangleCapacity = newTriangleCapacity;
        changed = true;
    }
    if (gGpuDepthRaster.ConfigBuffer.Buffer == VK_NULL_HANDLE) {
        if (!CreateGpuVisibilityBuffer(sizeof(GpuDepthBufferConfig),
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                       gGpuDepthRaster.ConfigBuffer)) return false;
        changed = true;
    }
    if (depthW != gGpuDepthRaster.DepthBufferWidth || depthH != gGpuDepthRaster.DepthBufferHeight) {
        if (!CreateGpuVisibilityBuffer(sizeof(uint32_t) * newDepthPixels,
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                       gGpuDepthRaster.DepthBuffer)) return false;
        gGpuDepthRaster.DepthBufferWidth = depthW;
        gGpuDepthRaster.DepthBufferHeight = depthH;
        changed = true;
    }
    if (!changed) return true;

    VkDescriptorBufferInfo triangleInfo{gGpuDepthRaster.TriangleBuffer.Buffer, 0,
                                        sizeof(GpuSceneTriangle) * gGpuDepthRaster.TriangleCapacity};
    VkDescriptorBufferInfo configInfo{gGpuDepthRaster.ConfigBuffer.Buffer, 0, sizeof(GpuDepthBufferConfig)};
    VkDescriptorBufferInfo depthInfo{gGpuDepthRaster.DepthBuffer.Buffer, 0,
                                     sizeof(uint32_t) * gGpuDepthRaster.DepthBufferWidth * gGpuDepthRaster.DepthBufferHeight};
    std::array<VkWriteDescriptorSet, 3> writes{};
    for (uint32_t i = 0; i < writes.size(); ++i) writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[0].dstSet = gGpuDepthRaster.DescriptorSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &triangleInfo;
    writes[1].dstSet = gGpuDepthRaster.DescriptorSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &configInfo;
    writes[2].dstSet = gGpuDepthRaster.DescriptorSet;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo = &depthInfo;
    vkUpdateDescriptorSets(gApp.device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    return true;
}

// ── 深度缓冲 Query Pipeline ──

static bool EnsureGpuDepthQueryResources() {
    if (gApp.device == VK_NULL_HANDLE || gApp.graphicsQueue == VK_NULL_HANDLE) return false;
    if (gGpuDepthQuery.Ready &&
        gGpuDepthQuery.Device == gApp.device &&
        gGpuDepthQuery.Queue == gApp.graphicsQueue &&
        gGpuDepthQuery.QueueFamily == gApp.graphicsQueueFamily) {
        return true;
    }

    ResetGpuDepthQueryResources();
    gGpuDepthQuery.Device = gApp.device;
    gGpuDepthQuery.Queue = gApp.graphicsQueue;
    gGpuDepthQuery.QueueFamily = gApp.graphicsQueueFamily;

    VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shaderInfo.codeSize = kVisibilityDepthQueryCompSpv_len;
    shaderInfo.pCode = reinterpret_cast<const uint32_t*>(kVisibilityDepthQueryCompSpv);
    VkShaderModule shaderModule = VK_NULL_HANDLE;
    if (vkCreateShaderModule(gApp.device, &shaderInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        ResetGpuDepthQueryResources();
        return false;
    }

    // 4 bindings: QueryBuffer(0), ConfigBuffer(1), DepthBuffer(2), ResultBuffer(3)
    VkDescriptorSetLayoutBinding bindings[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorCount = 1;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 4;
    layoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(gApp.device, &layoutInfo, nullptr, &gGpuDepthQuery.DescriptorSetLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(gApp.device, shaderModule, nullptr);
        ResetGpuDepthQueryResources();
        return false;
    }

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &gGpuDepthQuery.DescriptorSetLayout;
    if (vkCreatePipelineLayout(gApp.device, &pipelineLayoutInfo, nullptr, &gGpuDepthQuery.PipelineLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(gApp.device, shaderModule, nullptr);
        ResetGpuDepthQueryResources();
        return false;
    }

    VkPipelineShaderStageCreateInfo stageInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stageInfo.module = shaderModule;
    stageInfo.pName = "main";
    VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.stage = stageInfo;
    pipelineInfo.layout = gGpuDepthQuery.PipelineLayout;
    if (vkCreateComputePipelines(gApp.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &gGpuDepthQuery.Pipeline) != VK_SUCCESS) {
        vkDestroyShaderModule(gApp.device, shaderModule, nullptr);
        ResetGpuDepthQueryResources();
        return false;
    }
    vkDestroyShaderModule(gApp.device, shaderModule, nullptr);

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(gApp.device, &poolInfo, nullptr, &gGpuDepthQuery.DescriptorPool) != VK_SUCCESS) {
        ResetGpuDepthQueryResources();
        return false;
    }

    VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.descriptorPool = gGpuDepthQuery.DescriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &gGpuDepthQuery.DescriptorSetLayout;
    if (vkAllocateDescriptorSets(gApp.device, &allocInfo, &gGpuDepthQuery.DescriptorSet) != VK_SUCCESS) {
        ResetGpuDepthQueryResources();
        return false;
    }

    VkCommandPoolCreateInfo commandPoolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    commandPoolInfo.queueFamilyIndex = gApp.graphicsQueueFamily;
    commandPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(gApp.device, &commandPoolInfo, nullptr, &gGpuDepthQuery.CommandPool) != VK_SUCCESS) {
        ResetGpuDepthQueryResources();
        return false;
    }

    VkCommandBufferAllocateInfo commandBufferInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandBufferInfo.commandPool = gGpuDepthQuery.CommandPool;
    commandBufferInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandBufferInfo.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(gApp.device, &commandBufferInfo, &gGpuDepthQuery.CommandBuffer) != VK_SUCCESS) {
        ResetGpuDepthQueryResources();
        return false;
    }

    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(gApp.device, &fenceInfo, nullptr, &gGpuDepthQuery.Fence) != VK_SUCCESS) {
        ResetGpuDepthQueryResources();
        return false;
    }

    gGpuDepthQuery.Ready = true;
    return true;
}

static bool EnsureGpuDepthQueryCapacity(uint32_t queryCount) {
    const uint32_t newQueryCapacity = std::max(gGpuDepthQuery.QueryCapacity, std::max(64u, queryCount));
    bool changed = false;

    if (newQueryCapacity != gGpuDepthQuery.QueryCapacity) {
        if (!CreateGpuVisibilityBuffer(sizeof(GpuBoneQuery) * newQueryCapacity,
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                       gGpuDepthQuery.QueryBuffer)) return false;
        if (!CreateGpuVisibilityBuffer(sizeof(uint32_t) * newQueryCapacity,
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                       gGpuDepthQuery.ResultBuffer)) return false;
        gGpuDepthQuery.QueryCapacity = newQueryCapacity;
        changed = true;
    }
    if (gGpuDepthQuery.ConfigBuffer.Buffer == VK_NULL_HANDLE) {
        if (!CreateGpuVisibilityBuffer(sizeof(GpuDepthQueryConfig),
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                       gGpuDepthQuery.ConfigBuffer)) return false;
        changed = true;
    }
    if (!changed) return true;

    // DepthBuffer 共享自 raster pipeline
    VkDescriptorBufferInfo queryInfo{gGpuDepthQuery.QueryBuffer.Buffer, 0,
                                     sizeof(GpuBoneQuery) * gGpuDepthQuery.QueryCapacity};
    VkDescriptorBufferInfo configInfo{gGpuDepthQuery.ConfigBuffer.Buffer, 0, sizeof(GpuDepthQueryConfig)};
    VkDescriptorBufferInfo depthInfo{gGpuDepthRaster.DepthBuffer.Buffer, 0,
                                     sizeof(uint32_t) * gGpuDepthRaster.DepthBufferWidth * gGpuDepthRaster.DepthBufferHeight};
    VkDescriptorBufferInfo resultInfo{gGpuDepthQuery.ResultBuffer.Buffer, 0,
                                      sizeof(uint32_t) * gGpuDepthQuery.QueryCapacity};
    std::array<VkWriteDescriptorSet, 4> writes{};
    for (uint32_t i = 0; i < writes.size(); ++i) writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[0].dstSet = gGpuDepthQuery.DescriptorSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &queryInfo;
    writes[1].dstSet = gGpuDepthQuery.DescriptorSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &configInfo;
    writes[2].dstSet = gGpuDepthQuery.DescriptorSet;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo = &depthInfo;
    writes[3].dstSet = gGpuDepthQuery.DescriptorSet;
    writes[3].dstBinding = 3;
    writes[3].descriptorCount = 1;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[3].pBufferInfo = &resultInfo;
    vkUpdateDescriptorSets(gApp.device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    return true;
}



enum PhysXGeometryType : uint32_t {
    PX_GEOM_SPHERE = 0,
    PX_GEOM_PLANE = 1,
    PX_GEOM_CAPSULE = 2,
    PX_GEOM_BOX = 3,
    PX_GEOM_CONVEXMESH = 4,
    PX_GEOM_TRIANGLEMESH = 5,
    PX_GEOM_HEIGHTFIELD = 6,
};

enum PhysXActorType : uint16_t {
    PX_ACTOR_RIGID_STATIC = 6,
    PX_ACTOR_RIGID_DYNAMIC = 7,
};

enum PhysXScbType : uint8_t {
    PX_SCB_RIGID_STATIC = 5,
};

enum PhysXShapeFlagBits : uint8_t {
    PX_SHAPE_SIMULATION = 1 << 0,
    PX_SHAPE_SCENE_QUERY = 1 << 1,
    PX_SHAPE_TRIGGER = 1 << 2,
};

struct PxTransformData {
    D4DVector Rotation;
    Vec3 Translation;
};

struct PxMeshScaleData {
    Vec3 Scale;
    D4DVector Rotation;
};

struct PhysXSceneMapEntry {
    uint16_t Key;
    uint16_t Pad0;
    uint32_t Pad1;
    uint64_t ScenePtr;
    int32_t Next;
    int32_t Pad2;
};

struct PhysXConvexPolygonData {
    float Plane[4];
    uint16_t IndexBase;
    uint8_t VertexCount;
    uint8_t MinIndex;
};

struct PhysXHeightFieldSample {
    int16_t Height = 0;
    uint8_t Material0 = 0;
    uint8_t Material1 = 0;
};

struct PhysXBounds3 {
    Vec3 Min;
    Vec3 Max;
};

static constexpr uint8_t kPhysXHeightFieldHoleMaterial = 127u;

static bool HeightFieldTessFlag(const PhysXHeightFieldSample& sample) {
    return (sample.Material0 & 0x80u) != 0;
}

static uint8_t HeightFieldMaterial0(const PhysXHeightFieldSample& sample) {
    return sample.Material0 & 0x7Fu;
}

static uint8_t HeightFieldMaterial1(const PhysXHeightFieldSample& sample) {
    return sample.Material1 & 0x7Fu;
}

struct CachedTriangleMeshData {
    std::vector<Vec3> Vertices;
    std::vector<uint32_t> Indices;
    std::vector<PhysXBounds3> TriangleBounds;  // 每个三角形的未缩放 AABB（空间换时间）
    std::vector<PhysXBounds3> ChunkBounds;     // 每块三角形的未缩放 AABB，用于大 mesh 快速跳块
    PhysXBounds3 MeshAABB{};        // 顶点整体包围盒（未缩放），用于快速拒绝
    double CachedAt = 0.0;          // 缓存时间戳
    double LastUsedAt = 0.0;        // 最近一次命中时间
    double LastVerifiedAt = 0.0;    // 上次抽查时间戳
    uint32_t VertexCount = 0;
    uint32_t TriangleCount = 0;
    uint8_t Flags = 0;
    uint64_t VerticesAddr = 0;      // 远端顶点缓冲地址，用于抽查
    uint64_t TrianglesAddr = 0;     // 远端索引缓冲地址，用于头变更检测
    bool Verified = false;          // 通过抽查验证，可信任
};

struct CachedConvexMeshData {
    std::vector<Vec3> Vertices;
    std::vector<uint32_t> EdgeIndices;
};

struct CachedHeightFieldData {
    uint32_t Rows = 0;
    uint32_t Columns = 0;
    uint32_t SampleStride = 0;
    uint32_t SampleCount = 0;
    uint32_t ModifyCount = 0;
    uint64_t SamplesAddr = 0;
    double LastUsedAt = 0.0;
    std::vector<PhysXHeightFieldSample> Samples;
};

static std::unordered_map<uint64_t, CachedTriangleMeshData> gTriangleMeshCache;
static std::unordered_map<uint64_t, CachedConvexMeshData> gConvexMeshCache;
static std::unordered_map<uint64_t, CachedHeightFieldData> gHeightFieldCache;
static std::unordered_map<uint64_t, uint64_t> gTriangleMeshSignatures;
static std::unordered_map<uint64_t, uint64_t> gHeightFieldSignatures;

// 签名去重缓存：相同内容的 mesh 只存一份
static std::unordered_map<uint64_t, CachedTriangleMeshData> gTriMeshCacheBySig;
static std::unordered_map<uint64_t, CachedHeightFieldData>  gHfCacheBySig;

// 自动导出状态
static std::unordered_set<uint64_t> gAutoExportedSigs;
static int gAutoExportsThisFrame = 0;
static constexpr int kAutoExportBudgetPerFrame = 8;

struct TriangleMeshSourceInfo {
    uint32_t VertexCount = 0;
    uint32_t TriangleCount = 0;
    uint8_t Flags = 0;
    uint64_t VerticesAddr = 0;
    uint64_t TrianglesAddr = 0;
};

struct HeightFieldSourceInfo {
    uint32_t Rows = 0;
    uint32_t Columns = 0;
    uint32_t SampleStride = 0;
    uint32_t SampleCount = 0;
    uint32_t ModifyCount = 0;
    uint64_t SamplesAddr = 0;
    float Thickness = 0.0f;
};

static std::unordered_map<uint64_t, TriangleMeshSourceInfo> gTriangleMeshSources;
static std::unordered_map<uint64_t, HeightFieldSourceInfo> gHeightFieldSources;

struct LocalObjMeshData {
    std::vector<Vec3> Vertices;
    std::vector<uint32_t> Indices;
};

// 本地 OBJ 文件缓存（key = signature），避免每帧重新解析文本文件
static std::unordered_map<uint64_t, LocalObjMeshData> gLocalTriObjCache;
static std::unordered_map<uint64_t, LocalObjMeshData> gLocalHfObjCache;

struct LocalBvhNodeBounds {
    PhysXBounds3 Bounds{};
    uint32_t Ptr = 0;
};

struct LocalTriangleMeshBvhData {
    PhysXBounds3 RootBounds{};
    std::vector<LocalBvhNodeBounds> Nodes;
    bool Valid = false;
};

struct LocalTriangleMeshExportMeta {
    uint64_t Signature = 0;
    uint32_t VertexCount = 0;
    uint32_t TriangleCount = 0;
    uint8_t Flags = 0;
    bool Valid = false;
};

struct LocalHeightFieldExportMeta {
    uint64_t Signature = 0;
    uint32_t Rows = 0;
    uint32_t Columns = 0;
    bool Valid = false;
};

static std::unordered_map<uint64_t, LocalTriangleMeshBvhData> gLocalTriBvhCache;
static std::unordered_map<uint64_t, LocalTriangleMeshExportMeta> gLocalTriMetaBySig;
static std::unordered_map<uint64_t, LocalHeightFieldExportMeta> gLocalHfMetaBySig;
static bool gLocalExportMetaLoaded = false;

struct TriangleMeshReadCandidate {
    CachedTriangleMeshData Data;
    uint64_t Signature = 0;
    size_t TriangleCount = 0;
    size_t StableHits = 0;
    uint8_t IndexSizeBytes = 0;
};

struct StableTriangleMeshSnapshot {
    std::vector<Vec3> Vertices;
    std::vector<uint8_t> RawIndices;
    uint64_t VertexHash = 0;
    uint64_t IndexHash = 0;
};

struct HeightFieldReadCandidate {
    CachedHeightFieldData Data;
    uint64_t Signature = 0;
    size_t StableHits = 0;
};

struct PhysXHeightFieldHeaderSnapshot {
    uint32_t Rows = 0;
    uint32_t Columns = 0;
    uint32_t SampleStride = 0;
    uint32_t SampleCount = 0;
    uint32_t ModifyCount = 0;
    uint64_t SamplesAddr = 0;
    float Thickness = 0.0f;
};

struct PhysXPrunerPayload {
    uint64_t Shape = 0;
    uint64_t Actor = 0;
};

// 预缩放 mesh 数据缓存：按 (mesh地址, 缩放参数) 组合缓存，避免内循环重复计算
struct ScaledMeshData {
    std::vector<Vec3> Vertices;              // 缩放后的顶点
    std::vector<PhysXBounds3> TriangleBounds; // 缩放后的每三角形 AABB（含 1.0f 膨胀）
    std::vector<PhysXBounds3> ChunkBounds;    // 缩放后的块级 AABB
    PhysXBounds3 MeshAABB{};                 // 缩放后的整体 AABB（含 2.0f 膨胀）
    std::vector<uint32_t> Indices;           // 索引引用（直接拷贝，避免查 mesh cache）
    size_t TriangleCount = 0;
};

struct ScaledMeshKey {
    uint64_t MeshAddr;
    float ScaleX, ScaleY, ScaleZ;
    float RotX, RotY, RotZ, RotW;

    bool operator==(const ScaledMeshKey& o) const {
        return MeshAddr == o.MeshAddr &&
               ScaleX == o.ScaleX && ScaleY == o.ScaleY && ScaleZ == o.ScaleZ &&
               RotX == o.RotX && RotY == o.RotY && RotZ == o.RotZ && RotW == o.RotW;
    }
};

struct ScaledMeshKeyHash {
    size_t operator()(const ScaledMeshKey& k) const {
        size_t h = std::hash<uint64_t>{}(k.MeshAddr);
        auto hf = [](float f) -> size_t { uint32_t u; memcpy(&u, &f, 4); return std::hash<uint32_t>{}(u); };
        h ^= hf(k.ScaleX) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= hf(k.ScaleY) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= hf(k.ScaleZ) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= hf(k.RotW) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

static std::unordered_map<ScaledMeshKey, ScaledMeshData, ScaledMeshKeyHash> gScaledMeshCache;
static uint64_t gScaledMeshCacheGeneration = 0;

struct VisibilityOccluder {
    PhysXBounds3 InflatedBounds{};
    PxTransformData WorldPose{};
    D4DVector InvRotation{};        // 预计算的逆旋转四元数（空间换时间）
    const ScaledMeshData* PreScaled = nullptr;  // 指向预缩放 mesh 数据（如果是三角网格且已缓存）
    uint64_t GeometryAddr = 0;
    uint64_t Resource = 0;
    uint32_t GeometryType = 0;
    PxMeshScaleData MeshScale{{1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 1.0f}};
    float HeightScale = 1.0f;
    float RowScale = 1.0f;
    float ColumnScale = 1.0f;
    bool UseLocalData = false;
    float DistanceSq = 0.0f;  // 距离相机的距离平方，用于排序
};

struct VisibilityOccluderCache {
    std::vector<VisibilityOccluder> Items;
    uint64_t Generation = 0;
    int CameraCellX = INT32_MIN;
    int CameraCellY = INT32_MIN;
    int CameraCellZ = INT32_MIN;
    bool Valid = false;
};

static VisibilityOccluderCache gVisibilityOccluderCache;

enum class VisibilitySceneKind : uint8_t {
    StaticMesh,
    HeightField,
    DynamicMesh,
};

struct VisibilitySceneCacheState {
    VisibilityScene Scene;
    std::unordered_map<uint64_t, uint64_t> Signatures;
    std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> LastSeen;
    std::chrono::steady_clock::time_point LastRefresh{};
    int CameraCellX = INT32_MIN;
    int CameraCellY = INT32_MIN;
    int CameraCellZ = INT32_MIN;
    bool Valid = false;
};

static VisibilitySceneCacheState gStaticVisibilityScene;
static VisibilitySceneCacheState gHeightFieldVisibilityScene;
static VisibilitySceneCacheState gDynamicVisibilityScene;
static std::atomic<bool> gVisibilityLastUseLocalModelData{false};
static std::future<void> gVisibilityRefreshFuture;
static std::atomic<bool> gVisibilityRefreshInProgress{false};

struct PhysXDrawDedupKey {
    uint8_t Source = 0;
    uint32_t Type = 0;
    uint64_t Resource = 0;
    int32_t X = 0;
    int32_t Y = 0;
    int32_t Z = 0;

    bool operator==(const PhysXDrawDedupKey& other) const {
        return Source == other.Source &&
               Type == other.Type && Resource == other.Resource &&
               X == other.X && Y == other.Y && Z == other.Z;
    }
};

struct PhysXDrawDedupKeyHash {
    size_t operator()(const PhysXDrawDedupKey& key) const {
        size_t h = std::hash<uint8_t>{}(key.Source);
        h ^= std::hash<uint32_t>{}(key.Type) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint64_t>{}(key.Resource) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int32_t>{}(key.X) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int32_t>{}(key.Y) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int32_t>{}(key.Z) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

struct PhysXDrawStats {
    int ActorsScanned = 0;
    int ActorsDrawn = 0;
    int ShapesScanned = 0;
    int ShapesDrawn = 0;
    int MeshesDrawn = 0;
    int TrianglesDrawn = 0;
    int RejectedByFilter = 0;
};

enum PhysXDrawSource : uint8_t {
    PX_DRAW_SOURCE_PRUNER = 0,
    PX_DRAW_SOURCE_PXSCENE = 1,
};

struct LocalPhysXDrawCacheEntry {
    uint64_t Shape = 0;
    uint64_t Resource = 0;
    uint32_t GeometryType = 0;
    PxTransformData WorldPose{};
    PxMeshScaleData MeshScale{};
    float HeightScale = 1.0f;
    float RowScale = 1.0f;
    float ColumnScale = 1.0f;
    PhysXDrawSource Source = PX_DRAW_SOURCE_PRUNER;
    bool EnableTriangleDistanceCull = false;
    bool IsStaticMesh = false;
    double LastSeenTime = 0.0;
};
static std::unordered_map<uint64_t, LocalPhysXDrawCacheEntry> gLocalPhysXDrawCache;
static std::unordered_map<uint64_t, std::vector<uint64_t>> gPxSceneActorPointerCache;

struct PhysXActorCandidate {
    uint64_t Actor = 0;
    uint16_t ActorType = 0;
    PxTransformData Pose{};
    float DistanceSq = 0.0f;
};

struct PrunerShapeCandidate {
    uint32_t Index;       // index into gPrunerCache
    float    DistanceSq;  // distance to player (AABB center)
};

// TeamID → 颜色映射（使用固定色板，按 teamID % 数量 循环）
static ImU32 GetTeamColor(int teamID) {
    static const ImU32 kTeamColors[] = {
        IM_COL32(0,   200, 255, 255),  // 青色
        IM_COL32(255, 80,  80,  255),  // 红色
        IM_COL32(80,  255, 80,  255),  // 绿色
        IM_COL32(255, 200, 0,   255),  // 黄色
        IM_COL32(200, 80,  255, 255),  // 紫色
        IM_COL32(255, 140, 0,   255),  // 橙色
        IM_COL32(0,   200, 150, 255),  // 蓝绿
        IM_COL32(255, 100, 200, 255),  // 粉色
    };
    constexpr int kNumColors = sizeof(kTeamColors) / sizeof(kTeamColors[0]);
    if (teamID < 0) return IM_COL32(200, 200, 200, 255);  // 未知队伍：灰色
    return kTeamColors[teamID % kNumColors];
}

static ImU32 GetActorColor(const CachedActor& actor) {
    switch (actor.actorType) {
        case ActorType::BOT:
        case ActorType::NPC:
        case ActorType::MONSTER:
            return IM_COL32(80, 255, 80, 255);
        case ActorType::TOMB_BOX:
            return IM_COL32(255, 210, 90, 255);
        case ActorType::OTHER_BOX:
            return IM_COL32(255, 140, 120, 255);
        case ActorType::ESCAPE_BOX:
            return IM_COL32(255, 235, 120, 255);
        case ActorType::CONTAINER:
            return IM_COL32(110, 255, 150, 255);
        case ActorType::PLAYER:
            return GetTeamColor(actor.teamID);
        case ActorType::VEHICLE:
            return IM_COL32(120, 210, 255, 255);
        case ActorType::OTHER:
        default:
            return IM_COL32(220, 220, 220, 255);
    }
}

static bool UseCategoryLabelOnly(ActorType actorType) {
    return actorType == ActorType::BOT ||
           actorType == ActorType::NPC ||
           actorType == ActorType::MONSTER;
}

static bool IsCategoryPrefixActorType(ActorType actorType) {
    return actorType == ActorType::BOT ||
           actorType == ActorType::NPC ||
           actorType == ActorType::MONSTER;
}

static bool IsBoxLikeActorType(ActorType actorType) {
    return actorType == ActorType::TOMB_BOX ||
           actorType == ActorType::OTHER_BOX ||
           actorType == ActorType::ESCAPE_BOX;
}

static bool ShouldShowBoxContentNearCrosshair(const ImVec2& labelPos, float screenW, float screenH) {
    const ImVec2 crosshairCenter(screenW * 0.5f, screenH * 0.5f);
    const float shortSide = std::max(1.0f, std::min(screenW, screenH));
    const float showRadius = shortSide * 0.14f;
    const float dx = labelPos.x - crosshairCenter.x;
    const float dy = labelPos.y - crosshairCenter.y;
    return dx * dx + dy * dy <= showRadius * showRadius;
}

// 前向声明
static void DrawObjectsWithDataInternal(
    const std::vector<CachedActor>& actors,
    const FMatrix& VPMat,
    const Vec3& localPlayerPos,
    uint64_t engineFrame,
    float gameStepDeltaTime,
    BoneScreenCache* boneScreenCache);
static void DrawPhysXGeometry(
    const FMatrix& vpMat,
    const Vec3& localPlayerPos,
    ImDrawList* drawList,
    float screenW,
    float screenH);
static void InvalidateVisibilityCache();
static void DrawDepthBufferOverlay();
static void DrawMiniMapOverlay(const ClassifiedActors& classified, const Vec3& localPlayerPos,
                               float screenW, float screenH);
static void BatchBoneVisibilityCheck(const Vec3& cameraWorldPos,
                                     const Vec3 boneWorldPos[],
                                     const bool boneHasData[],
                                     bool boneVisible[],
                                     int count);
static bool EnsurePrunerDataCached();
static void RefreshVisibilityScenes(const Vec3& cameraWorldPos, bool forceRefreshAll);
static void AppendGpuSceneTriangle(std::vector<GpuSceneTriangle>& triangles,
                                   const Vec3& a, const Vec3& b, const Vec3& c);
static bool AppendTriangleMeshSceneTriangles(uint64_t geometryAddr, const PxTransformData& worldPose,
                                             std::vector<GpuSceneTriangle>& triangles, size_t maxTriangles);
static bool AppendHeightFieldSceneTriangles(uint64_t geometryAddr, const PxTransformData& worldPose,
                                            std::vector<GpuSceneTriangle>& triangles, size_t maxTriangles);
static bool EnsureGpuSceneTriangles(const Vec3& cameraWorldPos);

static bool TryReadActorWorldTransform(const CachedActor& actor, FTransform& outTransform) {
    const uint64_t transformAddr = actor.rootCompAddr != 0
        ? (actor.rootCompAddr + offset.ComponentToWorld)
        : (actor.skelMeshCompAddr != 0 ? actor.skelMeshCompAddr + offset.ComponentToWorld : 0);
    if (transformAddr == 0) {
        return false;
    }
    return GetDriverManager().read(transformAddr, &outTransform, sizeof(FTransform));
}

static bool TryReadLocalPlayerMapBasis(Vec3& outForward, Vec3& outRight) {
    if (address.LocalPlayerActor == 0) {
        return false;
    }

    const uint64_t rootComp = GetDriverManager().read<uint64_t>(address.LocalPlayerActor + offset.RootComponent);
    if (rootComp == 0) {
        return false;
    }

    FTransform transform{};
    if (!GetDriverManager().read(rootComp + offset.ComponentToWorld, &transform, sizeof(FTransform))) {
        return false;
    }

    Vec3 forward = QuatRotateVector(transform.Rotation, Vec3(1.0f, 0.0f, 0.0f));
    const float forwardLen = std::sqrt(forward.X * forward.X + forward.Y * forward.Y);
    if (forwardLen <= 1e-3f) {
        return false;
    }

    forward.X /= forwardLen;
    forward.Y /= forwardLen;
    forward.Z = 0.0f;
    outForward = forward;
    outRight = Vec3(-forward.Y, forward.X, 0.0f);
    return true;
}

static D4DVector NormalizeQuat(const D4DVector& q) {
    float len = std::sqrt(q.X * q.X + q.Y * q.Y + q.Z * q.Z + q.W * q.W);
    if (len <= 1e-6f) {
        return {0.0f, 0.0f, 0.0f, 1.0f};
    }
    return {q.X / len, q.Y / len, q.Z / len, q.W / len};
}

static D4DVector QuatMultiply(const D4DVector& a, const D4DVector& b) {
    return NormalizeQuat({
        a.W * b.X + a.X * b.W + a.Y * b.Z - a.Z * b.Y,
        a.W * b.Y - a.X * b.Z + a.Y * b.W + a.Z * b.X,
        a.W * b.Z + a.X * b.Y - a.Y * b.X + a.Z * b.W,
        a.W * b.W - a.X * b.X - a.Y * b.Y - a.Z * b.Z
    });
}

static Vec3 ScaleVector(const Vec3& v, const Vec3& s) {
    return {v.X * s.X, v.Y * s.Y, v.Z * s.Z};
}

static PxTransformData ComposeTransforms(const PxTransformData& parent, const PxTransformData& child) {
    Vec3 rotatedChild = QuatRotateVector(parent.Rotation, child.Translation);
    return {
        QuatMultiply(parent.Rotation, child.Rotation),
        parent.Translation + rotatedChild
    };
}

static inline Vec3 QuatRotateVectorFast(const D4DVector& q, const Vec3& v);
static inline Vec3 QuatRotateVectorInvFast(const D4DVector& q, const Vec3& v);

static Vec3 TransformPoint(const PxTransformData& transform, const Vec3& point) {
    return transform.Translation + QuatRotateVectorFast(transform.Rotation, point);
}

static D4DVector QuatConjugate(const D4DVector& q) {
    return {-q.X, -q.Y, -q.Z, q.W};
}

static Vec3 InverseTransformPoint(const PxTransformData& transform, const Vec3& point) {
    return QuatRotateVectorFast(QuatConjugate(transform.Rotation), point - transform.Translation);
}

static Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {
        a.Y * b.Z - a.Z * b.Y,
        a.Z * b.X - a.X * b.Z,
        a.X * b.Y - a.Y * b.X,
    };
}

#if defined(__ARM_NEON) || defined(__aarch64__)
static inline float32x4_t LoadVec3Neon(const Vec3& v) {
    return {v.X, v.Y, v.Z, 0.0f};
}

static inline Vec3 StoreVec3Neon(float32x4_t v) {
    alignas(16) float out[4];
    vst1q_f32(out, v);
    return {out[0], out[1], out[2]};
}

static inline float32x4_t NeonCross3(float32x4_t a, float32x4_t b) {
    const float ax = vgetq_lane_f32(a, 0);
    const float ay = vgetq_lane_f32(a, 1);
    const float az = vgetq_lane_f32(a, 2);
    const float bx = vgetq_lane_f32(b, 0);
    const float by = vgetq_lane_f32(b, 1);
    const float bz = vgetq_lane_f32(b, 2);
    return {
        ay * bz - az * by,
        az * bx - ax * bz,
        ax * by - ay * bx,
        0.0f
    };
}

static inline float NeonDot3(float32x4_t a, float32x4_t b) {
    alignas(16) float tmp[4];
    vst1q_f32(tmp, vmulq_f32(a, b));
    return tmp[0] + tmp[1] + tmp[2];
}
#endif

static inline Vec3 QuatRotateVectorFast(const D4DVector& q, const Vec3& v) {
#if defined(__ARM_NEON) || defined(__aarch64__)
    const float32x4_t qxyz = {q.X, q.Y, q.Z, 0.0f};
    const float32x4_t vv = LoadVec3Neon(v);
    const float32x4_t t = vmulq_n_f32(NeonCross3(qxyz, vv), 2.0f);
    return StoreVec3Neon(vaddq_f32(vaddq_f32(vv, vmulq_n_f32(t, q.W)), NeonCross3(qxyz, t)));
#else
    return QuatRotateVector(q, v);
#endif
}

static inline Vec3 QuatRotateVectorInvFast(const D4DVector& q, const Vec3& v) {
    return QuatRotateVectorFast(QuatConjugate(q), v);
}

// 使用预计算的逆旋转四元数的快速版本
static Vec3 InverseTransformPointFast(const Vec3& translation, const D4DVector& invRotation, const Vec3& point) {
#if defined(__ARM_NEON) || defined(__aarch64__)
    const float32x4_t tv = {translation.X, translation.Y, translation.Z, 0.0f};
    const float32x4_t pv = {point.X, point.Y, point.Z, 0.0f};
    return QuatRotateVectorFast(invRotation, StoreVec3Neon(vsubq_f32(pv, tv)));
#else
    return QuatRotateVectorFast(invRotation, point - translation);
#endif
}

static Vec3 ApplyMeshScale(const Vec3& point, const PxMeshScaleData& scale) {
    const Vec3 scalingFramePoint = QuatRotateVectorFast(scale.Rotation, point);
    const Vec3 scaled = ScaleVector(scalingFramePoint, scale.Scale);
    return QuatRotateVectorInvFast(scale.Rotation, scaled);
}

static PhysXBounds3 ApplyMeshScaleToBounds(const PhysXBounds3& bounds, const PxMeshScaleData& scale) {
    const Vec3 corners[8] = {
        {bounds.Min.X, bounds.Min.Y, bounds.Min.Z},
        {bounds.Min.X, bounds.Min.Y, bounds.Max.Z},
        {bounds.Min.X, bounds.Max.Y, bounds.Min.Z},
        {bounds.Min.X, bounds.Max.Y, bounds.Max.Z},
        {bounds.Max.X, bounds.Min.Y, bounds.Min.Z},
        {bounds.Max.X, bounds.Min.Y, bounds.Max.Z},
        {bounds.Max.X, bounds.Max.Y, bounds.Min.Z},
        {bounds.Max.X, bounds.Max.Y, bounds.Max.Z},
    };

    PhysXBounds3 out{
        {FLT_MAX, FLT_MAX, FLT_MAX},
        {-FLT_MAX, -FLT_MAX, -FLT_MAX}
    };
    for (const Vec3& corner : corners) {
        const Vec3 p = ApplyMeshScale(corner, scale);
        out.Min.X = std::min(out.Min.X, p.X);
        out.Min.Y = std::min(out.Min.Y, p.Y);
        out.Min.Z = std::min(out.Min.Z, p.Z);
        out.Max.X = std::max(out.Max.X, p.X);
        out.Max.Y = std::max(out.Max.Y, p.Y);
        out.Max.Z = std::max(out.Max.Z, p.Z);
    }
    return out;
}

static bool IsMeshScaleIdentity(const PxMeshScaleData& scale) {
    constexpr float kEps = 1e-4f;
    return std::fabs(scale.Scale.X - 1.0f) < kEps &&
           std::fabs(scale.Scale.Y - 1.0f) < kEps &&
           std::fabs(scale.Scale.Z - 1.0f) < kEps &&
           std::fabs(scale.Rotation.X) < kEps &&
           std::fabs(scale.Rotation.Y) < kEps &&
           std::fabs(scale.Rotation.Z) < kEps &&
           std::fabs(std::fabs(scale.Rotation.W) - 1.0f) < kEps;
}

static bool DrawLine3D(ImDrawList* drawList, const FMatrix& vpMat, float screenW, float screenH,
                       const Vec3& a, const Vec3& b, ImU32 color, float thickness) {
    // 计算两端点的齐次 W 分量（W <= 0 表示在相机背后）
    const float wa = a.X * vpMat.M[0][3] + a.Y * vpMat.M[1][3] + a.Z * vpMat.M[2][3] + vpMat.M[3][3];
    const float wb = b.X * vpMat.M[0][3] + b.Y * vpMat.M[1][3] + b.Z * vpMat.M[2][3] + vpMat.M[3][3];

    // 两端点都在相机背后 → 整条线不可见
    constexpr float kNearW = 0.1f;
    if (wa <= kNearW && wb <= kNearW) return false;

    Vec3 clippedA = a, clippedB = b;

    // 一端在相机背后时，沿线段插值到近平面交点
    if (wa <= kNearW) {
        const float t = (kNearW - wa) / (wb - wa);
        clippedA = {a.X + t * (b.X - a.X), a.Y + t * (b.Y - a.Y), a.Z + t * (b.Z - a.Z)};
    } else if (wb <= kNearW) {
        const float t = (kNearW - wb) / (wa - wb);
        clippedB = {b.X + t * (a.X - b.X), b.Y + t * (a.Y - b.Y), b.Z + t * (a.Z - b.Z)};
    }

    Vec2 sa, sb;
    if (!WorldToScreen(clippedA, vpMat, screenW, screenH, sa)) return false;
    if (!WorldToScreen(clippedB, vpMat, screenW, screenH, sb)) return false;
    drawList->AddLine(ImVec2(sa.x, sa.y), ImVec2(sb.x, sb.y), color, thickness);
    return true;
}

static bool ReadPxTransform(uint64_t addr, PxTransformData& out) {
    float raw[7] = {};
    if (!GetDriverManager().read(addr, raw, sizeof(raw))) return false;
    out.Rotation = NormalizeQuat({raw[0], raw[1], raw[2], raw[3]});
    out.Translation = {raw[4], raw[5], raw[6]};
    return true;
}

static bool ReadActorGlobalPose(uint64_t actor, uint16_t actorType, PxTransformData& out) {
    if (actorType == PX_ACTOR_RIGID_DYNAMIC) {
        uint32_t flags = GetDriverManager().read<uint32_t>(actor + offset.PxRigidDynamicFlags);
        uint64_t poseAddr = actor + offset.PxRigidDynamicBodyToWorld;
        if ((flags & 0x200u) != 0) {
            uint64_t core = GetDriverManager().read<uint64_t>(actor + offset.PxRigidDynamicCorePtr);
            if (core == 0) return false;
            poseAddr = core + offset.PxRigidDynamicCoreBodyToWorld;
        }
        return ReadPxTransform(poseAddr, out);
    }
    if (actorType == PX_ACTOR_RIGID_STATIC) {
        uint32_t flags = GetDriverManager().read<uint32_t>(actor + offset.PxRigidStaticFlags);
        uint64_t poseAddr = actor + offset.PxRigidStaticGlobalPose;
        if ((flags & 0x40u) != 0) {
            uint64_t core = GetDriverManager().read<uint64_t>(actor + offset.PxRigidStaticCorePtr);
            if (core == 0) return false;
            poseAddr = core + offset.PxRigidStaticCoreGlobalPose;
        }
        return ReadPxTransform(poseAddr, out);
    }
    return false;
}

static bool ReadShapeLocalPose(uint64_t shape, uint32_t npShapeFlags, PxTransformData& out) {
    uint64_t poseAddr = shape + offset.PxShapeLocalPose;
    if ((npShapeFlags & 0x4u) != 0) {
        uint64_t core = GetDriverManager().read<uint64_t>(shape + offset.PxShapeCorePtr);
        if (core == 0) return false;
        poseAddr = core + offset.PxShapeLocalPoseExternal;
    }
    return ReadPxTransform(poseAddr, out);
}

static bool ReadMeshScale(uint64_t addr, PxMeshScaleData& out) {
    float raw[7] = {};
    if (!GetDriverManager().read(addr, raw, sizeof(raw))) return false;
    out.Scale = {raw[0], raw[1], raw[2]};
    out.Rotation = NormalizeQuat({raw[3], raw[4], raw[5], raw[6]});
    return true;
}

static bool ReadRemoteBufferRobust(uint64_t addr, void* buf, size_t size) {
    if (addr == 0 || buf == nullptr || size == 0) return false;

    if (GetDriverManager().read(addr, buf, size)) {
        return true;
    }
    if (GetDriverManager().read_safe(addr, buf, size)) {
        return true;
    }

    auto* out = static_cast<uint8_t*>(buf);
    constexpr size_t kMaxChunkSize = 0x100;
    constexpr size_t kMinChunkSize = 0x10;
    size_t offsetBytes = 0;
    while (offsetBytes < size) {
        size_t chunkSize = std::min(kMaxChunkSize, size - offsetBytes);
        bool chunkRead = false;
        while (chunkSize >= kMinChunkSize) {
            if (GetDriverManager().read(addr + offsetBytes, out + offsetBytes, chunkSize) ||
                GetDriverManager().read_safe(addr + offsetBytes, out + offsetBytes, chunkSize)) {
                chunkRead = true;
                offsetBytes += chunkSize;
                break;
            }
            chunkSize >>= 1;
        }
        if (!chunkRead) {
            return false;
        }
    }
    return true;
}

template <typename T>
static bool ReadRemoteArrayElementSafe(uint64_t baseAddr, size_t index, T& out) {
    const uint64_t addr = baseAddr + static_cast<uint64_t>(index) * sizeof(T);
    return GetDriverManager().read_safe(addr, &out, sizeof(T)) || GetDriverManager().read(addr, &out, sizeof(T));
}

static std::filesystem::path GetPhysXExportDir() {
    return std::filesystem::path("physx_mesh_export");
}

static std::string gStablePhysXExportStatus = "未导出";

// 几何缓存刷新间隔（秒）：三角面/凸包/高度场几何数据的重读周期
float gPhysXGeomRefreshInterval = 120.0f;
// 上次清空几何缓存的时间戳（clock_gettime MONOTONIC）
static double gLastGeomCacheFlushTime = 0.0;

static double GetMonotoneSec() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// 每帧几何缓存未命中预算：防止 cache flush 后单帧读取全部 mesh 导致卡顿
static int gGeomCacheMissesThisFrame = 0;
static constexpr int kGeomCacheMissBudgetPerFrame = 512;

// mesh 读取失败冷却：连续读取失败的 mesh 暂时跳过，避免反复浪费预算
struct GeomCacheMissCooldown {
    int ConsecutiveFailures = 0;
    double LastAttemptTime = 0.0;
};
static std::unordered_map<uint64_t, GeomCacheMissCooldown> gGeomCacheMissCooldowns;

// 异步文件加载：避免文件 I/O 阻塞绘制线程
struct AsyncLocalMeshLoad {
    uint64_t sig = 0;
    std::future<LocalObjMeshData> future;
};
struct AsyncLocalBvhLoad {
    uint64_t sig = 0;
    std::future<LocalTriangleMeshBvhData> future;
};
static std::vector<AsyncLocalMeshLoad> gPendingMeshLoads;   // tri mesh
static std::vector<AsyncLocalMeshLoad> gPendingHfLoads;     // height field
static std::vector<AsyncLocalBvhLoad> gPendingBvhLoads;
static std::unordered_set<uint64_t> gInFlightMeshSigs;
static std::unordered_set<uint64_t> gInFlightHfSigs;
static std::unordered_set<uint64_t> gInFlightBvhSigs;

static void PollAsyncLocalLoads() {
    for (auto it = gPendingMeshLoads.begin(); it != gPendingMeshLoads.end(); ) {
        if (it->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            auto data = it->future.get();
            if (!data.Vertices.empty())
                gLocalTriObjCache.emplace(it->sig, std::move(data));
            gInFlightMeshSigs.erase(it->sig);
            it = gPendingMeshLoads.erase(it);
        } else { ++it; }
    }
    for (auto it = gPendingHfLoads.begin(); it != gPendingHfLoads.end(); ) {
        if (it->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            auto data = it->future.get();
            if (!data.Vertices.empty())
                gLocalHfObjCache.emplace(it->sig, std::move(data));
            gInFlightHfSigs.erase(it->sig);
            it = gPendingHfLoads.erase(it);
        } else { ++it; }
    }
    for (auto it = gPendingBvhLoads.begin(); it != gPendingBvhLoads.end(); ) {
        if (it->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            auto data = it->future.get();
            gLocalTriBvhCache.emplace(it->sig, std::move(data));
            gInFlightBvhSigs.erase(it->sig);
            it = gPendingBvhLoads.erase(it);
        } else { ++it; }
    }
}

// 全局每帧三角形绘制预算：防止大量 mesh 同时绘制导致 ImGui draw list 爆炸
static int gTrianglesDrawnThisFrame = 0;
static constexpr int kMaxTrianglesPerFrame = 50000;

// 若距上次刷新超过 gPhysXGeomRefreshInterval，则清空几何缓存，下次访问时重新从游戏内存读取
static void MaybeFlushGeomCaches() {
    const double now = GetMonotoneSec();
    if (now - gLastGeomCacheFlushTime < static_cast<double>(gPhysXGeomRefreshInterval)) return;
    gLastGeomCacheFlushTime = now;

    constexpr double kTriangleMeshKeepAliveSec = 600.0;
    constexpr double kHeightFieldKeepAliveSec = 600.0;
    constexpr double kConvexMeshKeepAliveSec = 180.0;

    for (auto it = gTriangleMeshCache.begin(); it != gTriangleMeshCache.end();) {
        const CachedTriangleMeshData& entry = it->second;
        const double idleSec = now - std::max(entry.LastUsedAt, entry.CachedAt);
        if (idleSec > kTriangleMeshKeepAliveSec) {
            it = gTriangleMeshCache.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = gHeightFieldCache.begin(); it != gHeightFieldCache.end();) {
        const CachedHeightFieldData& entry = it->second;
        const double idleSec = now - entry.LastUsedAt;
        if (idleSec > kHeightFieldKeepAliveSec) {
            it = gHeightFieldCache.erase(it);
        } else {
            ++it;
        }
    }

    if (!gConvexMeshCache.empty()) {
        static double gLastConvexCacheClearTime = 0.0;
        if (now - gLastConvexCacheClearTime > kConvexMeshKeepAliveSec) {
            gConvexMeshCache.clear();
            gLastConvexCacheClearTime = now;
        }
    }

    gGeomCacheMissCooldowns.clear();
    // 注意：不清空 gTriangleMeshSources / gHeightFieldSources / gTriangleMeshSignatures /
    // gHeightFieldSignatures / gLocalTriObjCache / gLocalHfObjCache
    // source/signature 是后续重新建立本地 OBJ/BVH 关联的材料；
    // mesh 指针→signature 映射只增不减；
    // 本地 OBJ 缓存读自磁盘文件，无需随远程几何缓存一起刷新
}

static bool EnsurePhysXExportDir() {
    std::error_code ec;
    std::filesystem::create_directories(GetPhysXExportDir(), ec);
    return !ec;
}

static uint64_t HashBytesFnv1a(const void* data, size_t size, uint64_t seed = 1469598103934665603ULL) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < size; ++i) {
        h ^= bytes[i];
        h *= 1099511628211ULL;
    }
    return h;
}

template <typename T>
static uint64_t HashVectorFnv1a(const std::vector<T>& values, uint64_t seed = 1469598103934665603ULL) {
    if (values.empty()) return seed;
    return HashBytesFnv1a(values.data(), values.size() * sizeof(T), seed);
}

static uint64_t BuildTriangleMeshSignature(uint64_t mesh, uint32_t vertexCount, uint32_t triangleCount,
                                           uint8_t flags, const std::vector<Vec3>& vertices,
                                           const std::vector<uint32_t>& indices) {
    uint64_t sig = 1469598103934665603ULL;
    sig = HashBytesFnv1a(&vertexCount, sizeof(vertexCount), sig);
    sig = HashBytesFnv1a(&triangleCount, sizeof(triangleCount), sig);
    sig = HashBytesFnv1a(&flags, sizeof(flags), sig);
    sig = HashVectorFnv1a(vertices, sig);
    sig = HashVectorFnv1a(indices, sig);
    return sig;
}

static uint64_t BuildHeightFieldSignature(uint64_t heightField, uint32_t rows, uint32_t columns,
                                          const std::vector<PhysXHeightFieldSample>& samples) {
    uint64_t sig = 1469598103934665603ULL;
    sig = HashBytesFnv1a(&rows, sizeof(rows), sig);
    sig = HashBytesFnv1a(&columns, sizeof(columns), sig);
    sig = HashVectorFnv1a(samples, sig);
    return sig;
}

static uint64_t BuildTriangleMeshLocalLookupKey(uint32_t vertexCount, uint32_t triangleCount, uint8_t flags) {
    uint64_t key = 1469598103934665603ULL;
    key = HashBytesFnv1a(&vertexCount, sizeof(vertexCount), key);
    key = HashBytesFnv1a(&triangleCount, sizeof(triangleCount), key);
    key = HashBytesFnv1a(&flags, sizeof(flags), key);
    return key;
}

static uint64_t BuildHeightFieldLocalLookupKey(uint32_t rows, uint32_t columns) {
    uint64_t key = 1469598103934665603ULL;
    key = HashBytesFnv1a(&rows, sizeof(rows), key);
    key = HashBytesFnv1a(&columns, sizeof(columns), key);
    return key;
}

static bool IsFiniteVec3(const Vec3& v);
static bool ValidateTriangleMeshData(const std::vector<Vec3>& vertices, const std::vector<uint32_t>& indices);
static bool LoadObjMeshFile(const std::filesystem::path& path, LocalObjMeshData& out);
static bool LoadBvhFile(const std::filesystem::path& path, LocalTriangleMeshBvhData& out);
static bool BuildTriangleMeshReadCandidate(const std::vector<Vec3>& vertices, std::vector<uint32_t> indices,
                                           uint32_t vertexCount, uint32_t triangleCount, uint8_t flags,
                                           uint8_t indexSizeBytes,
                                           TriangleMeshReadCandidate& out);
static bool IsValidHeightFieldHeader(const PhysXHeightFieldHeaderSnapshot& header);
static bool ReadHeightFieldHeaderSnapshot(uint64_t heightField, PhysXHeightFieldHeaderSnapshot& out);
static uint64_t GetSyncPxScene(uint64_t libUE4, uint64_t uworld);
static bool CollectPxSceneActorPointers(uint64_t pxScene, std::vector<uint64_t>& outActors);
static bool GetCachedTriangleMesh(uint64_t mesh, uint32_t vertexCount, uint32_t triangleCount, uint8_t flags,
                                  uint64_t verticesAddr, uint64_t trianglesAddr, CachedTriangleMeshData& out);
static bool GetCachedHeightField(uint64_t heightField, CachedHeightFieldData& out);
static bool GetHeightFieldCellTriangles(const CachedHeightFieldData& cache,
                                        uint32_t row, uint32_t column,
                                        float heightScale, float rowScale, float columnScale,
                                        Vec3& t0a, Vec3& t0b, Vec3& t0c, bool& t0Valid,
                                        Vec3& t1a, Vec3& t1b, Vec3& t1c, bool& t1Valid);
static bool EnsurePrunerDataCached();
static bool EnsureVisibilityOccludersCached(const Vec3& cameraWorldPos);
static void CollectActivePxScenes(uint64_t libUE4, uint64_t uworld, std::vector<uint64_t>& outScenes);

// --- 帧级 pruner 数据缓存（提前声明，供 DrawPhysXStaticPrunerGeometry 使用）---
static struct {
    std::vector<PhysXPrunerPayload> payloads;
    std::vector<PhysXBounds3> bounds;
    std::vector<PhysXBounds3> inflatedBounds;
    uint32_t objectCount = 0;
    uint64_t generation = 0;
    bool valid = false;
} gPrunerCache;

static std::atomic<uint64_t> gPrunerCacheGeneration{0};

static bool WriteTriangleMeshObj(const std::filesystem::path& path, const CachedTriangleMeshData& cache) {
    if (cache.Vertices.empty() || cache.Indices.size() < 3) return false;
    if (!ValidateTriangleMeshData(cache.Vertices, cache.Indices)) return false;

    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;

    out << "# stable physx triangle mesh export\n";
    out << "o " << path.stem().string() << "\n";
    for (const Vec3& v : cache.Vertices) {
        out << "v " << v.X << ' ' << v.Y << ' ' << v.Z << "\n";
    }

    size_t faceCount = 0;
    for (size_t i = 0; i + 2 < cache.Indices.size(); i += 3) {
        const uint32_t ia = cache.Indices[i];
        const uint32_t ib = cache.Indices[i + 1];
        const uint32_t ic = cache.Indices[i + 2];
        if (ia >= cache.Vertices.size() || ib >= cache.Vertices.size() || ic >= cache.Vertices.size()) continue;
        out << "f " << (ia + 1) << ' ' << (ib + 1) << ' ' << (ic + 1) << "\n";
        ++faceCount;
    }
    if (!(static_cast<bool>(out) && faceCount > 0)) return false;

    LocalObjMeshData verify;
    return LoadObjMeshFile(path, verify);
}

static bool IsFiniteVec3(const Vec3& v) {
    return std::isfinite(v.X) && std::isfinite(v.Y) && std::isfinite(v.Z);
}

static bool ValidateTriangleMeshData(const std::vector<Vec3>& vertices, const std::vector<uint32_t>& indices) {
    if (vertices.empty() || indices.size() < 3 || (indices.size() % 3) != 0) return false;

    size_t zeroVertexCount = 0;
    for (const Vec3& v : vertices) {
        if (!IsFiniteVec3(v)) return false;
        if (std::fabs(v.X) > 1.0e7f || std::fabs(v.Y) > 1.0e7f || std::fabs(v.Z) > 1.0e7f) return false;
        if (v.X == 0.0f && v.Y == 0.0f && v.Z == 0.0f) ++zeroVertexCount;
    }
    // 大量 (0,0,0) 顶点说明缓冲区未完全初始化，拒绝
    if (zeroVertexCount > std::max<size_t>(2, vertices.size() / 50)) return false;

    size_t validTriangles = 0;
    // 统计每个顶点被引用次数，检测异常扇形（某个顶点被过多三角形引用）
    size_t idx0RefCount = 0;
    const size_t triCount = indices.size() / 3;
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const uint32_t ia = indices[i];
        const uint32_t ib = indices[i + 1];
        const uint32_t ic = indices[i + 2];
        if (ia >= vertices.size() || ib >= vertices.size() || ic >= vertices.size()) continue;
        if (ia == ib || ib == ic || ia == ic) continue;
        ++validTriangles;
        // 统计索引 0 的引用频率：被破损数据填充时，大量索引为 0
        if (ia == 0 || ib == 0 || ic == 0) ++idx0RefCount;
    }

    if (validTriangles == 0) return false;
    // 如果 vertex[0] 是 (0,0,0) 且被超过 30% 的三角形引用，说明索引也被零填充
    if (zeroVertexCount > 0 && vertices[0].X == 0.0f && vertices[0].Y == 0.0f && vertices[0].Z == 0.0f) {
        if (idx0RefCount > std::max<size_t>(10, triCount * 3 / 10)) return false;
    }

    return true;
}

static float GetLabelTextScaleByDistance(float distanceMeters) {
    if (distanceMeters <= 20.0f) return 1.00f;
    if (distanceMeters <= 40.0f) return 0.92f;
    if (distanceMeters <= 60.0f) return 0.84f;
    if (distanceMeters <= 90.0f) return 0.68f;
    if (distanceMeters <= 130.0f) return 0.60f;
    return 0.50f;
}

static bool BuildTriangleMeshReadCandidate(const std::vector<Vec3>& vertices, std::vector<uint32_t> indices,
                                           uint32_t vertexCount, uint32_t triangleCount, uint8_t flags,
                                           uint8_t indexSizeBytes,
                                           TriangleMeshReadCandidate& out) {
    if (vertices.size() != vertexCount) return false;
    if (!ValidateTriangleMeshData(vertices, indices)) return false;

    out.Data.Vertices = vertices;
    out.Data.Indices = std::move(indices);
    out.Signature = BuildTriangleMeshSignature(0, vertexCount, triangleCount, flags,
                                               out.Data.Vertices, out.Data.Indices);
    out.TriangleCount = out.Data.Indices.size() / 3ULL;
    out.StableHits = 1;
    out.IndexSizeBytes = indexSizeBytes;
    return true;
}

static bool DecodeTriangleIndices(const std::vector<uint8_t>& rawIndices, uint8_t indexSizeBytes,
                                  size_t indexCount, uint32_t vertexCount,
                                  std::vector<uint32_t>& outIndices) {
    if (indexSizeBytes != 2 && indexSizeBytes != 4) return false;
    if (rawIndices.size() != indexCount * static_cast<size_t>(indexSizeBytes)) return false;

    outIndices.resize(indexCount);
    if (indexSizeBytes == 2) {
        if (vertexCount > 65535) return false;
        const uint16_t* src = reinterpret_cast<const uint16_t*>(rawIndices.data());
        for (size_t i = 0; i < indexCount; ++i) outIndices[i] = src[i];
        return true;
    }

    const uint32_t* src = reinterpret_cast<const uint32_t*>(rawIndices.data());
    for (size_t i = 0; i < indexCount; ++i) outIndices[i] = src[i];
    return true;
}

static bool ReadStableTriangleMeshSnapshot(uint64_t verticesAddr, uint64_t trianglesAddr,
                                           uint32_t vertexCount, size_t indexCount, uint8_t indexSizeBytes,
                                           StableTriangleMeshSnapshot& out, bool& outStable) {
    outStable = false;
    const size_t vertexBytes = static_cast<size_t>(vertexCount) * sizeof(Vec3);
    const size_t indexBytes = indexCount * static_cast<size_t>(indexSizeBytes);
    if (vertexBytes == 0 || indexBytes == 0) return false;

    StableTriangleMeshSnapshot first;
    first.Vertices.resize(vertexCount);
    first.RawIndices.resize(indexBytes);
    if (!ReadRemoteBufferRobust(verticesAddr, first.Vertices.data(), vertexBytes)) return false;
    if (!ReadRemoteBufferRobust(trianglesAddr, first.RawIndices.data(), indexBytes)) return false;
    for (const Vec3& v : first.Vertices) {
        if (!IsFiniteVec3(v)) return false;
    }
    first.VertexHash = HashBytesFnv1a(first.Vertices.data(), vertexBytes);
    first.IndexHash = HashBytesFnv1a(first.RawIndices.data(), indexBytes);

    StableTriangleMeshSnapshot second;
    second.Vertices.resize(vertexCount);
    second.RawIndices.resize(indexBytes);
    if (!ReadRemoteBufferRobust(verticesAddr, second.Vertices.data(), vertexBytes)) return false;
    if (!ReadRemoteBufferRobust(trianglesAddr, second.RawIndices.data(), indexBytes)) return false;
    for (const Vec3& v : second.Vertices) {
        if (!IsFiniteVec3(v)) return false;
    }
    second.VertexHash = HashBytesFnv1a(second.Vertices.data(), vertexBytes);
    second.IndexHash = HashBytesFnv1a(second.RawIndices.data(), indexBytes);

    if (first.VertexHash == second.VertexHash && first.IndexHash == second.IndexHash) {
        outStable = true;
    }
    // 即使哈希不匹配也返回第二次读取的数据：
    // 对于大 mesh，两次完整读取之间数据可能因内核非原子读取发生微小变化，
    // 但第二次快照通常是有效的。调用方可通过 ValidateTriangleMeshData 验证，
    // 缓存后的抽查机制（每秒抽查顶点）会捕获真正的脏数据。
    out = std::move(second);
    return true;
}

static bool ReadTriangleMeshForExportSafe(const TriangleMeshSourceInfo& source, CachedTriangleMeshData& out) {
    if (source.VertexCount == 0 || source.TriangleCount == 0 ||
        source.VerticesAddr == 0 || source.TrianglesAddr == 0) {
        return false;
    }

    const size_t indexCount = static_cast<size_t>(source.TriangleCount) * 3ULL;
    const uint8_t indexSizeBytes = (source.Flags & 0x02u) ? 2 : 4;
    constexpr int kMaxRounds = 3;

    for (int round = 0; round < kMaxRounds; ++round) {
        // 第一步：双读快照（两次完整读取 + hash 比对，任一次不一致则本轮失败）
        StableTriangleMeshSnapshot snapshot;
        bool exportStable = false;
        if (!ReadStableTriangleMeshSnapshot(source.VerticesAddr, source.TrianglesAddr,
                                            source.VertexCount, indexCount, indexSizeBytes, snapshot, exportStable)) {
            continue;
        }
        // 导出需要稳定数据，非稳定快照跳过
        if (!exportStable) continue;

        // 第二步：解码索引
        std::vector<uint32_t> decodedIndices;
        if (!DecodeTriangleIndices(snapshot.RawIndices, indexSizeBytes, indexCount,
                                   source.VertexCount, decodedIndices)) {
            continue;
        }

        // 第三步：验证数据合法性
        if (!ValidateTriangleMeshData(snapshot.Vertices, decodedIndices)) {
            continue;
        }

        // 第四步：终验——再次抽样顶点，确认数据在读取后未发生变化
        const size_t vc = snapshot.Vertices.size();
        bool stable = true;
        if (vc > 0) {
            const size_t step = std::max<size_t>(vc / 4, 1);
            for (size_t idx = 0; idx < vc && stable; idx += step) {
                Vec3 reread{};
                if (GetDriverManager().read(source.VerticesAddr + idx * sizeof(Vec3),
                                       &reread, sizeof(reread))) {
                    const Vec3& orig = snapshot.Vertices[idx];
                    if (reread.X != orig.X || reread.Y != orig.Y || reread.Z != orig.Z)
                        stable = false;
                }
            }
        }
        if (!stable) continue;

        out.Vertices = std::move(snapshot.Vertices);
        out.Indices = std::move(decodedIndices);
        return true;
    }
    return false;
}

static bool ReadHeightFieldForExportSafe(const HeightFieldSourceInfo& source, CachedHeightFieldData& out) {
    if (source.Rows < 2 || source.Columns < 2 || source.SamplesAddr == 0) return false;

    const uint64_t sampleCount64 = static_cast<uint64_t>(source.Rows) * static_cast<uint64_t>(source.Columns);
    if (sampleCount64 == 0 || sampleCount64 > (1ULL << 24)) return false;

    const size_t sampleCount = static_cast<size_t>(sampleCount64);
    const size_t sampleBytes = sampleCount * sizeof(PhysXHeightFieldSample);
    constexpr int kMaxRounds = 3;

    for (int round = 0; round < kMaxRounds; ++round) {
        // 第一步：第一次完整读取
        std::vector<PhysXHeightFieldSample> first(sampleCount);
        bool firstOk = ReadRemoteBufferRobust(source.SamplesAddr, first.data(), sampleBytes);
        if (!firstOk) {
            // 批量读取失败，逐个元素读取
            firstOk = true;
            for (size_t i = 0; i < sampleCount; ++i) {
                if (!ReadRemoteArrayElementSafe(source.SamplesAddr, i, first[i])) {
                    firstOk = false;
                    break;
                }
            }
        }
        if (!firstOk) continue;
        const uint64_t firstHash = HashBytesFnv1a(first.data(), sampleBytes);

        // 第二步：第二次完整读取 + hash 比对
        std::vector<PhysXHeightFieldSample> second(sampleCount);
        bool secondOk = ReadRemoteBufferRobust(source.SamplesAddr, second.data(), sampleBytes);
        if (!secondOk) {
            secondOk = true;
            for (size_t i = 0; i < sampleCount; ++i) {
                if (!ReadRemoteArrayElementSafe(source.SamplesAddr, i, second[i])) {
                    secondOk = false;
                    break;
                }
            }
        }
        if (!secondOk) continue;
        const uint64_t secondHash = HashBytesFnv1a(second.data(), sampleBytes);

        if (firstHash != secondHash) continue;  // 两次读取数据不一致

        // 第三步：验证——检测全零 sample（buffer 未初始化）
        size_t zeroCount = 0;
        for (size_t i = 0; i < sampleCount; ++i) {
            if (second[i].Height == 0 && second[i].Material0 == 0 && second[i].Material1 == 0)
                ++zeroCount;
        }
        // 超过一半的 sample 全零说明 buffer 未就绪
        if (zeroCount > sampleCount / 2) continue;

        // 第四步：抽样终验——再读几个 sample 确认数据未在读后变化
        bool stable = true;
        const size_t step = std::max<size_t>(sampleCount / 8, 1);
        for (size_t idx = 0; idx < sampleCount && stable; idx += step) {
            PhysXHeightFieldSample reread{};
            if (GetDriverManager().read(source.SamplesAddr + idx * sizeof(PhysXHeightFieldSample),
                                   &reread, sizeof(reread))) {
                if (reread.Height != second[idx].Height ||
                    reread.Material0 != second[idx].Material0 ||
                    reread.Material1 != second[idx].Material1) {
                    stable = false;
                }
            }
        }
        if (!stable) continue;

        // 填充输出
        out.Rows = source.Rows;
        out.Columns = source.Columns;
        out.SampleStride = source.SampleStride != 0 ? source.SampleStride : sizeof(PhysXHeightFieldSample);
        out.SampleCount = source.SampleCount != 0 ? source.SampleCount : static_cast<uint32_t>(sampleCount);
        out.ModifyCount = source.ModifyCount;
        out.SamplesAddr = source.SamplesAddr;
        out.Samples = std::move(second);
        return true;
    }
    return false;
}

static bool IsValidHeightFieldHeader(const PhysXHeightFieldHeaderSnapshot& header) {
    if (header.Rows < 2 || header.Columns < 2) return false;
    if (header.Rows > 8192 || header.Columns > 8192) return false;
    if (header.SamplesAddr == 0) return false;

    const uint64_t expectedCount = static_cast<uint64_t>(header.Rows) * static_cast<uint64_t>(header.Columns);
    if (expectedCount == 0 || expectedCount > 4ULL * 1024ULL * 1024ULL) return false;
    return true;
}

static bool ReadHeightFieldHeaderSnapshot(uint64_t heightField, PhysXHeightFieldHeaderSnapshot& out) {
    if (heightField == 0) return false;

    std::array<uint8_t, 0x78> raw{};
    if (!ReadRemoteBufferRobust(heightField, raw.data(), raw.size())) {
        return false;
    }

    auto read_u32 = [&](size_t off) -> uint32_t {
        uint32_t value = 0;
        std::memcpy(&value, raw.data() + off, sizeof(value));
        return value;
    };
    auto read_u64 = [&](size_t off) -> uint64_t {
        uint64_t value = 0;
        std::memcpy(&value, raw.data() + off, sizeof(value));
        return value;
    };
    auto read_f32 = [&](size_t off) -> float {
        float value = 0.0f;
        std::memcpy(&value, raw.data() + off, sizeof(value));
        return value;
    };

    out.Rows = read_u32(offset.PxHeightFieldRows);
    out.Columns = read_u32(offset.PxHeightFieldColumns);
    out.SamplesAddr = read_u64(offset.PxHeightFieldSampleBuffer);
    out.Thickness = read_f32(offset.PxHeightFieldThickness);
    out.SampleStride = read_u32(offset.PxHeightFieldSampleStride);
    out.SampleCount = read_u32(offset.PxHeightFieldSampleCount);
    out.ModifyCount = read_u32(offset.PxHeightFieldModifyCount);

    const uint64_t expectedCount = static_cast<uint64_t>(out.Rows) * static_cast<uint64_t>(out.Columns);
    if (out.SampleStride != sizeof(PhysXHeightFieldSample)) {
        out.SampleStride = 0;
    }
    if (out.SampleCount != expectedCount) {
        out.SampleCount = 0;
    }

    return IsValidHeightFieldHeader(out);
}

static bool BuildHeightFieldReadCandidate(uint64_t heightField, const PhysXHeightFieldHeaderSnapshot& header,
                                          HeightFieldReadCandidate& out) {
    const uint64_t sampleCount64 = static_cast<uint64_t>(header.Rows) * static_cast<uint64_t>(header.Columns);
    if (sampleCount64 == 0 || sampleCount64 > 4ULL * 1024ULL * 1024ULL) return false;

    CachedHeightFieldData cache;
    cache.Rows = header.Rows;
    cache.Columns = header.Columns;
    cache.SampleStride = header.SampleStride != 0 ? header.SampleStride : sizeof(PhysXHeightFieldSample);
    cache.SampleCount = header.SampleCount != 0 ? header.SampleCount : static_cast<uint32_t>(sampleCount64);
    cache.ModifyCount = header.ModifyCount;
    cache.SamplesAddr = header.SamplesAddr;
    cache.Samples.resize(static_cast<size_t>(sampleCount64));
    if (!ReadRemoteBufferRobust(header.SamplesAddr, cache.Samples.data(),
                                cache.Samples.size() * sizeof(PhysXHeightFieldSample))) {
        return false;
    }

    out.Data = std::move(cache);
    out.Signature = BuildHeightFieldSignature(heightField, out.Data.Rows, out.Data.Columns, out.Data.Samples);
    out.StableHits = 1;
    return true;
}

static bool WriteHeightFieldObj(const std::filesystem::path& path, const CachedHeightFieldData& cache) {
    if (cache.Rows < 2 || cache.Columns < 2) return false;
    if (cache.Samples.size() != static_cast<size_t>(cache.Rows) * static_cast<size_t>(cache.Columns)) return false;

    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;

    out << "# stable physx heightfield export\n";
    out << "o " << path.stem().string() << "\n";

    const auto vertex_index = [&](uint32_t row, uint32_t column) -> size_t {
        return static_cast<size_t>(row) * static_cast<size_t>(cache.Columns) + static_cast<size_t>(column);
    };
    const auto sample_at = [&](uint32_t row, uint32_t column) -> const PhysXHeightFieldSample& {
        return cache.Samples[vertex_index(row, column)];
    };

    for (uint32_t row = 0; row < cache.Rows; ++row) {
        for (uint32_t column = 0; column < cache.Columns; ++column) {
            const PhysXHeightFieldSample& sample = sample_at(row, column);
            out << "v " << static_cast<float>(row) << ' '
                << static_cast<float>(sample.Height) << ' '
                << static_cast<float>(column) << "\n";
        }
    }

    size_t faceCount = 0;
    for (uint32_t row = 0; row + 1 < cache.Rows; ++row) {
        for (uint32_t column = 0; column + 1 < cache.Columns; ++column) {
            const size_t i00 = vertex_index(row, column) + 1;
            const size_t i10 = vertex_index(row + 1, column) + 1;
            const size_t i01 = vertex_index(row, column + 1) + 1;
            const size_t i11 = vertex_index(row + 1, column + 1) + 1;
            const PhysXHeightFieldSample& s00 = sample_at(row, column);
            if (HeightFieldTessFlag(s00)) {
                if (HeightFieldMaterial0(s00) != kPhysXHeightFieldHoleMaterial) {
                    out << "f " << i10 << ' ' << i00 << ' ' << i11 << "\n";
                    ++faceCount;
                }
                if (HeightFieldMaterial1(s00) != kPhysXHeightFieldHoleMaterial) {
                    out << "f " << i01 << ' ' << i11 << ' ' << i00 << "\n";
                    ++faceCount;
                }
            } else {
                if (HeightFieldMaterial0(s00) != kPhysXHeightFieldHoleMaterial) {
                    out << "f " << i00 << ' ' << i01 << ' ' << i10 << "\n";
                    ++faceCount;
                }
                if (HeightFieldMaterial1(s00) != kPhysXHeightFieldHoleMaterial) {
                    out << "f " << i11 << ' ' << i10 << ' ' << i01 << "\n";
                    ++faceCount;
                }
            }
        }
    }

    if (!(static_cast<bool>(out) && faceCount > 0)) return false;

    LocalObjMeshData verify;
    return LoadObjMeshFile(path, verify);
}

static bool LoadObjMeshFile(const std::filesystem::path& path, LocalObjMeshData& out) {
    std::ifstream in(path);
    if (!in) return false;

    LocalObjMeshData mesh;
    std::string line;
    while (std::getline(in, line)) {
        if (line.size() < 2) continue;
        if (line[0] == 'v' && line[1] == ' ') {
            std::istringstream iss(line.substr(2));
            Vec3 v{};
            if (iss >> v.X >> v.Y >> v.Z) {
                mesh.Vertices.push_back(v);
            }
            continue;
        }
        if (line[0] == 'f' && line[1] == ' ') {
            std::istringstream iss(line.substr(2));
            std::string token;
            uint32_t face[3]{};
            int count = 0;
            while (count < 3 && iss >> token) {
                const size_t slash = token.find('/');
                if (slash != std::string::npos) {
                    token.resize(slash);
                }
                if (token.empty()) break;
                int index = 0;
                try {
                    index = std::stoi(token);
                } catch (...) {
                    index = 0;
                }
                if (index <= 0) {
                    count = 0;
                    break;
                }
                face[count++] = static_cast<uint32_t>(index - 1);
            }
            if (count == 3 &&
                face[0] < mesh.Vertices.size() &&
                face[1] < mesh.Vertices.size() &&
                face[2] < mesh.Vertices.size()) {
                mesh.Indices.push_back(face[0]);
                mesh.Indices.push_back(face[1]);
                mesh.Indices.push_back(face[2]);
            }
        }
    }

    if (!ValidateTriangleMeshData(mesh.Vertices, mesh.Indices)) return false;
    out = std::move(mesh);
    return true;
}

static bool WriteTriangleMeshMetaFile(const std::filesystem::path& path, uint64_t signature,
                                      uint32_t vertexCount, uint32_t triangleCount, uint8_t flags) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;
    out << "signature " << signature << "\n";
    out << "vertex_count " << vertexCount << "\n";
    out << "triangle_count " << triangleCount << "\n";
    out << "flags " << static_cast<uint32_t>(flags) << "\n";
    return static_cast<bool>(out);
}

static bool WriteHeightFieldMetaFile(const std::filesystem::path& path, uint64_t signature,
                                     uint32_t rows, uint32_t columns) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;
    out << "signature " << signature << "\n";
    out << "rows " << rows << "\n";
    out << "columns " << columns << "\n";
    return static_cast<bool>(out);
}

static void EnsureLocalExportMetaLoaded() {
    if (gLocalExportMetaLoaded) return;
    gLocalExportMetaLoaded = true;
    gLocalTriMetaBySig.clear();
    gLocalHfMetaBySig.clear();

    std::error_code ec;
    const auto exportDir = GetPhysXExportDir();
    if (!std::filesystem::exists(exportDir, ec)) return;

    for (const auto& entry : std::filesystem::directory_iterator(exportDir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        const auto& path = entry.path();
        if (path.extension() != ".txt") continue;
        const std::string name = path.filename().string();

        std::ifstream in(path);
        if (!in) continue;

        if (name.rfind("tri_", 0) == 0 && name.size() > 9 && name.find(".meta.txt") != std::string::npos) {
            LocalTriangleMeshExportMeta meta;
            std::string key;
            uint32_t flags = 0;
            while (in >> key) {
                if (key == "signature") in >> meta.Signature;
                else if (key == "vertex_count") in >> meta.VertexCount;
                else if (key == "triangle_count") in >> meta.TriangleCount;
                else if (key == "flags") in >> flags;
                else {
                    std::string rest;
                    std::getline(in, rest);
                }
            }
            meta.Flags = static_cast<uint8_t>(flags);
            meta.Valid = meta.Signature != 0 && meta.VertexCount > 0 && meta.TriangleCount > 0;
            if (meta.Valid) gLocalTriMetaBySig[meta.Signature] = meta;
            continue;
        }

        if (name.rfind("hf_", 0) == 0 && name.size() > 8 && name.find(".meta.txt") != std::string::npos) {
            LocalHeightFieldExportMeta meta;
            std::string key;
            while (in >> key) {
                if (key == "signature") in >> meta.Signature;
                else if (key == "rows") in >> meta.Rows;
                else if (key == "columns") in >> meta.Columns;
                else {
                    std::string rest;
                    std::getline(in, rest);
                }
            }
            meta.Valid = meta.Signature != 0 && meta.Rows > 0 && meta.Columns > 0;
            if (meta.Valid) gLocalHfMetaBySig[meta.Signature] = meta;
        }
    }
}

static bool ResolveTriangleMeshSignatureFromLocalMeta(uint64_t mesh, uint32_t vertexCount,
                                                      uint32_t triangleCount, uint8_t flags,
                                                      uint64_t& outSignature) {
    auto sigIt = gTriangleMeshSignatures.find(mesh);
    if (sigIt != gTriangleMeshSignatures.end()) {
        outSignature = sigIt->second;
        return true;
    }

    EnsureLocalExportMetaLoaded();
    const uint64_t key = BuildTriangleMeshLocalLookupKey(vertexCount, triangleCount, flags);
    uint64_t matchedSignature = 0;
    for (const auto& [sig, meta] : gLocalTriMetaBySig) {
        if (!meta.Valid) continue;
        if (BuildTriangleMeshLocalLookupKey(meta.VertexCount, meta.TriangleCount, meta.Flags) != key) continue;
        if (matchedSignature != 0) return false;
        matchedSignature = sig;
    }
    if (matchedSignature == 0) return false;
    gTriangleMeshSignatures[mesh] = matchedSignature;
    outSignature = matchedSignature;
    return true;
}

static bool ResolveHeightFieldSignatureFromLocalMeta(uint64_t heightField, uint32_t rows,
                                                     uint32_t columns, uint64_t& outSignature) {
    auto sigIt = gHeightFieldSignatures.find(heightField);
    if (sigIt != gHeightFieldSignatures.end()) {
        outSignature = sigIt->second;
        return true;
    }

    EnsureLocalExportMetaLoaded();
    const uint64_t key = BuildHeightFieldLocalLookupKey(rows, columns);
    uint64_t matchedSignature = 0;
    for (const auto& [sig, meta] : gLocalHfMetaBySig) {
        if (!meta.Valid) continue;
        if (BuildHeightFieldLocalLookupKey(meta.Rows, meta.Columns) != key) continue;
        if (matchedSignature != 0) return false;
        matchedSignature = sig;
    }
    if (matchedSignature == 0) return false;
    gHeightFieldSignatures[heightField] = matchedSignature;
    outSignature = matchedSignature;
    return true;
}

static const LocalObjMeshData* GetLocalTriangleMeshData(uint64_t mesh) {
    auto sigIt = gTriangleMeshSignatures.find(mesh);
    if (sigIt == gTriangleMeshSignatures.end()) {
        const uint32_t vertexCount = GetDriverManager().read<uint32_t>(mesh + offset.PxTriangleMeshVertexCount);
        const uint32_t triangleCount = GetDriverManager().read<uint32_t>(mesh + offset.PxTriangleMeshTriangleCount);
        const uint64_t verticesAddr = GetDriverManager().read<uint64_t>(mesh + offset.PxTriangleMeshVertices);
        const uint64_t trianglesAddr = GetDriverManager().read<uint64_t>(mesh + offset.PxTriangleMeshTriangles);
        const uint8_t flags = GetDriverManager().read<uint8_t>(mesh + offset.PxTriangleMeshFlags);
        if (vertexCount == 0 || triangleCount == 0 || verticesAddr == 0 || trianglesAddr == 0) return nullptr;

        CachedTriangleMeshData cache;
        if (!GetCachedTriangleMesh(mesh, vertexCount, triangleCount, flags, verticesAddr, trianglesAddr, cache)) {
            return nullptr;
        }
        sigIt = gTriangleMeshSignatures.find(mesh);
        if (sigIt == gTriangleMeshSignatures.end()) return nullptr;
    }
    const uint64_t sig = sigIt->second;

    auto cacheIt = gLocalTriObjCache.find(sig);
    if (cacheIt != gLocalTriObjCache.end()) return &cacheIt->second;

    // 如果已在异步加载中，返回 nullptr 等待下一帧
    if (gInFlightMeshSigs.find(sig) != gInFlightMeshSigs.end()) return nullptr;

    // 启动异步文件加载
    char name[64];
    std::snprintf(name, sizeof(name), "tri_%016llx.obj", static_cast<unsigned long long>(sig));
    auto path = GetPhysXExportDir() / name;
    gInFlightMeshSigs.insert(sig);
    gPendingMeshLoads.push_back({sig, std::async(std::launch::async, [path, sig]() {
        LocalObjMeshData loaded;
        if (!LoadObjMeshFile(path, loaded)) {
            auto memIt = gTriMeshCacheBySig.find(sig);
            if (memIt != gTriMeshCacheBySig.end()) {
                loaded.Vertices = memIt->second.Vertices;
                loaded.Indices = memIt->second.Indices;
            }
        }
        return loaded;
    })});
    return nullptr;
}

static const LocalObjMeshData* GetLocalTriangleMeshDataStrict(uint64_t mesh) {
    uint64_t sig = 0;
    auto sigIt = gTriangleMeshSignatures.find(mesh);
    if (sigIt != gTriangleMeshSignatures.end()) {
        sig = sigIt->second;
    } else {
        const uint32_t vertexCount = GetDriverManager().read<uint32_t>(mesh + offset.PxTriangleMeshVertexCount);
        const uint32_t triangleCount = GetDriverManager().read<uint32_t>(mesh + offset.PxTriangleMeshTriangleCount);
        const uint8_t flags = GetDriverManager().read<uint8_t>(mesh + offset.PxTriangleMeshFlags);

        // 先尝试按 count 快速匹配
        if (!ResolveTriangleMeshSignatureFromLocalMeta(mesh, vertexCount, triangleCount, flags, sig)) {
            // 快速匹配失败（无匹配或多个匹配），回退到读取内存计算真实内容哈希
            const uint64_t verticesAddr = GetDriverManager().read<uint64_t>(mesh + offset.PxTriangleMeshVertices);
            const uint64_t trianglesAddr = GetDriverManager().read<uint64_t>(mesh + offset.PxTriangleMeshTriangles);
            if (vertexCount == 0 || triangleCount == 0 || verticesAddr == 0 || trianglesAddr == 0) return nullptr;

            CachedTriangleMeshData cache;
            if (!GetCachedTriangleMesh(mesh, vertexCount, triangleCount, flags, verticesAddr, trianglesAddr, cache)) {
                return nullptr;
            }
            sigIt = gTriangleMeshSignatures.find(mesh);
            if (sigIt == gTriangleMeshSignatures.end()) return nullptr;
            sig = sigIt->second;
        }
    }

    auto cacheIt = gLocalTriObjCache.find(sig);
    if (cacheIt != gLocalTriObjCache.end()) return &cacheIt->second;

    // 如果已在异步加载中，返回 nullptr 等待下一帧
    if (gInFlightMeshSigs.find(sig) != gInFlightMeshSigs.end()) return nullptr;

    // 启动异步文件加载
    char name[64];
    std::snprintf(name, sizeof(name), "tri_%016llx.obj", static_cast<unsigned long long>(sig));
    auto path = GetPhysXExportDir() / name;
    gInFlightMeshSigs.insert(sig);
    gPendingMeshLoads.push_back({sig, std::async(std::launch::async, [path]() {
        LocalObjMeshData loaded;
        LoadObjMeshFile(path, loaded);
        return loaded;
    })});
    return nullptr;
}

static const LocalTriangleMeshBvhData* GetLocalTriangleMeshBvhData(uint64_t mesh) {
    auto sigIt = gTriangleMeshSignatures.find(mesh);
    if (sigIt == gTriangleMeshSignatures.end()) {
        const uint32_t vertexCount = GetDriverManager().read<uint32_t>(mesh + offset.PxTriangleMeshVertexCount);
        const uint32_t triangleCount = GetDriverManager().read<uint32_t>(mesh + offset.PxTriangleMeshTriangleCount);
        const uint64_t verticesAddr = GetDriverManager().read<uint64_t>(mesh + offset.PxTriangleMeshVertices);
        const uint64_t trianglesAddr = GetDriverManager().read<uint64_t>(mesh + offset.PxTriangleMeshTriangles);
        const uint8_t flags = GetDriverManager().read<uint8_t>(mesh + offset.PxTriangleMeshFlags);
        if (vertexCount == 0 || triangleCount == 0 || verticesAddr == 0 || trianglesAddr == 0) return nullptr;

        CachedTriangleMeshData cache;
        if (!GetCachedTriangleMesh(mesh, vertexCount, triangleCount, flags, verticesAddr, trianglesAddr, cache)) {
            return nullptr;
        }
        sigIt = gTriangleMeshSignatures.find(mesh);
        if (sigIt == gTriangleMeshSignatures.end()) return nullptr;
    }
    const uint64_t sig = sigIt->second;

    auto cacheIt = gLocalTriBvhCache.find(sig);
    if (cacheIt != gLocalTriBvhCache.end()) {
        return cacheIt->second.Valid ? &cacheIt->second : nullptr;
    }

    if (gInFlightBvhSigs.find(sig) != gInFlightBvhSigs.end()) return nullptr;

    char name[64];
    std::snprintf(name, sizeof(name), "tri_%016llx.bvh.txt", static_cast<unsigned long long>(sig));
    auto path = GetPhysXExportDir() / name;
    gInFlightBvhSigs.insert(sig);
    gPendingBvhLoads.push_back({sig, std::async(std::launch::async, [path]() {
        LocalTriangleMeshBvhData loaded;
        LoadBvhFile(path, loaded);
        return loaded;
    })});
    return nullptr;
}

static const LocalTriangleMeshBvhData* GetLocalTriangleMeshBvhDataStrict(uint64_t mesh) {
    uint64_t sig = 0;
    auto sigIt = gTriangleMeshSignatures.find(mesh);
    if (sigIt != gTriangleMeshSignatures.end()) {
        sig = sigIt->second;
    } else {
        const uint32_t vertexCount = GetDriverManager().read<uint32_t>(mesh + offset.PxTriangleMeshVertexCount);
        const uint32_t triangleCount = GetDriverManager().read<uint32_t>(mesh + offset.PxTriangleMeshTriangleCount);
        const uint8_t flags = GetDriverManager().read<uint8_t>(mesh + offset.PxTriangleMeshFlags);

        if (!ResolveTriangleMeshSignatureFromLocalMeta(mesh, vertexCount, triangleCount, flags, sig)) {
            const uint64_t verticesAddr = GetDriverManager().read<uint64_t>(mesh + offset.PxTriangleMeshVertices);
            const uint64_t trianglesAddr = GetDriverManager().read<uint64_t>(mesh + offset.PxTriangleMeshTriangles);
            if (vertexCount == 0 || triangleCount == 0 || verticesAddr == 0 || trianglesAddr == 0) return nullptr;

            CachedTriangleMeshData cache;
            if (!GetCachedTriangleMesh(mesh, vertexCount, triangleCount, flags, verticesAddr, trianglesAddr, cache)) {
                return nullptr;
            }
            sigIt = gTriangleMeshSignatures.find(mesh);
            if (sigIt == gTriangleMeshSignatures.end()) return nullptr;
            sig = sigIt->second;
        }
    }

    auto cacheIt = gLocalTriBvhCache.find(sig);
    if (cacheIt != gLocalTriBvhCache.end()) {
        return cacheIt->second.Valid ? &cacheIt->second : nullptr;
    }

    if (gInFlightBvhSigs.find(sig) != gInFlightBvhSigs.end()) return nullptr;

    char name[64];
    std::snprintf(name, sizeof(name), "tri_%016llx.bvh.txt", static_cast<unsigned long long>(sig));
    auto path = GetPhysXExportDir() / name;
    gInFlightBvhSigs.insert(sig);
    gPendingBvhLoads.push_back({sig, std::async(std::launch::async, [path]() {
        LocalTriangleMeshBvhData loaded;
        LoadBvhFile(path, loaded);
        return loaded;
    })});
    return nullptr;
}

static const LocalObjMeshData* GetLocalHeightFieldData(uint64_t heightField) {
    auto sigIt = gHeightFieldSignatures.find(heightField);
    if (sigIt == gHeightFieldSignatures.end()) {
        CachedHeightFieldData cache;
        if (!GetCachedHeightField(heightField, cache)) return nullptr;
        sigIt = gHeightFieldSignatures.find(heightField);
        if (sigIt == gHeightFieldSignatures.end()) return nullptr;
    }
    const uint64_t sig = sigIt->second;

    auto cacheIt = gLocalHfObjCache.find(sig);
    if (cacheIt != gLocalHfObjCache.end()) return &cacheIt->second;

    if (gInFlightHfSigs.find(sig) != gInFlightHfSigs.end()) return nullptr;

    char name[64];
    std::snprintf(name, sizeof(name), "hf_%016llx.obj", static_cast<unsigned long long>(sig));
    auto path = GetPhysXExportDir() / name;
    gInFlightHfSigs.insert(sig);
    gPendingHfLoads.push_back({sig, std::async(std::launch::async, [path]() {
        LocalObjMeshData loaded;
        LoadObjMeshFile(path, loaded);
        return loaded;
    })});
    return nullptr;
}

static const LocalObjMeshData* GetLocalHeightFieldDataStrict(uint64_t heightField) {
    uint64_t sig = 0;
    auto sigIt = gHeightFieldSignatures.find(heightField);
    if (sigIt != gHeightFieldSignatures.end()) {
        sig = sigIt->second;
    } else {
        const uint32_t rows = GetDriverManager().read<uint32_t>(heightField + offset.PxHeightFieldRows);
        const uint32_t columns = GetDriverManager().read<uint32_t>(heightField + offset.PxHeightFieldColumns);

        if (!ResolveHeightFieldSignatureFromLocalMeta(heightField, rows, columns, sig)) {
            CachedHeightFieldData cache;
            if (!GetCachedHeightField(heightField, cache)) return nullptr;
            sigIt = gHeightFieldSignatures.find(heightField);
            if (sigIt == gHeightFieldSignatures.end()) return nullptr;
            sig = sigIt->second;
        }
    }

    auto cacheIt = gLocalHfObjCache.find(sig);
    if (cacheIt != gLocalHfObjCache.end()) return &cacheIt->second;

    if (gInFlightHfSigs.find(sig) != gInFlightHfSigs.end()) return nullptr;

    char name[64];
    std::snprintf(name, sizeof(name), "hf_%016llx.obj", static_cast<unsigned long long>(sig));
    auto path = GetPhysXExportDir() / name;
    gInFlightHfSigs.insert(sig);
    gPendingHfLoads.push_back({sig, std::async(std::launch::async, [path]() {
        LocalObjMeshData loaded;
        LoadObjMeshFile(path, loaded);
        return loaded;
    })});
    return nullptr;
}

static bool LoadBvhFile(const std::filesystem::path& path, LocalTriangleMeshBvhData& out) {
    std::ifstream in(path);
    if (!in) return false;

    LocalTriangleMeshBvhData data;
    std::string token;
    uint32_t totalPages = 0;
    while (in >> token) {
        if (token == "bounds_min") {
            float w = 0.0f;
            in >> data.RootBounds.Min.X >> data.RootBounds.Min.Y >> data.RootBounds.Min.Z >> w;
        } else if (token == "bounds_max") {
            float w = 0.0f;
            in >> data.RootBounds.Max.X >> data.RootBounds.Max.Y >> data.RootBounds.Max.Z >> w;
        } else if (token == "rtree_total_pages") {
            in >> totalPages;
        } else if (token == "page") {
            uint32_t pageIndex = 0;
            in >> pageIndex;
            float minx[4]{}, miny[4]{}, minz[4]{}, maxx[4]{}, maxy[4]{}, maxz[4]{};
            uint32_t ptrs[4]{};
            std::string field;
            in >> field;
            if (field != "minx") return false;
            for (float& v : minx) in >> v;
            in >> field;
            if (field != "miny") return false;
            for (float& v : miny) in >> v;
            in >> field;
            if (field != "minz") return false;
            for (float& v : minz) in >> v;
            in >> field;
            if (field != "maxx") return false;
            for (float& v : maxx) in >> v;
            in >> field;
            if (field != "maxy") return false;
            for (float& v : maxy) in >> v;
            in >> field;
            if (field != "maxz") return false;
            for (float& v : maxz) in >> v;
            in >> field;
            if (field != "ptrs") return false;
            for (uint32_t& v : ptrs) in >> v;

            for (int lane = 0; lane < 4; ++lane) {
                if (minx[lane] > maxx[lane] || miny[lane] > maxy[lane] || minz[lane] > maxz[lane]) continue;
                LocalBvhNodeBounds node{};
                node.Bounds.Min = {minx[lane], miny[lane], minz[lane]};
                node.Bounds.Max = {maxx[lane], maxy[lane], maxz[lane]};
                node.Ptr = ptrs[lane];
                data.Nodes.push_back(node);
            }
        } else {
            std::string rest;
            std::getline(in, rest);
        }
    }

    data.Valid = totalPages > 0 &&
                 !data.Nodes.empty() &&
                 data.RootBounds.Min.X <= data.RootBounds.Max.X &&
                 data.RootBounds.Min.Y <= data.RootBounds.Max.Y &&
                 data.RootBounds.Min.Z <= data.RootBounds.Max.Z;
    if (!data.Valid) return false;
    out = std::move(data);
    return true;
}

bool ExportStablePhysXMeshes() {
    if (!EnsurePhysXExportDir()) {
        gStablePhysXExportStatus = "导出目录创建失败";
        return false;
    }
    if (address.libUE4 == 0) {
        gStablePhysXExportStatus = "驱动未就绪";
        return false;
    }

    const uint64_t uworld = GetDriverManager().read<uint64_t>(address.libUE4 + offset.Gworld);
    std::vector<uint64_t> pxScenes;
    CollectActivePxScenes(address.libUE4, uworld, pxScenes);
    if (pxScenes.empty()) {
        gStablePhysXExportStatus = "PxScene 未找到";
        return false;
    }

    // 收集场景中所有唯一的 TriangleMesh / HeightField
    std::unordered_set<uint64_t> seenTriMeshes;
    std::unordered_set<uint64_t> seenHeightFields;

    struct TriMeshExportInfo {
        uint64_t mesh;
        TriangleMeshSourceInfo source;
    };
    struct HfExportInfo {
        uint64_t heightField;
        HeightFieldSourceInfo source;
    };
    std::vector<TriMeshExportInfo> triMeshes;
    std::vector<HfExportInfo> hfMeshes;

    for (uint64_t pxScene : pxScenes) {
        const uint64_t sqm = pxScene + offset.NpSceneQueriesSceneQueryManager;
        const uint64_t pruner = GetDriverManager().read<uint64_t>(sqm + offset.PrunerExtPruner);
        const uint32_t prunerType = GetDriverManager().read<uint32_t>(sqm + offset.PrunerExtType);
        uint64_t pool = 0;
        if (prunerType == 1) pool = pruner + offset.AABBPrunerPool;
        else if (prunerType == 0) pool = pruner + offset.BucketPrunerPool;

        if (pruner != 0 && pool != 0) {
            const uint32_t objectCount = GetDriverManager().read<uint32_t>(pool + offset.PruningPoolNbObjects);
            const uint64_t objectsAddr = GetDriverManager().read<uint64_t>(pool + offset.PruningPoolObjects);
            if (objectCount > 0 &&
                objectCount <= static_cast<uint32_t>(std::max(gPhysXMaxPrunerObjectCount, 1)) &&
                objectsAddr != 0) {
                std::vector<PhysXPrunerPayload> payloads(objectCount);
                if (ReadRemoteBufferRobust(objectsAddr, payloads.data(), payloads.size() * sizeof(PhysXPrunerPayload))) {
                    for (uint32_t i = 0; i < objectCount; ++i) {
                        const PhysXPrunerPayload& payload = payloads[i];
                        if (payload.Shape == 0) continue;

                        const uint64_t geometryAddr = payload.Shape + offset.ScbShapeCoreGeometry;
                        const uint32_t geometryType = GetDriverManager().read<uint32_t>(geometryAddr);

                        if (geometryType == PX_GEOM_TRIANGLEMESH) {
                            const uint64_t mesh = GetDriverManager().read<uint64_t>(geometryAddr + offset.PxTriangleMeshGeometryTriangleMesh);
                            if (mesh == 0 || seenTriMeshes.count(mesh)) continue;
                            seenTriMeshes.insert(mesh);

                            const uint32_t vertexCount = GetDriverManager().read<uint32_t>(mesh + offset.PxTriangleMeshVertexCount);
                            const uint32_t triangleCount = GetDriverManager().read<uint32_t>(mesh + offset.PxTriangleMeshTriangleCount);
                            const uint64_t verticesAddr = GetDriverManager().read<uint64_t>(mesh + offset.PxTriangleMeshVertices);
                            const uint64_t trianglesAddr = GetDriverManager().read<uint64_t>(mesh + offset.PxTriangleMeshTriangles);
                            const uint8_t flags = GetDriverManager().read<uint8_t>(mesh + offset.PxTriangleMeshFlags);
                            if (vertexCount == 0 || triangleCount == 0 || verticesAddr == 0 || trianglesAddr == 0) continue;
                            if (triangleCount > 500000) continue;

                            triMeshes.push_back({mesh, {vertexCount, triangleCount, flags, verticesAddr, trianglesAddr}});
                        } else if (geometryType == PX_GEOM_HEIGHTFIELD) {
                            const uint64_t heightField = GetDriverManager().read<uint64_t>(geometryAddr + offset.PxHeightFieldGeometryHeightField);
                            if (heightField == 0 || seenHeightFields.count(heightField)) continue;
                            seenHeightFields.insert(heightField);

                            PhysXHeightFieldHeaderSnapshot header;
                            if (!ReadHeightFieldHeaderSnapshot(heightField, header)) continue;
                            if (!IsValidHeightFieldHeader(header)) continue;

                            hfMeshes.push_back({heightField, {header.Rows, header.Columns, header.SampleStride,
                                                               header.SampleCount, header.ModifyCount, header.SamplesAddr,
                                                               header.Thickness}});
                        }
                    }
                }
            }
        }

        std::vector<uint64_t> actors;
        if (CollectPxSceneActorPointers(pxScene, actors)) {
                for (uint64_t actor : actors) {
                if (actor == 0) continue;
                const uint16_t shapeCount = GetDriverManager().read<uint16_t>(actor + offset.PxActorShapeCount);
                if (shapeCount == 0 || shapeCount > 64) continue;

                std::vector<uint64_t> shapes(std::min<uint16_t>(shapeCount, 64));
                if (shapeCount == 1) {
                    shapes[0] = GetDriverManager().read<uint64_t>(actor + offset.PxActorShapes);
                } else {
                    const uint64_t shapePtrArray = GetDriverManager().read<uint64_t>(actor + offset.PxActorShapes);
                    if (shapePtrArray == 0) continue;
                    if (!ReadRemoteBufferRobust(shapePtrArray, shapes.data(), shapes.size() * sizeof(uint64_t))) continue;
                }

                    for (uint64_t shape : shapes) {
                        if (shape == 0) continue;
                        uint64_t geometryAddr = shape + offset.PxShapeGeometryInline;
                        const uint32_t npShapeFlags = GetDriverManager().read<uint32_t>(shape + offset.PxShapeFlags);
                        if ((npShapeFlags & 0x1u) != 0) {
                            const uint64_t corePtr = GetDriverManager().read<uint64_t>(shape + offset.PxShapeCorePtr);
                            if (corePtr == 0) continue;
                            geometryAddr = corePtr + offset.PxShapeCoreGeometry;
                        }
                        const uint32_t geometryType = GetDriverManager().read<uint32_t>(geometryAddr);

                        if (geometryType == PX_GEOM_TRIANGLEMESH) {
                            const uint64_t mesh = GetDriverManager().read<uint64_t>(geometryAddr + offset.PxTriangleMeshGeometryTriangleMesh);
                            if (mesh == 0 || seenTriMeshes.count(mesh)) continue;
                            seenTriMeshes.insert(mesh);

                            const uint32_t vertexCount = GetDriverManager().read<uint32_t>(mesh + offset.PxTriangleMeshVertexCount);
                            const uint32_t triangleCount = GetDriverManager().read<uint32_t>(mesh + offset.PxTriangleMeshTriangleCount);
                            const uint64_t verticesAddr = GetDriverManager().read<uint64_t>(mesh + offset.PxTriangleMeshVertices);
                            const uint64_t trianglesAddr = GetDriverManager().read<uint64_t>(mesh + offset.PxTriangleMeshTriangles);
                            const uint8_t flags = GetDriverManager().read<uint8_t>(mesh + offset.PxTriangleMeshFlags);
                            if (vertexCount == 0 || triangleCount == 0 || verticesAddr == 0 || trianglesAddr == 0) continue;
                            if (triangleCount > 500000) continue;

                            triMeshes.push_back({mesh, {vertexCount, triangleCount, flags, verticesAddr, trianglesAddr}});

                        } else if (geometryType == PX_GEOM_HEIGHTFIELD) {
                            const uint64_t heightField = GetDriverManager().read<uint64_t>(geometryAddr + offset.PxHeightFieldGeometryHeightField);
                            if (heightField == 0 || seenHeightFields.count(heightField)) continue;
                            seenHeightFields.insert(heightField);

                            PhysXHeightFieldHeaderSnapshot header;
                            if (!ReadHeightFieldHeaderSnapshot(heightField, header)) continue;
                            if (!IsValidHeightFieldHeader(header)) continue;

                            hfMeshes.push_back({heightField, {header.Rows, header.Columns, header.SampleStride,
                                                               header.SampleCount, header.ModifyCount, header.SamplesAddr,
                                                               header.Thickness}});
                        }
                    }
                }
        }
    }

    size_t triExported = 0;
    size_t hfExported = 0;
    size_t bvhExported = 0;
    size_t skipped = 0;

    // 逐个安全读取并导出 TriangleMesh
    for (const auto& info : triMeshes) {
        CachedTriangleMeshData exportMesh;
        if (!ReadTriangleMeshForExportSafe(info.source, exportMesh)) {
            ++skipped;
            continue;
        }
        const uint64_t signature = BuildTriangleMeshSignature(
            info.mesh, info.source.VertexCount, info.source.TriangleCount,
            info.source.Flags, exportMesh.Vertices, exportMesh.Indices);
        gTriangleMeshSignatures[info.mesh] = signature;
        gTriangleMeshSources[info.mesh] = info.source;
        char fileName[64];
        std::snprintf(fileName, sizeof(fileName), "tri_%016llx.obj", static_cast<unsigned long long>(signature));
        const std::filesystem::path objPath = GetPhysXExportDir() / fileName;
        if (WriteTriangleMeshObj(objPath, exportMesh)) {
            std::filesystem::path metaPath = objPath;
            metaPath.replace_extension(".meta.txt");
            WriteTriangleMeshMetaFile(metaPath, signature, info.source.VertexCount, info.source.TriangleCount, info.source.Flags);
            gLocalTriMetaBySig[signature] = LocalTriangleMeshExportMeta{
                signature, info.source.VertexCount, info.source.TriangleCount, info.source.Flags, true
            };
            gLocalExportMetaLoaded = true;
            ++triExported;
            PhysXBVH::ExportedBVHMesh exportedBVH;
            if (PhysXBVH::ExportBVHMeshFromMemory(info.mesh, exportedBVH)) {
                std::filesystem::path bvhPath = objPath;
                bvhPath.replace_extension(".bvh.txt");
                if (PhysXBVH::ExportBVHTextFile(bvhPath, exportedBVH)) {
                    ++bvhExported;
                }
            }
        } else {
            ++skipped;
        }
    }

    // 逐个安全读取并导出 HeightField
    for (const auto& info : hfMeshes) {
        CachedHeightFieldData exportHf;
        if (!ReadHeightFieldForExportSafe(info.source, exportHf)) {
            ++skipped;
            continue;
        }
        const uint64_t signature = BuildHeightFieldSignature(
            info.heightField, exportHf.Rows, exportHf.Columns, exportHf.Samples);
        gHeightFieldSignatures[info.heightField] = signature;
        gHeightFieldSources[info.heightField] = info.source;
        char fileName[64];
        std::snprintf(fileName, sizeof(fileName), "hf_%016llx.obj", static_cast<unsigned long long>(signature));
        const std::filesystem::path objPath = GetPhysXExportDir() / fileName;
        if (WriteHeightFieldObj(objPath, exportHf)) {
            std::filesystem::path metaPath = objPath;
            metaPath.replace_extension(".meta.txt");
            WriteHeightFieldMetaFile(metaPath, signature, exportHf.Rows, exportHf.Columns);
            gLocalHfMetaBySig[signature] = LocalHeightFieldExportMeta{
                signature, exportHf.Rows, exportHf.Columns, true
            };
            gLocalExportMetaLoaded = true;
            ++hfExported;
        } else {
            ++skipped;
        }
    }

    char status[256];
    std::snprintf(status, sizeof(status), "OBJ 导出完成: tri=%zu/%zu bvh=%zu hf=%zu/%zu skipped=%zu -> %s",
                  triExported, triMeshes.size(), bvhExported, hfExported, hfMeshes.size(), skipped,
                  GetPhysXExportDir().string().c_str());
    gStablePhysXExportStatus = status;
    return (triExported + hfExported) > 0;
}

const char* GetStablePhysXExportStatus() {
    return gStablePhysXExportStatus.c_str();
}

static bool IsWithinCenterRegion(const Vec3& worldPos, const FMatrix& vpMat, float screenW, float screenH) {
    Vec2 screenPos;
    if (!WorldToScreen(worldPos, vpMat, screenW, screenH, screenPos)) return false;
    const float cx = screenW * 0.5f;
    const float cy = screenH * 0.5f;
    const float radius = std::min(screenW, screenH) * std::clamp(gPhysXCenterRegionFovDegrees / 90.0f, 0.05f, 0.667f);
    const float dx = screenPos.x - cx;
    const float dy = screenPos.y - cy;
    return dx * dx + dy * dy <= radius * radius;
}

static bool IsBoundsWithinCenterRegion(const PhysXBounds3& bounds, const FMatrix& vpMat, float screenW, float screenH) {
    const float cx = screenW * 0.5f;
    const float cy = screenH * 0.5f;
    const float radius = std::min(screenW, screenH) * std::clamp(gPhysXCenterRegionFovDegrees / 90.0f, 0.05f, 0.667f);
    const float radiusSq = radius * radius;

    const Vec3 center = {
        (bounds.Min.X + bounds.Max.X) * 0.5f,
        (bounds.Min.Y + bounds.Max.Y) * 0.5f,
        (bounds.Min.Z + bounds.Max.Z) * 0.5f,
    };
    if (IsWithinCenterRegion(center, vpMat, screenW, screenH)) {
        return true;
    }

    Vec2 screenMin(FLT_MAX, FLT_MAX);
    Vec2 screenMax(-FLT_MAX, -FLT_MAX);
    bool hasProjectedCorner = false;
    for (int corner = 0; corner < 8; ++corner) {
        const Vec3 point = {
            (corner & 1) ? bounds.Max.X : bounds.Min.X,
            (corner & 2) ? bounds.Max.Y : bounds.Min.Y,
            (corner & 4) ? bounds.Max.Z : bounds.Min.Z,
        };

        Vec2 screenPos;
        if (!WorldToScreen(point, vpMat, screenW, screenH, screenPos)) {
            continue;
        }

        hasProjectedCorner = true;
        const float dx = screenPos.x - cx;
        const float dy = screenPos.y - cy;
        if (dx * dx + dy * dy <= radiusSq) {
            return true;
        }

        screenMin.x = std::min(screenMin.x, screenPos.x);
        screenMin.y = std::min(screenMin.y, screenPos.y);
        screenMax.x = std::max(screenMax.x, screenPos.x);
        screenMax.y = std::max(screenMax.y, screenPos.y);
    }

    if (!hasProjectedCorner) {
        return false;
    }

    const float nearestX = std::clamp(cx, screenMin.x, screenMax.x);
    const float nearestY = std::clamp(cy, screenMin.y, screenMax.y);
    const float dx = nearestX - cx;
    const float dy = nearestY - cy;
    return dx * dx + dy * dy <= radiusSq;
}

static bool IsPointInsideBounds(const Vec3& point, const PhysXBounds3& bounds, float epsilon = 0.0f) {
    return point.X >= bounds.Min.X - epsilon && point.X <= bounds.Max.X + epsilon &&
           point.Y >= bounds.Min.Y - epsilon && point.Y <= bounds.Max.Y + epsilon &&
           point.Z >= bounds.Min.Z - epsilon && point.Z <= bounds.Max.Z + epsilon;
}

static bool SegmentIntersectsBounds(const Vec3& start, const Vec3& end, const PhysXBounds3& bounds, float* outT = nullptr) {
    const Vec3 delta = end - start;
    float tMin = 0.0f;
    float tMax = 1.0f;

    const auto update_axis = [&](float startV, float deltaV, float minV, float maxV) -> bool {
        constexpr float kParallelEpsilon = 1e-4f;
        if (std::fabs(deltaV) < kParallelEpsilon) {
            return startV >= minV && startV <= maxV;
        }

        const float invDelta = 1.0f / deltaV;
        float t1 = (minV - startV) * invDelta;
        float t2 = (maxV - startV) * invDelta;
        if (t1 > t2) std::swap(t1, t2);
        tMin = std::max(tMin, t1);
        tMax = std::min(tMax, t2);
        return tMin <= tMax;
    };

    if (!update_axis(start.X, delta.X, bounds.Min.X, bounds.Max.X)) return false;
    if (!update_axis(start.Y, delta.Y, bounds.Min.Y, bounds.Max.Y)) return false;
    if (!update_axis(start.Z, delta.Z, bounds.Min.Z, bounds.Max.Z)) return false;

    if (outT) {
        *outT = tMin;
    }
    return tMax >= 0.0f && tMin <= 1.0f;
}

static bool BoundsOverlap(const PhysXBounds3& a, const PhysXBounds3& b) {
    return a.Min.X <= b.Max.X && a.Max.X >= b.Min.X &&
           a.Min.Y <= b.Max.Y && a.Max.Y >= b.Min.Y &&
           a.Min.Z <= b.Max.Z && a.Max.Z >= b.Min.Z;
}

static PhysXBounds3 MakeBoundsFromSegment(const Vec3& start, const Vec3& end, float inflate = 0.0f) {
    return {
        {
            std::min(start.X, end.X) - inflate,
            std::min(start.Y, end.Y) - inflate,
            std::min(start.Z, end.Z) - inflate,
        },
        {
            std::max(start.X, end.X) + inflate,
            std::max(start.Y, end.Y) + inflate,
            std::max(start.Z, end.Z) + inflate,
        }
    };
}

static PhysXBounds3 MakeBoundsFromTriangle(const Vec3& a, const Vec3& b, const Vec3& c, float inflate = 0.0f) {
    return {
        {
            std::min(std::min(a.X, b.X), c.X) - inflate,
            std::min(std::min(a.Y, b.Y), c.Y) - inflate,
            std::min(std::min(a.Z, b.Z), c.Z) - inflate,
        },
        {
            std::max(std::max(a.X, b.X), c.X) + inflate,
            std::max(std::max(a.Y, b.Y), c.Y) + inflate,
            std::max(std::max(a.Z, b.Z), c.Z) + inflate,
        }
    };
}

static constexpr size_t kVisibilityTriangleChunkSize = 32;

static void BuildTriangleChunkBounds(const std::vector<PhysXBounds3>& triangleBounds,
                                     std::vector<PhysXBounds3>& chunkBounds) {
    if (triangleBounds.empty()) {
        chunkBounds.clear();
        return;
    }

    const size_t chunkCount = (triangleBounds.size() + kVisibilityTriangleChunkSize - 1) / kVisibilityTriangleChunkSize;
    chunkBounds.resize(chunkCount);
    for (size_t chunk = 0; chunk < chunkCount; ++chunk) {
        const size_t begin = chunk * kVisibilityTriangleChunkSize;
        const size_t end = std::min(begin + kVisibilityTriangleChunkSize, triangleBounds.size());
        PhysXBounds3 merged = triangleBounds[begin];
        for (size_t i = begin + 1; i < end; ++i) {
            const PhysXBounds3& b = triangleBounds[i];
            merged.Min.X = std::min(merged.Min.X, b.Min.X);
            merged.Min.Y = std::min(merged.Min.Y, b.Min.Y);
            merged.Min.Z = std::min(merged.Min.Z, b.Min.Z);
            merged.Max.X = std::max(merged.Max.X, b.Max.X);
            merged.Max.Y = std::max(merged.Max.Y, b.Max.Y);
            merged.Max.Z = std::max(merged.Max.Z, b.Max.Z);
        }
        chunkBounds[chunk] = merged;
    }
}

static PhysXBounds3 TransformBoundsToWorld(const PhysXBounds3& localBounds, const PxTransformData& worldPose) {
    const Vec3 corners[8] = {
        {localBounds.Min.X, localBounds.Min.Y, localBounds.Min.Z},
        {localBounds.Min.X, localBounds.Min.Y, localBounds.Max.Z},
        {localBounds.Min.X, localBounds.Max.Y, localBounds.Min.Z},
        {localBounds.Min.X, localBounds.Max.Y, localBounds.Max.Z},
        {localBounds.Max.X, localBounds.Min.Y, localBounds.Min.Z},
        {localBounds.Max.X, localBounds.Min.Y, localBounds.Max.Z},
        {localBounds.Max.X, localBounds.Max.Y, localBounds.Min.Z},
        {localBounds.Max.X, localBounds.Max.Y, localBounds.Max.Z},
    };

    PhysXBounds3 out{
        {FLT_MAX, FLT_MAX, FLT_MAX},
        {-FLT_MAX, -FLT_MAX, -FLT_MAX}
    };
    for (const Vec3& corner : corners) {
        const Vec3 p = TransformPoint(worldPose, corner);
        out.Min.X = std::min(out.Min.X, p.X);
        out.Min.Y = std::min(out.Min.Y, p.Y);
        out.Min.Z = std::min(out.Min.Z, p.Z);
        out.Max.X = std::max(out.Max.X, p.X);
        out.Max.Y = std::max(out.Max.Y, p.Y);
        out.Max.Z = std::max(out.Max.Z, p.Z);
    }
    return out;
}

static bool SegmentIntersectsTriangle(const Vec3& start, const Vec3& end,
                                      const Vec3& a, const Vec3& b, const Vec3& c,
                                      float* outT = nullptr) {
#if defined(__ARM_NEON) || defined(__aarch64__)
    const float32x4_t startV = LoadVec3Neon(start);
    const float32x4_t endV = LoadVec3Neon(end);
    const float32x4_t aV = LoadVec3Neon(a);
    const float32x4_t bV = LoadVec3Neon(b);
    const float32x4_t cV = LoadVec3Neon(c);

    const float32x4_t dirV = vsubq_f32(endV, startV);
    const float32x4_t edge1V = vsubq_f32(bV, aV);
    const float32x4_t edge2V = vsubq_f32(cV, aV);
    const float32x4_t pvecV = NeonCross3(dirV, edge2V);
    const float det = NeonDot3(edge1V, pvecV);
    constexpr float kEpsilon = 1e-4f;
    if (std::fabs(det) < kEpsilon) return false;

    const float invDet = 1.0f / det;
    const float32x4_t tvecV = vsubq_f32(startV, aV);
    const float u = NeonDot3(tvecV, pvecV) * invDet;
    if (u < 0.0f || u > 1.0f) return false;

    const float32x4_t qvecV = NeonCross3(tvecV, edge1V);
    const float v = NeonDot3(dirV, qvecV) * invDet;
    if (v < 0.0f || u + v > 1.0f) return false;

    const float t = NeonDot3(edge2V, qvecV) * invDet;
    if (t < 0.0f || t > 1.0f) return false;
    if (outT) *outT = t;
    return true;
#else
    const Vec3 dir = end - start;
    const Vec3 edge1 = b - a;
    const Vec3 edge2 = c - a;
    const Vec3 pvec = Cross(dir, edge2);
    const float det = Vec3::Dot(edge1, pvec);
    constexpr float kEpsilon = 1e-4f;
    if (std::fabs(det) < kEpsilon) return false;

    const float invDet = 1.0f / det;
    const Vec3 tvec = start - a;
    const float u = Vec3::Dot(tvec, pvec) * invDet;
    if (u < 0.0f || u > 1.0f) return false;

    const Vec3 qvec = Cross(tvec, edge1);
    const float v = Vec3::Dot(dir, qvec) * invDet;
    if (v < 0.0f || u + v > 1.0f) return false;

    const float t = Vec3::Dot(edge2, qvec) * invDet;
    if (t < 0.0f || t > 1.0f) return false;
    if (outT) *outT = t;
    return true;
#endif
}

static float DistanceSqToBounds(const Vec3& point, const PhysXBounds3& bounds) {
    const auto axis_delta = [](float v, float minV, float maxV) -> float {
        if (v < minV) return minV - v;
        if (v > maxV) return v - maxV;
        return 0.0f;
    };

    const float dx = axis_delta(point.X, bounds.Min.X, bounds.Max.X);
    const float dy = axis_delta(point.Y, bounds.Min.Y, bounds.Max.Y);
    const float dz = axis_delta(point.Z, bounds.Min.Z, bounds.Max.Z);
    return dx * dx + dy * dy + dz * dz;
}

static uint64_t ResolvePhysXGeometryResource(uint32_t geometryType, uint64_t geometryAddr) {
    switch (geometryType) {
        case PX_GEOM_TRIANGLEMESH:
            return GetDriverManager().read<uint64_t>(geometryAddr + offset.PxTriangleMeshGeometryTriangleMesh);
        case PX_GEOM_CONVEXMESH:
            return GetDriverManager().read<uint64_t>(geometryAddr + offset.PxConvexMeshGeometryConvexMesh);
        case PX_GEOM_HEIGHTFIELD:
            return GetDriverManager().read<uint64_t>(geometryAddr + offset.PxHeightFieldGeometryHeightField);
        default:
            return geometryAddr;
    }
}

static PhysXDrawDedupKey BuildPhysXDrawDedupKeyForResource(PhysXDrawSource source, uint32_t geometryType,
                                                           uint64_t resource, const PxTransformData& worldPose) {
    const auto quantize = [](float v) -> int32_t {
        return static_cast<int32_t>(std::lround(v * 0.01f));
    };

    return {
        source,
        geometryType,
        resource,
        quantize(worldPose.Translation.X),
        quantize(worldPose.Translation.Y),
        quantize(worldPose.Translation.Z),
    };
}

static PhysXDrawDedupKey BuildPhysXDrawDedupKey(PhysXDrawSource source, uint32_t geometryType, uint64_t geometryAddr,
                                                const PxTransformData& worldPose) {
    return BuildPhysXDrawDedupKeyForResource(source, geometryType,
                                             ResolvePhysXGeometryResource(geometryType, geometryAddr), worldPose);
}

static bool ReadScbShapeLocalPose(uint64_t scbShape, PxTransformData& out) {
    uint64_t poseAddr = scbShape + offset.ScbShapeCoreTransform;
    const uint32_t controlState = GetDriverManager().read<uint32_t>(scbShape + offset.ScbBaseControlState);
    if ((controlState & 0x4u) != 0) {
        const uint64_t stream = GetDriverManager().read<uint64_t>(scbShape + offset.ScbBaseStreamPtr);
        if (stream == 0) return false;
        poseAddr = stream;
    }
    return ReadPxTransform(poseAddr, out);
}

static bool ReadScbStaticActorGlobalPose(uint64_t scbActor, PxTransformData& out) {
    const uint32_t controlState = GetDriverManager().read<uint32_t>(scbActor + offset.ScbBaseControlState);
    const uint8_t scbType = static_cast<uint8_t>((controlState >> 24) & 0xF);
    if (scbType != PX_SCB_RIGID_STATIC) return false;

    uint64_t poseAddr = scbActor + offset.ScbRigidStaticActor2World;
    if ((controlState & 0x40u) != 0) {
        const uint64_t stream = GetDriverManager().read<uint64_t>(scbActor + offset.ScbBaseStreamPtr);
        if (stream == 0) return false;
        poseAddr = stream + offset.ScbRigidStaticBufferedActor2World;
    }
    return ReadPxTransform(poseAddr, out);
}

static uint64_t LookupPxSceneByIndex(uint64_t libUE4, uint16_t sceneIndex) {
    const uint32_t hashSize = GetDriverManager().read<uint32_t>(libUE4 + offset.GPhysXSceneMapHashSize);
    if (hashSize == 0) return 0;

    const uint64_t entryArray = GetDriverManager().read<uint64_t>(libUE4 + offset.GPhysXSceneMap);
    if (entryArray == 0) return 0;

    uint64_t bucketBase = GetDriverManager().read<uint64_t>(libUE4 + offset.GPhysXSceneMapBucketPtr);
    if (bucketBase == 0) {
        bucketBase = libUE4 + offset.GPhysXSceneMapBuckets;
    }

    int32_t bucket = GetDriverManager().read<int32_t>(
        bucketBase + 4ULL * ((hashSize - 1u) & static_cast<uint32_t>(sceneIndex)));
    while (bucket != -1) {
        PhysXSceneMapEntry entry{};
        if (!GetDriverManager().read(entryArray + static_cast<uint64_t>(bucket) * sizeof(entry), &entry, sizeof(entry))) {
            return 0;
        }
        if (entry.Key == sceneIndex) {
            return entry.ScenePtr;
        }
        bucket = entry.Next;
    }
    return 0;
}

static void CollectActivePxScenes(uint64_t libUE4, uint64_t uworld, std::vector<uint64_t>& outScenes) {
    outScenes.clear();
    if (libUE4 == 0 || uworld == 0) return;

    const uint64_t physScene = GetDriverManager().read<uint64_t>(uworld + offset.PhysicsScene);
    if (physScene == 0 || physScene <= 0x10000) return;

    const uint32_t sceneCount = GetDriverManager().read<uint32_t>(physScene + offset.PhysSceneSceneCount);
    if (sceneCount == 0 || sceneCount >= 16) return;

    if (gPhysXManualSceneIndexEnabled) {
        const uint16_t sceneIndex = static_cast<uint16_t>(std::max(gPhysXManualSceneIndex, 0) & 0xFFFF);
        const uint64_t pxScene = LookupPxSceneByIndex(libUE4, sceneIndex);
        if (pxScene != 0) {
            outScenes.push_back(pxScene);
        }
        return;
    }

    std::unordered_set<uint16_t> seenIndices;
    for (uint32_t i = 0; i < sceneCount; ++i) {
        const uint16_t sceneIndex = GetDriverManager().read<uint16_t>(
            physScene + offset.PhysSceneSceneIndexArray + static_cast<uint64_t>(i) * sizeof(uint16_t));
        if (!seenIndices.insert(sceneIndex).second) continue;
        const uint64_t pxScene = LookupPxSceneByIndex(libUE4, sceneIndex);
        if (pxScene != 0) {
            outScenes.push_back(pxScene);
        }
    }
}

static uint64_t GetSyncPxScene(uint64_t libUE4, uint64_t uworld) {
    std::vector<uint64_t> scenes;
    CollectActivePxScenes(libUE4, uworld, scenes);
    if (scenes.empty()) return 0;

    for (uint64_t pxScene : scenes) {
        if (pxScene == 0) continue;
        const uint64_t actorArray = GetDriverManager().read<uint64_t>(pxScene + offset.PxSceneActors);
        const uint32_t actorCount = GetDriverManager().read<uint32_t>(pxScene + offset.PxSceneActorCount);
        if (actorArray != 0 && actorCount > 0 && actorCount <= 100000) {
            return pxScene;
        }
    }

    return scenes.front();
}

static bool IsLikelyPhysXActorPointer(uint64_t actor) {
    if (actor == 0 || actor <= 0x10000) return false;
    const uint16_t actorType = GetDriverManager().read<uint16_t>(actor + offset.PxActorType);
    if (actorType != PX_ACTOR_RIGID_DYNAMIC && actorType != PX_ACTOR_RIGID_STATIC) return false;

    const uint16_t shapeCount = GetDriverManager().read<uint16_t>(actor + offset.PxActorShapeCount);
    if (shapeCount == 0 || shapeCount > 64) return false;

    const uint64_t shapes = GetDriverManager().read<uint64_t>(actor + offset.PxActorShapes);
    return shapes != 0;
}

static bool ReadPxSceneActorPointersBulk(uint64_t actorArray, uint32_t actorCount, std::vector<uint64_t>& outActors) {
    outActors.assign(actorCount, 0);
    return ReadRemoteBufferRobust(actorArray, outActors.data(), actorCount * sizeof(uint64_t));
}

static void ReadPxSceneActorPointersElementWise(uint64_t actorArray, uint32_t actorCount, std::vector<uint64_t>& outActors) {
    outActors.clear();
    outActors.resize(actorCount, 0);
    for (uint32_t i = 0; i < actorCount; ++i) {
        ReadRemoteArrayElementSafe<uint64_t>(actorArray, i, outActors[i]);
    }
}

static size_t CountLikelyPhysXActors(const std::vector<uint64_t>& actors, size_t sampleBudget = 256) {
    size_t scanned = 0;
    size_t valid = 0;
    for (uint64_t actor : actors) {
        if (actor == 0) continue;
        ++scanned;
        if (IsLikelyPhysXActorPointer(actor)) ++valid;
        if (scanned >= sampleBudget) break;
    }
    return valid;
}

static void FilterLikelyPhysXActors(const std::vector<uint64_t>& source, std::vector<uint64_t>& outActors) {
    outActors.clear();
    outActors.reserve(source.size());
    for (uint64_t actor : source) {
        if (!IsLikelyPhysXActorPointer(actor)) continue;
        outActors.push_back(actor);
    }
}

static bool CollectPxSceneActorPointers(uint64_t pxScene, std::vector<uint64_t>& outActors) {
    outActors.clear();

    const uint64_t actorArray = GetDriverManager().read<uint64_t>(pxScene + offset.PxSceneActors);
    const uint32_t actorCount = GetDriverManager().read<uint32_t>(pxScene + offset.PxSceneActorCount);
    if (actorArray == 0 || actorCount == 0 || actorCount > 100000) return false;

    std::vector<uint64_t> bulkActors;
    const bool bulkOk = ReadPxSceneActorPointersBulk(actorArray, actorCount, bulkActors);
    const size_t bulkValid = bulkOk ? CountLikelyPhysXActors(bulkActors) : 0;
    if (bulkOk && bulkValid >= 8) {
        FilterLikelyPhysXActors(bulkActors, outActors);
        if (!outActors.empty()) {
            gPxSceneActorPointerCache[pxScene] = outActors;
            return true;
        }
    }

    std::vector<uint64_t> elementActors;
    ReadPxSceneActorPointersElementWise(actorArray, actorCount, elementActors);
    const size_t elementValid = CountLikelyPhysXActors(elementActors);
    if (elementValid >= 4) {
        FilterLikelyPhysXActors(elementActors, outActors);
        if (!outActors.empty()) {
            gPxSceneActorPointerCache[pxScene] = outActors;
            return true;
        }
    }

    auto cacheIt = gPxSceneActorPointerCache.find(pxScene);
    if (cacheIt != gPxSceneActorPointerCache.end()) {
        std::vector<uint64_t> stillValid;
        stillValid.reserve(cacheIt->second.size());
        for (uint64_t actor : cacheIt->second) {
            if (!IsLikelyPhysXActorPointer(actor)) continue;
            stillValid.push_back(actor);
        }
        if (!stillValid.empty()) {
            cacheIt->second = stillValid;
            outActors = stillValid;
            return true;
        }
    }

    return false;
}

static Vec3 ReadCameraWorldPosForPhysX() {
    if (address.libUE4 == 0) return Vec3::Zero();

    const uint64_t uworld = GetDriverManager().read<uint64_t>(address.libUE4 + offset.Gworld);
    if (uworld == 0) return Vec3::Zero();
    const uint64_t netDriver = GetDriverManager().read<uint64_t>(uworld + offset.NetDriver);
    if (netDriver == 0) return Vec3::Zero();
    const uint64_t serverConn = GetDriverManager().read<uint64_t>(netDriver + offset.ServerConnection);
    if (serverConn == 0) return Vec3::Zero();
    const uint64_t playerController = GetDriverManager().read<uint64_t>(serverConn + offset.PlayerController);
    if (playerController == 0) return Vec3::Zero();
    const uint64_t pcm = GetDriverManager().read<uint64_t>(playerController + offset.PlayerCameraManager);
    if (pcm == 0) return Vec3::Zero();

    const Vec3 cameraWorldPos = GetDriverManager().read<Vec3>(pcm + offset.CameraCache + offset.POV);
    if (!std::isfinite(cameraWorldPos.X) || !std::isfinite(cameraWorldPos.Y) || !std::isfinite(cameraWorldPos.Z)) {
        return Vec3::Zero();
    }
    return cameraWorldPos;
}

static void DrawSphereWireframe(ImDrawList* drawList, const FMatrix& vpMat, float screenW, float screenH,
                                const PxTransformData& pose, float radius, ImU32 color) {
    constexpr int kSegments = 18;
    for (int axis = 0; axis < 3; ++axis) {
        Vec3 prev{};
        bool hasPrev = false;
        for (int i = 0; i <= kSegments; ++i) {
            float t = (2.0f * static_cast<float>(M_PI) * i) / kSegments;
            Vec3 local{};
            switch (axis) {
                case 0: local = {0.0f, std::cos(t) * radius, std::sin(t) * radius}; break;
                case 1: local = {std::cos(t) * radius, 0.0f, std::sin(t) * radius}; break;
                default: local = {std::cos(t) * radius, std::sin(t) * radius, 0.0f}; break;
            }
            Vec3 world = TransformPoint(pose, local);
            if (hasPrev) {
                DrawLine3D(drawList, vpMat, screenW, screenH, prev, world, color, 1.0f);
            }
            prev = world;
            hasPrev = true;
        }
    }
}

static void DrawBoxWireframe(ImDrawList* drawList, const FMatrix& vpMat, float screenW, float screenH,
                             const PxTransformData& pose, const Vec3& halfExtents, ImU32 color) {
    Vec3 corners[8] = {
        {-halfExtents.X, -halfExtents.Y, -halfExtents.Z},
        { halfExtents.X, -halfExtents.Y, -halfExtents.Z},
        { halfExtents.X,  halfExtents.Y, -halfExtents.Z},
        {-halfExtents.X,  halfExtents.Y, -halfExtents.Z},
        {-halfExtents.X, -halfExtents.Y,  halfExtents.Z},
        { halfExtents.X, -halfExtents.Y,  halfExtents.Z},
        { halfExtents.X,  halfExtents.Y,  halfExtents.Z},
        {-halfExtents.X,  halfExtents.Y,  halfExtents.Z},
    };
    for (Vec3& c : corners) c = TransformPoint(pose, c);
    static const int edges[12][2] = {
        {0,1},{1,2},{2,3},{3,0},
        {4,5},{5,6},{6,7},{7,4},
        {0,4},{1,5},{2,6},{3,7},
    };
    for (const auto& e : edges) {
        DrawLine3D(drawList, vpMat, screenW, screenH, corners[e[0]], corners[e[1]], color, 1.2f);
    }
}

static void DrawWorldAabb(ImDrawList* drawList, const FMatrix& vpMat, float screenW, float screenH,
                          const Vec3& minV, const Vec3& maxV, ImU32 color) {
    const PxTransformData identity{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
    const Vec3 halfExtents{
        (maxV.X - minV.X) * 0.5f,
        (maxV.Y - minV.Y) * 0.5f,
        (maxV.Z - minV.Z) * 0.5f
    };
    const PxTransformData pose{
        identity.Rotation,
        {(minV.X + maxV.X) * 0.5f, (minV.Y + maxV.Y) * 0.5f, (minV.Z + maxV.Z) * 0.5f}
    };
    DrawBoxWireframe(drawList, vpMat, screenW, screenH, pose, halfExtents, color);
}

static void DrawCapsuleWireframe(ImDrawList* drawList, const FMatrix& vpMat, float screenW, float screenH,
                                 const PxTransformData& pose, float halfHeight, float radius, ImU32 color) {
    const Vec3 localA{-halfHeight, 0.0f, 0.0f};
    const Vec3 localB{ halfHeight, 0.0f, 0.0f};
    const Vec3 worldA = TransformPoint(pose, localA);
    const Vec3 worldB = TransformPoint(pose, localB);
    DrawLine3D(drawList, vpMat, screenW, screenH, worldA, worldB, color, 1.2f);

    const Vec3 up = QuatRotateVector(pose.Rotation, {0.0f, radius, 0.0f});
    const Vec3 forward = QuatRotateVector(pose.Rotation, {0.0f, 0.0f, radius});
    DrawLine3D(drawList, vpMat, screenW, screenH, worldA + up, worldB + up, color, 1.0f);
    DrawLine3D(drawList, vpMat, screenW, screenH, worldA - up, worldB - up, color, 1.0f);
    DrawLine3D(drawList, vpMat, screenW, screenH, worldA + forward, worldB + forward, color, 1.0f);
    DrawLine3D(drawList, vpMat, screenW, screenH, worldA - forward, worldB - forward, color, 1.0f);

    PxTransformData endA{pose.Rotation, worldA};
    PxTransformData endB{pose.Rotation, worldB};
    DrawSphereWireframe(drawList, vpMat, screenW, screenH, endA, radius, color);
    DrawSphereWireframe(drawList, vpMat, screenW, screenH, endB, radius, color);
}

// --- 自动导出辅助函数 ---

static void MaybeAutoExportTriangleMesh(uint64_t signature, const CachedTriangleMeshData& cache,
                                         uint32_t vertexCount, uint32_t triangleCount, uint8_t flags) {
    if (!gPhysXAutoExport) return;
    if (gAutoExportsThisFrame >= kAutoExportBudgetPerFrame) return;
    if (gAutoExportedSigs.count(signature)) return;

    char objName[64], metaName[64];
    std::snprintf(objName, sizeof(objName), "tri_%016llx.obj", static_cast<unsigned long long>(signature));
    std::snprintf(metaName, sizeof(metaName), "tri_%016llx.meta.txt", static_cast<unsigned long long>(signature));
    auto dir = GetPhysXExportDir();
    auto objPath = dir / objName;

    if (std::filesystem::exists(objPath)) {
        gAutoExportedSigs.insert(signature);
        return;
    }

    EnsurePhysXExportDir();
    if (WriteTriangleMeshObj(objPath, cache)) {
        WriteTriangleMeshMetaFile(dir / metaName, signature, vertexCount, triangleCount, flags);
        gLocalExportMetaLoaded = false;
        ++gAutoExportsThisFrame;
    }
    gAutoExportedSigs.insert(signature);
}

static void MaybeAutoExportHeightField(uint64_t signature, const CachedHeightFieldData& cache,
                                        uint32_t rows, uint32_t columns) {
    if (!gPhysXAutoExport) return;
    if (gAutoExportsThisFrame >= kAutoExportBudgetPerFrame) return;
    if (gAutoExportedSigs.count(signature)) return;

    char objName[64], metaName[64];
    std::snprintf(objName, sizeof(objName), "hf_%016llx.obj", static_cast<unsigned long long>(signature));
    std::snprintf(metaName, sizeof(metaName), "hf_%016llx.meta.txt", static_cast<unsigned long long>(signature));
    auto dir = GetPhysXExportDir();
    auto objPath = dir / objName;

    if (std::filesystem::exists(objPath)) {
        gAutoExportedSigs.insert(signature);
        return;
    }

    EnsurePhysXExportDir();
    if (WriteHeightFieldObj(objPath, cache)) {
        WriteHeightFieldMetaFile(dir / metaName, signature, rows, columns);
        gLocalExportMetaLoaded = false;
        ++gAutoExportsThisFrame;
    }
    gAutoExportedSigs.insert(signature);
}

static bool GetCachedTriangleMesh(uint64_t mesh, uint32_t vertexCount, uint32_t triangleCount, uint8_t flags,
                                  uint64_t verticesAddr, uint64_t trianglesAddr, CachedTriangleMeshData& out) {
    auto it = gTriangleMeshCache.find(mesh);
    bool cacheEvicted = false;
    if (it != gTriangleMeshCache.end()) {
        auto& entry = it->second;
        if (entry.VertexCount != vertexCount ||
            entry.TriangleCount != triangleCount ||
            entry.Flags != flags ||
            entry.VerticesAddr != verticesAddr ||
            entry.TrianglesAddr != trianglesAddr) {
            gTriangleMeshCache.erase(it);
            cacheEvicted = true;
        }
    }
    if (!cacheEvicted) {
        it = gTriangleMeshCache.find(mesh);
    }
    if (it != gTriangleMeshCache.end()) {
        auto& entry = it->second;
        // 新缓存的 mesh 数据可能是游戏尚未流式加载完毕时的快照，
        // 在前10秒内每秒抽查几个顶点，发现数据变化则淘汰重读
        if (!entry.Verified && entry.VerticesAddr != 0) {
            const double now = GetMonotoneSec();
            const double age = now - entry.CachedAt;
            if (age >= 10.0) {
                entry.Verified = true;
            } else if (age >= 1.0 && now - entry.LastVerifiedAt >= 1.0) {
                entry.LastVerifiedAt = now;
                const size_t vc = entry.Vertices.size();
                bool stale = false;
                if (vc > 0) {
                    const size_t step = std::max<size_t>(vc / 4, 1);
                    for (size_t idx = 0; idx < vc && !stale; idx += step) {
                        Vec3 remote{};
                        if (GetDriverManager().read(entry.VerticesAddr + idx * sizeof(Vec3),
                                               &remote, sizeof(remote))) {
                            const Vec3& c = entry.Vertices[idx];
                            if (remote.X != c.X || remote.Y != c.Y || remote.Z != c.Z)
                                stale = true;
                        }
                    }
                }
                if (stale) {
                    gTriangleMeshCache.erase(it);
                    cacheEvicted = true;
                }
            }
        }
        if (!cacheEvicted) {
            entry.LastUsedAt = GetMonotoneSec();
            out = it->second;
            return true;
        }
    }
    // 预算限制：每帧只允许若干次 cache miss 读取，避免 flush 后单帧卡顿
    if (gGeomCacheMissesThisFrame >= kGeomCacheMissBudgetPerFrame) return false;

    // 反复读取失败的 mesh 冷却：每次失败冷却时间翻倍（1s, 2s, 4s, 最大8s）
    {
        auto cdIt = gGeomCacheMissCooldowns.find(mesh);
        if (cdIt != gGeomCacheMissCooldowns.end()) {
            const double now = GetMonotoneSec();
            const double cooldownSec = std::min(1.0 * (1 << std::min(cdIt->second.ConsecutiveFailures - 1, 1)), 1.0);
            if (now - cdIt->second.LastAttemptTime < cooldownSec) return false;
        }
    }

    ++gGeomCacheMissesThisFrame;

    auto record_failure = [&]() {
        auto& cd = gGeomCacheMissCooldowns[mesh];
        cd.ConsecutiveFailures = std::min(cd.ConsecutiveFailures + 1, 3);
        cd.LastAttemptTime = GetMonotoneSec();
    };

    if (vertexCount == 0 || triangleCount == 0 || verticesAddr == 0 || trianglesAddr == 0) {
        record_failure();
        return false;
    }

    const size_t indexCount = static_cast<size_t>(triangleCount) * 3ULL;
    if (indexCount == 0 || indexCount > (1ULL << 27)) {
        record_failure();
        return false;
    }
    std::vector<TriangleMeshReadCandidate> candidates;
    candidates.reserve(10);

    auto register_candidate = [&](TriangleMeshReadCandidate&& candidate) {
        for (TriangleMeshReadCandidate& existing : candidates) {
            if (existing.Signature == candidate.Signature) {
                existing.StableHits += 1;
                if (candidate.TriangleCount > existing.TriangleCount) {
                    const size_t stableHits = existing.StableHits;
                    existing = std::move(candidate);
                    existing.StableHits = stableHits;
                }
                return;
            }
        }
        candidates.push_back(std::move(candidate));
    };

    const bool expect16BitIndices = (flags & 0x02u) != 0;

    int degradedReadCount = 0;

    auto try_read_snapshot = [&](uint8_t indexSizeBytes) -> bool {
        StableTriangleMeshSnapshot snapshot;
        bool stable = false;
        if (!ReadStableTriangleMeshSnapshot(verticesAddr, trianglesAddr, vertexCount, indexCount, indexSizeBytes, snapshot, stable)) {
            return false;
        }
        std::vector<uint32_t> decodedIndices;
        if (!DecodeTriangleIndices(snapshot.RawIndices, indexSizeBytes, indexCount, vertexCount, decodedIndices)) {
            return false;
        }
        TriangleMeshReadCandidate candidate;
        if (!BuildTriangleMeshReadCandidate(snapshot.Vertices, std::move(decodedIndices),
                                            vertexCount, triangleCount, flags, indexSizeBytes, candidate)) {
            return false;
        }
        // 稳定读取（两次哈希匹配）正常注册，可累积 StableHits；
        // 非稳定读取（大 mesh 常见）使用唯一签名避免错误合并，StableHits 保持 1
        if (!stable) {
            candidate.Signature ^= static_cast<uint64_t>(++degradedReadCount) * 0x517cc1b727220a95ULL;
        }
        register_candidate(std::move(candidate));
        return true;
    };

    int successfulReads = 0;
    const int maxAttempts = indexCount > (1ULL << 18) ? 16 : 8;
    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        bool gotRead = false;
        if (expect16BitIndices) {
            gotRead = try_read_snapshot(2);
            if (!gotRead) {
                gotRead = try_read_snapshot(4);
            }
        } else {
            gotRead = try_read_snapshot(4);
            if (!gotRead && vertexCount <= 65535) {
                gotRead = try_read_snapshot(2);
            }
        }
        if (gotRead) {
            ++successfulReads;
        }
        if (!candidates.empty()) {
            const TriangleMeshReadCandidate* best = &candidates.front();
            for (const TriangleMeshReadCandidate& candidate : candidates) {
                if (candidate.StableHits > best->StableHits ||
                    (candidate.StableHits == best->StableHits &&
                     candidate.IndexSizeBytes == (expect16BitIndices ? 2 : 4) &&
                     best->IndexSizeBytes != (expect16BitIndices ? 2 : 4)) ||
                    (candidate.StableHits == best->StableHits &&
                     candidate.IndexSizeBytes == best->IndexSizeBytes &&
                     candidate.TriangleCount > best->TriangleCount)) {
                    best = &candidate;
                }
            }
            if (best->StableHits >= 2 && best->TriangleCount >= std::max<size_t>(triangleCount / 4U, 64)) {
                break;
            }
        }
    }
    if (successfulReads == 0 || candidates.empty()) {
        // 记录失败冷却
        auto& cd = gGeomCacheMissCooldowns[mesh];
        cd.ConsecutiveFailures = std::min(cd.ConsecutiveFailures + 1, 3);
        cd.LastAttemptTime = GetMonotoneSec();
        return false;
    }

    const TriangleMeshReadCandidate* best = &candidates.front();
    for (const TriangleMeshReadCandidate& candidate : candidates) {
        if (candidate.StableHits > best->StableHits ||
            (candidate.StableHits == best->StableHits &&
             candidate.IndexSizeBytes == (expect16BitIndices ? 2 : 4) &&
             best->IndexSizeBytes != (expect16BitIndices ? 2 : 4)) ||
            (candidate.StableHits == best->StableHits &&
             candidate.IndexSizeBytes == best->IndexSizeBytes &&
             candidate.TriangleCount > best->TriangleCount)) {
            best = &candidate;
        }
    }

    CachedTriangleMeshData cache = best->Data;
    cache.CachedAt = GetMonotoneSec();
    cache.LastUsedAt = cache.CachedAt;
    cache.VertexCount = vertexCount;
    cache.TriangleCount = triangleCount;
    cache.Flags = flags;
    cache.VerticesAddr = verticesAddr;
    cache.TrianglesAddr = trianglesAddr;
    cache.Verified = false;
    cache.LastVerifiedAt = 0.0;

    // 先计算签名，用于去重检查
    const uint64_t signature = BuildTriangleMeshSignature(mesh, vertexCount, triangleCount, flags,
                                                          cache.Vertices, cache.Indices);
    gTriangleMeshSignatures[mesh] = signature;
    gTriangleMeshSources[mesh] = TriangleMeshSourceInfo{vertexCount, triangleCount, flags, verticesAddr, trianglesAddr};

    // 签名去重：相同内容已缓存则直接复用，跳过 AABB 重算
    auto sigCacheIt = gTriMeshCacheBySig.find(signature);
    if (sigCacheIt != gTriMeshCacheBySig.end()) {
        sigCacheIt->second.LastUsedAt = GetMonotoneSec();
        gTriangleMeshCache[mesh] = sigCacheIt->second;
        gGeomCacheMissCooldowns.erase(mesh);
        out = sigCacheIt->second;
        MaybeAutoExportTriangleMesh(signature, sigCacheIt->second, vertexCount, triangleCount, flags);
        return true;
    }

    // 计算顶点整体包围盒
    if (!cache.Vertices.empty()) {
        PhysXBounds3 aabb{{cache.Vertices[0].X, cache.Vertices[0].Y, cache.Vertices[0].Z},
                          {cache.Vertices[0].X, cache.Vertices[0].Y, cache.Vertices[0].Z}};
        for (size_t vi = 1; vi < cache.Vertices.size(); ++vi) {
            const Vec3& v = cache.Vertices[vi];
            aabb.Min.X = std::min(aabb.Min.X, v.X); aabb.Min.Y = std::min(aabb.Min.Y, v.Y); aabb.Min.Z = std::min(aabb.Min.Z, v.Z);
            aabb.Max.X = std::max(aabb.Max.X, v.X); aabb.Max.Y = std::max(aabb.Max.Y, v.Y); aabb.Max.Z = std::max(aabb.Max.Z, v.Z);
        }
        cache.MeshAABB = aabb;
    }

    // 预计算每个三角形的 AABB（空间换时间，避免内循环重复计算）
    {
        const size_t triCount = cache.Indices.size() / 3;
        cache.TriangleBounds.resize(triCount);
        for (size_t i = 0; i < triCount; ++i) {
            const uint32_t ia = cache.Indices[i * 3];
            const uint32_t ib = cache.Indices[i * 3 + 1];
            const uint32_t ic = cache.Indices[i * 3 + 2];
            if (ia >= cache.Vertices.size() || ib >= cache.Vertices.size() || ic >= cache.Vertices.size()) {
                cache.TriangleBounds[i] = {};
                continue;
            }
            const Vec3& a = cache.Vertices[ia];
            const Vec3& b = cache.Vertices[ib];
            const Vec3& c = cache.Vertices[ic];
            cache.TriangleBounds[i] = {
                {std::min(std::min(a.X, b.X), c.X), std::min(std::min(a.Y, b.Y), c.Y), std::min(std::min(a.Z, b.Z), c.Z)},
                {std::max(std::max(a.X, b.X), c.X), std::max(std::max(a.Y, b.Y), c.Y), std::max(std::max(a.Z, b.Z), c.Z)}
            };
        }
        BuildTriangleChunkBounds(cache.TriangleBounds, cache.ChunkBounds);
    }

    gTriMeshCacheBySig[signature] = cache;
    gTriangleMeshCache.emplace(mesh, cache);
    gGeomCacheMissCooldowns.erase(mesh);  // 成功缓存，清除冷却
    MaybeAutoExportTriangleMesh(signature, cache, vertexCount, triangleCount, flags);
    out = cache;
    return true;
}

static bool GetCachedConvexMesh(uint64_t mesh, CachedConvexMeshData& out) {
    auto it = gConvexMeshCache.find(mesh);
    if (it != gConvexMeshCache.end()) {
        out = it->second;
        return true;
    }
    // 预算限制
    if (gGeomCacheMissesThisFrame >= kGeomCacheMissBudgetPerFrame) return false;
    {
        auto cdIt = gGeomCacheMissCooldowns.find(mesh);
        if (cdIt != gGeomCacheMissCooldowns.end()) {
            const double now = GetMonotoneSec();
            const double cooldownSec = std::min(1.0 * (1 << std::min(cdIt->second.ConsecutiveFailures - 1, 1)), 1.0);
            if (now - cdIt->second.LastAttemptTime < cooldownSec) return false;
        }
    }
    ++gGeomCacheMissesThisFrame;

    auto record_failure = [&]() {
        auto& cd = gGeomCacheMissCooldowns[mesh];
        cd.ConsecutiveFailures = std::min(cd.ConsecutiveFailures + 1, 3);
        cd.LastAttemptTime = GetMonotoneSec();
    };

    const uint8_t vertexCount = GetDriverManager().read<uint8_t>(mesh + offset.PxConvexMeshVertexCount);
    const uint8_t polygonCount = GetDriverManager().read<uint8_t>(mesh + offset.PxConvexMeshPolygonCount);
    if (vertexCount == 0 || polygonCount == 0 || vertexCount > 255) {
        record_failure();
        return false;
    }

    const uint64_t hullBase = GetDriverManager().read<uint64_t>(mesh + offset.PxConvexMeshHullData);
    if (hullBase == 0) { record_failure(); return false; }

    const uint64_t verticesAddr = hullBase + 20ULL * polygonCount;
    std::vector<Vec3> vertices(vertexCount);
    if (!ReadRemoteBufferRobust(verticesAddr, vertices.data(), vertices.size() * sizeof(Vec3))) { record_failure(); return false; }

    std::vector<PhysXConvexPolygonData> polygons(polygonCount);
    if (!ReadRemoteBufferRobust(hullBase, polygons.data(), polygons.size() * sizeof(PhysXConvexPolygonData))) { record_failure(); return false; }

    const uint16_t edgeInfo = GetDriverManager().read<uint16_t>(mesh + offset.PxConvexMeshEdgeCount);
    const uint16_t edgeCount = edgeInfo & 0x7FFFu;
    uint64_t indexBufferAddr = hullBase + 20ULL * polygonCount + 15ULL * vertexCount + 2ULL * edgeCount;
    if ((edgeInfo & 0x8000u) != 0) {
        indexBufferAddr += 4ULL * edgeCount;
    }

    size_t indexBytesToRead = 0;
    for (const auto& poly : polygons) {
        indexBytesToRead = std::max(indexBytesToRead, static_cast<size_t>(poly.IndexBase) + poly.VertexCount);
    }
    if (indexBytesToRead == 0 || indexBytesToRead > 65535) { record_failure(); return false; }

    std::vector<uint8_t> indexBuffer(indexBytesToRead);
    if (!ReadRemoteBufferRobust(indexBufferAddr, indexBuffer.data(), indexBuffer.size())) { record_failure(); return false; }

    CachedConvexMeshData cache;
    cache.Vertices = std::move(vertices);
    for (const auto& poly : polygons) {
        if (poly.VertexCount < 3) continue;
        const uint32_t first = poly.IndexBase;
        const uint32_t last = first + poly.VertexCount;
        if (last > indexBuffer.size()) continue;
        for (uint32_t i = first; i < last; ++i) {
            const uint32_t a = indexBuffer[i];
            const uint32_t b = indexBuffer[(i + 1 < last) ? (i + 1) : first];
            if (a < cache.Vertices.size() && b < cache.Vertices.size()) {
                cache.EdgeIndices.push_back(a);
                cache.EdgeIndices.push_back(b);
            }
        }
    }

    gConvexMeshCache.emplace(mesh, cache);
    gGeomCacheMissCooldowns.erase(mesh);
    out = cache;
    return true;
}

static bool GetCachedHeightField(uint64_t heightField, CachedHeightFieldData& out) {
    // 快速路径：缓存命中直接返回（跳过远程 header 读取）
    auto it = gHeightFieldCache.find(heightField);
    if (it != gHeightFieldCache.end()) {
        it->second.LastUsedAt = GetMonotoneSec();
        out = it->second;
        return true;
    }

    // 预算限制
    if (gGeomCacheMissesThisFrame >= kGeomCacheMissBudgetPerFrame) return false;
    {
        auto cdIt = gGeomCacheMissCooldowns.find(heightField);
        if (cdIt != gGeomCacheMissCooldowns.end()) {
            const double now = GetMonotoneSec();
            const double cooldownSec = std::min(1.0 * (1 << std::min(cdIt->second.ConsecutiveFailures - 1, 1)), 1.0);
            if (now - cdIt->second.LastAttemptTime < cooldownSec) return false;
        }
    }
    ++gGeomCacheMissesThisFrame;

    std::vector<HeightFieldReadCandidate> candidates;
    candidates.reserve(4);
    HeightFieldReadCandidate best{};

    for (int attempt = 0; attempt < 4; ++attempt) {
        PhysXHeightFieldHeaderSnapshot header;
        if (!ReadHeightFieldHeaderSnapshot(heightField, header)) continue;

        HeightFieldReadCandidate candidate;
        if (!BuildHeightFieldReadCandidate(heightField, header, candidate)) continue;

        bool merged = false;
        for (auto& existing : candidates) {
            if (existing.Signature == candidate.Signature &&
                existing.Data.Rows == candidate.Data.Rows &&
                existing.Data.Columns == candidate.Data.Columns &&
                existing.Data.SamplesAddr == candidate.Data.SamplesAddr) {
                existing.StableHits++;
                if (existing.StableHits > best.StableHits) {
                    best = existing;
                }
                merged = true;
                break;
            }
        }
        if (!merged) {
            candidates.push_back(std::move(candidate));
            if (candidates.back().StableHits > best.StableHits) {
                best = candidates.back();
            }
        }
        if (best.StableHits >= 2) break;
    }

    if (best.Signature == 0) {
        auto& cd = gGeomCacheMissCooldowns[heightField];
        cd.ConsecutiveFailures = std::min(cd.ConsecutiveFailures + 1, 3);
        cd.LastAttemptTime = GetMonotoneSec();
        return false;
    }

    gHeightFieldSignatures[heightField] = best.Signature;
    best.Data.LastUsedAt = GetMonotoneSec();
    gHeightFieldSources[heightField] = HeightFieldSourceInfo{
        best.Data.Rows,
        best.Data.Columns,
        best.Data.SampleStride,
        best.Data.SampleCount,
        best.Data.ModifyCount,
        best.Data.SamplesAddr,
        0.0f  // Thickness — not stored in CachedHeightFieldData, use 0
    };

    // 签名去重：相同内容已缓存则直接复用
    auto sigCacheIt = gHfCacheBySig.find(best.Signature);
    if (sigCacheIt != gHfCacheBySig.end()) {
        sigCacheIt->second.LastUsedAt = GetMonotoneSec();
        gHeightFieldCache[heightField] = sigCacheIt->second;
        gGeomCacheMissCooldowns.erase(heightField);
        out = sigCacheIt->second;
        MaybeAutoExportHeightField(best.Signature, sigCacheIt->second, sigCacheIt->second.Rows, sigCacheIt->second.Columns);
        return true;
    }

    gHfCacheBySig[best.Signature] = best.Data;
    gHeightFieldCache[heightField] = best.Data;
    gGeomCacheMissCooldowns.erase(heightField);
    MaybeAutoExportHeightField(best.Signature, best.Data, best.Data.Rows, best.Data.Columns);
    out = best.Data;
    return true;
}

static Vec3 HeightFieldLocalPoint(uint32_t row, uint32_t column, const PhysXHeightFieldSample& sample,
                                  float heightScale, float rowScale, float columnScale) {
    return {
        static_cast<float>(row) * rowScale,
        static_cast<float>(sample.Height) * heightScale,
        static_cast<float>(column) * columnScale
    };
}

static bool GetHeightFieldCellTriangles(const CachedHeightFieldData& cache,
                                        uint32_t row, uint32_t column,
                                        float heightScale, float rowScale, float columnScale,
                                        Vec3& t0a, Vec3& t0b, Vec3& t0c, bool& t0Valid,
                                        Vec3& t1a, Vec3& t1b, Vec3& t1c, bool& t1Valid) {
    if (row + 1 >= cache.Rows || column + 1 >= cache.Columns) return false;

    const auto sample_at = [&](uint32_t r, uint32_t c) -> const PhysXHeightFieldSample& {
        return cache.Samples[static_cast<size_t>(r) * static_cast<size_t>(cache.Columns) + c];
    };

    const PhysXHeightFieldSample& s00 = sample_at(row, column);
    const PhysXHeightFieldSample& s10 = sample_at(row + 1, column);
    const PhysXHeightFieldSample& s01 = sample_at(row, column + 1);
    const PhysXHeightFieldSample& s11 = sample_at(row + 1, column + 1);

    const Vec3 p00 = HeightFieldLocalPoint(row, column, s00, heightScale, rowScale, columnScale);
    const Vec3 p10 = HeightFieldLocalPoint(row + 1, column, s10, heightScale, rowScale, columnScale);
    const Vec3 p01 = HeightFieldLocalPoint(row, column + 1, s01, heightScale, rowScale, columnScale);
    const Vec3 p11 = HeightFieldLocalPoint(row + 1, column + 1, s11, heightScale, rowScale, columnScale);

    if (HeightFieldTessFlag(s00)) {
        t0a = p10; t0b = p00; t0c = p11;
        t1a = p01; t1b = p11; t1c = p00;
    } else {
        t0a = p00; t0b = p01; t0c = p10;
        t1a = p11; t1b = p10; t1c = p01;
    }

    t0Valid = HeightFieldMaterial0(s00) != kPhysXHeightFieldHoleMaterial;
    t1Valid = HeightFieldMaterial1(s00) != kPhysXHeightFieldHoleMaterial;
    return true;
}

static void GetHeightFieldCellRange(const PhysXBounds3& localBounds,
                                    float rowScale, float columnScale,
                                    uint32_t rows, uint32_t columns,
                                    uint32_t& minRow, uint32_t& maxRow,
                                    uint32_t& minColumn, uint32_t& maxColumn) {
    const float rowCoord0 = localBounds.Min.X / rowScale;
    const float rowCoord1 = localBounds.Max.X / rowScale;
    const float minRowCoord = std::min(rowCoord0, rowCoord1);
    const float maxRowCoord = std::max(rowCoord0, rowCoord1);

    const float columnCoord0 = localBounds.Min.Z / columnScale;
    const float columnCoord1 = localBounds.Max.Z / columnScale;
    const float minColumnCoord = std::min(columnCoord0, columnCoord1);
    const float maxColumnCoord = std::max(columnCoord0, columnCoord1);

    minRow = static_cast<uint32_t>(std::clamp(std::floor(minRowCoord) - 1.0f, 0.0f, static_cast<float>(rows - 2)));
    maxRow = static_cast<uint32_t>(std::clamp(std::ceil(maxRowCoord) + 1.0f, 0.0f, static_cast<float>(rows - 1)));
    minColumn = static_cast<uint32_t>(std::clamp(std::floor(minColumnCoord) - 1.0f, 0.0f, static_cast<float>(columns - 2)));
    maxColumn = static_cast<uint32_t>(std::clamp(std::ceil(maxColumnCoord) + 1.0f, 0.0f, static_cast<float>(columns - 1)));
}

static const LocalObjMeshData* GetRenderableLocalTriangleMesh(uint64_t mesh) {
    const LocalObjMeshData* localMesh = GetLocalTriangleMeshDataStrict(mesh);
    if (localMesh) return localMesh;
    return GetLocalTriangleMeshData(mesh);
}

static const LocalObjMeshData* GetRenderableLocalHeightField(uint64_t heightField) {
    const LocalObjMeshData* localMesh = GetLocalHeightFieldDataStrict(heightField);
    if (localMesh) return localMesh;
    return GetLocalHeightFieldData(heightField);
}

static bool DrawLocalTriangleMeshInstance(ImDrawList* drawList, const FMatrix& vpMat, float screenW, float screenH,
                                          const Vec3& localPlayerPos, const PxTransformData& worldPose,
                                          uint64_t mesh, const PxMeshScaleData& scale,
                                          bool enableTriangleDistanceCull, bool isStaticMesh,
                                          PhysXDrawSource source, PhysXDrawStats& stats) {
    const LocalObjMeshData* localMesh = GetRenderableLocalTriangleMesh(mesh);
    if (!localMesh) return false;

    const ImU32 color = (source == PX_DRAW_SOURCE_PXSCENE)
        ? IM_COL32(135, 206, 235, 150)
        : (isStaticMesh ? IM_COL32(255, 255, 255, 140) : IM_COL32(0, 220, 255, 120));
    const float lineThickness = (isStaticMesh || source == PX_DRAW_SOURCE_PXSCENE) ? 1.15f : 0.85f;
    const bool useDistanceCull = enableTriangleDistanceCull &&
                                 address.LocalPlayerActor != 0 &&
                                 gPhysXDrawRadiusMeters > 0.0f;
    const float maxTriangleDistance = gPhysXDrawRadiusMeters * 100.0f * 1.5f;
    const float maxTriangleDistanceSq = maxTriangleDistance * maxTriangleDistance;

    bool drewAny = false;
    uint32_t drawnTriangles = 0;
    const uint32_t triangleBudget = static_cast<uint32_t>(localMesh->Indices.size() / 3);
    for (size_t triBase = 0; triBase + 2 < localMesh->Indices.size(); triBase += 3) {
        if (drawnTriangles >= triangleBudget || gTrianglesDrawnThisFrame >= kMaxTrianglesPerFrame) break;
        const uint32_t ia = localMesh->Indices[triBase];
        const uint32_t ib = localMesh->Indices[triBase + 1];
        const uint32_t ic = localMesh->Indices[triBase + 2];
        if (ia >= localMesh->Vertices.size() || ib >= localMesh->Vertices.size() || ic >= localMesh->Vertices.size()) continue;

        const Vec3 p0 = TransformPoint(worldPose, ApplyMeshScale(localMesh->Vertices[ia], scale));
        const Vec3 p1 = TransformPoint(worldPose, ApplyMeshScale(localMesh->Vertices[ib], scale));
        const Vec3 p2 = TransformPoint(worldPose, ApplyMeshScale(localMesh->Vertices[ic], scale));
        if (useDistanceCull) {
            const float ds0 = Vec3::Dot(p0 - localPlayerPos, p0 - localPlayerPos);
            const float ds1 = Vec3::Dot(p1 - localPlayerPos, p1 - localPlayerPos);
            const float ds2 = Vec3::Dot(p2 - localPlayerPos, p2 - localPlayerPos);
            if (std::min(std::min(ds0, ds1), ds2) > maxTriangleDistanceSq) continue;
        }
        DrawLine3D(drawList, vpMat, screenW, screenH, p0, p1, color, lineThickness);
        DrawLine3D(drawList, vpMat, screenW, screenH, p1, p2, color, lineThickness);
        DrawLine3D(drawList, vpMat, screenW, screenH, p2, p0, color, lineThickness);
        ++stats.TrianglesDrawn;
        ++drawnTriangles;
        ++gTrianglesDrawnThisFrame;
        drewAny = true;
    }
    if (drewAny) ++stats.MeshesDrawn;
    return drewAny;
}

static bool DrawLocalHeightFieldInstance(ImDrawList* drawList, const FMatrix& vpMat, float screenW, float screenH,
                                         const Vec3& localPlayerPos, const PxTransformData& worldPose,
                                         uint64_t heightField, float heightScale, float rowScale, float columnScale,
                                         PhysXDrawSource source, PhysXDrawStats& stats) {
    const LocalObjMeshData* localMesh = GetRenderableLocalHeightField(heightField);
    if (!localMesh) return false;

    const ImU32 color = (source == PX_DRAW_SOURCE_PXSCENE)
        ? IM_COL32(90, 255, 180, 155)
        : IM_COL32(40, 255, 120, 145);
    const bool useDistanceCull = address.LocalPlayerActor != 0 && gPhysXDrawRadiusMeters > 0.0f;
    const float maxTriangleDistance = gPhysXDrawRadiusMeters * 100.0f * 1.5f;
    const float maxTriangleDistanceSq = maxTriangleDistance * maxTriangleDistance;

    bool drewAny = false;
    uint32_t drawnTriangles = 0;
    const uint32_t triangleBudget = static_cast<uint32_t>(localMesh->Indices.size() / 3);
    for (size_t i = 0; i + 2 < localMesh->Indices.size(); i += 3) {
        if (drawnTriangles >= triangleBudget || gTrianglesDrawnThisFrame >= kMaxTrianglesPerFrame) break;
        const uint32_t ia = localMesh->Indices[i];
        const uint32_t ib = localMesh->Indices[i + 1];
        const uint32_t ic = localMesh->Indices[i + 2];
        if (ia >= localMesh->Vertices.size() || ib >= localMesh->Vertices.size() || ic >= localMesh->Vertices.size()) continue;

        const Vec3 lv0{localMesh->Vertices[ia].X * rowScale, localMesh->Vertices[ia].Y * heightScale, localMesh->Vertices[ia].Z * columnScale};
        const Vec3 lv1{localMesh->Vertices[ib].X * rowScale, localMesh->Vertices[ib].Y * heightScale, localMesh->Vertices[ib].Z * columnScale};
        const Vec3 lv2{localMesh->Vertices[ic].X * rowScale, localMesh->Vertices[ic].Y * heightScale, localMesh->Vertices[ic].Z * columnScale};
        const Vec3 p0 = TransformPoint(worldPose, lv0);
        const Vec3 p1 = TransformPoint(worldPose, lv1);
        const Vec3 p2 = TransformPoint(worldPose, lv2);
        if (useDistanceCull) {
            const float ds0 = Vec3::Dot(p0 - localPlayerPos, p0 - localPlayerPos);
            const float ds1 = Vec3::Dot(p1 - localPlayerPos, p1 - localPlayerPos);
            const float ds2 = Vec3::Dot(p2 - localPlayerPos, p2 - localPlayerPos);
            if (std::min({ds0, ds1, ds2}) > maxTriangleDistanceSq) continue;
        }
        DrawLine3D(drawList, vpMat, screenW, screenH, p0, p1, color, 1.15f);
        DrawLine3D(drawList, vpMat, screenW, screenH, p1, p2, color, 1.15f);
        DrawLine3D(drawList, vpMat, screenW, screenH, p2, p0, color, 1.15f);
        ++drawnTriangles;
        ++stats.TrianglesDrawn;
        ++gTrianglesDrawnThisFrame;
        drewAny = true;
    }
    if (drewAny) ++stats.MeshesDrawn;
    return drewAny;
}

static void UpdateLocalPhysXDrawCache(uint64_t shape, uint32_t geometryType, uint64_t geometryAddr,
                                      const PxTransformData& worldPose, PhysXDrawSource source,
                                      bool enableTriangleDistanceCull, bool isStaticMesh) {
    if (!gPhysXUseLocalModelData || shape == 0) return;

    LocalPhysXDrawCacheEntry entry{};
    entry.Shape = shape;
    entry.GeometryType = geometryType;
    entry.WorldPose = worldPose;
    entry.Source = source;
    entry.EnableTriangleDistanceCull = enableTriangleDistanceCull;
    entry.IsStaticMesh = isStaticMesh;
    entry.LastSeenTime = GetMonotoneSec();

    switch (geometryType) {
        case PX_GEOM_TRIANGLEMESH: {
            const uint64_t mesh = GetDriverManager().read<uint64_t>(geometryAddr + offset.PxTriangleMeshGeometryTriangleMesh);
            if (mesh == 0 || !GetRenderableLocalTriangleMesh(mesh)) return;
            if (!ReadMeshScale(geometryAddr + offset.PxTriangleMeshGeometryScale, entry.MeshScale)) return;
            entry.Resource = mesh;
            break;
        }
        case PX_GEOM_HEIGHTFIELD: {
            const uint64_t heightField = GetDriverManager().read<uint64_t>(geometryAddr + offset.PxHeightFieldGeometryHeightField);
            if (heightField == 0 || !GetRenderableLocalHeightField(heightField)) return;
            entry.HeightScale = GetDriverManager().read<float>(geometryAddr + offset.PxHeightFieldGeometryHeightScale);
            entry.RowScale = GetDriverManager().read<float>(geometryAddr + offset.PxHeightFieldGeometryRowScale);
            entry.ColumnScale = GetDriverManager().read<float>(geometryAddr + offset.PxHeightFieldGeometryColumnScale);
            if (std::fabs(entry.HeightScale) <= 1e-6f || std::fabs(entry.RowScale) <= 1e-6f ||
                std::fabs(entry.ColumnScale) <= 1e-6f) {
                return;
            }
            entry.Resource = heightField;
            break;
        }
        default:
            return;
    }

    gLocalPhysXDrawCache[shape] = entry;
}

static void DrawCachedLocalPhysXGeometry(const FMatrix& vpMat, const Vec3& localPlayerPos,
                                         ImDrawList* drawList, float screenW, float screenH,
                                         PhysXDrawStats& stats,
                                         const std::unordered_set<PhysXDrawDedupKey, PhysXDrawDedupKeyHash>& dedupKeys,
                                         const std::unordered_set<uint64_t>& liveShapes) {
    if (!gPhysXUseLocalModelData || gLocalPhysXDrawCache.empty()) return;

    const double now = GetMonotoneSec();
    const float farDistance = std::max(gPhysXDrawRadiusMeters * 100.0f * 3.0f, 12000.0f);
    const float farDistanceSq = farDistance * farDistance;
    constexpr double kNearTtlSec = 180.0;
    constexpr double kFarTtlSec = 20.0;
    constexpr size_t kMaxCachedShapes = 8192;

    for (auto it = gLocalPhysXDrawCache.begin(); it != gLocalPhysXDrawCache.end();) {
        const LocalPhysXDrawCacheEntry& entry = it->second;
        const Vec3 delta = entry.WorldPose.Translation - localPlayerPos;
        const float distSq = Vec3::Dot(delta, delta);
        const double ttl = distSq > farDistanceSq ? kFarTtlSec : kNearTtlSec;
        if ((now - entry.LastSeenTime) > ttl) {
            it = gLocalPhysXDrawCache.erase(it);
            continue;
        }
        ++it;
    }

    if (gLocalPhysXDrawCache.size() > kMaxCachedShapes) {
        std::vector<std::pair<uint64_t, double>> ages;
        ages.reserve(gLocalPhysXDrawCache.size());
        for (const auto& [shape, entry] : gLocalPhysXDrawCache) {
            ages.push_back({shape, entry.LastSeenTime});
        }
        std::sort(ages.begin(), ages.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
        const size_t eraseCount = gLocalPhysXDrawCache.size() - kMaxCachedShapes;
        for (size_t i = 0; i < eraseCount; ++i) {
            gLocalPhysXDrawCache.erase(ages[i].first);
        }
    }

    for (const auto& [shape, entry] : gLocalPhysXDrawCache) {
        if (liveShapes.find(shape) != liveShapes.end()) continue;
        if (entry.Resource == 0) continue;

        const PhysXDrawDedupKey prunerKey =
            BuildPhysXDrawDedupKeyForResource(PX_DRAW_SOURCE_PRUNER, entry.GeometryType, entry.Resource, entry.WorldPose);
        const PhysXDrawDedupKey sceneKey =
            BuildPhysXDrawDedupKeyForResource(PX_DRAW_SOURCE_PXSCENE, entry.GeometryType, entry.Resource, entry.WorldPose);
        if (dedupKeys.find(prunerKey) != dedupKeys.end() || dedupKeys.find(sceneKey) != dedupKeys.end()) continue;

        bool drewShape = false;
        switch (entry.GeometryType) {
            case PX_GEOM_TRIANGLEMESH:
                drewShape = DrawLocalTriangleMeshInstance(drawList, vpMat, screenW, screenH, localPlayerPos,
                                                          entry.WorldPose, entry.Resource, entry.MeshScale,
                                                          entry.EnableTriangleDistanceCull, entry.IsStaticMesh,
                                                          entry.Source, stats);
                break;
            case PX_GEOM_HEIGHTFIELD:
                drewShape = DrawLocalHeightFieldInstance(drawList, vpMat, screenW, screenH, localPlayerPos,
                                                         entry.WorldPose, entry.Resource,
                                                         entry.HeightScale, entry.RowScale, entry.ColumnScale,
                                                         entry.Source, stats);
                break;
            default:
                break;
        }
        if (drewShape) {
            ++stats.ShapesDrawn;
            ++stats.ActorsDrawn;
        }
    }
}

static bool DrawHeightFieldShape(ImDrawList* drawList, const FMatrix& vpMat, float screenW, float screenH,
                                 const Vec3& localPlayerPos, const PxTransformData& worldPose,
                                 uint64_t geometryAddr, PhysXDrawSource source, PhysXDrawStats& stats) {
    const uint64_t heightField = GetDriverManager().read<uint64_t>(geometryAddr + offset.PxHeightFieldGeometryHeightField);
    if (heightField == 0) return false;

    const float heightScale = GetDriverManager().read<float>(geometryAddr + offset.PxHeightFieldGeometryHeightScale);
    const float rowScale = GetDriverManager().read<float>(geometryAddr + offset.PxHeightFieldGeometryRowScale);
    const float columnScale = GetDriverManager().read<float>(geometryAddr + offset.PxHeightFieldGeometryColumnScale);
    if (std::fabs(heightScale) <= 1e-6f || std::fabs(rowScale) <= 1e-6f || std::fabs(columnScale) <= 1e-6f) {
        return false;
    }

    const ImU32 color = (source == PX_DRAW_SOURCE_PXSCENE)
        ? IM_COL32(90, 255, 180, 155)
        : IM_COL32(40, 255, 120, 145);

    // 磁盘优先：尝试从本地 OBJ 加载
    {
        const LocalObjMeshData* localMesh = GetLocalHeightFieldDataStrict(heightField);
        if (localMesh) {
            return DrawLocalHeightFieldInstance(drawList, vpMat, screenW, screenH, localPlayerPos, worldPose,
                                                heightField, heightScale, rowScale, columnScale, source, stats);
        }
        if (gPhysXUseLocalModelData) return false;
    }

    CachedHeightFieldData cache;
    if (!GetCachedHeightField(heightField, cache)) return false;
    const uint32_t rows = cache.Rows;
    const uint32_t columns = cache.Columns;

    const uint32_t triangleBudget = std::max(gPhysXMaxTrianglesPerMesh, 1);
    const bool useDistanceCull = address.LocalPlayerActor != 0 && gPhysXDrawRadiusMeters > 0.0f;
    const float maxTriangleDistance = gPhysXDrawRadiusMeters * 100.0f * 1.5f;
    const float maxTriangleDistanceSq = maxTriangleDistance * maxTriangleDistance;

    // 计算玩家在 HeightField 中的 cell 位置，从附近开始遍历
    uint32_t centerRow = rows / 2;
    uint32_t centerColumn = columns / 2;
    if (useDistanceCull) {
        const Vec3 localPlayerLocal = InverseTransformPoint(worldPose, localPlayerPos);
        centerRow = static_cast<uint32_t>(std::clamp(localPlayerLocal.X / rowScale, 0.0f, static_cast<float>(rows - 1)));
        centerColumn = static_cast<uint32_t>(std::clamp(localPlayerLocal.Z / columnScale, 0.0f, static_cast<float>(columns - 1)));
    }

    bool drewAny = false;
    uint32_t drawnTriangles = 0;

    // 螺旋遍历：从中心向外扩展
    const int maxRadius = std::max(static_cast<int>(rows), static_cast<int>(columns));
    for (int radius = 0; radius <= maxRadius; ++radius) {
        for (int dRow = -radius; dRow <= radius; ++dRow) {
            for (int dCol = -radius; dCol <= radius; ++dCol) {
                if (std::max(std::abs(dRow), std::abs(dCol)) != radius) continue; // 只处理当前圈层

                const int row = static_cast<int>(centerRow) + dRow;
                const int column = static_cast<int>(centerColumn) + dCol;
                if (row < 0 || row + 1 >= static_cast<int>(rows)) continue;
                if (column < 0 || column + 1 >= static_cast<int>(columns)) continue;
            Vec3 lt0a{}, lt0b{}, lt0c{}, lt1a{}, lt1b{}, lt1c{};
            bool t0Valid = false, t1Valid = false;
            if (!GetHeightFieldCellTriangles(cache, row, column, heightScale, rowScale, columnScale,
                                             lt0a, lt0b, lt0c, t0Valid, lt1a, lt1b, lt1c, t1Valid)) {
                continue;
            }

            auto draw_triangle = [&](const Vec3& a, const Vec3& b, const Vec3& c) {
                if (drawnTriangles >= triangleBudget || gTrianglesDrawnThisFrame >= kMaxTrianglesPerFrame) return;
                const Vec3 p0 = TransformPoint(worldPose, a);
                const Vec3 p1 = TransformPoint(worldPose, b);
                const Vec3 p2 = TransformPoint(worldPose, c);
                if (useDistanceCull) {
                    const float ds0 = Vec3::Dot(p0 - localPlayerPos, p0 - localPlayerPos);
                    const float ds1 = Vec3::Dot(p1 - localPlayerPos, p1 - localPlayerPos);
                    const float ds2 = Vec3::Dot(p2 - localPlayerPos, p2 - localPlayerPos);
                    if (std::min({ds0, ds1, ds2}) > maxTriangleDistanceSq) return;
                }
                DrawLine3D(drawList, vpMat, screenW, screenH, p0, p1, color, 1.15f);
                DrawLine3D(drawList, vpMat, screenW, screenH, p1, p2, color, 1.15f);
                DrawLine3D(drawList, vpMat, screenW, screenH, p2, p0, color, 1.15f);
                ++drawnTriangles;
                ++stats.TrianglesDrawn;
                ++gTrianglesDrawnThisFrame;
                drewAny = true;
            };

                if (t0Valid) draw_triangle(lt0a, lt0b, lt0c);
                if (t1Valid) draw_triangle(lt1a, lt1b, lt1c);
                if (drawnTriangles >= triangleBudget) goto done_drawing;
            }
        }
        if (drawnTriangles >= triangleBudget) break;
    }
done_drawing:

    if (drawnTriangles > 0) {
        ++stats.MeshesDrawn;
        drewAny = true;
    }
    return drewAny;
}


static bool DrawTriangleMeshShape(ImDrawList* drawList, const FMatrix& vpMat, float screenW, float screenH,
                                  const Vec3& localPlayerPos, const PxTransformData& worldPose, uint64_t meshGeometryAddr,
                                  bool enableTriangleDistanceCull, bool isStaticMesh, PhysXDrawSource source,
                                  PhysXDrawStats& stats) {
    const uint64_t mesh = GetDriverManager().read<uint64_t>(meshGeometryAddr + offset.PxTriangleMeshGeometryTriangleMesh);
    if (mesh == 0) return false;

    PxMeshScaleData scale{};
    if (!ReadMeshScale(meshGeometryAddr + offset.PxTriangleMeshGeometryScale, scale)) return false;

    const uint32_t triangleBudget = std::max(gPhysXMaxTrianglesPerMesh, 1);
    const bool isPxSceneSource = source == PX_DRAW_SOURCE_PXSCENE;
    const ImU32 color = isPxSceneSource
        ? IM_COL32(135, 206, 235, 150)
        : (isStaticMesh ? IM_COL32(255, 255, 255, 140) : IM_COL32(0, 220, 255, 120));
    const float lineThickness = (isStaticMesh || isPxSceneSource) ? 1.15f : 0.85f;

    // 磁盘优先：尝试从本地 OBJ 加载（仅需轻量 header 读取来解析签名）
    {
        const LocalObjMeshData* localMesh = GetLocalTriangleMeshDataStrict(mesh);
        if (localMesh) {
            return DrawLocalTriangleMeshInstance(drawList, vpMat, screenW, screenH, localPlayerPos, worldPose,
                                                 mesh, scale, enableTriangleDistanceCull, isStaticMesh, source, stats);
        }
        // gPhysXUseLocalModelData = 仅磁盘模式，不回退内存
        if (gPhysXUseLocalModelData) return false;
    }

    const uint32_t vertexCount = GetDriverManager().read<uint32_t>(mesh + offset.PxTriangleMeshVertexCount);
    const uint32_t triangleCount = GetDriverManager().read<uint32_t>(mesh + offset.PxTriangleMeshTriangleCount);
    if (vertexCount == 0 || triangleCount == 0) return false;

    const uint64_t verticesAddr = GetDriverManager().read<uint64_t>(mesh + offset.PxTriangleMeshVertices);
    const uint64_t trianglesAddr = GetDriverManager().read<uint64_t>(mesh + offset.PxTriangleMeshTriangles);
    if (verticesAddr == 0 || trianglesAddr == 0) return false;

    const uint8_t flags = GetDriverManager().read<uint8_t>(mesh + offset.PxTriangleMeshFlags);
    CachedTriangleMeshData cache;
    if (!GetCachedTriangleMesh(mesh, vertexCount, triangleCount, flags, verticesAddr, trianglesAddr, cache)) {
        return false;
    }
    const auto& vertices = cache.Vertices;
    const auto& indices = cache.Indices;
    bool drewAny = false;

    {
        const bool useDistanceCull = enableTriangleDistanceCull &&
                                     address.LocalPlayerActor != 0 &&
                                     gPhysXDrawRadiusMeters > 0.0f;
        const float maxTriangleDistance = gPhysXDrawRadiusMeters * 100.0f * 1.5f;
        const float maxTriangleDistanceSq = maxTriangleDistance * maxTriangleDistance;
        const size_t cachedVertexCount = vertices.size();
        for (uint32_t i = 0; i < triangleCount; ++i) {
            if (gTrianglesDrawnThisFrame >= kMaxTrianglesPerFrame) break;
            const size_t triBase = static_cast<size_t>(i) * 3ULL;
            if (triBase + 2 >= indices.size()) break;
            uint32_t idx[3] = {indices[triBase], indices[triBase + 1], indices[triBase + 2]};
            if (idx[0] >= cachedVertexCount || idx[1] >= cachedVertexCount || idx[2] >= cachedVertexCount) continue;

            const Vec3 p0 = TransformPoint(worldPose, ApplyMeshScale(vertices[idx[0]], scale));
            const Vec3 p1 = TransformPoint(worldPose, ApplyMeshScale(vertices[idx[1]], scale));
            const Vec3 p2 = TransformPoint(worldPose, ApplyMeshScale(vertices[idx[2]], scale));

            if (useDistanceCull) {
                const float ds0 = Vec3::Dot(p0 - localPlayerPos, p0 - localPlayerPos);
                const float ds1 = Vec3::Dot(p1 - localPlayerPos, p1 - localPlayerPos);
                const float ds2 = Vec3::Dot(p2 - localPlayerPos, p2 - localPlayerPos);
                if (std::min(ds0, std::min(ds1, ds2)) > maxTriangleDistanceSq) continue;
            }

            DrawLine3D(drawList, vpMat, screenW, screenH, p0, p1, color, lineThickness);
            DrawLine3D(drawList, vpMat, screenW, screenH, p1, p2, color, lineThickness);
            DrawLine3D(drawList, vpMat, screenW, screenH, p2, p0, color, lineThickness);
            ++stats.TrianglesDrawn;
            ++gTrianglesDrawnThisFrame;
            drewAny = true;
        }
    }

    if (drewAny) ++stats.MeshesDrawn;
    return drewAny;
}

static bool DrawConvexMeshShape(ImDrawList* drawList, const FMatrix& vpMat, float screenW, float screenH,
                                const PxTransformData& worldPose, uint64_t meshGeometryAddr,
                                PhysXDrawSource source, PhysXDrawStats& stats) {
    const uint64_t mesh = GetDriverManager().read<uint64_t>(meshGeometryAddr + offset.PxConvexMeshGeometryConvexMesh);
    if (mesh == 0) return false;

    PxMeshScaleData scale{};
    if (!ReadMeshScale(meshGeometryAddr + offset.PxConvexMeshGeometryScale, scale)) return false;

    CachedConvexMeshData cache;
    if (!GetCachedConvexMesh(mesh, cache)) return false;

    const ImU32 color = (source == PX_DRAW_SOURCE_PXSCENE)
        ? IM_COL32(135, 206, 235, 150)
        : IM_COL32(255, 165, 0, 135);
    bool drewAny = false;
    for (size_t i = 0; i + 1 < cache.EdgeIndices.size(); i += 2) {
        if (gTrianglesDrawnThisFrame >= kMaxTrianglesPerFrame) break;
        const uint32_t a = cache.EdgeIndices[i];
        const uint32_t b = cache.EdgeIndices[i + 1];
        if (a >= cache.Vertices.size() || b >= cache.Vertices.size()) continue;
        const Vec3 pa = TransformPoint(worldPose, ApplyMeshScale(cache.Vertices[a], scale));
        const Vec3 pb = TransformPoint(worldPose, ApplyMeshScale(cache.Vertices[b], scale));
        DrawLine3D(drawList, vpMat, screenW, screenH, pa, pb, color, 0.85f);
        ++gTrianglesDrawnThisFrame;
        drewAny = true;
    }
    if (drewAny) ++stats.MeshesDrawn;
    return drewAny;
}

static void DrawPhysXStaticPrunerGeometry(const FMatrix& vpMat, const Vec3& localPlayerPos,
                                          ImDrawList* drawList, float screenW, float screenH,
                                          PhysXDrawStats& stats,
                                          std::unordered_set<PhysXDrawDedupKey, PhysXDrawDedupKeyHash>& dedupKeys,
                                          std::unordered_set<uint64_t>& liveShapes) {
    // 复用帧级 gPrunerCache（由 EnsurePrunerDataCached 填充），避免重复读取
    if (!EnsurePrunerDataCached()) return;
    const uint32_t objectCount = gPrunerCache.objectCount;
    const auto& payloads = gPrunerCache.payloads;
    const auto& bounds = gPrunerCache.bounds;

    const float maxDistance = std::max(gPhysXDrawRadiusMeters * 100.0f * 1.5f, 4000.0f);
    const float maxDistanceSq = maxDistance * maxDistance;

    // Phase 1：收集所有通过距离+FOV过滤的候选 shape，按距离排序
    // （仅使用本地缓存的 AABB 数据，无远程读取，开销极低）
    std::vector<PrunerShapeCandidate> candidates;
    candidates.reserve(std::min<uint32_t>(objectCount, 512));

    for (uint32_t i = 0; i < objectCount; ++i) {
        const PhysXPrunerPayload& payload = payloads[i];
        if (payload.Shape == 0 || payload.Actor == 0) continue;

        ++stats.ShapesScanned;
        const float distSq = DistanceSqToBounds(localPlayerPos, bounds[i]);
        if (distSq > maxDistanceSq) {
            ++stats.RejectedByFilter;
            continue;
        }
        // 不再做 FOV 中心区域过滤：AABB 数据是本地缓存，过滤不节省远程读取；
        // 按距离排序 + shape 预算已足够控制开销，去掉 FOV 过滤确保屏幕边缘可见模型不被丢弃

        candidates.push_back({i, distSq});
    }

    // 按距离升序排序，确保最近的 shape 优先处理
    std::sort(candidates.begin(), candidates.end(),
              [](const PrunerShapeCandidate& a, const PrunerShapeCandidate& b) {
                  return a.DistanceSq < b.DistanceSq;
              });

    // Phase 2：按距离顺序处理最近的 N 个 shape（需要远程读取，受预算限制）
    int shapesProcessed = 0;
    const int maxShapesPerFrame = std::max(gPhysXMaxActorsPerFrame, 32);

    for (const auto& candidate : candidates) {
        if (shapesProcessed >= maxShapesPerFrame) break;
        ++shapesProcessed;

        const PhysXPrunerPayload& payload = payloads[candidate.Index];
        liveShapes.insert(payload.Shape);

        PxTransformData actorPose{};
        PxTransformData shapePose{};
        if (!ReadScbStaticActorGlobalPose(payload.Actor, actorPose)) continue;
        if (!ReadScbShapeLocalPose(payload.Shape, shapePose)) continue;

        const PxTransformData worldPose = ComposeTransforms(actorPose, shapePose);
        const uint64_t geometryAddr = payload.Shape + offset.ScbShapeCoreGeometry;
        const uint32_t geometryType = GetDriverManager().read<uint32_t>(geometryAddr);
        if (geometryType > PX_GEOM_HEIGHTFIELD) {
            ++stats.RejectedByFilter;
            continue;
        }
        const PhysXDrawDedupKey dedupKey = BuildPhysXDrawDedupKey(PX_DRAW_SOURCE_PRUNER, geometryType, geometryAddr, worldPose);
        if (dedupKey.Resource == 0 || dedupKeys.find(dedupKey) != dedupKeys.end()) {
            ++stats.RejectedByFilter;
            continue;
        }
        UpdateLocalPhysXDrawCache(payload.Shape, geometryType, geometryAddr, worldPose,
                                  PX_DRAW_SOURCE_PRUNER, true, true);

        bool drewShape = false;
        switch (geometryType) {
            case PX_GEOM_SPHERE:
                if (gPhysXDrawPrimitives && !gPhysXUseLocalModelData) {
                    const float radius = GetDriverManager().read<float>(geometryAddr + offset.PxSphereGeometryRadius);
                    DrawSphereWireframe(drawList, vpMat, screenW, screenH, worldPose, radius,
                                        IM_COL32(255, 80, 160, 220));
                    drewShape = true;
                }
                break;
            case PX_GEOM_CAPSULE:
                if (gPhysXDrawPrimitives && !gPhysXUseLocalModelData) {
                    const float halfHeight = GetDriverManager().read<float>(geometryAddr + offset.PxCapsuleGeometryHalfHeight);
                    const float radius = GetDriverManager().read<float>(geometryAddr + offset.PxCapsuleGeometryRadius);
                    DrawCapsuleWireframe(drawList, vpMat, screenW, screenH, worldPose, halfHeight, radius,
                                         IM_COL32(255, 215, 0, 220));
                    drewShape = true;
                }
                break;
            case PX_GEOM_BOX:
                if (gPhysXDrawPrimitives && !gPhysXUseLocalModelData) {
                    Vec3 halfExtents{};
                    if (GetDriverManager().read(geometryAddr + offset.PxBoxGeometryHalfExtents, &halfExtents, sizeof(halfExtents))) {
                        DrawBoxWireframe(drawList, vpMat, screenW, screenH, worldPose, halfExtents,
                                         IM_COL32(80, 255, 80, 220));
                        drewShape = true;
                    }
                }
                break;
            case PX_GEOM_CONVEXMESH:
                if (gPhysXDrawMeshes && !gPhysXUseLocalModelData) {
                    drewShape = DrawConvexMeshShape(drawList, vpMat, screenW, screenH, worldPose, geometryAddr,
                                                    PX_DRAW_SOURCE_PRUNER, stats);
                }
                break;
            case PX_GEOM_TRIANGLEMESH:
                if (gPhysXDrawMeshes) {
                    drewShape = DrawTriangleMeshShape(drawList, vpMat, screenW, screenH, localPlayerPos, worldPose,
                                                      geometryAddr, true, true, PX_DRAW_SOURCE_PRUNER, stats);
                }
                break;
            case PX_GEOM_HEIGHTFIELD:
                if (gPhysXDrawMeshes) {
                    drewShape = DrawHeightFieldShape(drawList, vpMat, screenW, screenH, localPlayerPos,
                                                     worldPose, geometryAddr, PX_DRAW_SOURCE_PRUNER, stats);
                }
                break;
            default:
                break;
        }

        if (drewShape) {
            dedupKeys.insert(dedupKey);
            ++stats.ShapesDrawn;
            ++stats.ActorsDrawn;
        }
    }
}

// 使用预读数据绘制（在 fence wait 后调用）
void DrawObjectsWithData(const GameFrameData& data, float gameStepDeltaTime) {
    if (!gShowObjects) return;
    if (!data.valid) return;

    PollAsyncLocalLoads();

    // 使本帧的 pruner 缓存失效，下一次可视性判断会重新读取
    InvalidateVisibilityCache();

    // 重置每帧几何缓存未命中计数
    gGeomCacheMissesThisFrame = 0;
    gTrianglesDrawnThisFrame = 0;
    gAutoExportsThisFrame = 0;

    // 按大间隔定期清空几何缓存（三角面/凸包/高度场），触发下次按需重读
    MaybeFlushGeomCaches();

    auto nextBoneScreenCache = std::make_shared<BoneScreenCache>();

    // 定期清理缓存（每 2 秒，假设 60 FPS）
    static int cleanupCounter = 0;
    if (++cleanupCounter >= 120) {
        cleanupCounter = 0;
        // 清理过期的插值缓存（超过 120 帧未更新的 actor）
        for (auto it = gBoneWorldCache.begin(); it != gBoneWorldCache.end(); ) {
            if (data.frameCounter > it->second.lastFrameCounter + 120) {
                it = gBoneWorldCache.erase(it);
            } else {
                ++it;
            }
        }

    }

    // 更新引擎帧号（用于插值缓存过期判断）
    gLastEngineFrame = data.frameCounter;

    // VP 矩阵直接使用帧数据中读取的（每帧一次，不再高频轮询）
    const FMatrix& vpToUse = data.VPMat;

    if (gDrawPhysXGeometry) {
        ImGuiIO& io = ImGui::GetIO();
        DrawPhysXGeometry(vpToUse, data.localPlayerPos, ImGui::GetBackgroundDrawList(),
                          io.DisplaySize.x, io.DisplaySize.y);
    }

    if (gShowAllClassNames) {
        auto classified = GetClassifiedActorsSnapshot();
        if (!classified) { return; }
        std::vector<CachedActor> singleActor;
        singleActor.reserve(1);
        ForEachCachedActor(*classified, [&](const CachedActor& actor) {
            singleActor.clear();
            singleActor.push_back(actor);
            DrawObjectsWithDataInternal(singleActor, vpToUse, data.localPlayerPos,
                                        data.frameCounter, gameStepDeltaTime,
                                        nextBoneScreenCache.get());
        });
        std::atomic_store_explicit(&gBoneScreenCacheSnapshot,
                                   BoneScreenCacheSnapshot(nextBoneScreenCache),
                                   std::memory_order_release);
        return;
    }

    // 获取分类后的 actor 列表
    auto classified = GetClassifiedActorsSnapshot();
    if (!classified) return;

    // 按类型分别绘制（根据开关控制，直接传引用避免拷贝）
    if (gShowPlayers && !classified->players.empty()) {
        DrawObjectsWithDataInternal(classified->players, vpToUse, data.localPlayerPos,
                                    data.frameCounter, gameStepDeltaTime,
                                    nextBoneScreenCache.get());
    }
    if (gShowBots && !classified->bots.empty()) {
        DrawObjectsWithDataInternal(classified->bots, vpToUse, data.localPlayerPos,
                                    data.frameCounter, gameStepDeltaTime,
                                    nextBoneScreenCache.get());
    }
    if (gShowNPCs && !classified->npcs.empty()) {
        DrawObjectsWithDataInternal(classified->npcs, vpToUse, data.localPlayerPos,
                                    data.frameCounter, gameStepDeltaTime,
                                    nextBoneScreenCache.get());
    }
    if (gShowMonsters && !classified->monsters.empty()) {
        DrawObjectsWithDataInternal(classified->monsters, vpToUse, data.localPlayerPos,
                                    data.frameCounter, gameStepDeltaTime,
                                    nextBoneScreenCache.get());
    }
    if (gShowTombBoxes && !classified->tombBoxes.empty()) {
        DrawObjectsWithDataInternal(classified->tombBoxes, vpToUse, data.localPlayerPos,
                                    data.frameCounter, gameStepDeltaTime,
                                    nextBoneScreenCache.get());
    }
    if (gShowOtherBoxes && !classified->otherBoxes.empty()) {
        DrawObjectsWithDataInternal(classified->otherBoxes, vpToUse, data.localPlayerPos,
                                    data.frameCounter, gameStepDeltaTime,
                                    nextBoneScreenCache.get());
    }
    if (gShowEscapeBoxes && !classified->escapeBoxes.empty()) {
        DrawObjectsWithDataInternal(classified->escapeBoxes, vpToUse, data.localPlayerPos,
                                    data.frameCounter, gameStepDeltaTime,
                                    nextBoneScreenCache.get());
    }
    if (gShowVehicles && !classified->vehicles.empty()) {
        DrawObjectsWithDataInternal(classified->vehicles, vpToUse, data.localPlayerPos,
                                    data.frameCounter, gameStepDeltaTime,
                                    nextBoneScreenCache.get());
    }
    if (gShowOthers && !classified->others.empty()) {
        DrawObjectsWithDataInternal(classified->others, vpToUse, data.localPlayerPos,
                                    data.frameCounter, gameStepDeltaTime,
                                    nextBoneScreenCache.get());
    }

    if (gDrawMiniMap) {
        ImGuiIO& io = ImGui::GetIO();
        DrawMiniMapOverlay(*classified, data.localPlayerPos, io.DisplaySize.x, io.DisplaySize.y);
    }

    std::atomic_store_explicit(&gBoneScreenCacheSnapshot,
                               BoneScreenCacheSnapshot(nextBoneScreenCache),
                               std::memory_order_release);

}

// Compute Shader 诊断叠加层（在 DrawObjectsWithData 后由 main loop 调用）

static void DrawMiniMapOverlay(const ClassifiedActors& classified, const Vec3& localPlayerPos,
                               float screenW, float screenH) {
    const float sizePx = std::clamp(gMiniMapSizePx, 120.0f, 420.0f);
    const float zoomMeters = std::clamp(gMiniMapZoomMeters, 20.0f, 400.0f);
    const float x = std::clamp(gMiniMapPosX, 8.0f, std::max(8.0f, screenW - sizePx - 8.0f));
    const float y = std::clamp(gMiniMapPosY, 8.0f, std::max(8.0f, screenH - sizePx - 8.0f));
    const ImVec2 min(x, y);
    const ImVec2 max(x + sizePx, y + sizePx);
    const ImVec2 center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
    const float radius = sizePx * 0.5f;
    const float innerRadius = radius - 12.0f;
    const float pixelsPerMeter = innerRadius / zoomMeters;

    Vec3 forward;
    Vec3 right;
    if (!TryReadLocalPlayerMapBasis(forward, right)) {
        forward = Vec3(1.0f, 0.0f, 0.0f);
        right = Vec3(0.0f, -1.0f, 0.0f);
    }

    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    drawList->AddRectFilled(min, max, IM_COL32(8, 16, 28, 168), 14.0f);
    drawList->AddRect(min, max, IM_COL32(110, 190, 255, 180), 14.0f, 0, 1.8f);
    drawList->AddCircleFilled(center, innerRadius, IM_COL32(12, 24, 38, 208), 64);
    drawList->AddCircle(center, innerRadius, IM_COL32(125, 210, 255, 110), 64, 1.4f);
    drawList->AddCircle(center, innerRadius * 0.5f, IM_COL32(125, 210, 255, 56), 64, 1.0f);
    drawList->AddLine(ImVec2(center.x - innerRadius, center.y), ImVec2(center.x + innerRadius, center.y),
                      IM_COL32(125, 210, 255, 42), 1.0f);
    drawList->AddLine(ImVec2(center.x, center.y - innerRadius), ImVec2(center.x, center.y + innerRadius),
                      IM_COL32(125, 210, 255, 42), 1.0f);

    char zoomLabel[64];
    std::snprintf(zoomLabel, sizeof(zoomLabel), "Radar %.0fm", zoomMeters);
    drawList->AddText(ImVec2(min.x + 10.0f, min.y + 8.0f), IM_COL32(220, 240, 255, 220), zoomLabel);

    auto drawActors = [&](const std::vector<CachedActor>& actors) {
        for (const CachedActor& actor : actors) {
            if (actor.actorAddr == 0 || actor.actorAddr == address.LocalPlayerActor) {
                continue;
            }
            if (address.LocalPlayerKey != 0 && actor.playerKey == address.LocalPlayerKey) {
                continue;
            }
            if (address.LocalPlayerTeamID >= 0 &&
                actor.teamID >= 0 &&
                actor.teamID == address.LocalPlayerTeamID) {
                continue;
            }

            FTransform actorTransform{};
            if (!TryReadActorWorldTransform(actor, actorTransform)) {
                continue;
            }

            const Vec3 deltaCm = actorTransform.Translation - localPlayerPos;
            const Vec3 deltaMeters(deltaCm.X / 100.0f, deltaCm.Y / 100.0f, 0.0f);
            const float localX = Vec3::Dot(deltaMeters, right);
            const float localY = Vec3::Dot(deltaMeters, forward);
            const float distanceSq = localX * localX + localY * localY;
            if (distanceSq > zoomMeters * zoomMeters) {
                continue;
            }

            const ImVec2 point(center.x + localX * pixelsPerMeter,
                               center.y - localY * pixelsPerMeter);
            const float dx = point.x - center.x;
            const float dy = point.y - center.y;
            if (dx * dx + dy * dy > innerRadius * innerRadius) {
                continue;
            }

            drawList->AddCircleFilled(point, 3.6f, IM_COL32(255, 72, 72, 235), 16);
            drawList->AddCircle(point, 4.8f, IM_COL32(255, 160, 160, 160), 16, 1.0f);
        }
    };

    drawActors(classified.players);
    drawActors(classified.bots);
    drawActors(classified.npcs);

    const ImVec2 tip(center.x, center.y - 10.0f);
    const ImVec2 left(center.x - 7.0f, center.y + 8.0f);
    const ImVec2 rightPt(center.x + 7.0f, center.y + 8.0f);
    drawList->AddTriangleFilled(tip, left, rightPt, IM_COL32(255, 255, 255, 235));
    drawList->AddCircleFilled(center, 2.8f, IM_COL32(255, 255, 255, 220), 12);
}

static void DrawPhysXGeometry(const FMatrix& vpMat, const Vec3& localPlayerPos, ImDrawList* drawList,
                              float screenW, float screenH) {
    if (address.libUE4 == 0) return;

    const uint64_t uworld = GetDriverManager().read<uint64_t>(address.libUE4 + offset.Gworld);
    std::vector<uint64_t> pxScenes;
    CollectActivePxScenes(address.libUE4, uworld, pxScenes);
    if (pxScenes.empty()) return;

    PhysXDrawStats stats;
    std::unordered_set<PhysXDrawDedupKey, PhysXDrawDedupKeyHash> dedupKeys;
    dedupKeys.reserve(4096);
    std::unordered_set<uint64_t> liveShapes;
    liveShapes.reserve(4096);
    DrawPhysXStaticPrunerGeometry(vpMat, localPlayerPos, drawList, screenW, screenH, stats, dedupKeys, liveShapes);

    std::vector<PhysXActorCandidate> sceneActors;
    sceneActors.reserve(512);
    const int maxActorScans = std::max(gPhysXMaxActorsPerFrame * 4, 256);
    int actorScans = 0;

    for (uint64_t pxScene : pxScenes) {
        std::vector<uint64_t> actors;
        if (!CollectPxSceneActorPointers(pxScene, actors)) continue;
        for (uint64_t actor : actors) {
            if (actor == 0) continue;
            if (actorScans >= maxActorScans) break;
            ++actorScans;

            const uint16_t actorType = GetDriverManager().read<uint16_t>(actor + offset.PxActorType);
            if (actorType != PX_ACTOR_RIGID_DYNAMIC && actorType != PX_ACTOR_RIGID_STATIC) {
                continue;
            }

            PxTransformData actorPose{};
            if (!ReadActorGlobalPose(actor, actorType, actorPose)) {
                continue;
            }

            if (actorType != PX_ACTOR_RIGID_STATIC) {
                if (address.LocalPlayerActor != 0 && gPhysXDrawRadiusMeters > 0.0f) {
                    const float distanceMeters = Vec3::Distance(localPlayerPos, actorPose.Translation) / 100.0f;
                    if (distanceMeters > gPhysXDrawRadiusMeters) {
                        ++stats.RejectedByFilter;
                        continue;
                    }
                }

                if (!IsWithinCenterRegion(actorPose.Translation, vpMat, screenW, screenH)) {
                    ++stats.RejectedByFilter;
                    continue;
                }
            }

            const Vec3 delta = actorPose.Translation - localPlayerPos;
            PhysXActorCandidate candidate{};
            candidate.Actor = actor;
            candidate.ActorType = actorType;
            candidate.Pose = actorPose;
            candidate.DistanceSq = Vec3::Dot(delta, delta);
            sceneActors.push_back(candidate);
        }

        if (actorScans >= maxActorScans) break;
    }

    std::sort(sceneActors.begin(), sceneActors.end(), [](const PhysXActorCandidate& a, const PhysXActorCandidate& b) {
        return a.DistanceSq < b.DistanceSq;
    });

    const size_t sceneLimit = std::min(sceneActors.size(), static_cast<size_t>(std::max(gPhysXMaxActorsPerFrame, 1)));

    for (size_t di = 0; di < sceneLimit; ++di) {
        const PhysXActorCandidate& candidate = sceneActors[di];
        const uint64_t actor = candidate.Actor;
        const uint16_t actorType = candidate.ActorType;
        const PxTransformData& actorPose = candidate.Pose;
        ++stats.ActorsScanned;

        const bool isStaticActor = actorType == PX_ACTOR_RIGID_STATIC;
        const uint16_t shapeCount = GetDriverManager().read<uint16_t>(actor + offset.PxActorShapeCount);
        if (shapeCount == 0) continue;

        const uint32_t shapeLimit = std::min<uint32_t>(shapeCount, std::max(gPhysXMaxShapesPerActor, 1));
        std::vector<uint64_t> shapes(shapeLimit);
        if (shapeCount == 1) {
            shapes[0] = GetDriverManager().read<uint64_t>(actor + offset.PxActorShapes);
        } else {
            const uint64_t shapePtrArray = GetDriverManager().read<uint64_t>(actor + offset.PxActorShapes);
            if (shapePtrArray == 0) continue;
            if (!ReadRemoteBufferRobust(shapePtrArray, shapes.data(), shapeLimit * sizeof(uint64_t))) continue;
        }

        bool actorHadDrawnShape = false;
        for (uint64_t shape : shapes) {
            if (shape == 0) continue;
            liveShapes.insert(shape);
            ++stats.ShapesScanned;

            const uint32_t npShapeFlags = GetDriverManager().read<uint32_t>(shape + offset.PxShapeFlags);
            const uint64_t corePtr = GetDriverManager().read<uint64_t>(shape + offset.PxShapeCorePtr);

            uint8_t shapeFlags = 0;
            if ((npShapeFlags & 0x40u) != 0) {
                if (corePtr == 0) continue;
                shapeFlags = GetDriverManager().read<uint8_t>(corePtr + offset.PxShapeCoreShapeFlags);
            } else {
                shapeFlags = GetDriverManager().read<uint8_t>(shape + offset.PxShapeShapeFlags);
            }

            if ((shapeFlags & (PX_SHAPE_SIMULATION | PX_SHAPE_SCENE_QUERY)) == 0 ||
                (shapeFlags & PX_SHAPE_TRIGGER) != 0) {
                ++stats.RejectedByFilter;
                continue;
            }

            PxTransformData shapePose{};
            if (!ReadShapeLocalPose(shape, npShapeFlags, shapePose)) continue;
            const PxTransformData worldPose = ComposeTransforms(actorPose, shapePose);

            uint64_t geometryAddr = shape + offset.PxShapeGeometryInline;
            if ((npShapeFlags & 0x1u) != 0) {
                if (corePtr == 0) continue;
                geometryAddr = corePtr + offset.PxShapeCoreGeometry;
            }

            const uint32_t geometryType = GetDriverManager().read<uint32_t>(geometryAddr + 0x0);
            if (geometryType > PX_GEOM_HEIGHTFIELD) {
                ++stats.RejectedByFilter;
                continue;
            }
            const PhysXDrawDedupKey dedupKey = BuildPhysXDrawDedupKey(PX_DRAW_SOURCE_PXSCENE, geometryType, geometryAddr, worldPose);
            if (dedupKey.Resource == 0 || dedupKeys.find(dedupKey) != dedupKeys.end()) {
                ++stats.RejectedByFilter;
                continue;
            }
            UpdateLocalPhysXDrawCache(shape, geometryType, geometryAddr, worldPose,
                                      PX_DRAW_SOURCE_PXSCENE, !isStaticActor, isStaticActor);
            bool drewShape = false;
            switch (geometryType) {
                case PX_GEOM_SPHERE:
                    if (gPhysXDrawPrimitives && !gPhysXUseLocalModelData) {
                        const float radius = GetDriverManager().read<float>(geometryAddr + offset.PxSphereGeometryRadius);
                        DrawSphereWireframe(drawList, vpMat, screenW, screenH, worldPose, radius,
                                            IM_COL32(255, 80, 160, 220));
                        drewShape = true;
                    }
                    break;
                case PX_GEOM_CAPSULE:
                    if (gPhysXDrawPrimitives && !gPhysXUseLocalModelData) {
                        const float halfHeight = GetDriverManager().read<float>(geometryAddr + offset.PxCapsuleGeometryHalfHeight);
                        const float radius = GetDriverManager().read<float>(geometryAddr + offset.PxCapsuleGeometryRadius);
                        DrawCapsuleWireframe(drawList, vpMat, screenW, screenH, worldPose, halfHeight, radius,
                                             IM_COL32(255, 215, 0, 220));
                        drewShape = true;
                    }
                    break;
                case PX_GEOM_BOX:
                    if (gPhysXDrawPrimitives && !gPhysXUseLocalModelData) {
                        Vec3 halfExtents{};
                        if (GetDriverManager().read(geometryAddr + offset.PxBoxGeometryHalfExtents, &halfExtents, sizeof(halfExtents))) {
                            DrawBoxWireframe(drawList, vpMat, screenW, screenH, worldPose, halfExtents,
                                             IM_COL32(80, 255, 80, 220));
                            drewShape = true;
                        }
                    }
                    break;
                case PX_GEOM_CONVEXMESH:
                    if (gPhysXDrawMeshes && !gPhysXUseLocalModelData) {
                        drewShape = DrawConvexMeshShape(drawList, vpMat, screenW, screenH, worldPose, geometryAddr,
                                                        PX_DRAW_SOURCE_PXSCENE, stats);
                    }
                    break;
                case PX_GEOM_TRIANGLEMESH:
                    if (gPhysXDrawMeshes) {
                        drewShape = DrawTriangleMeshShape(drawList, vpMat, screenW, screenH, localPlayerPos, worldPose,
                                                          geometryAddr, !isStaticActor, isStaticActor,
                                                          PX_DRAW_SOURCE_PXSCENE, stats);
                    }
                    break;
                case PX_GEOM_HEIGHTFIELD:
                    if (gPhysXDrawMeshes) {
                        drewShape = DrawHeightFieldShape(drawList, vpMat, screenW, screenH, localPlayerPos,
                                                         worldPose, geometryAddr, PX_DRAW_SOURCE_PXSCENE, stats);
                    }
                    break;
                default:
                    break;
            }

            if (drewShape) {
                dedupKeys.insert(dedupKey);
                ++stats.ShapesDrawn;
                actorHadDrawnShape = true;
            }
        }

        if (actorHadDrawnShape) {
            ++stats.ActorsDrawn;
        }
    }

    DrawCachedLocalPhysXGeometry(vpMat, localPlayerPos, drawList, screenW, screenH, stats, dedupKeys, liveShapes);

    if (gShowAllClassNames) {
        char text[160];
        snprintf(text, sizeof(text), "PhysX A:%d/%d S:%d/%d M:%d T:%d F:%d",
                 stats.ActorsDrawn, stats.ActorsScanned, stats.ShapesDrawn, stats.ShapesScanned,
                 stats.MeshesDrawn, stats.TrianglesDrawn, stats.RejectedByFilter);
        drawList->AddText(ImVec2(20.0f, 20.0f), IM_COL32(255, 220, 120, 255), text);
    }
}

// 旧接口（保留兼容性，立即读取并绘制）
void DrawObjects() {
    GameFrameData data = ReadGameData();
    DrawObjectsWithData(data, 0.016f);  // 默认 ~60fps
}

// 实际的绘制逻辑（提取为独立函数）
static void DrawObjectsWithDataInternal(
    const std::vector<CachedActor>& actors,
    const FMatrix& VPMat,
    const Vec3& localPlayerPos,
    uint64_t engineFrame,
    float gameStepDeltaTime,
    BoneScreenCache* boneScreenCache)
{
    // 绘制有映射的 actor
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    const float screenW = io.DisplaySize.x;
    const float screenH = io.DisplaySize.y;
    if (!gDrawPredictedAimPoint) {
        gPredictedAimHoverTimes.clear();
    }
    for (size_t actorIndex = 0; actorIndex < actors.size(); ++actorIndex) {
        const auto& ca = actors[actorIndex];
        if ((address.LocalPlayerActor != 0 && ca.actorAddr == address.LocalPlayerActor) ||
            (ca.actorType == ActorType::PLAYER && address.LocalPlayerKey != 0 && ca.playerKey == address.LocalPlayerKey)) {
            continue;
        }
        if ((ca.actorType == ActorType::PLAYER || ca.actorType == ActorType::BOT) &&
            address.LocalPlayerTeamID >= 0 &&
            ca.teamID >= 0 &&
            ca.teamID == address.LocalPlayerTeamID) {
            continue;
        }

        bool hasBoneMap = ca.boneMapBuilt && ca.skelMeshCompAddr != 0;
        Vec3 actorPos;
        float distance;
        FTransform meshTransform{};
        bool hasMeshTransform = false;

        if (hasBoneMap) {
            meshTransform = GetDriverManager().read<FTransform>(ca.skelMeshCompAddr + offset.ComponentToWorld);
            actorPos = meshTransform.Translation;
            hasMeshTransform = true;
            distance = Vec3::Distance(localPlayerPos, actorPos) / 100.0f;
        } else if (ca.rootCompAddr != 0) {
            FTransform actorTransform = GetDriverManager().read<FTransform>(ca.rootCompAddr + offset.ComponentToWorld);
            actorPos = actorTransform.Translation;
            distance = Vec3::Distance(localPlayerPos, actorPos) / 100.0f;
        } else {
            continue;
        }

        if (distance > gMaxSkeletonDistance) {
            continue;
        }

        // 开启“显示所有类名”时始终显示原始 className，不使用 ClassNameMap 的映射名。
        // 默认模式下仍优先显示映射名。
        const char* label = nullptr;
        if (gShowAllClassNames && !ca.className.empty()) {
            label = ca.className.c_str();
        } else if (!ca.displayName.empty()) {
            label = ca.displayName.c_str();
        } else {
            continue;
        }

        // 距离衰减参数（可调整）
        constexpr float MIN_DISTANCE = 10.0f;   // 10米内全亮、全尺寸
        constexpr float MAX_DISTANCE = 200.0f;  // 200米外最小透明度、最小尺寸

        // 计算距离因子 [0, 1]：0=近，1=远
        float distanceFactor = 0.0f;
        if (distance > MIN_DISTANCE) {
            distanceFactor = (distance - MIN_DISTANCE) / (MAX_DISTANCE - MIN_DISTANCE);
            distanceFactor = std::min(1.0f, std::max(0.0f, distanceFactor));
        }

        // 文本大小使用固定距离分段，避免实时连续缩放导致抖动
        const float textScale = GetLabelTextScaleByDistance(distance);

        // 透明度：255 → 100（近→远，保持一定可见度）
        int alpha = (int)(255 - distanceFactor * 200);

        // 骨骼透明度：根据距离和设定阈值计算
        int boneAlpha = alpha;
        if (distance > gMaxSkeletonDistance) {
            // 超过设定距离，骨骼变为几乎透明（alpha=20）
            boneAlpha = 20;
        }

        // 是否绘制骨骼：有骨骼映射即绘制（不再受距离限制，通过透明度控制）
        bool drawSkeleton = hasBoneMap;

        // 用于存储标签位置
        Vec3 topLabelWorldPos = Vec3::Zero();    // 头部位置（绘制名称）
        Vec3 bottomLabelWorldPos = Vec3::Zero(); // 根骨骼位置（绘制距离）
        bool hasTopLabel = false;
        bool hasBottomLabel = false;

        // 无骨骼对象也给一个基于 actor 原点的文本锚点，确保“显示所有类名”能覆盖整个 actor 数组。
        topLabelWorldPos = actorPos;
        if (ca.actorType == ActorType::PLAYER) {
            topLabelWorldPos.Z += 18.0f;
        } else {
            topLabelWorldPos.Z -= 8.0f;
        }
        hasTopLabel = true;

        if (drawSkeleton) {
            // 屏幕外剔除：先用 actor 位置做粗略检测，屏幕外跳过骨骼读取
            Vec2 cullScreenPos;
            bool onScreen = WorldToScreen(actorPos, VPMat, screenW, screenH, cullScreenPos);
            // 带边距检测（角色可能部分在屏幕外但骨骼可见）
            float margin = 200.0f;
            bool inBounds = onScreen &&
                cullScreenPos.x > -margin && cullScreenPos.x < screenW + margin &&
                cullScreenPos.y > -margin && cullScreenPos.y < screenH + margin;

            if (inBounds && hasMeshTransform && ca.cachedBoneCount > 0 && ca.boneDataPtr != 0) {
                // 使用缓存的静态指针，无需再读取 SkeletalMeshComponent/BoneCount/BoneDataPtr
                int BoneCount = ca.cachedBoneCount;
                uint64_t BoneDataPtr = ca.boneDataPtr;
                constexpr int MAX_BONE_COUNT = 150;

                if (BoneCount <= MAX_BONE_COUNT) {
                    // meshTransform 已在位置读取时获取，直接使用
                    FMatrix meshMatrix = TransformToMatrix(meshTransform);

                    // 提取关键骨骼（简化：只使用 translation，无验证）
                    Vec3 boneTranslations[BONE_COUNT];
                    int validCount = 0;

                    if (gUseBatchBoneRead) {
                        // 优化：只读取需要的骨骼，而不是全部
                        // 找出需要读取的最大索引
                        int maxIndex = -1;
                        for (int boneID = 0; boneID < BONE_COUNT; boneID++) {
                            int cstIndex = ca.boneMap[boneID];
                            if (cstIndex >= 0 && cstIndex < BoneCount) {
                                maxIndex = std::max(maxIndex, cstIndex);
                            }
                        }

                        if (maxIndex >= 0 && maxIndex < MAX_BONE_COUNT) {
                            // 只读取到最大索引的骨骼
                            int readCount = maxIndex + 1;
                            FTransform allBones[MAX_BONE_COUNT];
                            if (!GetDriverManager().read(BoneDataPtr, allBones, readCount * sizeof(FTransform))) {
                                continue;
                            }

                            for (int boneID = 0; boneID < BONE_COUNT; boneID++) {
                                int cstIndex = ca.boneMap[boneID];
                                if (cstIndex < 0 || cstIndex >= readCount) continue;

                                boneTranslations[boneID] = allBones[cstIndex].Translation;
                                if (boneID == BONE_HEAD){
                                    boneTranslations[boneID].Z += 7;
                                }

                                validCount++;
                            }
                        }
                    } else {
                        // 逐个读取（单缓冲）
                        for (int boneID = 0; boneID < BONE_COUNT; boneID++) {
                            int cstIndex = ca.boneMap[boneID];
                            if (cstIndex < 0 || cstIndex >= BoneCount) continue;

                            FTransform bone;
                            if (GetDriverManager().read(BoneDataPtr + cstIndex * sizeof(FTransform), &bone, sizeof(FTransform))) {
                                boneTranslations[boneID] = bone.Translation;
                                validCount++;
                            }
                        }
                    }

                    // 只有足够多的骨骼有效时才绘制
                    if (validCount >= 10) {
                        // 计算所有关键骨骼的世界坐标 + 插值平滑 + 屏幕投影
                        Vec2 boneScreenPos[BONE_COUNT];
                        bool boneOnScreen[BONE_COUNT] = {false};
                        bool boneVisible[BONE_COUNT];
                        std::fill(std::begin(boneVisible), std::end(boneVisible), true);
                        const bool autoAimNeedsVisibility =
                            (gAutoAim != nullptr) &&
                            gAutoAim->GetConfig().enabled &&
                            gAutoAim->GetConfig().visibilityCheck;
                        const bool needVisibility =
                            gUseDepthBufferVisibility && (gDrawSkeleton || autoAimNeedsVisibility);
                        const Vec3 cameraWorldPos = needVisibility ? ReadCameraWorldPosForPhysX() : Vec3::Zero();
                        const bool useVisibilityCheck = needVisibility &&
                                                        (cameraWorldPos.X != 0.0f || cameraWorldPos.Y != 0.0f || cameraWorldPos.Z != 0.0f);

                        // 查找/创建插值缓存
                        auto& cache = gBoneWorldCache[ca.actorAddr];

                        float lerpFactor = 1.0f;
                        if (gEnableBoneSmoothing) {
                            // lerpFactor 越小越平滑，越大越跟手。
                            lerpFactor = std::min(1.0f, gameStepDeltaTime * 35.0f);
                        }

                        // 收集所有骨骼世界坐标
                        Vec3 boneWorldPositions[BONE_COUNT];
                        bool boneHasData[BONE_COUNT] = {false};

                        for (int boneID = 0; boneID < BONE_COUNT; boneID++) {
                            if (ca.boneMap[boneID] < 0) continue;

                            // 骨骼世界坐标 = mesh transform + bone translation
                            Vec3 boneLocal = boneTranslations[boneID];
                            Vec3 worldPos = {
                                meshMatrix.M[0][0] * boneLocal.X + meshMatrix.M[1][0] * boneLocal.Y + meshMatrix.M[2][0] * boneLocal.Z + meshMatrix.M[3][0],
                                meshMatrix.M[0][1] * boneLocal.X + meshMatrix.M[1][1] * boneLocal.Y + meshMatrix.M[2][1] * boneLocal.Z + meshMatrix.M[3][1],
                                meshMatrix.M[0][2] * boneLocal.X + meshMatrix.M[1][2] * boneLocal.Y + meshMatrix.M[2][2] * boneLocal.Z + meshMatrix.M[3][2]
                            };

                            // 插值平滑：与上一帧缓存位置做 lerp
                            if (gEnableBoneSmoothing && cache.initialized && cache.valid[boneID]) {
                                Vec3 prev = cache.positions[boneID];
                                // 距离过大说明瞬移/传送，不插值
                                float dist = Vec3::Distance(prev, worldPos);
                                if (dist < 500.0f) {
                                    worldPos = Vec3{
                                        prev.X + (worldPos.X - prev.X) * lerpFactor,
                                        prev.Y + (worldPos.Y - prev.Y) * lerpFactor,
                                        prev.Z + (worldPos.Z - prev.Z) * lerpFactor
                                    };
                                }
                            }

                            // 更新缓存
                            cache.positions[boneID] = worldPos;
                            cache.valid[boneID] = true;

                            // 保存标签位置
                            if (boneID == BONE_HEAD) {
                                topLabelWorldPos = worldPos;
                                topLabelWorldPos.Z += 15.0f;  // 人物标签整体上移，给血条留空间
                                hasTopLabel = true;
                            } else if (boneID == BONE_ROOT) {  // 根骨骼（用于 bottom label）
                                bottomLabelWorldPos = worldPos;
                                bottomLabelWorldPos.Z -= 10.0f;  // 根骨骼下方 10 单位
                                hasBottomLabel = true;
                            }

                            if (WorldToScreen(worldPos, VPMat, io.DisplaySize.x, io.DisplaySize.y, boneScreenPos[boneID])) {
                                boneOnScreen[boneID] = true;
                            }

                            boneWorldPositions[boneID] = worldPos;
                            boneHasData[boneID] = true;
                        }

                        // 批量可视性判断：遍历 pruner 对象一次，测试所有骨骼
                        // 远距离 actor 屏幕占比小，跳过遮挡检测以节省开销
                        if (useVisibilityCheck && distance < gMaxSkeletonDistance * 0.6f) {
                            BatchBoneVisibilityCheck(cameraWorldPos, boneWorldPositions, boneHasData, boneVisible, BONE_COUNT);
                        }

                        // 标记缓存已初始化
                        cache.lastFrameCounter = engineFrame;
                        cache.initialized = true;

                        // 绘制骨骼连接线（使用距离衰减的透明度，超过设定距离几乎透明）
                        if (gDrawSkeleton) {
                            for (const auto& conn : kBoneConnections) {
                                int bone1 = conn.first;
                                int bone2 = conn.second;
                                if (boneOnScreen[bone1] && boneOnScreen[bone2] &&
                                    boneVisible[bone1] && boneVisible[bone2]) {
                                    ImVec2 p1(boneScreenPos[bone1].x, boneScreenPos[bone1].y);
                                    ImVec2 p2(boneScreenPos[bone2].x, boneScreenPos[bone2].y);
                                    draw_list->AddLine(p1, p2, IM_COL32(0, 255, 0, boneAlpha), 2.0f);
                                }
                            }
                        }

                        if (gDrawPredictedAimPoint &&
                            (ca.actorType == ActorType::PLAYER || ca.actorType == ActorType::BOT)) {
                            const int targetBoneID =
                                (gAutoAim != nullptr) ? gAutoAim->GetConfig().targetBone : BONE_HEAD;
                            if (targetBoneID >= 0 && targetBoneID < BONE_COUNT && boneOnScreen[targetBoneID]) {
                                const Vec2 crosshairCenter(screenW * 0.5f, screenH * 0.5f);
                                const float shortSide = std::max(1.0f, std::min(screenW, screenH));
                                const float hoverRadius = shortSide * kPredictedAimCenterRadiusFraction;
                                const float dx = boneScreenPos[targetBoneID].x - crosshairCenter.x;
                                const float dy = boneScreenPos[targetBoneID].y - crosshairCenter.y;
                                const float centerDistance = std::sqrt(dx * dx + dy * dy);
                                float& hoverTime = gPredictedAimHoverTimes[ca.actorAddr];
                                if (centerDistance <= hoverRadius) {
                                    hoverTime = std::min(kPredictedAimShowDelaySeconds,
                                                         hoverTime + std::max(gameStepDeltaTime, 0.0f));
                                } else {
                                    hoverTime = 0.0f;
                                }

                                if (hoverTime >= kPredictedAimShowDelaySeconds) {
                                    PredictedAimPoint prediction;
                                    if (ComputePredictedAimPoint(ca.actorAddr, distance,
                                                                 boneScreenPos[targetBoneID], VPMat,
                                                                 screenW, screenH, prediction) &&
                                        prediction.predictedOnScreen) {
                                        const ImVec2 currentPoint(prediction.currentScreenPos.x,
                                                                  prediction.currentScreenPos.y);
                                        const ImVec2 predictedPoint(prediction.predictedScreenPos.x,
                                                                    prediction.predictedScreenPos.y);
                                        const ImU32 lineColor = IM_COL32(80, 190, 255, boneAlpha);
                                        const ImU32 ringColor = IM_COL32(140, 220, 255, boneAlpha);
                                        draw_list->AddLine(currentPoint, predictedPoint, lineColor, 1.5f);
                                        draw_list->AddCircle(predictedPoint, 8.0f, ringColor, 24, 2.0f);
                                        draw_list->AddLine(ImVec2(predictedPoint.x - 6.0f, predictedPoint.y),
                                                           ImVec2(predictedPoint.x + 6.0f, predictedPoint.y),
                                                           ringColor, 1.8f);
                                        draw_list->AddLine(ImVec2(predictedPoint.x, predictedPoint.y - 6.0f),
                                                           ImVec2(predictedPoint.x, predictedPoint.y + 6.0f),
                                                           ringColor, 1.8f);
                                    }
                                }
                            } else {
                                gPredictedAimHoverTimes[ca.actorAddr] = 0.0f;
                            }
                        }

                        // 缓存骨骼屏幕坐标供 auto-aim 使用
                        BoneScreenData bsd;
                        std::copy(std::begin(boneScreenPos), std::end(boneScreenPos), std::begin(bsd.screenPos));
                        std::copy(std::begin(boneOnScreen), std::end(boneOnScreen), std::begin(bsd.onScreen));
                        std::copy(std::begin(boneVisible), std::end(boneVisible), std::begin(bsd.visible));
                        bsd.distance = distance;
                        bsd.teamID = ca.teamID;
                        bsd.playerKey = ca.playerKey;
                        bsd.actorAddr = ca.actorAddr;
                        bsd.frameCounter = engineFrame;
                        bsd.valid = true;
                        bsd.skelMeshCompAddr = ca.skelMeshCompAddr;
                        bsd.boneDataPtr = ca.boneDataPtr;
                        std::copy(std::begin(ca.boneMap), std::end(ca.boneMap), std::begin(bsd.boneMap));
                        bsd.cachedBoneCount = ca.cachedBoneCount;

                        if (boneScreenCache != nullptr) {
                            (*boneScreenCache)[ca.actorAddr] = bsd;
                        }
                    }
                }
            }
        }

        // 绘制 Top Label（名称）
        if (gDrawName && hasTopLabel) {
            Vec2 topLabelScreenPos;
            if (WorldToScreen(topLabelWorldPos, VPMat, io.DisplaySize.x, io.DisplaySize.y, topLabelScreenPos)) {
                ImVec2 labelPos(topLabelScreenPos.x, topLabelScreenPos.y);
                ImU32 teamColor = GetActorColor(ca);
                float health = -1.0f;
                float healthMax = -1.0f;
                if (ca.actorType == ActorType::PLAYER || ca.actorType == ActorType::BOT) {
                    health = GetDriverManager().read<float>(ca.actorAddr + offset.Health);
                    healthMax = GetDriverManager().read<float>(ca.actorAddr + offset.HealthMax);
                }

                char displayName[256];
                const bool showBoxContent = IsBoxLikeActorType(ca.actorType) &&
                                            ShouldShowBoxContentNearCrosshair(labelPos, screenW, screenH);
                if (gShowAllClassNames) {
                    snprintf(displayName, sizeof(displayName), "%s", label);
                } else if (IsCategoryPrefixActorType(ca.actorType)) {
                    if (!ca.playerName.empty()) {
                        snprintf(displayName, sizeof(displayName), "%s: %s", label, ca.playerName.c_str());
                    } else {
                        snprintf(displayName, sizeof(displayName), "%s", label);
                    }
                } else if (!ca.playerName.empty() && ca.teamID >= 0) {
                    snprintf(displayName, sizeof(displayName), "[%d] %s", ca.teamID, ca.playerName.c_str());
                } else if (!ca.playerName.empty()) {
                    snprintf(displayName, sizeof(displayName), "%s", ca.playerName.c_str());
                } else if (showBoxContent && !ca.containerSummary.empty()) {
                    snprintf(displayName, sizeof(displayName), "%s [%s]", label, ca.containerSummary.c_str());
                } else {
                    snprintf(displayName, sizeof(displayName), "%s", label);
                }

                ImFont* font = io.FontDefault;
                float originalFontSize = font->LegacySize;
                float scaledFontSize = originalFontSize * textScale;
                ImVec2 textSize = font->CalcTextSizeA(scaledFontSize, FLT_MAX, 0.0f, displayName);
                const float labelYOffset = (ca.actorType == ActorType::PLAYER || ca.actorType == ActorType::BOT) ? 18.0f : 2.0f;
                ImVec2 textPos(labelPos.x - textSize.x * 0.5f, labelPos.y - textSize.y - labelYOffset);
                ImVec2 bgMin(textPos.x - 6.0f, textPos.y - 3.0f);
                ImVec2 bgMax(textPos.x + textSize.x + 6.0f, textPos.y + textSize.y + 3.0f);

                if (!UseCategoryLabelOnly(ca.actorType)) {
                    DrawLabelBackground(draw_list, bgMin, bgMax, IM_COL32(0, 0, 0, 140));
                    draw_list->AddRect(bgMin, bgMax, IM_COL32(255, 255, 255, 32), 6.0f);
                }

                draw_list->AddText(font, scaledFontSize, textPos, teamColor, displayName);

                if (health > 0.0f && healthMax > 1.0f && std::isfinite(health) && std::isfinite(healthMax)) {
                    const float healthRatio = std::clamp(health / healthMax, 0.0f, 1.0f);
                    const float barWidth = std::max(36.0f, textSize.x);
                    const float barHeight = std::max(4.0f, scaledFontSize * 0.18f);
                    ImVec2 barAnchor(labelPos.x, bgMin.y - barHeight - 7.0f);
                    DrawHealthBar(draw_list, barAnchor, barWidth, barHeight, healthRatio);
                }
            }
        }

        // 绘制 Bottom Label（距离）
        if (gDrawDistance && hasBottomLabel && distance > 0.0f) {
            Vec2 bottomLabelScreenPos;
            if (WorldToScreen(bottomLabelWorldPos, VPMat, io.DisplaySize.x, io.DisplaySize.y, bottomLabelScreenPos)) {
                ImVec2 labelPos(bottomLabelScreenPos.x, bottomLabelScreenPos.y);

                char distanceText[64];
                snprintf(distanceText, sizeof(distanceText), "%.0fm", distance);

                ImFont* font = io.FontDefault;
                float originalFontSize = font->LegacySize;
                float scaledFontSize = originalFontSize * textScale * 0.8f;
                ImVec2 textSize = font->CalcTextSizeA(scaledFontSize, FLT_MAX, 0.0f, distanceText);
                ImVec2 textPos(labelPos.x - textSize.x * 0.5f, labelPos.y + 5.0f);

                draw_list->AddText(font, scaledFontSize, textPos, IM_COL32(255, 255, 255, 255), distanceText);
            }
        }

    }
}

BoneScreenCacheSnapshot GetBoneScreenCacheSnapshot() {
    return std::atomic_load_explicit(&gBoneScreenCacheSnapshot, std::memory_order_acquire);
}

bool GetCachedBoneWorldPos(uint64_t actorAddr, int boneID, uint64_t frameCounter, Vec3& outWorldPos) {
    const auto it = gBoneWorldCache.find(actorAddr);
    if (it == gBoneWorldCache.end()) return false;
    if (boneID < 0 || boneID >= BONE_COUNT) return false;
    if (!it->second.initialized || !it->second.valid[boneID]) return false;
    if (frameCounter != 0 && it->second.lastFrameCounter != 0 && it->second.lastFrameCounter != frameCounter) {
        return false;
    }
    outWorldPos = it->second.positions[boneID];
    return true;
}

static bool SegmentIntersectsBoxShape(const Vec3& startWorld, const Vec3& endWorld,
                                      const PxTransformData& worldPose, const Vec3& halfExtents) {
    const Vec3 startLocal = InverseTransformPoint(worldPose, startWorld);
    const Vec3 endLocal = InverseTransformPoint(worldPose, endWorld);
    const PhysXBounds3 localBounds{{-halfExtents.X, -halfExtents.Y, -halfExtents.Z},
                                   { halfExtents.X,  halfExtents.Y,  halfExtents.Z}};
    return SegmentIntersectsBounds(startLocal, endLocal, localBounds, nullptr);
}

static bool SegmentIntersectsTriangleMeshShape(const Vec3& startWorld, const Vec3& endWorld,
                                               const PxTransformData& worldPose, uint64_t geometryAddr,
                                               const D4DVector* invRotation = nullptr,
                                               const ScaledMeshData* preScaled = nullptr) {
    // 快速路径：使用预缩放数据（避免读取远端 + 避免 ApplyMeshScale + 避免 MakeBoundsFromTriangle）
    if (preScaled && preScaled->TriangleCount > 0) {
        const D4DVector invRot = invRotation ? *invRotation : QuatConjugate(worldPose.Rotation);
        const Vec3 startLocal = InverseTransformPointFast(worldPose.Translation, invRot, startWorld);
        const Vec3 endLocal = InverseTransformPointFast(worldPose.Translation, invRot, endWorld);

        if (!SegmentIntersectsBounds(startLocal, endLocal, preScaled->MeshAABB, nullptr)) {
            return false;
        }

        const PhysXBounds3 segBounds = MakeBoundsFromSegment(startLocal, endLocal, 2.0f);
        const size_t triCount = std::min(static_cast<size_t>(preScaled->TriangleCount),
                                         static_cast<size_t>(preScaled->Indices.size() / 3ULL));
        const auto& verts = preScaled->Vertices;
        const auto& inds = preScaled->Indices;
        const auto& tBounds = preScaled->TriangleBounds;
        const auto& chunkBounds = preScaled->ChunkBounds;

        if (!chunkBounds.empty()) {
            for (size_t chunk = 0; chunk < chunkBounds.size(); ++chunk) {
                if (!BoundsOverlap(segBounds, chunkBounds[chunk])) continue;
                const size_t begin = chunk * kVisibilityTriangleChunkSize;
                const size_t end = std::min(begin + kVisibilityTriangleChunkSize, triCount);
                for (size_t i = begin; i < end; ++i) {
                    if (!BoundsOverlap(segBounds, tBounds[i])) continue;
                    const uint32_t ia = inds[i * 3];
                    const uint32_t ib = inds[i * 3 + 1];
                    const uint32_t ic = inds[i * 3 + 2];
                    if (ia >= verts.size() || ib >= verts.size() || ic >= verts.size()) continue;
                    if (SegmentIntersectsTriangle(startLocal, endLocal, verts[ia], verts[ib], verts[ic], nullptr)) {
                        return true;
                    }
                }
            }
            return false;
        }

        for (size_t i = 0; i < triCount; ++i) {
            if (!BoundsOverlap(segBounds, tBounds[i])) continue;
            const uint32_t ia = inds[i * 3];
            const uint32_t ib = inds[i * 3 + 1];
            const uint32_t ic = inds[i * 3 + 2];
            if (ia >= verts.size() || ib >= verts.size() || ic >= verts.size()) continue;
            if (SegmentIntersectsTriangle(startLocal, endLocal, verts[ia], verts[ib], verts[ic], nullptr)) {
                return true;
            }
        }
        return false;
    }

    const uint64_t mesh = GetDriverManager().read<uint64_t>(geometryAddr + offset.PxTriangleMeshGeometryTriangleMesh);
    if (mesh == 0) return false;

    PxMeshScaleData scale{};
    if (!ReadMeshScale(geometryAddr + offset.PxTriangleMeshGeometryScale, scale)) return false;

    // 使用预计算逆旋转（如有），避免重复 QuatConjugate
    const D4DVector invRot = invRotation ? *invRotation : QuatConjugate(worldPose.Rotation);
    const Vec3 startLocal = InverseTransformPointFast(worldPose.Translation, invRot, startWorld);
    const Vec3 endLocal = InverseTransformPointFast(worldPose.Translation, invRot, endWorld);
    const PhysXBounds3 segmentBounds = MakeBoundsFromSegment(startLocal, endLocal, 2.0f);
    if (gPhysXUseLocalModelData) {
        const LocalObjMeshData* localMesh = GetLocalTriangleMeshDataStrict(mesh);
        if (localMesh) {
            const LocalTriangleMeshBvhData* localBvh = GetLocalTriangleMeshBvhDataStrict(mesh);
            if (localBvh) {
                const PhysXBounds3 scaledRootBounds = IsMeshScaleIdentity(scale)
                    ? localBvh->RootBounds
                    : ApplyMeshScaleToBounds(localBvh->RootBounds, scale);
                if (!SegmentIntersectsBounds(startLocal, endLocal, scaledRootBounds, nullptr)) {
                    return false;
                }
                bool intersectsAnyNode = false;
                for (const LocalBvhNodeBounds& node : localBvh->Nodes) {
                    const PhysXBounds3 scaledNodeBounds = IsMeshScaleIdentity(scale)
                        ? node.Bounds
                        : ApplyMeshScaleToBounds(node.Bounds, scale);
                    if (SegmentIntersectsBounds(startLocal, endLocal, scaledNodeBounds, nullptr)) {
                        intersectsAnyNode = true;
                        break;
                    }
                }
                if (!intersectsAnyNode) {
                    return false;
                }
            }

            const size_t localTriLimit = localMesh->Indices.size();
            const bool localScaleIsIdentity = IsMeshScaleIdentity(scale);
            for (size_t i = 0; i + 2 < localTriLimit; i += 3) {
                const uint32_t ia = localMesh->Indices[i];
                const uint32_t ib = localMesh->Indices[i + 1];
                const uint32_t ic = localMesh->Indices[i + 2];
                if (ia >= localMesh->Vertices.size() || ib >= localMesh->Vertices.size() || ic >= localMesh->Vertices.size()) continue;
                const Vec3 p0 = localScaleIsIdentity ? localMesh->Vertices[ia] : ApplyMeshScale(localMesh->Vertices[ia], scale);
                const Vec3 p1 = localScaleIsIdentity ? localMesh->Vertices[ib] : ApplyMeshScale(localMesh->Vertices[ib], scale);
                const Vec3 p2 = localScaleIsIdentity ? localMesh->Vertices[ic] : ApplyMeshScale(localMesh->Vertices[ic], scale);
                if (!BoundsOverlap(segmentBounds, MakeBoundsFromTriangle(p0, p1, p2, 1.0f))) continue;
                if (SegmentIntersectsTriangle(startLocal, endLocal, p0, p1, p2, nullptr)) {
                    return true;
                }
            }
            return false;
        }
        return false;  // 本地模式下无本地文件则跳过
    }

    const uint32_t vertexCount = GetDriverManager().read<uint32_t>(mesh + offset.PxTriangleMeshVertexCount);
    const uint32_t triangleCount = GetDriverManager().read<uint32_t>(mesh + offset.PxTriangleMeshTriangleCount);
    const uint64_t verticesAddr = GetDriverManager().read<uint64_t>(mesh + offset.PxTriangleMeshVertices);
    const uint64_t trianglesAddr = GetDriverManager().read<uint64_t>(mesh + offset.PxTriangleMeshTriangles);
    if (vertexCount == 0 || triangleCount == 0 || verticesAddr == 0 || trianglesAddr == 0) return false;

    CachedTriangleMeshData cache;
    const uint8_t flags = GetDriverManager().read<uint8_t>(mesh + offset.PxTriangleMeshFlags);
    if (!GetCachedTriangleMesh(mesh, vertexCount, triangleCount, flags, verticesAddr, trianglesAddr, cache)) {
        return false;
    }

    // 用缓存的整体包围盒做快速拒绝（对包围盒应用 mesh scale 的粗近似）
    {
        const PhysXBounds3 scaledMeshBounds = ApplyMeshScaleToBounds(cache.MeshAABB, scale);
        const PhysXBounds3 scaledAABB{
            {scaledMeshBounds.Min.X - 2.0f, scaledMeshBounds.Min.Y - 2.0f, scaledMeshBounds.Min.Z - 2.0f},
            {scaledMeshBounds.Max.X + 2.0f, scaledMeshBounds.Max.Y + 2.0f, scaledMeshBounds.Max.Z + 2.0f}
        };
        if (!SegmentIntersectsBounds(startLocal, endLocal, scaledAABB, nullptr)) {
            return false;
        }
    }

    const auto& vertices = cache.Vertices;
    const auto& indices = cache.Indices;
    const auto& triBounds = cache.TriangleBounds;
    const auto& chunkBounds = cache.ChunkBounds;
    const size_t triangleCountToTest = std::min(static_cast<size_t>(triangleCount),
                                                indices.size() / static_cast<size_t>(3));
    const bool hasPrecomputedBounds = (triBounds.size() >= triangleCountToTest);

    const bool scaleIsIdentity = IsMeshScaleIdentity(scale);
    const size_t chunkCount = chunkBounds.empty() ? 0
        : std::min(chunkBounds.size(),
                   (triangleCountToTest + kVisibilityTriangleChunkSize - 1) / kVisibilityTriangleChunkSize);

    auto test_triangle = [&](size_t i) -> bool {
        if (hasPrecomputedBounds) {
            if (scaleIsIdentity) {
                const PhysXBounds3 inflated{
                    {triBounds[i].Min.X - 1.0f, triBounds[i].Min.Y - 1.0f, triBounds[i].Min.Z - 1.0f},
                    {triBounds[i].Max.X + 1.0f, triBounds[i].Max.Y + 1.0f, triBounds[i].Max.Z + 1.0f}
                };
                if (!BoundsOverlap(segmentBounds, inflated)) return false;
            } else {
                const PhysXBounds3 scaledTriBounds = ApplyMeshScaleToBounds(triBounds[i], scale);
                const PhysXBounds3 inflated{
                    {scaledTriBounds.Min.X - 1.0f, scaledTriBounds.Min.Y - 1.0f, scaledTriBounds.Min.Z - 1.0f},
                    {scaledTriBounds.Max.X + 1.0f, scaledTriBounds.Max.Y + 1.0f, scaledTriBounds.Max.Z + 1.0f}
                };
                if (!BoundsOverlap(segmentBounds, inflated)) return false;
            }
        }

        const size_t triBase = i * 3ULL;
        const uint32_t ia = indices[triBase];
        const uint32_t ib = indices[triBase + 1];
        const uint32_t ic = indices[triBase + 2];
        if (ia >= vertices.size() || ib >= vertices.size() || ic >= vertices.size()) return false;

        const Vec3 p0 = scaleIsIdentity ? vertices[ia] : ApplyMeshScale(vertices[ia], scale);
        const Vec3 p1 = scaleIsIdentity ? vertices[ib] : ApplyMeshScale(vertices[ib], scale);
        const Vec3 p2 = scaleIsIdentity ? vertices[ic] : ApplyMeshScale(vertices[ic], scale);
        if (!scaleIsIdentity || !hasPrecomputedBounds) {
            if (!BoundsOverlap(segmentBounds, MakeBoundsFromTriangle(p0, p1, p2, 1.0f))) return false;
        }
        return SegmentIntersectsTriangle(startLocal, endLocal, p0, p1, p2, nullptr);
    };

    if (chunkCount > 0) {
        for (size_t chunk = 0; chunk < chunkCount; ++chunk) {
            PhysXBounds3 candidateChunkBounds = chunkBounds[chunk];
            if (!scaleIsIdentity) {
                candidateChunkBounds = ApplyMeshScaleToBounds(candidateChunkBounds, scale);
            }
            if (!BoundsOverlap(segmentBounds, candidateChunkBounds)) continue;

            const size_t begin = chunk * kVisibilityTriangleChunkSize;
            const size_t end = std::min(begin + kVisibilityTriangleChunkSize, triangleCountToTest);
            for (size_t i = begin; i < end; ++i) {
                if (test_triangle(i)) return true;
            }
        }
    } else {
        for (size_t i = 0; i < triangleCountToTest; ++i) {
            if (test_triangle(i)) return true;
        }
    }

    return false;
}

static bool SegmentIntersectsLocalTriangleMeshShape(const Vec3& startWorld, const Vec3& endWorld,
                                                    const PxTransformData& worldPose, uint64_t mesh,
                                                    const PxMeshScaleData& scale,
                                                    const D4DVector* invRotation = nullptr) {
    if (mesh == 0) return false;

    const D4DVector invRot = invRotation ? *invRotation : QuatConjugate(worldPose.Rotation);
    const Vec3 startLocal = InverseTransformPointFast(worldPose.Translation, invRot, startWorld);
    const Vec3 endLocal = InverseTransformPointFast(worldPose.Translation, invRot, endWorld);
    const PhysXBounds3 segmentBounds = MakeBoundsFromSegment(startLocal, endLocal, 2.0f);

    const LocalObjMeshData* localMesh = GetRenderableLocalTriangleMesh(mesh);
    if (!localMesh) return false;

    const LocalTriangleMeshBvhData* localBvh = GetLocalTriangleMeshBvhDataStrict(mesh);
    if (localBvh) {
        const PhysXBounds3 scaledRootBounds = IsMeshScaleIdentity(scale)
            ? localBvh->RootBounds
            : ApplyMeshScaleToBounds(localBvh->RootBounds, scale);
        if (!SegmentIntersectsBounds(startLocal, endLocal, scaledRootBounds, nullptr)) {
            return false;
        }
        bool intersectsAnyNode = false;
        for (const LocalBvhNodeBounds& node : localBvh->Nodes) {
            const PhysXBounds3 scaledNodeBounds = IsMeshScaleIdentity(scale)
                ? node.Bounds
                : ApplyMeshScaleToBounds(node.Bounds, scale);
            if (SegmentIntersectsBounds(startLocal, endLocal, scaledNodeBounds, nullptr)) {
                intersectsAnyNode = true;
                break;
            }
        }
        if (!intersectsAnyNode) {
            return false;
        }
    }

    const size_t localTriLimit = localMesh->Indices.size();
    const bool localScaleIsIdentity = IsMeshScaleIdentity(scale);
    for (size_t i = 0; i + 2 < localTriLimit; i += 3) {
        const uint32_t ia = localMesh->Indices[i];
        const uint32_t ib = localMesh->Indices[i + 1];
        const uint32_t ic = localMesh->Indices[i + 2];
        if (ia >= localMesh->Vertices.size() || ib >= localMesh->Vertices.size() || ic >= localMesh->Vertices.size()) continue;
        const Vec3 p0 = localScaleIsIdentity ? localMesh->Vertices[ia] : ApplyMeshScale(localMesh->Vertices[ia], scale);
        const Vec3 p1 = localScaleIsIdentity ? localMesh->Vertices[ib] : ApplyMeshScale(localMesh->Vertices[ib], scale);
        const Vec3 p2 = localScaleIsIdentity ? localMesh->Vertices[ic] : ApplyMeshScale(localMesh->Vertices[ic], scale);
        if (!BoundsOverlap(segmentBounds, MakeBoundsFromTriangle(p0, p1, p2, 1.0f))) continue;
        if (SegmentIntersectsTriangle(startLocal, endLocal, p0, p1, p2, nullptr)) {
            return true;
        }
    }
    return false;
}

static bool SegmentIntersectsHeightFieldShape(const Vec3& startWorld, const Vec3& endWorld,
                                              const PxTransformData& worldPose, uint64_t geometryAddr,
                                              const D4DVector* invRotation = nullptr) {
    const uint64_t heightField = GetDriverManager().read<uint64_t>(geometryAddr + offset.PxHeightFieldGeometryHeightField);
    if (heightField == 0) return false;

    const float heightScale = GetDriverManager().read<float>(geometryAddr + offset.PxHeightFieldGeometryHeightScale);
    const float rowScale = GetDriverManager().read<float>(geometryAddr + offset.PxHeightFieldGeometryRowScale);
    const float columnScale = GetDriverManager().read<float>(geometryAddr + offset.PxHeightFieldGeometryColumnScale);
    if (std::fabs(heightScale) <= 1e-6f || std::fabs(rowScale) <= 1e-6f || std::fabs(columnScale) <= 1e-6f) {
        return false;
    }

    const D4DVector invRot = invRotation ? *invRotation : QuatConjugate(worldPose.Rotation);
    const Vec3 startLocal = InverseTransformPointFast(worldPose.Translation, invRot, startWorld);
    const Vec3 endLocal = InverseTransformPointFast(worldPose.Translation, invRot, endWorld);
    const PhysXBounds3 segmentBounds = MakeBoundsFromSegment(startLocal, endLocal, 2.0f);

    if (gPhysXUseLocalModelData) {
        const LocalObjMeshData* localMesh = GetLocalHeightFieldDataStrict(heightField);
        if (localMesh) {
            const size_t hfTriLimit = localMesh->Indices.size();
            for (size_t i = 0; i + 2 < hfTriLimit; i += 3) {
                const uint32_t ia = localMesh->Indices[i];
                const uint32_t ib = localMesh->Indices[i + 1];
                const uint32_t ic = localMesh->Indices[i + 2];
                if (ia >= localMesh->Vertices.size() || ib >= localMesh->Vertices.size() || ic >= localMesh->Vertices.size()) continue;
                const Vec3 p0{localMesh->Vertices[ia].X * rowScale, localMesh->Vertices[ia].Y * heightScale, localMesh->Vertices[ia].Z * columnScale};
                const Vec3 p1{localMesh->Vertices[ib].X * rowScale, localMesh->Vertices[ib].Y * heightScale, localMesh->Vertices[ib].Z * columnScale};
                const Vec3 p2{localMesh->Vertices[ic].X * rowScale, localMesh->Vertices[ic].Y * heightScale, localMesh->Vertices[ic].Z * columnScale};
                if (!BoundsOverlap(segmentBounds, MakeBoundsFromTriangle(p0, p1, p2, 1.0f))) continue;
                if (SegmentIntersectsTriangle(startLocal, endLocal, p0, p1, p2, nullptr)) {
                    return true;
                }
            }
            return false;
        }
        return false;  // 本地模式下无本地文件则跳过
    }

    CachedHeightFieldData cache;
    if (!GetCachedHeightField(heightField, cache)) return false;
    if (cache.Rows < 2 || cache.Columns < 2) return false;

    uint32_t minRow = 0;
    uint32_t maxRow = 0;
    uint32_t minColumn = 0;
    uint32_t maxColumn = 0;
    GetHeightFieldCellRange(segmentBounds, rowScale, columnScale, cache.Rows, cache.Columns,
                            minRow, maxRow, minColumn, maxColumn);
    for (uint32_t row = minRow; row < maxRow; ++row) {
        for (uint32_t column = minColumn; column < maxColumn; ++column) {
            Vec3 t0a{}, t0b{}, t0c{}, t1a{}, t1b{}, t1c{};
            bool t0Valid = false, t1Valid = false;
            if (!GetHeightFieldCellTriangles(cache, row, column, heightScale, rowScale, columnScale,
                                             t0a, t0b, t0c, t0Valid, t1a, t1b, t1c, t1Valid)) {
                continue;
            }

            if (t0Valid &&
                BoundsOverlap(segmentBounds, MakeBoundsFromTriangle(t0a, t0b, t0c, 1.0f)) &&
                SegmentIntersectsTriangle(startLocal, endLocal, t0a, t0b, t0c, nullptr)) {
                return true;
            }
            if (t1Valid &&
                BoundsOverlap(segmentBounds, MakeBoundsFromTriangle(t1a, t1b, t1c, 1.0f)) &&
                SegmentIntersectsTriangle(startLocal, endLocal, t1a, t1b, t1c, nullptr)) {
                return true;
            }
        }
    }

    return false;
}

static bool SegmentIntersectsLocalHeightFieldShape(const Vec3& startWorld, const Vec3& endWorld,
                                                   const PxTransformData& worldPose, uint64_t heightField,
                                                   float heightScale, float rowScale, float columnScale,
                                                   const D4DVector* invRotation = nullptr) {
    if (heightField == 0) return false;
    if (std::fabs(heightScale) <= 1e-6f || std::fabs(rowScale) <= 1e-6f || std::fabs(columnScale) <= 1e-6f) {
        return false;
    }

    const D4DVector invRot = invRotation ? *invRotation : QuatConjugate(worldPose.Rotation);
    const Vec3 startLocal = InverseTransformPointFast(worldPose.Translation, invRot, startWorld);
    const Vec3 endLocal = InverseTransformPointFast(worldPose.Translation, invRot, endWorld);
    const PhysXBounds3 segmentBounds = MakeBoundsFromSegment(startLocal, endLocal, 2.0f);

    const LocalObjMeshData* localMesh = GetRenderableLocalHeightField(heightField);
    if (!localMesh) return false;

    const size_t hfTriLimit = localMesh->Indices.size();
    for (size_t i = 0; i + 2 < hfTriLimit; i += 3) {
        const uint32_t ia = localMesh->Indices[i];
        const uint32_t ib = localMesh->Indices[i + 1];
        const uint32_t ic = localMesh->Indices[i + 2];
        if (ia >= localMesh->Vertices.size() || ib >= localMesh->Vertices.size() || ic >= localMesh->Vertices.size()) continue;
        const Vec3 p0{localMesh->Vertices[ia].X * rowScale, localMesh->Vertices[ia].Y * heightScale, localMesh->Vertices[ia].Z * columnScale};
        const Vec3 p1{localMesh->Vertices[ib].X * rowScale, localMesh->Vertices[ib].Y * heightScale, localMesh->Vertices[ib].Z * columnScale};
        const Vec3 p2{localMesh->Vertices[ic].X * rowScale, localMesh->Vertices[ic].Y * heightScale, localMesh->Vertices[ic].Z * columnScale};
        if (!BoundsOverlap(segmentBounds, MakeBoundsFromTriangle(p0, p1, p2, 1.0f))) continue;
        if (SegmentIntersectsTriangle(startLocal, endLocal, p0, p1, p2, nullptr)) {
            return true;
        }
    }
    return false;
}

struct VisibilityBuildCandidate {
    uint64_t Key = 0;
    uint64_t Resource = 0;
    uint64_t Shape = 0;
    uint32_t GeometryType = 0;
    PxTransformData WorldPose{};
    PxMeshScaleData MeshScale{{1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 1.0f}};
    float HeightScale = 1.0f;
    float RowScale = 1.0f;
    float ColumnScale = 1.0f;
    bool IsStatic = true;
};

static uint64_t VisibilityHashCombine(uint64_t seed, uint64_t value) {
    return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
}

static uint64_t VisibilityFloatBits(float value) {
    uint32_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static uint64_t BuildVisibilityCandidateSignature(const VisibilityBuildCandidate& candidate) {
    uint64_t seed = candidate.Resource;
    seed = VisibilityHashCombine(seed, candidate.GeometryType);
    seed = VisibilityHashCombine(seed, VisibilityFloatBits(candidate.WorldPose.Translation.X));
    seed = VisibilityHashCombine(seed, VisibilityFloatBits(candidate.WorldPose.Translation.Y));
    seed = VisibilityHashCombine(seed, VisibilityFloatBits(candidate.WorldPose.Translation.Z));
    seed = VisibilityHashCombine(seed, VisibilityFloatBits(candidate.WorldPose.Rotation.X));
    seed = VisibilityHashCombine(seed, VisibilityFloatBits(candidate.WorldPose.Rotation.Y));
    seed = VisibilityHashCombine(seed, VisibilityFloatBits(candidate.WorldPose.Rotation.Z));
    seed = VisibilityHashCombine(seed, VisibilityFloatBits(candidate.WorldPose.Rotation.W));
    seed = VisibilityHashCombine(seed, VisibilityFloatBits(candidate.MeshScale.Scale.X));
    seed = VisibilityHashCombine(seed, VisibilityFloatBits(candidate.MeshScale.Scale.Y));
    seed = VisibilityHashCombine(seed, VisibilityFloatBits(candidate.MeshScale.Scale.Z));
    seed = VisibilityHashCombine(seed, VisibilityFloatBits(candidate.MeshScale.Rotation.X));
    seed = VisibilityHashCombine(seed, VisibilityFloatBits(candidate.MeshScale.Rotation.Y));
    seed = VisibilityHashCombine(seed, VisibilityFloatBits(candidate.MeshScale.Rotation.Z));
    seed = VisibilityHashCombine(seed, VisibilityFloatBits(candidate.MeshScale.Rotation.W));
    seed = VisibilityHashCombine(seed, VisibilityFloatBits(candidate.HeightScale));
    seed = VisibilityHashCombine(seed, VisibilityFloatBits(candidate.RowScale));
    seed = VisibilityHashCombine(seed, VisibilityFloatBits(candidate.ColumnScale));
    return seed;
}

static void ComputeVisibilityMeshBounds(VisibilityMeshData& mesh) {
    if (mesh.Vertices.empty()) {
        mesh.Bounds = {};
        return;
    }

    VisibilityBounds bounds{mesh.Vertices[0], mesh.Vertices[0]};
    for (const Vec3& vertex : mesh.Vertices) {
        bounds.Min.X = std::min(bounds.Min.X, vertex.X);
        bounds.Min.Y = std::min(bounds.Min.Y, vertex.Y);
        bounds.Min.Z = std::min(bounds.Min.Z, vertex.Z);
        bounds.Max.X = std::max(bounds.Max.X, vertex.X);
        bounds.Max.Y = std::max(bounds.Max.Y, vertex.Y);
        bounds.Max.Z = std::max(bounds.Max.Z, vertex.Z);
    }
    mesh.Bounds = bounds;
}

static bool BuildVisibilityTriangleMeshData(const VisibilityBuildCandidate& candidate,
                                            VisibilityMeshData& out) {
    out = {};
    out.Key = candidate.Key;
    out.Resource = candidate.Resource;
    out.Shape = candidate.Shape;
    out.GeometryType = candidate.GeometryType;
    if (candidate.Resource == 0) return false;

    if (gPhysXUseLocalModelData) {
        const LocalObjMeshData* localMesh = GetRenderableLocalTriangleMesh(candidate.Resource);
        if (!localMesh || localMesh->Vertices.empty() || localMesh->Indices.empty()) return false;

        out.Indices = localMesh->Indices;
        out.Vertices.reserve(localMesh->Vertices.size());
        const bool identityScale = IsMeshScaleIdentity(candidate.MeshScale);
        for (const Vec3& localVertex : localMesh->Vertices) {
            const Vec3 scaled = identityScale ? localVertex : ApplyMeshScale(localVertex, candidate.MeshScale);
            out.Vertices.push_back(TransformPoint(candidate.WorldPose, scaled));
        }
    } else {
        const uint64_t mesh = candidate.Resource;
        const uint32_t vertexCount = GetDriverManager().read<uint32_t>(mesh + offset.PxTriangleMeshVertexCount);
        const uint32_t triangleCount = GetDriverManager().read<uint32_t>(mesh + offset.PxTriangleMeshTriangleCount);
        const uint64_t verticesAddr = GetDriverManager().read<uint64_t>(mesh + offset.PxTriangleMeshVertices);
        const uint64_t trianglesAddr = GetDriverManager().read<uint64_t>(mesh + offset.PxTriangleMeshTriangles);
        const uint8_t flags = GetDriverManager().read<uint8_t>(mesh + offset.PxTriangleMeshFlags);
        if (vertexCount == 0 || triangleCount == 0 || verticesAddr == 0 || trianglesAddr == 0) return false;

        CachedTriangleMeshData cache;
        if (!GetCachedTriangleMesh(mesh, vertexCount, triangleCount, flags, verticesAddr, trianglesAddr, cache)) return false;
        if (cache.Vertices.empty() || cache.Indices.empty()) return false;

        out.Indices = cache.Indices;
        out.Vertices.reserve(cache.Vertices.size());
        const bool identityScale = IsMeshScaleIdentity(candidate.MeshScale);
        for (const Vec3& localVertex : cache.Vertices) {
            const Vec3 scaled = identityScale ? localVertex : ApplyMeshScale(localVertex, candidate.MeshScale);
            out.Vertices.push_back(TransformPoint(candidate.WorldPose, scaled));
        }
    }

    if (out.Indices.size() < 3 || out.Vertices.empty()) return false;
    ComputeVisibilityMeshBounds(out);
    return true;
}

static bool BuildVisibilityHeightFieldData(const VisibilityBuildCandidate& candidate,
                                           VisibilityMeshData& out) {
    out = {};
    out.Key = candidate.Key;
    out.Resource = candidate.Resource;
    out.Shape = candidate.Shape;
    out.GeometryType = candidate.GeometryType;
    if (candidate.Resource == 0) return false;

    auto append_triangle = [&](const Vec3& a, const Vec3& b, const Vec3& c) {
        const uint32_t base = static_cast<uint32_t>(out.Vertices.size());
        out.Vertices.push_back(a);
        out.Vertices.push_back(b);
        out.Vertices.push_back(c);
        out.Indices.push_back(base);
        out.Indices.push_back(base + 1);
        out.Indices.push_back(base + 2);
    };

    if (gPhysXUseLocalModelData) {
        const LocalObjMeshData* localMesh = GetRenderableLocalHeightField(candidate.Resource);
        if (!localMesh || localMesh->Vertices.empty() || localMesh->Indices.empty()) return false;

        out.Indices = localMesh->Indices;
        out.Vertices.reserve(localMesh->Vertices.size());
        for (const Vec3& vertex : localMesh->Vertices) {
            const Vec3 localPoint{
                vertex.X * candidate.RowScale,
                vertex.Y * candidate.HeightScale,
                vertex.Z * candidate.ColumnScale
            };
            out.Vertices.push_back(TransformPoint(candidate.WorldPose, localPoint));
        }
    } else {
        CachedHeightFieldData cache;
        if (!GetCachedHeightField(candidate.Resource, cache)) return false;
        if (cache.Rows < 2 || cache.Columns < 2) return false;

        const size_t estimatedTriangles = static_cast<size_t>(cache.Rows - 1) *
                                          static_cast<size_t>(cache.Columns - 1) * 2ULL;
        out.Vertices.reserve(estimatedTriangles * 3ULL);
        out.Indices.reserve(estimatedTriangles * 3ULL);

        for (uint32_t row = 0; row + 1 < cache.Rows; ++row) {
            for (uint32_t column = 0; column + 1 < cache.Columns; ++column) {
                Vec3 t0a{}, t0b{}, t0c{}, t1a{}, t1b{}, t1c{};
                bool t0Valid = false;
                bool t1Valid = false;
                if (!GetHeightFieldCellTriangles(cache, row, column,
                                                 candidate.HeightScale,
                                                 candidate.RowScale,
                                                 candidate.ColumnScale,
                                                 t0a, t0b, t0c, t0Valid,
                                                 t1a, t1b, t1c, t1Valid)) {
                    continue;
                }
                if (t0Valid) {
                    append_triangle(TransformPoint(candidate.WorldPose, t0a),
                                    TransformPoint(candidate.WorldPose, t0b),
                                    TransformPoint(candidate.WorldPose, t0c));
                }
                if (t1Valid) {
                    append_triangle(TransformPoint(candidate.WorldPose, t1a),
                                    TransformPoint(candidate.WorldPose, t1b),
                                    TransformPoint(candidate.WorldPose, t1c));
                }
            }
        }
    }

    if (out.Indices.size() < 3 || out.Vertices.empty()) return false;
    ComputeVisibilityMeshBounds(out);
    return true;
}

static bool MatchesVisibilitySceneKind(VisibilitySceneKind kind, uint32_t geometryType, bool isStatic) {
    switch (kind) {
        case VisibilitySceneKind::StaticMesh:
            return geometryType == PX_GEOM_TRIANGLEMESH && isStatic;
        case VisibilitySceneKind::HeightField:
            return geometryType == PX_GEOM_HEIGHTFIELD;
        case VisibilitySceneKind::DynamicMesh:
            return geometryType == PX_GEOM_TRIANGLEMESH && !isStatic;
    }
    return false;
}

static void GatherPrunerVisibilityCandidates(VisibilitySceneKind kind,
                                             const Vec3& cameraWorldPos,
                                             float maxDistanceSq,
                                             std::unordered_map<uint64_t, VisibilityBuildCandidate>& candidates) {
    if (kind == VisibilitySceneKind::DynamicMesh) return;
    if (!EnsurePrunerDataCached()) return;

    for (uint32_t i = 0; i < gPrunerCache.objectCount; ++i) {
        if (DistanceSqToBounds(cameraWorldPos, gPrunerCache.bounds[i]) > maxDistanceSq) continue;

        const PhysXPrunerPayload& payload = gPrunerCache.payloads[i];
        if (payload.Shape == 0 || payload.Actor == 0) continue;

        const uint64_t geometryAddr = payload.Shape + offset.ScbShapeCoreGeometry;
        const uint32_t geometryType = GetDriverManager().read<uint32_t>(geometryAddr);
        if (!MatchesVisibilitySceneKind(kind, geometryType, true)) continue;

        PxTransformData actorPose{}, shapePose{};
        if (!ReadScbStaticActorGlobalPose(payload.Actor, actorPose)) continue;
        if (!ReadScbShapeLocalPose(payload.Shape, shapePose)) continue;

        VisibilityBuildCandidate candidate{};
        candidate.Key = payload.Shape;
        candidate.Shape = payload.Shape;
        candidate.GeometryType = geometryType;
        candidate.WorldPose = ComposeTransforms(actorPose, shapePose);
        candidate.IsStatic = true;

        if (geometryType == PX_GEOM_TRIANGLEMESH) {
            candidate.Resource = GetDriverManager().read<uint64_t>(geometryAddr + offset.PxTriangleMeshGeometryTriangleMesh);
            if (!ReadMeshScale(geometryAddr + offset.PxTriangleMeshGeometryScale, candidate.MeshScale)) continue;
        } else if (geometryType == PX_GEOM_HEIGHTFIELD) {
            candidate.Resource = GetDriverManager().read<uint64_t>(geometryAddr + offset.PxHeightFieldGeometryHeightField);
            candidate.HeightScale = GetDriverManager().read<float>(geometryAddr + offset.PxHeightFieldGeometryHeightScale);
            candidate.RowScale = GetDriverManager().read<float>(geometryAddr + offset.PxHeightFieldGeometryRowScale);
            candidate.ColumnScale = GetDriverManager().read<float>(geometryAddr + offset.PxHeightFieldGeometryColumnScale);
            if (std::fabs(candidate.HeightScale) <= 1e-6f ||
                std::fabs(candidate.RowScale) <= 1e-6f ||
                std::fabs(candidate.ColumnScale) <= 1e-6f) {
                continue;
            }
        }

        if (candidate.Resource == 0) continue;
        candidates[candidate.Key] = candidate;
    }
}

static void GatherPxSceneVisibilityCandidates(VisibilitySceneKind kind,
                                              const Vec3& cameraWorldPos,
                                              float maxDistanceSq,
                                              std::unordered_map<uint64_t, VisibilityBuildCandidate>& candidates) {
    if (address.libUE4 == 0) return;

    const uint64_t uworld = GetDriverManager().read<uint64_t>(address.libUE4 + offset.Gworld);
    std::vector<uint64_t> pxScenes;
    CollectActivePxScenes(address.libUE4, uworld, pxScenes);
    for (uint64_t pxScene : pxScenes) {
        std::vector<uint64_t> actors;
        if (!CollectPxSceneActorPointers(pxScene, actors)) continue;
        for (uint64_t actor : actors) {
            if (actor == 0) continue;
            const uint16_t actorType = GetDriverManager().read<uint16_t>(actor + offset.PxActorType);
            const bool isStatic = actorType == PX_ACTOR_RIGID_STATIC;
            const bool isDynamic = actorType == PX_ACTOR_RIGID_DYNAMIC;
            if (!isStatic && !isDynamic) continue;

            PxTransformData actorPose{};
            if (!ReadActorGlobalPose(actor, actorType, actorPose)) continue;
            const Vec3 delta = actorPose.Translation - cameraWorldPos;
            if (Vec3::Dot(delta, delta) > maxDistanceSq * 4.0f) continue;

            const uint16_t shapeCount = GetDriverManager().read<uint16_t>(actor + offset.PxActorShapeCount);
            if (shapeCount == 0) continue;

            std::vector<uint64_t> shapes(std::min<uint16_t>(shapeCount, 64));
            if (shapeCount == 1) {
                shapes[0] = GetDriverManager().read<uint64_t>(actor + offset.PxActorShapes);
            } else {
                const uint64_t shapePtrArray = GetDriverManager().read<uint64_t>(actor + offset.PxActorShapes);
                if (shapePtrArray == 0) continue;
                if (!ReadRemoteBufferRobust(shapePtrArray, shapes.data(), shapes.size() * sizeof(uint64_t))) continue;
            }

            for (uint64_t shape : shapes) {
                if (shape == 0) continue;
                const uint32_t npShapeFlags = GetDriverManager().read<uint32_t>(shape + offset.PxShapeFlags);
                PxTransformData shapePose{};
                if (!ReadShapeLocalPose(shape, npShapeFlags, shapePose)) continue;

                uint64_t geometryAddr = shape + offset.PxShapeGeometryInline;
                if ((npShapeFlags & 0x1u) != 0) {
                    const uint64_t corePtr = GetDriverManager().read<uint64_t>(shape + offset.PxShapeCorePtr);
                    if (corePtr == 0) continue;
                    geometryAddr = corePtr + offset.PxShapeCoreGeometry;
                }

                const uint32_t geometryType = GetDriverManager().read<uint32_t>(geometryAddr);
                if (!MatchesVisibilitySceneKind(kind, geometryType, isStatic)) continue;

                VisibilityBuildCandidate candidate{};
                candidate.Key = shape;
                candidate.Shape = shape;
                candidate.GeometryType = geometryType;
                candidate.WorldPose = ComposeTransforms(actorPose, shapePose);
                candidate.IsStatic = isStatic;

                if (geometryType == PX_GEOM_TRIANGLEMESH) {
                    candidate.Resource = GetDriverManager().read<uint64_t>(geometryAddr + offset.PxTriangleMeshGeometryTriangleMesh);
                    if (!ReadMeshScale(geometryAddr + offset.PxTriangleMeshGeometryScale, candidate.MeshScale)) continue;
                } else if (geometryType == PX_GEOM_HEIGHTFIELD) {
                    candidate.Resource = GetDriverManager().read<uint64_t>(geometryAddr + offset.PxHeightFieldGeometryHeightField);
                    candidate.HeightScale = GetDriverManager().read<float>(geometryAddr + offset.PxHeightFieldGeometryHeightScale);
                    candidate.RowScale = GetDriverManager().read<float>(geometryAddr + offset.PxHeightFieldGeometryRowScale);
                    candidate.ColumnScale = GetDriverManager().read<float>(geometryAddr + offset.PxHeightFieldGeometryColumnScale);
                    if (std::fabs(candidate.HeightScale) <= 1e-6f ||
                        std::fabs(candidate.RowScale) <= 1e-6f ||
                        std::fabs(candidate.ColumnScale) <= 1e-6f) {
                        continue;
                    }
                } else {
                    continue;
                }

                if (candidate.Resource == 0) continue;
                candidates[candidate.Key] = candidate;
            }
        }
    }
}

static void GatherLocalVisibilityCandidates(VisibilitySceneKind kind,
                                            const Vec3& cameraWorldPos,
                                            float maxDistanceSq,
                                            std::unordered_map<uint64_t, VisibilityBuildCandidate>& candidates) {
    if (!gPhysXUseLocalModelData || gLocalPhysXDrawCache.empty()) return;

    for (const auto& [shape, entry] : gLocalPhysXDrawCache) {
        (void)shape;
        const Vec3 delta = entry.WorldPose.Translation - cameraWorldPos;
        if (Vec3::Dot(delta, delta) > maxDistanceSq) continue;
        if (!MatchesVisibilitySceneKind(kind, entry.GeometryType, entry.IsStaticMesh)) continue;

        if (entry.GeometryType == PX_GEOM_TRIANGLEMESH) {
            if (!GetRenderableLocalTriangleMesh(entry.Resource)) continue;
        } else if (entry.GeometryType == PX_GEOM_HEIGHTFIELD) {
            if (!GetRenderableLocalHeightField(entry.Resource)) continue;
        } else {
            continue;
        }

        VisibilityBuildCandidate candidate{};
        candidate.Key = entry.Shape;
        candidate.Resource = entry.Resource;
        candidate.Shape = entry.Shape;
        candidate.GeometryType = entry.GeometryType;
        candidate.WorldPose = entry.WorldPose;
        candidate.MeshScale = entry.MeshScale;
        candidate.HeightScale = entry.HeightScale;
        candidate.RowScale = entry.RowScale;
        candidate.ColumnScale = entry.ColumnScale;
        candidate.IsStatic = entry.IsStaticMesh;
        candidates[candidate.Key] = candidate;
    }
}

static std::chrono::milliseconds VisibilitySceneRefreshInterval(VisibilitySceneKind kind) {
    switch (kind) {
        case VisibilitySceneKind::StaticMesh:
            return std::chrono::milliseconds(2500);
        case VisibilitySceneKind::HeightField:
            return std::chrono::milliseconds(2500);
        case VisibilitySceneKind::DynamicMesh:
            return std::chrono::milliseconds(250);
    }
    return std::chrono::milliseconds(1000);
}

static std::chrono::milliseconds VisibilitySceneKeepAlive(VisibilitySceneKind kind) {
    switch (kind) {
        case VisibilitySceneKind::StaticMesh:
        case VisibilitySceneKind::HeightField:
            return std::chrono::milliseconds(120000);
        case VisibilitySceneKind::DynamicMesh:
            return std::chrono::milliseconds(1000);
    }
    return std::chrono::milliseconds(2000);
}

static void RefreshVisibilitySceneCache(VisibilitySceneKind kind,
                                        VisibilitySceneCacheState& state,
                                        const Vec3& cameraWorldPos,
                                        bool forceRefresh) {
    const float maxDistanceCm = (kind == VisibilitySceneKind::StaticMesh || kind == VisibilitySceneKind::HeightField)
        ? std::max(gMaxSkeletonDistance * 100.0f, 50000.0f)
        : std::max(gMaxSkeletonDistance * 100.0f, 10000.0f);
    const float maxDistanceSq = maxDistanceCm * maxDistanceCm;
    std::unordered_map<uint64_t, VisibilityBuildCandidate> candidates;
    candidates.reserve(2048);

    GatherPrunerVisibilityCandidates(kind, cameraWorldPos, maxDistanceSq, candidates);
    GatherPxSceneVisibilityCandidates(kind, cameraWorldPos, maxDistanceSq, candidates);
    GatherLocalVisibilityCandidates(kind, cameraWorldPos, maxDistanceSq, candidates);

    std::vector<VisibilityMeshData> adds;
    std::vector<uint64_t> removes;
    const auto now = std::chrono::steady_clock::now();
    const auto keepAlive = VisibilitySceneKeepAlive(kind);
    std::unordered_map<uint64_t, uint64_t> nextSignatures = state.Signatures;

    for (const auto& [key, candidate] : candidates) {
        const uint64_t signature = BuildVisibilityCandidateSignature(candidate);
        state.LastSeen[key] = now;

        auto existingIt = state.Signatures.find(key);
        if (existingIt != state.Signatures.end() && existingIt->second == signature) {
            nextSignatures[key] = signature;
            continue;
        }

        VisibilityMeshData mesh;
        const bool built = (candidate.GeometryType == PX_GEOM_TRIANGLEMESH)
            ? BuildVisibilityTriangleMeshData(candidate, mesh)
            : BuildVisibilityHeightFieldData(candidate, mesh);
        if (built) {
            adds.push_back(std::move(mesh));
            nextSignatures[key] = signature;
        } else if (existingIt == state.Signatures.end()) {
            nextSignatures.erase(key);
        }
    }

    for (const auto& [key, oldSignature] : state.Signatures) {
        if (candidates.find(key) != candidates.end()) continue;

        auto lastSeenIt = state.LastSeen.find(key);
        if (lastSeenIt != state.LastSeen.end() && (now - lastSeenIt->second) <= keepAlive) {
            nextSignatures[key] = oldSignature;
            continue;
        }

        removes.push_back(key);
        nextSignatures.erase(key);
        state.LastSeen.erase(key);
    }

    if (!adds.empty() || !removes.empty() || forceRefresh) {
        state.Scene.UpdateMeshes(adds, removes);
    }
    state.Signatures = std::move(nextSignatures);
    state.LastRefresh = now;
    state.Valid = true;
}

static void RefreshVisibilityScenes(const Vec3& cameraWorldPos, bool forceRefreshAll) {
    if (gVisibilityLastUseLocalModelData != gPhysXUseLocalModelData) {
        gVisibilityLastUseLocalModelData = gPhysXUseLocalModelData;
        forceRefreshAll = true;
    }

    if (forceRefreshAll) {
        auto reset_state = [](VisibilitySceneCacheState& state) {
            state.Scene.Clear();
            state.Signatures.clear();
            state.LastSeen.clear();
            state.Valid = false;
            state.LastRefresh = {};
            state.CameraCellX = INT32_MIN;
            state.CameraCellY = INT32_MIN;
            state.CameraCellZ = INT32_MIN;
        };
        reset_state(gStaticVisibilityScene);
        reset_state(gHeightFieldVisibilityScene);
        reset_state(gDynamicVisibilityScene);
    }

    const int cellX = static_cast<int>(std::floor(cameraWorldPos.X / 500.0f));
    const int cellY = static_cast<int>(std::floor(cameraWorldPos.Y / 500.0f));
    const int cellZ = static_cast<int>(std::floor(cameraWorldPos.Z / 500.0f));
    const auto now = std::chrono::steady_clock::now();

    auto refresh_one = [&](VisibilitySceneKind kind, VisibilitySceneCacheState& state) {
        const bool movedCell = state.CameraCellX != cellX ||
                               state.CameraCellY != cellY ||
                               state.CameraCellZ != cellZ;
        const bool intervalElapsed = !state.Valid ||
                                     (now - state.LastRefresh) >= VisibilitySceneRefreshInterval(kind);
        if (!forceRefreshAll && !movedCell && !intervalElapsed) return;

        RefreshVisibilitySceneCache(kind, state, cameraWorldPos, forceRefreshAll || movedCell);
        state.CameraCellX = cellX;
        state.CameraCellY = cellY;
        state.CameraCellZ = cellZ;
    };

    refresh_one(VisibilitySceneKind::StaticMesh, gStaticVisibilityScene);
    refresh_one(VisibilitySceneKind::HeightField, gHeightFieldVisibilityScene);
    refresh_one(VisibilitySceneKind::DynamicMesh, gDynamicVisibilityScene);
}

// --- EnsurePrunerDataCached: 填充 gPrunerCache ---

// 每帧开始前调用一次，使缓存失效
static void InvalidateVisibilityCache() {
    ++gPrunerCacheGeneration;
    gGpuSceneCache.Valid = false;
    gVisibilityOccluderCache.Valid = false;
}

// 确保当前帧的 pruner 数据已缓存，返回是否有有效数据
static bool EnsurePrunerDataCached() {
    if (gPrunerCache.valid && gPrunerCache.generation == gPrunerCacheGeneration) {
        return gPrunerCache.objectCount > 0;
    }

    gPrunerCache.valid = false;
    gPrunerCache.objectCount = 0;
    gPrunerCache.generation = gPrunerCacheGeneration;

    if (address.libUE4 == 0) return false;

    const uint64_t uworld = GetDriverManager().read<uint64_t>(address.libUE4 + offset.Gworld);
    std::vector<uint64_t> pxScenes;
    CollectActivePxScenes(address.libUE4, uworld, pxScenes);
    if (pxScenes.empty()) return false;

    const uint32_t kMaxPrunerObjects = static_cast<uint32_t>(std::max(gPhysXMaxPrunerObjectCount, 1));
    uint32_t totalObjectCount = 0;
    for (uint64_t pxScene : pxScenes) {
        const uint64_t sqm = pxScene + offset.NpSceneQueriesSceneQueryManager;
        const uint64_t staticExt = sqm;
        const uint64_t pruner = GetDriverManager().read<uint64_t>(staticExt + offset.PrunerExtPruner);
        const uint32_t prunerType = GetDriverManager().read<uint32_t>(staticExt + offset.PrunerExtType);
        if (pruner == 0) continue;

        uint64_t pool = 0;
        if (prunerType == 1) {
            pool = pruner + offset.AABBPrunerPool;
        } else if (prunerType == 0) {
            pool = pruner + offset.BucketPrunerPool;
        } else {
            continue;
        }

        const uint32_t objectCount = GetDriverManager().read<uint32_t>(pool + offset.PruningPoolNbObjects);
        if (objectCount == 0 || objectCount > kMaxPrunerObjects) continue;
        if (totalObjectCount > kMaxPrunerObjects - objectCount) break;
        totalObjectCount += objectCount;
    }

    if (totalObjectCount == 0) return false;

    // 限制单帧读取时间，防止大场景导致 1-2s 卡顿
    const uint32_t kMaxPrunerObjectsPerFrame =
        static_cast<uint32_t>(std::max(gPhysXMaxPrunerObjectsPerFrame, 1));
    const uint32_t readLimit = std::min(totalObjectCount, kMaxPrunerObjectsPerFrame);

    gPrunerCache.payloads.clear();
    gPrunerCache.bounds.clear();
    gPrunerCache.payloads.reserve(readLimit);
    gPrunerCache.bounds.reserve(readLimit);

    for (uint64_t pxScene : pxScenes) {
        if (gPrunerCache.payloads.size() >= readLimit) break;

        const uint64_t sqm = pxScene + offset.NpSceneQueriesSceneQueryManager;
        const uint64_t staticExt = sqm;
        const uint64_t pruner = GetDriverManager().read<uint64_t>(staticExt + offset.PrunerExtPruner);
        const uint32_t prunerType = GetDriverManager().read<uint32_t>(staticExt + offset.PrunerExtType);
        if (pruner == 0) continue;

        uint64_t pool = 0;
        if (prunerType == 1) {
            pool = pruner + offset.AABBPrunerPool;
        } else if (prunerType == 0) {
            pool = pruner + offset.BucketPrunerPool;
        } else {
            continue;
        }

        const uint32_t objectCount = GetDriverManager().read<uint32_t>(pool + offset.PruningPoolNbObjects);
        if (objectCount == 0 || objectCount > kMaxPrunerObjects) continue;

        // 限制本次读取数量，避免超出预算
        const uint32_t remainingBudget = readLimit - static_cast<uint32_t>(gPrunerCache.payloads.size());
        const uint32_t readCount = std::min(objectCount, remainingBudget);
        if (readCount == 0) break;

        const uint64_t objectsAddr = GetDriverManager().read<uint64_t>(pool + offset.PruningPoolObjects);
        const uint64_t boundsAddr = GetDriverManager().read<uint64_t>(pool + offset.PruningPoolWorldBoxes);
        if (objectsAddr == 0 || boundsAddr == 0) continue;

        const size_t oldPayloadSize = gPrunerCache.payloads.size();
        const size_t oldBoundsSize = gPrunerCache.bounds.size();
        gPrunerCache.payloads.resize(oldPayloadSize + readCount);
        gPrunerCache.bounds.resize(oldBoundsSize + readCount);
        if (!ReadRemoteBufferRobust(objectsAddr, gPrunerCache.payloads.data() + oldPayloadSize,
                                    readCount * sizeof(PhysXPrunerPayload))) {
            gPrunerCache.payloads.resize(oldPayloadSize);
            gPrunerCache.bounds.resize(oldBoundsSize);
            continue;
        }
        if (!ReadRemoteBufferRobust(boundsAddr, gPrunerCache.bounds.data() + oldBoundsSize,
                                    readCount * sizeof(PhysXBounds3))) {
            gPrunerCache.payloads.resize(oldPayloadSize);
            gPrunerCache.bounds.resize(oldBoundsSize);
            continue;
        }
    }

    const uint32_t objectCount = static_cast<uint32_t>(gPrunerCache.payloads.size());
    if (objectCount == 0 || objectCount != gPrunerCache.bounds.size()) {
        gPrunerCache.payloads.clear();
        gPrunerCache.bounds.clear();
        return false;
    }

    // 预计算膨胀后的 AABB
    constexpr float inflate = 3.0f;
    gPrunerCache.inflatedBounds.resize(objectCount);
    for (uint32_t i = 0; i < objectCount; ++i) {
        PhysXBounds3& dst = gPrunerCache.inflatedBounds[i];
        const PhysXBounds3& src = gPrunerCache.bounds[i];
        dst.Min.X = src.Min.X - inflate;
        dst.Min.Y = src.Min.Y - inflate;
        dst.Min.Z = src.Min.Z - inflate;
        dst.Max.X = src.Max.X + inflate;
        dst.Max.Y = src.Max.Y + inflate;
        dst.Max.Z = src.Max.Z + inflate;
    }

    gPrunerCache.objectCount = objectCount;
    gPrunerCache.valid = true;
    return true;
}

static bool EnsureVisibilityOccludersCached(const Vec3& cameraWorldPos) {
    if (!EnsurePrunerDataCached()) return false;

    const int cellX = static_cast<int>(std::floor(cameraWorldPos.X / 500.0f));
    const int cellY = static_cast<int>(std::floor(cameraWorldPos.Y / 500.0f));
    const int cellZ = static_cast<int>(std::floor(cameraWorldPos.Z / 500.0f));
    if (gVisibilityOccluderCache.Valid &&
        gVisibilityOccluderCache.Generation == gPrunerCacheGeneration &&
        gVisibilityOccluderCache.CameraCellX == cellX &&
        gVisibilityOccluderCache.CameraCellY == cellY &&
        gVisibilityOccluderCache.CameraCellZ == cellZ) {
        return true;
    }

    const float maxDistanceCm = std::max(gMaxSkeletonDistance * 100.0f, 10000.0f);
    const float maxDistanceSq = maxDistanceCm * maxDistanceCm;
    gVisibilityOccluderCache.Items.clear();
    gVisibilityOccluderCache.Items.reserve(std::min<uint32_t>(gPrunerCache.objectCount, 4096u));

    auto is_duplicate_occluder = [&](uint32_t geometryType, uint64_t resource, const PxTransformData& worldPose) -> bool {
        for (const VisibilityOccluder& occ : gVisibilityOccluderCache.Items) {
            if (occ.GeometryType != geometryType || occ.Resource != resource) continue;
            const Vec3 d = occ.WorldPose.Translation - worldPose.Translation;
            if (Vec3::Dot(d, d) <= 25.0f) return true;
        }
        return false;
    };

    auto build_fallback_bounds = [&](const PxTransformData& worldPose, float extent) -> PhysXBounds3 {
        return {
            {worldPose.Translation.X - extent, worldPose.Translation.Y - extent, worldPose.Translation.Z - extent},
            {worldPose.Translation.X + extent, worldPose.Translation.Y + extent, worldPose.Translation.Z + extent}
        };
    };

    auto append_occluder = [&](const PhysXBounds3* coarseBounds,
                               const PxTransformData& worldPose,
                               uint64_t geometryAddr,
                               uint32_t geometryType,
                               uint64_t resource,
                               const PxMeshScaleData* meshScale,
                               float heightScale,
                               float rowScale,
                               float columnScale) {
        if (resource == 0) return;
        if (is_duplicate_occluder(geometryType, resource, worldPose)) return;

        VisibilityOccluder occluder{};
        occluder.WorldPose = worldPose;
        occluder.InvRotation = QuatConjugate(occluder.WorldPose.Rotation);
        occluder.GeometryAddr = geometryAddr;
        occluder.GeometryType = geometryType;
        occluder.Resource = resource;
        occluder.DistanceSq = coarseBounds ? DistanceSqToBounds(cameraWorldPos, *coarseBounds)
                                           : Vec3::Dot(worldPose.Translation - cameraWorldPos,
                                                       worldPose.Translation - cameraWorldPos);
        if (meshScale) occluder.MeshScale = *meshScale;
        occluder.HeightScale = heightScale;
        occluder.RowScale = rowScale;
        occluder.ColumnScale = columnScale;

        if (coarseBounds) {
            occluder.InflatedBounds = *coarseBounds;
        } else if (geometryType == PX_GEOM_TRIANGLEMESH) {
            auto meshIt = gTriangleMeshCache.find(resource);
            if (meshIt != gTriangleMeshCache.end() && !meshIt->second.Vertices.empty()) {
                PhysXBounds3 scaledLocal = ApplyMeshScaleToBounds(meshIt->second.MeshAABB, occluder.MeshScale);
                PhysXBounds3 worldBounds = TransformBoundsToWorld(scaledLocal, worldPose);
                occluder.InflatedBounds = {
                    {worldBounds.Min.X - 3.0f, worldBounds.Min.Y - 3.0f, worldBounds.Min.Z - 3.0f},
                    {worldBounds.Max.X + 3.0f, worldBounds.Max.Y + 3.0f, worldBounds.Max.Z + 3.0f}
                };
            } else {
                occluder.InflatedBounds = build_fallback_bounds(worldPose, 4000.0f);
            }
        } else if (geometryType == PX_GEOM_HEIGHTFIELD) {
            occluder.InflatedBounds = build_fallback_bounds(worldPose, 4000.0f);
        } else {
            occluder.InflatedBounds = build_fallback_bounds(worldPose, 2000.0f);
        }

        gVisibilityOccluderCache.Items.push_back(occluder);
    };

    for (uint32_t i = 0; i < gPrunerCache.objectCount; ++i) {
        const PhysXPrunerPayload& payload = gPrunerCache.payloads[i];
        if (payload.Shape == 0 || payload.Actor == 0) continue;
        const float distSq = DistanceSqToBounds(cameraWorldPos, gPrunerCache.bounds[i]);
        if (distSq > maxDistanceSq) continue;

        const uint64_t geometryAddr = payload.Shape + offset.ScbShapeCoreGeometry;
        const uint32_t geometryType = GetDriverManager().read<uint32_t>(geometryAddr);
        if (geometryType != PX_GEOM_TRIANGLEMESH && geometryType != PX_GEOM_HEIGHTFIELD) continue;

        PxTransformData actorPose{}, shapePose{};
        if (!ReadScbStaticActorGlobalPose(payload.Actor, actorPose)) continue;
        if (!ReadScbShapeLocalPose(payload.Shape, shapePose)) continue;
        const PxTransformData worldPose = ComposeTransforms(actorPose, shapePose);
        if (geometryType == PX_GEOM_TRIANGLEMESH) {
            const uint64_t mesh = GetDriverManager().read<uint64_t>(geometryAddr + offset.PxTriangleMeshGeometryTriangleMesh);
            PxMeshScaleData meshScale{};
            ReadMeshScale(geometryAddr + offset.PxTriangleMeshGeometryScale, meshScale);
            append_occluder(&gPrunerCache.inflatedBounds[i], worldPose, geometryAddr, geometryType, mesh,
                            &meshScale, 1.0f, 1.0f, 1.0f);
        } else if (geometryType == PX_GEOM_HEIGHTFIELD) {
            const uint64_t heightField = GetDriverManager().read<uint64_t>(geometryAddr + offset.PxHeightFieldGeometryHeightField);
            const float heightScale = GetDriverManager().read<float>(geometryAddr + offset.PxHeightFieldGeometryHeightScale);
            const float rowScale = GetDriverManager().read<float>(geometryAddr + offset.PxHeightFieldGeometryRowScale);
            const float columnScale = GetDriverManager().read<float>(geometryAddr + offset.PxHeightFieldGeometryColumnScale);
            append_occluder(&gPrunerCache.inflatedBounds[i], worldPose, geometryAddr, geometryType, heightField,
                            nullptr, heightScale, rowScale, columnScale);
        }
    }

    // 也从 PxScene actor 列表补充遮挡体。
    // 有些可绘制 mesh 不在 pruner 静态池里，之前因此完全不会参与可视性检测。
    if (address.libUE4 != 0) {
        const uint64_t uworld = GetDriverManager().read<uint64_t>(address.libUE4 + offset.Gworld);
        const uint64_t pxScene = GetSyncPxScene(address.libUE4, uworld);
        std::vector<uint64_t> actors;
        if (pxScene != 0 && CollectPxSceneActorPointers(pxScene, actors)) {
                for (uint64_t actor : actors) {
                    if (actor == 0) continue;
                    const uint16_t actorType = GetDriverManager().read<uint16_t>(actor + offset.PxActorType);
                    if (actorType != PX_ACTOR_RIGID_DYNAMIC && actorType != PX_ACTOR_RIGID_STATIC) continue;

                    PxTransformData actorPose{};
                    if (!ReadActorGlobalPose(actor, actorType, actorPose)) continue;

                    const uint16_t shapeCount = GetDriverManager().read<uint16_t>(actor + offset.PxActorShapeCount);
                    if (shapeCount == 0 || shapeCount > 64) continue;

                    std::vector<uint64_t> shapes(std::min<uint16_t>(shapeCount, 64));
                    if (shapeCount == 1) {
                        shapes[0] = GetDriverManager().read<uint64_t>(actor + offset.PxActorShapes);
                    } else {
                        const uint64_t shapePtrArray = GetDriverManager().read<uint64_t>(actor + offset.PxActorShapes);
                        if (shapePtrArray == 0) continue;
                        if (!ReadRemoteBufferRobust(shapePtrArray, shapes.data(), shapes.size() * sizeof(uint64_t))) continue;
                    }

                    for (uint64_t shape : shapes) {
                        if (shape == 0) continue;
                        const uint32_t npShapeFlags = GetDriverManager().read<uint32_t>(shape + offset.PxShapeFlags);
                        PxTransformData shapePose{};
                        if (!ReadShapeLocalPose(shape, npShapeFlags, shapePose)) continue;
                        const PxTransformData worldPose = ComposeTransforms(actorPose, shapePose);

                        uint64_t geometryAddr = shape + offset.PxShapeGeometryInline;
                        if ((npShapeFlags & 0x1u) != 0) {
                            const uint64_t corePtr = GetDriverManager().read<uint64_t>(shape + offset.PxShapeCorePtr);
                            if (corePtr == 0) continue;
                            geometryAddr = corePtr + offset.PxShapeCoreGeometry;
                        }
                        const uint32_t geometryType = GetDriverManager().read<uint32_t>(geometryAddr);
                        if (geometryType != PX_GEOM_TRIANGLEMESH && geometryType != PX_GEOM_HEIGHTFIELD) continue;

                        if (geometryType == PX_GEOM_TRIANGLEMESH) {
                            const uint64_t mesh = GetDriverManager().read<uint64_t>(geometryAddr + offset.PxTriangleMeshGeometryTriangleMesh);
                            PxMeshScaleData meshScale{};
                            ReadMeshScale(geometryAddr + offset.PxTriangleMeshGeometryScale, meshScale);
                            append_occluder(nullptr, worldPose, geometryAddr, geometryType, mesh,
                                            &meshScale, 1.0f, 1.0f, 1.0f);
                        } else {
                            const uint64_t heightField = GetDriverManager().read<uint64_t>(geometryAddr + offset.PxHeightFieldGeometryHeightField);
                            const float heightScale = GetDriverManager().read<float>(geometryAddr + offset.PxHeightFieldGeometryHeightScale);
                            const float rowScale = GetDriverManager().read<float>(geometryAddr + offset.PxHeightFieldGeometryRowScale);
                            const float columnScale = GetDriverManager().read<float>(geometryAddr + offset.PxHeightFieldGeometryColumnScale);
                            append_occluder(nullptr, worldPose, geometryAddr, geometryType, heightField,
                                            nullptr, heightScale, rowScale, columnScale);
                        }
                    }
                }
        }
    }

    std::sort(gVisibilityOccluderCache.Items.begin(),
              gVisibilityOccluderCache.Items.end(),
              [](const VisibilityOccluder& a, const VisibilityOccluder& b) {
                  return a.DistanceSq < b.DistanceSq;
              });

    if (!gPhysXUseLocalModelData) {
        // 为三角网格类型 occluder 构建/查找预缩放缓存
        // 本地模型模式下，可视性检测只允许使用本地 OBJ/BVH，不再读取远端三角面缓存
        if (gScaledMeshCacheGeneration != gPrunerCacheGeneration) {
            gScaledMeshCache.clear();
            gScaledMeshCacheGeneration = gPrunerCacheGeneration;
        }
        constexpr size_t kMaxTrianglesForPreScale = 4096;
        for (VisibilityOccluder& occ : gVisibilityOccluderCache.Items) {
            if (occ.GeometryType != PX_GEOM_TRIANGLEMESH) continue;
            const uint64_t meshAddr = GetDriverManager().read<uint64_t>(occ.GeometryAddr + offset.PxTriangleMeshGeometryTriangleMesh);
            if (meshAddr == 0) continue;

            PxMeshScaleData meshScale{};
            if (!ReadMeshScale(occ.GeometryAddr + offset.PxTriangleMeshGeometryScale, meshScale)) continue;

            ScaledMeshKey key{meshAddr, meshScale.Scale.X, meshScale.Scale.Y, meshScale.Scale.Z,
                              meshScale.Rotation.X, meshScale.Rotation.Y, meshScale.Rotation.Z, meshScale.Rotation.W};
            auto smIt = gScaledMeshCache.find(key);
            if (smIt != gScaledMeshCache.end()) {
                occ.PreScaled = &smIt->second;
                continue;
            }

            auto meshIt = gTriangleMeshCache.find(meshAddr);
            if (meshIt == gTriangleMeshCache.end()) continue;
            const CachedTriangleMeshData& src = meshIt->second;
            if (src.Vertices.empty() || src.Indices.empty()) continue;
            const size_t fullTriCount = src.Indices.size() / 3;
            if (fullTriCount == 0 || fullTriCount > kMaxTrianglesForPreScale) {
                continue;
            }

            const bool isIdentity = IsMeshScaleIdentity(meshScale);
            const size_t triCount = fullTriCount;

            ScaledMeshData smd;
            smd.TriangleCount = triCount;
            smd.Indices = src.Indices;
            smd.Vertices.resize(src.Vertices.size());
            if (isIdentity) {
                smd.Vertices = src.Vertices;
            } else {
                for (size_t vi = 0; vi < src.Vertices.size(); ++vi) {
                    smd.Vertices[vi] = ApplyMeshScale(src.Vertices[vi], meshScale);
                }
            }

            smd.TriangleBounds.resize(triCount);
            PhysXBounds3 meshBounds{{1e18f, 1e18f, 1e18f}, {-1e18f, -1e18f, -1e18f}};
            for (size_t ti = 0; ti < triCount; ++ti) {
                const uint32_t ia = src.Indices[ti * 3];
                const uint32_t ib = src.Indices[ti * 3 + 1];
                const uint32_t ic = src.Indices[ti * 3 + 2];
                if (ia >= smd.Vertices.size() || ib >= smd.Vertices.size() || ic >= smd.Vertices.size()) {
                    smd.TriangleBounds[ti] = {};
                    continue;
                }
                const Vec3& a = smd.Vertices[ia];
                const Vec3& b = smd.Vertices[ib];
                const Vec3& c = smd.Vertices[ic];
                smd.TriangleBounds[ti] = {
                    {std::min(std::min(a.X, b.X), c.X) - 1.0f, std::min(std::min(a.Y, b.Y), c.Y) - 1.0f, std::min(std::min(a.Z, b.Z), c.Z) - 1.0f},
                    {std::max(std::max(a.X, b.X), c.X) + 1.0f, std::max(std::max(a.Y, b.Y), c.Y) + 1.0f, std::max(std::max(a.Z, b.Z), c.Z) + 1.0f}
                };
                meshBounds.Min.X = std::min(meshBounds.Min.X, smd.TriangleBounds[ti].Min.X);
                meshBounds.Min.Y = std::min(meshBounds.Min.Y, smd.TriangleBounds[ti].Min.Y);
                meshBounds.Min.Z = std::min(meshBounds.Min.Z, smd.TriangleBounds[ti].Min.Z);
                meshBounds.Max.X = std::max(meshBounds.Max.X, smd.TriangleBounds[ti].Max.X);
                meshBounds.Max.Y = std::max(meshBounds.Max.Y, smd.TriangleBounds[ti].Max.Y);
                meshBounds.Max.Z = std::max(meshBounds.Max.Z, smd.TriangleBounds[ti].Max.Z);
            }
            BuildTriangleChunkBounds(smd.TriangleBounds, smd.ChunkBounds);
            smd.MeshAABB = {{meshBounds.Min.X - 2.0f, meshBounds.Min.Y - 2.0f, meshBounds.Min.Z - 2.0f},
                             {meshBounds.Max.X + 2.0f, meshBounds.Max.Y + 2.0f, meshBounds.Max.Z + 2.0f}};

            auto [insIt, _] = gScaledMeshCache.emplace(key, std::move(smd));
            occ.PreScaled = &insIt->second;
        }
    } else {
        gScaledMeshCache.clear();
        gScaledMeshCacheGeneration = gPrunerCacheGeneration;
        for (VisibilityOccluder& occ : gVisibilityOccluderCache.Items) {
            occ.PreScaled = nullptr;
            occ.UseLocalData = occ.Resource != 0;
        }

        // 本地模型模式下，将本地绘制缓存中的 mesh/heightfield 也加入遮挡体集合。
        // 这些对象可能已经无法从当前内存路径稳定重建，但本地 OBJ 仍可绘制，
        // 因此遮挡检测也必须使用同一批本地模型。
        for (const auto& [shape, entry] : gLocalPhysXDrawCache) {
            (void)shape;
            if (entry.Resource == 0) continue;
            if (entry.GeometryType != PX_GEOM_TRIANGLEMESH && entry.GeometryType != PX_GEOM_HEIGHTFIELD) continue;

            bool hasLocalData = false;
            if (entry.GeometryType == PX_GEOM_TRIANGLEMESH) {
                hasLocalData = GetRenderableLocalTriangleMesh(entry.Resource) != nullptr;
            } else {
                hasLocalData = GetRenderableLocalHeightField(entry.Resource) != nullptr;
            }
            if (!hasLocalData) continue;

            const float distSq = Vec3::Dot(entry.WorldPose.Translation - cameraWorldPos,
                                           entry.WorldPose.Translation - cameraWorldPos);
            if (distSq > maxDistanceSq) continue;

            bool duplicate = false;
            for (const VisibilityOccluder& occ : gVisibilityOccluderCache.Items) {
                if (occ.GeometryType != entry.GeometryType || occ.Resource != entry.Resource) continue;
                const Vec3 d = occ.WorldPose.Translation - entry.WorldPose.Translation;
                if (Vec3::Dot(d, d) <= 25.0f) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;

            VisibilityOccluder occ{};
            occ.WorldPose = entry.WorldPose;
            occ.InvRotation = QuatConjugate(entry.WorldPose.Rotation);
            occ.GeometryType = entry.GeometryType;
            occ.Resource = entry.Resource;
            occ.MeshScale = entry.MeshScale;
            occ.HeightScale = entry.HeightScale;
            occ.RowScale = entry.RowScale;
            occ.ColumnScale = entry.ColumnScale;
            occ.UseLocalData = true;
            occ.DistanceSq = distSq;
            occ.InflatedBounds = {
                {entry.WorldPose.Translation.X - 400.0f, entry.WorldPose.Translation.Y - 400.0f, entry.WorldPose.Translation.Z - 400.0f},
                {entry.WorldPose.Translation.X + 400.0f, entry.WorldPose.Translation.Y + 400.0f, entry.WorldPose.Translation.Z + 400.0f}
            };
            gVisibilityOccluderCache.Items.push_back(occ);
        }

    }
    std::sort(gVisibilityOccluderCache.Items.begin(),
              gVisibilityOccluderCache.Items.end(),
              [](const VisibilityOccluder& a, const VisibilityOccluder& b) {
                  return a.DistanceSq < b.DistanceSq;
              });
    gVisibilityOccluderCache.Generation = gPrunerCacheGeneration;
    gVisibilityOccluderCache.CameraCellX = cellX;
    gVisibilityOccluderCache.CameraCellY = cellY;
    gVisibilityOccluderCache.CameraCellZ = cellZ;
    gVisibilityOccluderCache.Valid = true;
    return true;
}

static void AppendGpuSceneTriangle(std::vector<GpuSceneTriangle>& triangles,
                                   const Vec3& a, const Vec3& b, const Vec3& c) {
    GpuSceneTriangle tri{};
    tri.A[0] = a.X; tri.A[1] = a.Y; tri.A[2] = a.Z; tri.A[3] = 1.0f;
    tri.B[0] = b.X; tri.B[1] = b.Y; tri.B[2] = b.Z; tri.B[3] = 1.0f;
    tri.C[0] = c.X; tri.C[1] = c.Y; tri.C[2] = c.Z; tri.C[3] = 1.0f;
    triangles.push_back(tri);
}

static bool AppendTriangleMeshSceneTriangles(uint64_t geometryAddr, const PxTransformData& worldPose,
                                             std::vector<GpuSceneTriangle>& triangles, size_t maxTriangles) {
    const uint64_t mesh = GetDriverManager().read<uint64_t>(geometryAddr + offset.PxTriangleMeshGeometryTriangleMesh);
    if (mesh == 0) return false;
    PxMeshScaleData scale{};
    if (!ReadMeshScale(geometryAddr + offset.PxTriangleMeshGeometryScale, scale)) return false;

    const uint32_t vertexCount = GetDriverManager().read<uint32_t>(mesh + offset.PxTriangleMeshVertexCount);
    const uint32_t triangleCount = GetDriverManager().read<uint32_t>(mesh + offset.PxTriangleMeshTriangleCount);
    const uint64_t verticesAddr = GetDriverManager().read<uint64_t>(mesh + offset.PxTriangleMeshVertices);
    const uint64_t trianglesAddr = GetDriverManager().read<uint64_t>(mesh + offset.PxTriangleMeshTriangles);
    if (vertexCount == 0 || triangleCount == 0 || verticesAddr == 0 || trianglesAddr == 0) return false;

    CachedTriangleMeshData cache;
    const uint8_t flags = GetDriverManager().read<uint8_t>(mesh + offset.PxTriangleMeshFlags);
    if (!GetCachedTriangleMesh(mesh, vertexCount, triangleCount, flags, verticesAddr, trianglesAddr, cache)) return false;

    const size_t limit = std::min(static_cast<size_t>(cache.Indices.size() / 3ULL), static_cast<size_t>(triangleCount));
    for (size_t i = 0; i < limit && triangles.size() < maxTriangles; ++i) {
        const size_t triBase = i * 3ULL;
        const uint32_t ia = cache.Indices[triBase];
        const uint32_t ib = cache.Indices[triBase + 1];
        const uint32_t ic = cache.Indices[triBase + 2];
        if (ia >= cache.Vertices.size() || ib >= cache.Vertices.size() || ic >= cache.Vertices.size()) continue;
        AppendGpuSceneTriangle(triangles,
                               TransformPoint(worldPose, ApplyMeshScale(cache.Vertices[ia], scale)),
                               TransformPoint(worldPose, ApplyMeshScale(cache.Vertices[ib], scale)),
                               TransformPoint(worldPose, ApplyMeshScale(cache.Vertices[ic], scale)));
    }
    return true;
}

static bool AppendHeightFieldSceneTriangles(uint64_t geometryAddr, const PxTransformData& worldPose,
                                            std::vector<GpuSceneTriangle>& triangles, size_t maxTriangles) {
    const uint64_t heightField = GetDriverManager().read<uint64_t>(geometryAddr + offset.PxHeightFieldGeometryHeightField);
    if (heightField == 0) return false;
    const float heightScale = GetDriverManager().read<float>(geometryAddr + offset.PxHeightFieldGeometryHeightScale);
    const float rowScale = GetDriverManager().read<float>(geometryAddr + offset.PxHeightFieldGeometryRowScale);
    const float columnScale = GetDriverManager().read<float>(geometryAddr + offset.PxHeightFieldGeometryColumnScale);
    if (std::fabs(heightScale) <= 1e-6f || std::fabs(rowScale) <= 1e-6f || std::fabs(columnScale) <= 1e-6f) return false;

    CachedHeightFieldData cache;
    if (!GetCachedHeightField(heightField, cache)) return false;
    for (uint32_t row = 0; row + 1 < cache.Rows && triangles.size() < maxTriangles; ++row) {
        for (uint32_t column = 0; column + 1 < cache.Columns && triangles.size() < maxTriangles; ++column) {
            Vec3 t0a{}, t0b{}, t0c{}, t1a{}, t1b{}, t1c{};
            bool t0Valid = false, t1Valid = false;
            if (!GetHeightFieldCellTriangles(cache, row, column, heightScale, rowScale, columnScale,
                                             t0a, t0b, t0c, t0Valid, t1a, t1b, t1c, t1Valid)) {
                continue;
            }
            if (t0Valid && triangles.size() < maxTriangles) {
                AppendGpuSceneTriangle(triangles, TransformPoint(worldPose, t0a), TransformPoint(worldPose, t0b), TransformPoint(worldPose, t0c));
            }
            if (t1Valid && triangles.size() < maxTriangles) {
                AppendGpuSceneTriangle(triangles, TransformPoint(worldPose, t1a), TransformPoint(worldPose, t1b), TransformPoint(worldPose, t1c));
            }
        }
    }
    return true;
}

static bool EnsureGpuSceneTriangles(const Vec3& cameraWorldPos) {
    if (!EnsurePrunerDataCached()) return false;

    const int cellX = static_cast<int>(std::floor(cameraWorldPos.X / 500.0f));
    const int cellY = static_cast<int>(std::floor(cameraWorldPos.Y / 500.0f));
    const int cellZ = static_cast<int>(std::floor(cameraWorldPos.Z / 500.0f));
    if (gGpuSceneCache.Valid &&
        gGpuSceneCache.Generation == gPrunerCacheGeneration &&
        gGpuSceneCache.CameraCellX == cellX &&
        gGpuSceneCache.CameraCellY == cellY &&
        gGpuSceneCache.CameraCellZ == cellZ) {
        return true;
    }

    constexpr size_t kMaxSceneTriangles = 30000;
    const float maxDistanceCm = std::max(gMaxSkeletonDistance * 100.0f, 10000.0f);
    const float maxDistanceSq = maxDistanceCm * maxDistanceCm;
    gGpuSceneCache.Triangles.clear();
    gGpuSceneCache.Triangles.reserve(std::min<size_t>(kMaxSceneTriangles, 8192));

    for (uint32_t i = 0; i < gPrunerCache.objectCount && gGpuSceneCache.Triangles.size() < kMaxSceneTriangles; ++i) {
        const PhysXPrunerPayload& payload = gPrunerCache.payloads[i];
        if (payload.Shape == 0 || payload.Actor == 0) continue;
        if (DistanceSqToBounds(cameraWorldPos, gPrunerCache.bounds[i]) > maxDistanceSq) continue;

        PxTransformData actorPose{}, shapePose{};
        if (!ReadScbStaticActorGlobalPose(payload.Actor, actorPose)) continue;
        if (!ReadScbShapeLocalPose(payload.Shape, shapePose)) continue;
        const PxTransformData worldPose = ComposeTransforms(actorPose, shapePose);
        const uint64_t geometryAddr = payload.Shape + offset.ScbShapeCoreGeometry;
        const uint32_t geometryType = GetDriverManager().read<uint32_t>(geometryAddr);
        if (geometryType == PX_GEOM_TRIANGLEMESH) {
            AppendTriangleMeshSceneTriangles(geometryAddr, worldPose, gGpuSceneCache.Triangles, kMaxSceneTriangles);
        } else if (geometryType == PX_GEOM_HEIGHTFIELD) {
            AppendHeightFieldSceneTriangles(geometryAddr, worldPose, gGpuSceneCache.Triangles, kMaxSceneTriangles);
        }
    }

    gGpuSceneCache.Generation = gPrunerCacheGeneration;
    gGpuSceneCache.CameraCellX = cellX;
    gGpuSceneCache.CameraCellY = cellY;
    gGpuSceneCache.CameraCellZ = cellZ;
    gGpuSceneCache.Valid = true;
    return true;
}






// ── 深度缓冲可视性判断 ──
// 两阶段 compute dispatch：
//   Pass 1：将场景三角形光栅化到低分辨率深度缓冲
//   Pass 2：查询骨骼世界坐标是否被遮挡
static bool RunGpuDepthBufferVisibility(const Vec3& cameraWorldPos,
                                        const FMatrix& vpMat,
                                        float screenW, float screenH,
                                        const Vec3 boneWorldPos[],
                                        const bool boneHasData[],
                                        bool visible[],
                                        int count) {
    std::fill(visible, visible + count, true);
    gDepthDiag.failReason = nullptr;
    gDepthDiag.ranThisFrame = false;
    if (count <= 0) { gDepthDiag.failReason = "no queries"; return true; }
    if (!gUseDepthBufferVisibility) { gDepthDiag.failReason = "disabled"; return false; }

    // 计算降采样后的深度缓冲尺寸
    const int downscale = std::max(1, gDepthBufferDownscale);
    const uint32_t depthW = std::max(1u, static_cast<uint32_t>(screenW) / downscale);
    const uint32_t depthH = std::max(1u, static_cast<uint32_t>(screenH) / downscale);

    // 确保场景三角形就绪
    if (!EnsureGpuSceneTriangles(cameraWorldPos)) { gDepthDiag.failReason = "scene triangles failed"; return false; }
    if (gGpuSceneCache.Triangles.empty()) { gDepthDiag.failReason = "no scene triangles"; return false; }

    // 初始化两个 pipeline
    if (!EnsureGpuDepthRasterResources()) { gDepthDiag.failReason = "raster pipeline init failed"; return false; }
    gDepthDiag.rasterPipelineReady = true;
    if (!EnsureGpuDepthQueryResources()) { gDepthDiag.failReason = "query pipeline init failed"; return false; }
    gDepthDiag.queryPipelineReady = true;

    // 确保 buffer 容量
    const uint32_t triCount = static_cast<uint32_t>(gGpuSceneCache.Triangles.size());
    if (!EnsureGpuDepthRasterCapacity(triCount, depthW, depthH)) { gDepthDiag.failReason = "raster buffer alloc failed"; return false; }

    // 构建有效查询列表
    std::vector<GpuBoneQuery> queries;
    std::vector<int> queryToIndex;
    queries.reserve(count);
    queryToIndex.reserve(count);
    for (int i = 0; i < count; ++i) {
        if (!boneHasData[i]) continue;
        GpuBoneQuery q{};
        q.WorldPos[0] = boneWorldPos[i].X;
        q.WorldPos[1] = boneWorldPos[i].Y;
        q.WorldPos[2] = boneWorldPos[i].Z;
        q.WorldPos[3] = 0.0f;  // per-query extra tolerance
        queries.push_back(q);
        queryToIndex.push_back(i);
    }
    if (queries.empty()) return true;

    if (!EnsureGpuDepthQueryCapacity(static_cast<uint32_t>(queries.size()))) { gDepthDiag.failReason = "query buffer alloc failed"; return false; }

    // ── 上传所有数据（host → buffer），在录制 command buffer 之前完成 ──

    void* mapped = nullptr;
    const uint32_t depthPixels = depthW * depthH;

    // 上传三角形
    if (vkMapMemory(gApp.device, gGpuDepthRaster.TriangleBuffer.Memory, 0,
                    sizeof(GpuSceneTriangle) * triCount, 0, &mapped) != VK_SUCCESS) return false;
    memcpy(mapped, gGpuSceneCache.Triangles.data(), sizeof(GpuSceneTriangle) * triCount);
    vkUnmapMemory(gApp.device, gGpuDepthRaster.TriangleBuffer.Memory);

    // 上传光栅配置
    GpuDepthBufferConfig rasterConfig{};
    memcpy(rasterConfig.VP, vpMat.M, sizeof(rasterConfig.VP));
    rasterConfig.DepthBufferSize[0] = static_cast<float>(depthW);
    rasterConfig.DepthBufferSize[1] = static_cast<float>(depthH);
    rasterConfig.DepthBufferSize[2] = static_cast<float>(triCount);
    rasterConfig.DepthBufferSize[3] = gDepthBufferBias;
    if (vkMapMemory(gApp.device, gGpuDepthRaster.ConfigBuffer.Memory, 0,
                    sizeof(rasterConfig), 0, &mapped) != VK_SUCCESS) return false;
    memcpy(mapped, &rasterConfig, sizeof(rasterConfig));
    vkUnmapMemory(gApp.device, gGpuDepthRaster.ConfigBuffer.Memory);

    // 初始化深度缓冲为 0（Reversed-Z 下 0.0 = 最远，无几何体）
    // 光栅 shader 用 atomicMax 保留最近（最大）的深度值；
    // query shader 跳过值为 0 的像素（表示该位置无场景几何体）
    if (vkMapMemory(gApp.device, gGpuDepthRaster.DepthBuffer.Memory, 0,
                    sizeof(uint32_t) * depthPixels, 0, &mapped) != VK_SUCCESS) return false;
    memset(mapped, 0x00, sizeof(uint32_t) * depthPixels);
    vkUnmapMemory(gApp.device, gGpuDepthRaster.DepthBuffer.Memory);

    // 更新 query descriptor set 的 DepthBuffer binding（指向 raster 的 depth buffer）
    {
        VkDescriptorBufferInfo depthInfo{gGpuDepthRaster.DepthBuffer.Buffer, 0,
                                         sizeof(uint32_t) * depthPixels};
        VkWriteDescriptorSet depthWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        depthWrite.dstSet = gGpuDepthQuery.DescriptorSet;
        depthWrite.dstBinding = 2;
        depthWrite.descriptorCount = 1;
        depthWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        depthWrite.pBufferInfo = &depthInfo;
        vkUpdateDescriptorSets(gApp.device, 1, &depthWrite, 0, nullptr);
    }

    // 上传骨骼查询数据
    if (vkMapMemory(gApp.device, gGpuDepthQuery.QueryBuffer.Memory, 0,
                    sizeof(GpuBoneQuery) * queries.size(), 0, &mapped) != VK_SUCCESS) return false;
    memcpy(mapped, queries.data(), sizeof(GpuBoneQuery) * queries.size());
    vkUnmapMemory(gApp.device, gGpuDepthQuery.QueryBuffer.Memory);

    // 上传查询配置
    GpuDepthQueryConfig queryConfig{};
    memcpy(queryConfig.VP, vpMat.M, sizeof(queryConfig.VP));
    queryConfig.DepthBufferSize[0] = static_cast<float>(depthW);
    queryConfig.DepthBufferSize[1] = static_cast<float>(depthH);
    queryConfig.DepthBufferSize[2] = static_cast<float>(queries.size());
    queryConfig.DepthBufferSize[3] = gDepthBufferTolerance;
    if (vkMapMemory(gApp.device, gGpuDepthQuery.ConfigBuffer.Memory, 0,
                    sizeof(queryConfig), 0, &mapped) != VK_SUCCESS) return false;
    memcpy(mapped, &queryConfig, sizeof(queryConfig));
    vkUnmapMemory(gApp.device, gGpuDepthQuery.ConfigBuffer.Memory);

    // 初始化结果缓冲（默认全部可见）
    if (vkMapMemory(gApp.device, gGpuDepthQuery.ResultBuffer.Memory, 0,
                    sizeof(uint32_t) * queries.size(), 0, &mapped) != VK_SUCCESS) return false;
    memset(mapped, 0xFF, sizeof(uint32_t) * queries.size());
    vkUnmapMemory(gApp.device, gGpuDepthQuery.ResultBuffer.Memory);

    // ── 录制单个 command buffer：Pass 1 (光栅) → barrier → Pass 2 (查询) ──
    // 两个 dispatch 在同一个 command buffer 中，barrier 保证光栅写入对查询读取可见。
    // 之前分成两个 submission 时，跨 submission 的 barrier 无效，
    // 导致 query shader 在某些 GPU 上读到未更新的深度缓冲（全零）。

    vkResetFences(gApp.device, 1, &gGpuDepthRaster.Fence);
    vkResetCommandPool(gApp.device, gGpuDepthRaster.CommandPool, 0);
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if (vkBeginCommandBuffer(gGpuDepthRaster.CommandBuffer, &beginInfo) != VK_SUCCESS) return false;

    // Pass 1: 深度光栅化
    vkCmdBindPipeline(gGpuDepthRaster.CommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, gGpuDepthRaster.Pipeline);
    vkCmdBindDescriptorSets(gGpuDepthRaster.CommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            gGpuDepthRaster.PipelineLayout, 0, 1, &gGpuDepthRaster.DescriptorSet, 0, nullptr);
    vkCmdDispatch(gGpuDepthRaster.CommandBuffer, (triCount + 63u) / 64u, 1, 1);

    // 内存屏障：光栅 compute 写入 → 查询 compute 读取（同一 command buffer 内有效）
    VkMemoryBarrier rasterBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    rasterBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    rasterBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(gGpuDepthRaster.CommandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &rasterBarrier, 0, nullptr, 0, nullptr);

    // Pass 2: 深度查询
    vkCmdBindPipeline(gGpuDepthRaster.CommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, gGpuDepthQuery.Pipeline);
    vkCmdBindDescriptorSets(gGpuDepthRaster.CommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            gGpuDepthQuery.PipelineLayout, 0, 1, &gGpuDepthQuery.DescriptorSet, 0, nullptr);
    vkCmdDispatch(gGpuDepthRaster.CommandBuffer, (static_cast<uint32_t>(queries.size()) + 63u) / 64u, 1, 1);

    // 内存屏障：查询 compute 写入 → host 读取
    VkMemoryBarrier queryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    queryBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    queryBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(gGpuDepthRaster.CommandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &queryBarrier, 0, nullptr, 0, nullptr);

    if (vkEndCommandBuffer(gGpuDepthRaster.CommandBuffer) != VK_SUCCESS) return false;

    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &gGpuDepthRaster.CommandBuffer;
    if (vkQueueSubmit(gGpuDepthRaster.Queue, 1, &submitInfo, gGpuDepthRaster.Fence) != VK_SUCCESS) return false;
    // 使用短超时避免阻塞绘制线程
    constexpr uint64_t kGpuTimeoutNs = 8'000'000; // 8ms
    VkResult waitResult = vkWaitForFences(gApp.device, 1, &gGpuDepthRaster.Fence, VK_TRUE, kGpuTimeoutNs);
    if (waitResult == VK_TIMEOUT) {
        return false; // GPU 超时，跳过本帧可见性检测
    }
    if (waitResult != VK_SUCCESS) return false;

    // 读回结果
    std::vector<uint32_t> results(queries.size(), 1u);
    if (vkMapMemory(gApp.device, gGpuDepthQuery.ResultBuffer.Memory, 0,
                    sizeof(uint32_t) * queries.size(), 0, &mapped) != VK_SUCCESS) return false;
    memcpy(results.data(), mapped, sizeof(uint32_t) * queries.size());
    vkUnmapMemory(gApp.device, gGpuDepthQuery.ResultBuffer.Memory);

    for (size_t i = 0; i < queryToIndex.size(); ++i) {
        visible[queryToIndex[i]] = results[i] != 0;
    }

    // 诊断统计
    gDepthDiag.ranThisFrame = true;
    gDepthDiag.depthBufferWidth = static_cast<int>(depthW);
    gDepthDiag.depthBufferHeight = static_cast<int>(depthH);
    gDepthDiag.triangleCount = static_cast<int>(triCount);
    gDepthDiag.queryCount = static_cast<int>(queries.size());
    gDepthDiag.visibleCount = 0;
    gDepthDiag.occludedCount = 0;
    for (size_t i = 0; i < results.size(); ++i) {
        if (results[i] != 0) ++gDepthDiag.visibleCount;
        else ++gDepthDiag.occludedCount;
    }

    // 深度缓冲诊断：统计非零像素、最小/最大深度
    {
        gDepthDiag.nonZeroPixels = 0;
        gDepthDiag.minDepth = 1.0f;
        gDepthDiag.maxDepth = 0.0f;
        if (vkMapMemory(gApp.device, gGpuDepthRaster.DepthBuffer.Memory, 0,
                        sizeof(uint32_t) * depthPixels, 0, &mapped) == VK_SUCCESS) {
            const uint32_t* depthData = static_cast<const uint32_t*>(mapped);
            for (uint32_t p = 0; p < depthPixels; ++p) {
                if (depthData[p] != 0u) {
                    ++gDepthDiag.nonZeroPixels;
                    float d;
                    memcpy(&d, &depthData[p], sizeof(float));
                    if (d < gDepthDiag.minDepth) gDepthDiag.minDepth = d;
                    if (d > gDepthDiag.maxDepth) gDepthDiag.maxDepth = d;
                }
            }
            vkUnmapMemory(gApp.device, gGpuDepthRaster.DepthBuffer.Memory);
        }
    }

    return true;
}

static bool SegmentIntersectsSceneShape(const Vec3& startWorld, const Vec3& endWorld,
                                        const PxTransformData& worldPose,
                                        uint64_t geometryAddr, uint32_t geometryType,
                                        const Vec3* boxHalfExtents = nullptr,
                                        const D4DVector* invRotation = nullptr,
                                        const ScaledMeshData* preScaled = nullptr) {
    switch (geometryType) {
        case PX_GEOM_TRIANGLEMESH:
            return SegmentIntersectsTriangleMeshShape(startWorld, endWorld, worldPose, geometryAddr, invRotation, preScaled);
        case PX_GEOM_HEIGHTFIELD:
            return SegmentIntersectsHeightFieldShape(startWorld, endWorld, worldPose, geometryAddr, invRotation);
        default:
            return false;
    }
}

static bool SegmentIntersectsVisibilityOccluder(const Vec3& startWorld, const Vec3& endWorld,
                                                const VisibilityOccluder& occluder) {
    if (occluder.UseLocalData) {
        switch (occluder.GeometryType) {
            case PX_GEOM_TRIANGLEMESH:
                return SegmentIntersectsLocalTriangleMeshShape(startWorld, endWorld, occluder.WorldPose,
                                                               occluder.Resource, occluder.MeshScale,
                                                               &occluder.InvRotation);
            case PX_GEOM_HEIGHTFIELD:
                return SegmentIntersectsLocalHeightFieldShape(startWorld, endWorld, occluder.WorldPose,
                                                              occluder.Resource,
                                                              occluder.HeightScale,
                                                              occluder.RowScale,
                                                              occluder.ColumnScale,
                                                              &occluder.InvRotation);
            default:
                return false;
        }
    }

    return SegmentIntersectsSceneShape(startWorld, endWorld, occluder.WorldPose,
                                       occluder.GeometryAddr, occluder.GeometryType, nullptr,
                                       &occluder.InvRotation, occluder.PreScaled);
}

static void BatchBoneVisibilityCheck(const Vec3& cameraWorldPos,
                                     const Vec3 boneWorldPos[],
                                     const bool boneHasData[],
                                     bool boneVisible[],
                                     int count) {
    std::fill(boneVisible, boneVisible + count, true);
    if (count <= 0) return;
    if (cameraWorldPos.X == 0.0f && cameraWorldPos.Y == 0.0f && cameraWorldPos.Z == 0.0f) return;
    if (!gUseDepthBufferVisibility) return;

    if (!gVisibilityRefreshInProgress.load(std::memory_order_acquire)) {
        gVisibilityRefreshInProgress.store(true, std::memory_order_relaxed);
        Vec3 camPos = cameraWorldPos;
        std::thread([camPos]() {
            RefreshVisibilityScenes(camPos, false);
            gVisibilityRefreshInProgress.store(false, std::memory_order_release);
        }).detach();
    }

    // 一次性获取三个 scene 的 snapshot，避免每条射线重复加锁 + 原子引用计数
    auto staticSnap = gStaticVisibilityScene.Scene.AcquireSnapshot();
    auto hfSnap     = gHeightFieldVisibilityScene.Scene.AcquireSnapshot();
    auto dynamicSnap = gDynamicVisibilityScene.Scene.AcquireSnapshot();

    for (int i = 0; i < count; ++i) {
        if (!boneHasData[i]) continue;
        const VisibilityRaycastQuery query = VisibilityScene::BuildRaycastQuery(cameraWorldPos, boneWorldPos[i]);
        if (!query.Valid) continue;

        // any-hit: 首次命中立即标记遮挡，跳过剩余 scene
        if (gStaticVisibilityScene.Scene.RaycastAnyWithSnapshot(query, staticSnap)) {
            boneVisible[i] = false;
            continue;
        }
        if (gHeightFieldVisibilityScene.Scene.RaycastAnyWithSnapshot(query, hfSnap)) {
            boneVisible[i] = false;
            continue;
        }
        if (gDynamicVisibilityScene.Scene.RaycastAnyWithSnapshot(query, dynamicSnap)) {
            boneVisible[i] = false;
        }
    }
}

// ── 深度缓冲调试可视化 ──
// 读回 GPU 深度缓冲，以灰度色块叠加在屏幕左下角
static void DrawDepthBufferOverlay() {
    if (!gDrawDepthBuffer) return;
    if (!gDepthDiag.ranThisFrame) return;
    if (gGpuDepthRaster.DepthBuffer.Buffer == VK_NULL_HANDLE) return;

    const uint32_t depthW = gGpuDepthRaster.DepthBufferWidth;
    const uint32_t depthH = gGpuDepthRaster.DepthBufferHeight;
    if (depthW == 0 || depthH == 0) return;

    const uint32_t pixelCount = depthW * depthH;
    void* mapped = nullptr;
    if (vkMapMemory(gApp.device, gGpuDepthRaster.DepthBuffer.Memory, 0,
                    sizeof(uint32_t) * pixelCount, 0, &mapped) != VK_SUCCESS) return;

    // 拷贝到栈外临时缓冲（避免长时间持有映射）
    std::vector<uint32_t> depthData(pixelCount);
    memcpy(depthData.data(), mapped, sizeof(uint32_t) * pixelCount);
    vkUnmapMemory(gApp.device, gGpuDepthRaster.DepthBuffer.Memory);

    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* drawList = ImGui::GetForegroundDrawList();

    // 叠加区域：屏幕左下角，宽度 = 屏幕宽度 30%，等比缩放
    const float overlayW = io.DisplaySize.x * 0.3f;
    const float scale = overlayW / static_cast<float>(depthW);
    const float overlayH = static_cast<float>(depthH) * scale;
    const float originX = 10.0f;
    const float originY = io.DisplaySize.y - overlayH - 10.0f;

    // 半透明黑底
    drawList->AddRectFilled(ImVec2(originX - 2, originY - 2),
                            ImVec2(originX + overlayW + 2, originY + overlayH + 2),
                            IM_COL32(0, 0, 0, 180));

    // 采样步长：限制最大绘制块数防止 ImGui 顶点爆炸
    constexpr int kMaxBlocks = 80000;
    int stepX = 1, stepY = 1;
    while (static_cast<int>(depthW / stepX) * static_cast<int>(depthH / stepY) > kMaxBlocks) {
        if (stepX <= stepY) ++stepX; else ++stepY;
    }

    const float blockW = scale * stepX;
    const float blockH = scale * stepY;

    // Reversed-Z: depth=1.0 近 (0x3F800000), depth=0.0 远 (0x00000000)
    // 0x00000000 = sentinel (无几何，初始值)
    // 可视化：近=亮白，远=暗灰，sentinel=红色
    for (uint32_t y = 0; y < depthH; y += stepY) {
        for (uint32_t x = 0; x < depthW; x += stepX) {
            const uint32_t raw = depthData[y * depthW + x];
            ImU32 color;
            if (raw == 0x00000000) {
                color = IM_COL32(80, 0, 0, 200);  // sentinel：暗红（无几何体）
            } else {
                // IEEE 754 → float, clamp [0,1]
                float depth;
                memcpy(&depth, &raw, sizeof(float));
                depth = std::max(0.0f, std::min(1.0f, depth));
                // reversed-Z: 1=near(亮), 0=far(暗)
                uint8_t lum = static_cast<uint8_t>(depth * 255.0f);
                color = IM_COL32(lum, lum, lum, 220);
            }
            float px = originX + static_cast<float>(x) * scale;
            float py = originY + static_cast<float>(y) * scale;
            drawList->AddRectFilled(ImVec2(px, py), ImVec2(px + blockW, py + blockH), color);
        }
    }

    // 叠加文字信息
    char info[128];
    snprintf(info, sizeof(info), "Depth %ux%u  tri:%d  step:%dx%d",
             depthW, depthH, gDepthDiag.triangleCount, stepX, stepY);
    drawList->AddText(ImVec2(originX + 4, originY + 4), IM_COL32(0, 255, 0, 255), info);

    char info2[128];
    snprintf(info2, sizeof(info2), "queries:%d  vis:%d  occ:%d",
             gDepthDiag.queryCount, gDepthDiag.visibleCount, gDepthDiag.occludedCount);
    drawList->AddText(ImVec2(originX + 4, originY + 20), IM_COL32(0, 255, 0, 255), info2);

    char info3[128];
    snprintf(info3, sizeof(info3), "filled:%d/%u  depth:[%.4f,%.4f]",
             gDepthDiag.nonZeroPixels, depthW * depthH,
             gDepthDiag.minDepth, gDepthDiag.maxDepth);
    drawList->AddText(ImVec2(originX + 4, originY + 36), IM_COL32(0, 255, 0, 255), info3);
}


// 清理资源
void ShutdownDrawObjects() {
    ResetGpuDepthRasterResources();
    ResetGpuDepthQueryResources();
    std::atomic_store_explicit(&gBoneScreenCacheSnapshot,
                               BoneScreenCacheSnapshot(std::make_shared<BoneScreenCache>()),
                               std::memory_order_release);
    gPredictedAimHoverTimes.clear();
    gBoneWorldCache.clear();
    gGpuSceneCache.Triangles.clear();
    gGpuSceneCache.Valid = false;
    gPrunerCache.payloads.clear();
    gPrunerCache.bounds.clear();
    gPrunerCache.inflatedBounds.clear();
    gTriangleMeshSources.clear();
    gHeightFieldSources.clear();
    gTriangleMeshCache.clear();
    gConvexMeshCache.clear();
    gHeightFieldCache.clear();
    gTriMeshCacheBySig.clear();
    gHfCacheBySig.clear();
    gAutoExportedSigs.clear();
    gTriangleMeshSignatures.clear();
    gHeightFieldSignatures.clear();
    gLocalTriObjCache.clear();
    gLocalHfObjCache.clear();
    gLocalTriBvhCache.clear();
    gLocalPhysXDrawCache.clear();
    gPxSceneActorPointerCache.clear();
    gLocalTriMetaBySig.clear();
    gLocalHfMetaBySig.clear();
    gLocalExportMetaLoaded = false;
    gGeomCacheMissCooldowns.clear();
    gScaledMeshCache.clear();
    gScaledMeshCacheGeneration = 0;
    gStaticVisibilityScene.Scene.Clear();
    gStaticVisibilityScene.Signatures.clear();
    gStaticVisibilityScene.LastSeen.clear();
    gStaticVisibilityScene.Valid = false;
    gStaticVisibilityScene.LastRefresh = {};
    gStaticVisibilityScene.CameraCellX = INT32_MIN;
    gStaticVisibilityScene.CameraCellY = INT32_MIN;
    gStaticVisibilityScene.CameraCellZ = INT32_MIN;
    gHeightFieldVisibilityScene.Scene.Clear();
    gHeightFieldVisibilityScene.Signatures.clear();
    gHeightFieldVisibilityScene.LastSeen.clear();
    gHeightFieldVisibilityScene.Valid = false;
    gHeightFieldVisibilityScene.LastRefresh = {};
    gHeightFieldVisibilityScene.CameraCellX = INT32_MIN;
    gHeightFieldVisibilityScene.CameraCellY = INT32_MIN;
    gHeightFieldVisibilityScene.CameraCellZ = INT32_MIN;
    gDynamicVisibilityScene.Scene.Clear();
    gDynamicVisibilityScene.Signatures.clear();
    gDynamicVisibilityScene.LastSeen.clear();
    gDynamicVisibilityScene.Valid = false;
    gDynamicVisibilityScene.LastRefresh = {};
    gDynamicVisibilityScene.CameraCellX = INT32_MIN;
    gDynamicVisibilityScene.CameraCellY = INT32_MIN;
    gDynamicVisibilityScene.CameraCellZ = INT32_MIN;
    gVisibilityLastUseLocalModelData = false;
    gLastGeomCacheFlushTime = 0.0;
    gPrunerCache.objectCount = 0;
    gPrunerCache.valid = false;
}

// 测试：从内存导出带BVH的mesh
bool TestExportBVHMesh() {
    // 从缓存中找一个 TriangleMesh 进行测试
    if (gTriangleMeshCache.empty()) {
        printf("[BVH Export] No cached triangle meshes available\n");
        return false;
    }

    auto it = gTriangleMeshCache.begin();
    uint64_t meshAddr = it->first;

    PhysXBVH::ExportedBVHMesh exportedMesh;
    if (!PhysXBVH::ExportBVHMeshFromMemory(meshAddr, exportedMesh)) {
        printf("[BVH Export] Failed to export mesh at 0x%llx\n", meshAddr);
        return false;
    }

    printf("[BVH Export] Success! Mesh at 0x%llx:\n", meshAddr);
    printf("  Vertices: %zu\n", exportedMesh.vertices.size());
    printf("  Triangles: %zu\n", exportedMesh.indices.size() / 3);
    printf("  RTree valid: %s\n", exportedMesh.rtree.valid ? "yes" : "no");
    printf("  RTree pages: %u\n", exportedMesh.rtree.totalPages);
    printf("  RTree nodes: %u\n", exportedMesh.rtree.totalNodes);

    return true;
}
