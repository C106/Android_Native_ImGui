#pragma once
#include "cpu_affinity.h"
#include <atomic>
#include <thread>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <mutex>
#include <deque>

#ifdef NDEBUG
#define GFPS_LOGD(...) ((void)0)
#else
#define GFPS_LOGD(...) do { printf(__VA_ARGS__); fflush(stdout); } while(0)
#endif

namespace game_fps_monitor {

struct GameFPSStats {
    float fps            = 0.0f;
    float frametime_avg_ms = 0.0f;
    float frametime_max_ms = 0.0f;
    int   jank_count     = 0;
};

// 从渲染线程推送的 delta time 样本（滑动窗口）
struct DeltaTimeSample {
    double deltaTime;  // 秒
    std::chrono::steady_clock::time_point timestamp;
};

inline std::atomic<float> gGameFPS{0.0f};
inline GameFPSStats       gGameFPSStats{};
inline std::mutex         gStatsMutex;
inline std::atomic<bool>  gMonitorRunning{false};
inline std::thread        gMonitorThread;
inline std::string        gPackageName;

// 内存读取模式（从渲染线程推送 delta time）
inline std::deque<DeltaTimeSample> gDeltaSamples;
inline std::mutex                  gDeltaSamplesMutex;
inline std::atomic<bool>           gUseMemoryMode{false};  // true = 从内存读取，false = dumpsys

// 从 RequestedLayerState{HEXID LAYERNAME#NNN parentId=...} 中提取 layer name
// 旧格式（直接是 layer name）原样返回
inline std::string ExtractLayerName(const char* line) {
    // 新格式: RequestedLayerState{hexid Name#NNN parentId=...}
    const char* prefix = "RequestedLayerState{";
    const char* p = strstr(line, prefix);
    if (!p) return line; // 旧格式，原样返回

    p += strlen(prefix);
    // 跳过 hex id + 空格
    while (*p && *p != ' ') p++;
    if (*p == ' ') p++;

    // 找到 " parentId=" 或 "}" 作为结尾
    const char* end = strstr(p, " parentId=");
    if (!end) end = strchr(p, '}');
    if (!end) return line;

    return std::string(p, end);
}

// 从滑动窗口计算 FPS 统计（内存读取模式）
inline GameFPSStats CalculateStatsFromSamples() {
    GameFPSStats stats{};
    std::lock_guard<std::mutex> lock(gDeltaSamplesMutex);

    if (gDeltaSamples.size() < 2) return stats;

    // 清理超过 1 秒的旧样本
    auto now = std::chrono::steady_clock::now();
    auto cutoff = now - std::chrono::seconds(1);
    while (!gDeltaSamples.empty() && gDeltaSamples.front().timestamp < cutoff) {
        gDeltaSamples.pop_front();
    }

    if (gDeltaSamples.size() < 2) return stats;

    // 计算统计
    double sumDelta = 0.0;
    double maxDelta = 0.0;
    int count = 0;

    for (const auto& sample : gDeltaSamples) {
        if (sample.deltaTime > 0.0 && sample.deltaTime < 1.0) {  // 过滤异常值
            sumDelta += sample.deltaTime;
            if (sample.deltaTime > maxDelta) maxDelta = sample.deltaTime;
            count++;
        }
    }

    if (count < 2) return stats;

    double avgDelta = sumDelta / count;
    stats.fps = (float)(1.0 / avgDelta);
    stats.frametime_avg_ms = (float)(avgDelta * 1000.0);
    stats.frametime_max_ms = (float)(maxDelta * 1000.0);

    // jank: delta > 2x 平均值
    double jankThreshold = avgDelta * 2.0;
    for (const auto& sample : gDeltaSamples) {
        if (sample.deltaTime > jankThreshold) stats.jank_count++;
    }

    return stats;
}

// 渲染线程推送 delta time 样本（从内存读取）
inline void PushDeltaTimeSample(double deltaTime) {
    if (!gUseMemoryMode.load(std::memory_order_relaxed)) return;

    std::lock_guard<std::mutex> lock(gDeltaSamplesMutex);
    gDeltaSamples.push_back({deltaTime, std::chrono::steady_clock::now()});

    // 限制队列大小（最多保留 2 秒数据）
    if (gDeltaSamples.size() > 240) {  // 120 FPS * 2s
        gDeltaSamples.pop_front();
    }
}

// 从 dumpsys SurfaceFlinger --list 中找到游戏 layer 名称
inline std::string FindGameLayerName(const std::string& packageName) {
    FILE* fp = popen("dumpsys SurfaceFlinger --list", "r");
    if (!fp) return "";

    std::string bestMatch;
    std::string surfaceViewMatch;
    char line[512];

    while (fgets(line, sizeof(line), fp)) {
        // 去掉换行
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        if (strstr(line, packageName.c_str()) == nullptr)
            continue;

        std::string layerName = ExtractLayerName(line);

        // 优先选含 SurfaceView 的行（游戏 GPU 渲染面）
        if (strstr(line, "SurfaceView")) {
            // 优先选 BLAST 层（实际渲染面）
            if (strstr(line, "BLAST") || surfaceViewMatch.empty())
                surfaceViewMatch = layerName;
        } else if (bestMatch.empty()) {
            bestMatch = layerName;
        }
    }
    pclose(fp);

    return surfaceViewMatch.empty() ? bestMatch : surfaceViewMatch;
}

// 解析 dumpsys SurfaceFlinger --latency 输出，计算 FPS + 抖动指标
inline GameFPSStats CalculateFPSFromLatency(const std::string& layerName) {
    GameFPSStats stats{};
    std::string cmd = "dumpsys SurfaceFlinger --latency \"" + layerName + "\"";
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return stats;

    char line[256];
    // 第 1 行: refreshPeriodNs（跳过）
    if (!fgets(line, sizeof(line), fp)) { pclose(fp); return stats; }

    // 后续 127 行: desiredPresentTime \t actualPresentTime \t frameReadyTime
    std::vector<int64_t> timestamps;
    timestamps.reserve(128);

    while (fgets(line, sizeof(line), fp)) {
        int64_t desired = 0, actual = 0, ready = 0;
        if (sscanf(line, "%ld\t%ld\t%ld", &desired, &actual, &ready) >= 2) {
            // 跳过无效值
            if (actual > 0 && actual < INT64_MAX / 2) {
                timestamps.push_back(actual);
            }
        }
    }
    pclose(fp);

    if (timestamps.size() < 2) return stats;

    std::sort(timestamps.begin(), timestamps.end());

    // 取最近 1 秒窗口内的帧
    int64_t newest = timestamps.back();
    int64_t windowNs = 1000000000LL; // 1 秒
    int64_t cutoff = newest - windowNs;

    // 找到窗口起始位置
    auto it = std::lower_bound(timestamps.begin(), timestamps.end(), cutoff);
    int count = (int)(timestamps.end() - it);

    if (count < 2) return stats;

    int64_t timeSpanNs = newest - *it;
    if (timeSpanNs <= 0) return stats;

    stats.fps = (float)(count - 1) * 1e9f / (float)timeSpanNs;

    // 计算帧时间统计（窗口内的帧间隔）
    int64_t sumDeltaNs = 0;
    int64_t maxDeltaNs = 0;
    int numDeltas = 0;

    auto prev = it;
    for (auto cur = it + 1; cur != timestamps.end(); ++cur) {
        int64_t delta = *cur - *prev;
        if (delta > 0) {
            sumDeltaNs += delta;
            if (delta > maxDeltaNs) maxDeltaNs = delta;
            numDeltas++;
        }
        prev = cur;
    }

    if (numDeltas > 0) {
        stats.frametime_avg_ms = (float)sumDeltaNs / (float)numDeltas / 1e6f;
        stats.frametime_max_ms = (float)maxDeltaNs / 1e6f;

        // jank: 帧时间 > 2x 平均值
        int64_t avgDeltaNs = sumDeltaNs / numDeltas;
        int64_t jankThreshold = avgDeltaNs * 2;
        prev = it;
        for (auto cur = it + 1; cur != timestamps.end(); ++cur) {
            int64_t delta = *cur - *prev;
            if (delta > jankThreshold) stats.jank_count++;
            prev = cur;
        }
    }

    return stats;
}

inline void monitorThreadFunc() {
    SetThreadAffinity(CoreTier::LITTLE);

    std::string layerName;
    int zeroCount = 0;

    while (gMonitorRunning.load(std::memory_order_relaxed)) {
        // 内存读取模式：从滑动窗口计算统计
        if (gUseMemoryMode.load(std::memory_order_relaxed)) {
            GameFPSStats stats = CalculateStatsFromSamples();
            gGameFPS.store(stats.fps, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lock(gStatsMutex);
                gGameFPSStats = stats;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        // dumpsys 模式（原逻辑）
        // Layer 发现 / 重发现
        if (layerName.empty()) {
            layerName = FindGameLayerName(gPackageName);
            if (layerName.empty()) {
                GFPS_LOGD("[GameFPS] Layer not found for %s, retrying in 2s\n", gPackageName.c_str());
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }
            GFPS_LOGD("[GameFPS] Found layer: %s\n", layerName.c_str());
            zeroCount = 0;
        }

        GameFPSStats stats = CalculateFPSFromLatency(layerName);
        gGameFPS.store(stats.fps, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(gStatsMutex);
            gGameFPSStats = stats;
        }

        // 连续 5 次返回 0 → 重新发现 layer（游戏可能重启）
        if (stats.fps < 0.5f) {
            if (++zeroCount >= 5) {
                GFPS_LOGD("[GameFPS] 5 consecutive zero readings, re-discovering layer\n");
                layerName.clear();
                zeroCount = 0;
            }
        } else {
            zeroCount = 0;
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

inline float GetGameFPS() {
    return gGameFPS.load(std::memory_order_relaxed);
}

inline GameFPSStats GetGameFPSStats() {
    std::lock_guard<std::mutex> lock(gStatsMutex);
    return gGameFPSStats;
}

inline void StartGameFPSMonitor(const char* pkgName) {
    if (gMonitorRunning.load()) return;
    gPackageName = pkgName;
    gMonitorRunning.store(true);
    gMonitorThread = std::thread(monitorThreadFunc);
    GFPS_LOGD("[GameFPS] Monitor started for %s\n", pkgName);
}

inline void StopGameFPSMonitor() {
    gMonitorRunning.store(false);
    if (gMonitorThread.joinable())
        gMonitorThread.join();
    GFPS_LOGD("[GameFPS] Monitor stopped\n");
}

inline void EnableMemoryMode(bool enable) {
    gUseMemoryMode.store(enable, std::memory_order_relaxed);
    if (enable) {
        std::lock_guard<std::mutex> lock(gDeltaSamplesMutex);
        gDeltaSamples.clear();
        GFPS_LOGD("[GameFPS] Memory mode enabled\n");
    } else {
        GFPS_LOGD("[GameFPS] Memory mode disabled, using dumpsys\n");
    }
}

} // namespace game_fps_monitor

// 便捷全局接口
inline float GetGameFPS() { return game_fps_monitor::GetGameFPS(); }
inline game_fps_monitor::GameFPSStats GetGameFPSStats() { return game_fps_monitor::GetGameFPSStats(); }
inline void  StartGameFPSMonitor(const char* pkgName) { game_fps_monitor::StartGameFPSMonitor(pkgName); }
inline void  StopGameFPSMonitor() { game_fps_monitor::StopGameFPSMonitor(); }
inline void  EnableMemoryMode(bool enable) { game_fps_monitor::EnableMemoryMode(enable); }
inline void  PushGameDeltaTime(double deltaTime) { game_fps_monitor::PushDeltaTimeSample(deltaTime); }
