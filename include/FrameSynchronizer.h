#pragma once
#include <mutex>
#include <vector>
#include "mem_struct.h"

// 读取线程打包的一帧数据
struct ActorRenderData {
    Vec3 worldPos;
};

struct ReadFrameData {
    uint64_t uworld = 0;
    uint64_t persistentLevel = 0;
    int actorCount = 0;
    std::vector<ActorRenderData> actors; // 已过滤的 actor
    FMatrix vpMatrix;      // 与 actor 位置同时采样的 VP 矩阵
    bool valid = false;
};

// 泛型双缓冲同步器：读取线程 submit()，渲染线程 fetch()
template <typename T>
class FrameSynchronizer {
private:
    T sharedBuffer;
    std::mutex mtx;
    bool hasNewFrame = false;

public:
    FrameSynchronizer() = default;

    // 读取线程：提交新的一帧
    void submit(T& data) {
        std::lock_guard<std::mutex> lock(mtx);
        std::swap(sharedBuffer, data);
        hasNewFrame = true;
    }

    // 渲染线程：获取最新帧，无新帧时返回 false
    bool fetch(T& out) {
        std::lock_guard<std::mutex> lock(mtx);
        if (!hasNewFrame) return false;
        std::swap(out, sharedBuffer);
        hasNewFrame = false;
        return true;
    }

    // 只读拷贝当前缓冲（不消费）
    void peek(T& out) {
        std::lock_guard<std::mutex> lock(mtx);
        out = sharedBuffer;
    }
};
