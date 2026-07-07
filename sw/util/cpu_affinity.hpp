// cpu_affinity.hpp — pin a thread to an isolated core and request real-time
// scheduling. This removes scheduler migration and most OS jitter from the hot
// path, which is what turns a noisy software latency distribution into a tight
// one. The calls are best-effort and platform-specific; on platforms where they
// are unavailable (Windows/MinGW dev host) they degrade to no-ops so the rest of
// the pipeline still builds and runs.
#pragma once

#include <cstdio>

#if defined(__linux__)
#  include <pthread.h>
#  include <sched.h>
#endif

namespace ob {

// Pin the calling thread to a single logical core. Returns true on success.
inline bool pin_to_core(int core_id) {
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core_id, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
    (void)core_id;
    return false;  // not supported on this platform; busy-poll still works
#endif
}

// Request SCHED_FIFO real-time priority for the calling thread.
inline bool set_realtime_priority(int priority = 80) {
#if defined(__linux__)
    sched_param sp{};
    sp.sched_priority = priority;
    return pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0;
#else
    (void)priority;
    return false;
#endif
}

}  // namespace ob
