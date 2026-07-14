// Aggregates every software test suite into one binary; exit 0 = all passed.
#include "test_harness.hpp"

void run_util_tests();
void run_order_ref_table_tests();
void run_itch_parser_tests();
void run_mold_parser_tests();
void run_book_engine_tests();
void run_stats_tests();
void run_full_pipeline_tests();
void run_program_loader_tests();
void run_dsl_vm_tests();
void run_bar_builder_tests();
void run_fill_engine_tests();
void run_order_manager_tests();
void run_trade_e2e_tests();

int main() {
    std::printf("======== Order Book — Software Test Suite ========\n");
    std::printf("[util]\n");            run_util_tests();
    std::printf("[order_ref_table]\n"); run_order_ref_table_tests();
    std::printf("[itch_parser]\n");     run_itch_parser_tests();
    std::printf("[mold_parser]\n");     run_mold_parser_tests();
    std::printf("[book_engine]\n");     run_book_engine_tests();
    std::printf("[stats]\n");           run_stats_tests();
    std::printf("[full_pipeline]\n");   run_full_pipeline_tests();
    std::printf("[program_loader]\n");  run_program_loader_tests();
    std::printf("[dsl_vm]\n");          run_dsl_vm_tests();
    std::printf("[bar_builder]\n");     run_bar_builder_tests();
    std::printf("[fill_engine]\n");     run_fill_engine_tests();
    std::printf("[order_manager]\n");   run_order_manager_tests();
    std::printf("[trade_e2e]\n");       run_trade_e2e_tests();
    return obtest::summary();
}
