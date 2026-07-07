// order_book.cpp — out-of-line helpers for the order book (depth snapshots).
#include "order_book.hpp"

#include <cstdio>

namespace ob {

// Write a top-of-book depth snapshot row (used by analysis/book_depth.py).
void append_book_depth_csv(FILE* f, uint64_t seq, uint16_t locate,
                           const OrderBook& b) {
    if (!f) return;
    std::fprintf(f, "%llu,%u,%u,%u,%u,%u,%u\n",
                 (unsigned long long)seq, locate,
                 b.best_bid(), b.best_bid_depth(),
                 b.best_ask(), b.best_ask_depth(), b.spread());
}

}  // namespace ob
