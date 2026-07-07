// Aggregates every software test suite into one binary; exit 0 = all passed.
#include "test_harness.hpp"

void run_util_tests();
void run_order_ref_table_tests();
void run_itch_parser_tests();
void run_book_engine_tests();
void run_stats_tests();
void run_full_pipeline_tests();

int main() {
    std::printf("======== Order Book — Software Test Suite ========\n");
    std::printf("[util]\n");            run_util_tests();
    std::printf("[order_ref_table]\n"); run_order_ref_table_tests();
    std::printf("[itch_parser]\n");     run_itch_parser_tests();
    std::printf("[book_engine]\n");     run_book_engine_tests();
    std::printf("[stats]\n");           run_stats_tests();
    std::printf("[full_pipeline]\n");   run_full_pipeline_tests();
    return obtest::summary();
}
