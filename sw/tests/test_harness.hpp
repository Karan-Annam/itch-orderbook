// Minimal dependency-free test harness — no GoogleTest, builds with just g++.
// CHECK/CHECK_EQ record failures; a test function returns its failure count;
// RUN_TEST aggregates. Nonzero total = nonzero exit, which is what run_all.sh
// keys on.
#pragma once

#include <cstdio>
#include <cstdint>
#include <string>

namespace obtest {

struct Ctx {
    int checks = 0;
    int failures = 0;
    const char* name = "";
};

inline Ctx& ctx() { static Ctx c; return c; }

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++obtest::ctx().checks;                                                \
        if (!(cond)) {                                                         \
            ++obtest::ctx().failures;                                          \
            std::printf("    [FAIL] %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond); \
        }                                                                      \
    } while (0)

#define CHECK_EQ(a, b)                                                         \
    do {                                                                       \
        ++obtest::ctx().checks;                                                \
        auto _va = (a); auto _vb = (b);                                        \
        if (!(_va == _vb)) {                                                   \
            ++obtest::ctx().failures;                                          \
            std::printf("    [FAIL] %s:%d  CHECK_EQ(%s, %s): %lld != %lld\n",  \
                        __FILE__, __LINE__, #a, #b,                            \
                        (long long)_va, (long long)_vb);                       \
        }                                                                      \
    } while (0)

// Run a test function: int fn(). Prints PASS/FAIL with the local failure count.
#define RUN_TEST(fn)                                                           \
    do {                                                                       \
        int before = obtest::ctx().failures;                                   \
        std::printf("  RUN  %-34s ", #fn);                                     \
        std::fflush(stdout);                                                   \
        fn();                                                                  \
        int delta = obtest::ctx().failures - before;                          \
        std::printf("%s\n", delta == 0 ? "PASS" : "FAIL");                     \
    } while (0)

inline int summary() {
    std::printf("\n==== %d checks, %d failures ====\n",
                ctx().checks, ctx().failures);
    return ctx().failures == 0 ? 0 : 1;
}

}  // namespace obtest
