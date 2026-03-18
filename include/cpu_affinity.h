#pragma once
#include <sched.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include <climits>

#ifdef NDEBUG
#define CPU_LOGD(...) ((void)0)
#else
#define CPU_LOGD(...) do { printf(__VA_ARGS__); fflush(stdout); } while(0)
#endif

enum class CoreTier { BIG, MEDIUM, LITTLE };

namespace cpu_affinity_detail {

struct Topology {
    cpu_set_t big;
    cpu_set_t medium;
    cpu_set_t little;
    bool valid = false;
};

inline Topology& get() {
    static Topology topo;
    return topo;
}

} // namespace cpu_affinity_detail

inline void InitCpuTopology() {
    auto& topo = cpu_affinity_detail::get();
    CPU_ZERO(&topo.big);
    CPU_ZERO(&topo.medium);
    CPU_ZERO(&topo.little);

    int numCpus = sysconf(_SC_NPROCESSORS_CONF);
    if (numCpus <= 0) numCpus = 8;

    std::vector<std::pair<int, long>> cpuFreqs;
    for (int i = 0; i < numCpus; i++) {
        char path[128];
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
        FILE* f = fopen(path, "r");
        long freq = 0;
        if (f) {
            fscanf(f, "%ld", &freq);
            fclose(f);
        }
        cpuFreqs.push_back({i, freq});
    }

    // 收集所有不同的频率值
    std::vector<long> uniqueFreqs;
    for (const auto& p : cpuFreqs) {
        if (p.second > 0) {
            if (std::find(uniqueFreqs.begin(), uniqueFreqs.end(), p.second) == uniqueFreqs.end())
                uniqueFreqs.push_back(p.second);
        }
    }
    std::sort(uniqueFreqs.begin(), uniqueFreqs.end());

    if (uniqueFreqs.size() < 2) {
        // 所有频率相同或无法读取，不设置亲和性
        CPU_LOGD("CPU topology: all cores same frequency (%zu tiers), skipping affinity\n",
                 uniqueFreqs.size());
        return;
    }

    long bigFreq = uniqueFreqs.back();
    long littleFreq = uniqueFreqs.front();

    for (const auto& p : cpuFreqs) {
        if (p.second == bigFreq) {
            CPU_SET(p.first, &topo.big);
        } else if (p.second == littleFreq) {
            CPU_SET(p.first, &topo.little);
        } else {
            CPU_SET(p.first, &topo.medium);
        }
    }

    // 如果只有 2 档频率，medium 为空，将 medium 指向 big
    if (uniqueFreqs.size() == 2) {
        topo.medium = topo.big;
        CPU_LOGD("CPU topology: 2 tiers detected, MEDIUM=BIG\n");
    }

    topo.valid = true;

    CPU_LOGD("CPU topology: %zu tiers, BIG=%d MEDIUM=%d LITTLE=%d cores\n",
             uniqueFreqs.size(),
             CPU_COUNT(&topo.big), CPU_COUNT(&topo.medium), CPU_COUNT(&topo.little));
}

inline void SetThreadAffinity(CoreTier tier) {
    const auto& topo = cpu_affinity_detail::get();
    if (!topo.valid) return;

    const cpu_set_t* mask = nullptr;
    const char* name = "";
    switch (tier) {
        case CoreTier::BIG:    mask = &topo.big;    name = "BIG";    break;
        case CoreTier::MEDIUM: mask = &topo.medium;  name = "MEDIUM"; break;
        case CoreTier::LITTLE: mask = &topo.little;  name = "LITTLE"; break;
    }

    if (sched_setaffinity(0, sizeof(cpu_set_t), mask) == 0) {
        CPU_LOGD("Thread %d affinity -> %s (%d cores)\n",
                 (int)gettid(), name, CPU_COUNT(mask));
    } else {
        CPU_LOGD("Thread %d affinity -> %s FAILED: %s\n",
                 (int)gettid(), name, strerror(errno));
    }
}
