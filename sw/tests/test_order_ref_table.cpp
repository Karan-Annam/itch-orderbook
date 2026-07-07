// Open-addressing hash table tests, incl. a differential run vs unordered_map.
#include "test_harness.hpp"
#include "../book/order_ref_table.hpp"

#include <unordered_map>
#include <random>
#include <vector>

using namespace ob;

static void test_basic_insert_find_erase() {
    OrderRefTable t(1024);
    OrderRefTable::Value v{150000, 100, 1, 'B', 0};
    CHECK(t.insert(42, v));
    auto* f = t.find(42);
    CHECK(f != nullptr);
    CHECK_EQ(f->price, 150000u);
    CHECK_EQ(f->shares, 100u);
    CHECK_EQ(f->side, 'B');
    CHECK_EQ(t.size(), size_t(1));
    CHECK(t.erase(42));
    CHECK(t.find(42) == nullptr);
    CHECK_EQ(t.size(), size_t(0));
    CHECK(!t.erase(42));          // already gone
    CHECK(!t.insert(0, v));       // 0 is the empty sentinel
}

static void test_update_overwrite() {
    OrderRefTable t(1024);
    t.insert(7, {100, 10, 1, 'B', 0});
    t.insert(7, {200, 20, 1, 'S', 0});   // overwrite
    auto* f = t.find(7);
    CHECK(f && f->price == 200u && f->shares == 20u && f->side == 'S');
    CHECK_EQ(t.size(), size_t(1));
}

// Force collisions: many keys whose home slot collides, then erase from the
// middle and confirm the rest are still findable (backward-shift correctness).
static void test_collision_and_backshift() {
    OrderRefTable t(64);
    std::vector<uint64_t> refs;
    // keys that all hash near each other are hard to construct; instead insert
    // many and rely on linear probing, then erase a random subset.
    for (uint64_t r = 1; r <= 40; ++r) {
        t.insert(r, {uint32_t(r * 10), uint32_t(r), 1, 'B', 0});
        refs.push_back(r);
    }
    CHECK_EQ(t.size(), size_t(40));
    // erase evens
    for (uint64_t r = 2; r <= 40; r += 2) CHECK(t.erase(r));
    CHECK_EQ(t.size(), size_t(20));
    // odds still present with correct values
    for (uint64_t r = 1; r <= 39; r += 2) {
        auto* f = t.find(r);
        CHECK(f != nullptr);
        if (f) { CHECK_EQ(f->price, uint32_t(r * 10)); }
    }
    // evens gone
    for (uint64_t r = 2; r <= 40; r += 2) CHECK(t.find(r) == nullptr);
}

// Randomised differential test against std::unordered_map as the oracle.
static void test_random_vs_oracle() {
    OrderRefTable t(1 << 16);
    std::unordered_map<uint64_t, OrderRefTable::Value> oracle;
    std::mt19937_64 rng(12345);
    for (int i = 0; i < 200000; ++i) {
        uint64_t ref = 1 + (rng() % 40000);
        int op = int(rng() % 3);
        if (op == 0) {  // insert/update
            OrderRefTable::Value v{uint32_t(rng() % 1000000),
                                   uint32_t(rng() % 1000), 1,
                                   (rng() & 1) ? 'B' : 'S', 0};
            t.insert(ref, v);
            oracle[ref] = v;
        } else if (op == 1) {  // erase
            t.erase(ref);
            oracle.erase(ref);
        } else {  // lookup must agree
            auto* f = t.find(ref);
            auto  o = oracle.find(ref);
            if (o == oracle.end()) { CHECK(f == nullptr); }
            else { CHECK(f != nullptr);
                   if (f) { CHECK_EQ(f->price, o->second.price);
                            CHECK_EQ(f->shares, o->second.shares); } }
        }
    }
    CHECK_EQ(t.size(), oracle.size());
    // final full agreement sweep
    int mism = 0;
    for (auto& kv : oracle) if (!t.find(kv.first)) ++mism;
    CHECK_EQ(mism, 0);
}

void run_order_ref_table_tests() {
    RUN_TEST(test_basic_insert_find_erase);
    RUN_TEST(test_update_overwrite);
    RUN_TEST(test_collision_and_backshift);
    RUN_TEST(test_random_vs_oracle);
}

#ifdef TEST_STANDALONE
int main() { run_order_ref_table_tests(); return obtest::summary(); }
#endif
