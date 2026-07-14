// cpu_affinity.hpp — pin a thread to a chosen logical core and request elevated
// scheduling priority. These best-effort, platform-specific calls reduce
// migration and scheduler jitter; they do not reserve or isolate the core.
#pragma once

#include <cstdio>

#if defined(__linux__)
#  include <pthread.h>
#  include <sched.h>
#elif defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace ob {

// Pin the calling thread to a single logical core. Returns true on success.
inline bool pin_to_core(int core_id) {
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core_id, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#elif defined(_WIN32)
    if (core_id < 0 || core_id >= 64) return false;
    const DWORD_PTR mask = DWORD_PTR(1) << unsigned(core_id);
    return SetThreadAffinityMask(GetCurrentThread(), mask) != 0;
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
#elif defined(_WIN32)
    (void)priority;
    return SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST) != 0;
#else
    (void)priority;
    return false;
#endif
}

}  // namespace ob
