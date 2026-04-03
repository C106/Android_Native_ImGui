#pragma once

#include <linux/types.h>
#include <sys/ioctl.h>

#ifdef __has_include
#if __has_include(<linux/arm64_hwbp_debugger.h>)
#include <linux/arm64_hwbp_debugger.h>
#define HAVE_SYSTEM_ARM64_HWBP_DEBUGGER_HEADER 1
#endif
#endif

#ifndef HAVE_SYSTEM_ARM64_HWBP_DEBUGGER_HEADER
/*
 * Fallback UAPI definitions for local builds where the kernel header is not
 * installed. These values must match the target kernel driver interface.
 */
#define ARM64_HWBP_IOC_MAGIC 'H'

struct arm64_hwbp_request {
    __s32 pid;
    __u32 reserved;
    __u64 addr;
};

struct arm64_hwbp_status {
    __u64 addr;
    __u64 hit_count;
    __u32 thread_count;
    __u32 flags;
    __s32 tgid;
    __s32 last_tid;
};

struct arm64_hwbp_regs_snapshot {
    __u64 regs[31];
    __u64 sp;
    __u64 pc;
    __u64 pstate;
    __u64 bp_addr;
    __u64 hit_count;
    __s32 tgid;
    __s32 tid;
};

#define ARM64_HWBP_STATUS_ACTIVE (1U << 0)
#define ARM64_HWBP_STATUS_HIT_READY (1U << 1)
#define ARM64_HWBP_STATUS_DISABLED (1U << 2)

#define ARM64_HWBP_IOC_SET_BREAKPOINT _IOW(ARM64_HWBP_IOC_MAGIC, 0x01, struct arm64_hwbp_request)
#define ARM64_HWBP_IOC_CLEAR_BREAKPOINT _IO(ARM64_HWBP_IOC_MAGIC, 0x02)
#define ARM64_HWBP_IOC_GET_STATUS _IOR(ARM64_HWBP_IOC_MAGIC, 0x03, struct arm64_hwbp_status)
#endif
