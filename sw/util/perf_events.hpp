// Hardware performance counters via perf_event_open.
//
// Wraps the Linux perf_event_open syscall to count CPU cache misses and branch
// mispredicts around the measured region, which is what explains the software
// latency tail (a single L3 miss is ~40-200 ns; a branch mispredict flushes the
// pipeline). On non-Linux hosts the class compiles to a no-op stub that reports
// "unsupported", so the rest of the program is unchanged.
#pragma once

#include <cstdint>

#if defined(__linux__)
#  include <linux/perf_event.h>
#  include <sys/syscall.h>
#  include <sys/ioctl.h>
#  include <unistd.h>
#  include <cstring>
#endif

namespace ob {

class PerfCounters {
public:
    struct Snapshot {
        uint64_t cache_misses     = 0;
        uint64_t branch_mispredicts = 0;
        bool     valid = false;
    };

    bool open() {
#if defined(__linux__)
        cache_fd_  = open_one(PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES);
        branch_fd_ = open_one(PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES);
        return cache_fd_ >= 0 && branch_fd_ >= 0;
#else
        return false;
#endif
    }

    void start() {
#if defined(__linux__)
        reset_enable(cache_fd_);
        reset_enable(branch_fd_);
#endif
    }

    Snapshot stop() {
        Snapshot s;
#if defined(__linux__)
        if (cache_fd_ >= 0)  { disable(cache_fd_);  s.cache_misses = read_one(cache_fd_); }
        if (branch_fd_ >= 0) { disable(branch_fd_); s.branch_mispredicts = read_one(branch_fd_); }
        s.valid = (cache_fd_ >= 0 && branch_fd_ >= 0);
#endif
        return s;
    }

    void close() {
#if defined(__linux__)
        if (cache_fd_  >= 0) { ::close(cache_fd_);  cache_fd_  = -1; }
        if (branch_fd_ >= 0) { ::close(branch_fd_); branch_fd_ = -1; }
#endif
    }

    static bool supported() {
#if defined(__linux__)
        return true;
#else
        return false;
#endif
    }

private:
#if defined(__linux__)
    int open_one(uint32_t type, uint64_t config) {
        perf_event_attr attr;
        std::memset(&attr, 0, sizeof(attr));
        attr.type = type;
        attr.size = sizeof(attr);
        attr.config = config;
        attr.disabled = 1;
        attr.exclude_kernel = 1;
        attr.exclude_hv = 1;
        long fd = syscall(SYS_perf_event_open, &attr, 0, -1, -1, 0);
        return int(fd);
    }
    static void reset_enable(int fd) {
        if (fd < 0) return;
        ioctl(fd, PERF_EVENT_IOC_RESET, 0);
        ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
    }
    static void disable(int fd) { if (fd >= 0) ioctl(fd, PERF_EVENT_IOC_DISABLE, 0); }
    static uint64_t read_one(int fd) {
        uint64_t v = 0;
        if (fd >= 0 && ::read(fd, &v, sizeof(v)) != (long)sizeof(v)) v = 0;
        return v;
    }
    int cache_fd_  = -1;
    int branch_fd_ = -1;
#endif
};

}  // namespace ob
