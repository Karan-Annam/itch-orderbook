// mold_wrap.cpp — wrap a raw length-prefixed .itch file into MoldUDP64
// packets (.mold): session[10] | seq[8] BE | count[2] BE | count x [len|body].
//
//   mold_wrap <in.itch> <out.mold> [--msgs-per-packet N] [--start-seq S]
//             [--drop-packet K]... [--no-end]
//
// --drop-packet K omits the K-th packet (0-based) from the output but still
// advances the sequence, fabricating a real gap for gap-detection tests.
// A session-end packet (count=0xFFFF) is appended unless --no-end.
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

static void put_be16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x & 0xFF));
}
static void put_be64(std::vector<uint8_t>& v, uint64_t x) {
    for (int i = 7; i >= 0; --i) v.push_back(uint8_t(x >> (8 * i)));
}
static void put_header(std::vector<uint8_t>& v, uint64_t seq, uint16_t count) {
    static const char session[10] = {'O','B','S','E','S','S','0','0','0','1'};
    v.insert(v.end(), session, session + 10);
    put_be64(v, seq);
    put_be16(v, count);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
            "usage: %s <in.itch> <out.mold> [--msgs-per-packet N] [--start-seq S]"
            " [--drop-packet K]... [--no-end]\n", argv[0]);
        return 2;
    }
    const char* in_path  = argv[1];
    const char* out_path = argv[2];
    size_t   per_pkt   = 4;
    uint64_t seq       = 1;
    bool     end_pkt   = true;
    std::set<uint64_t> drop;
    for (int a = 3; a < argc; ++a) {
        if (!std::strcmp(argv[a], "--msgs-per-packet") && a + 1 < argc)
            per_pkt = std::strtoull(argv[++a], nullptr, 10);
        else if (!std::strcmp(argv[a], "--start-seq") && a + 1 < argc)
            seq = std::strtoull(argv[++a], nullptr, 10);
        else if (!std::strcmp(argv[a], "--drop-packet") && a + 1 < argc)
            drop.insert(std::strtoull(argv[++a], nullptr, 10));
        else if (!std::strcmp(argv[a], "--no-end"))
            end_pkt = false;
        else { std::fprintf(stderr, "unknown arg: %s\n", argv[a]); return 2; }
    }
    if (per_pkt == 0 || per_pkt >= 0xFFFF) { std::fprintf(stderr, "bad --msgs-per-packet\n"); return 2; }

    std::FILE* fi = std::fopen(in_path, "rb");
    if (!fi) { std::fprintf(stderr, "cannot read %s\n", in_path); return 1; }
    std::fseek(fi, 0, SEEK_END); long sz = std::ftell(fi); std::fseek(fi, 0, SEEK_SET);
    std::vector<uint8_t> in(static_cast<size_t>(sz));
    if (sz > 0 && std::fread(in.data(), 1, size_t(sz), fi) != size_t(sz)) {
        std::fclose(fi); std::fprintf(stderr, "short read\n"); return 1;
    }
    std::fclose(fi);

    // split the input into [len|body] blocks
    std::vector<std::pair<size_t, size_t>> msgs;  // (offset, total bytes)
    size_t o = 0;
    while (o + 2 <= in.size()) {
        uint16_t blen = uint16_t(in[o]) << 8 | in[o + 1];
        if (blen == 0 || o + 2 + blen > in.size()) break;
        msgs.emplace_back(o, size_t(2 + blen));
        o += 2 + blen;
    }

    std::vector<uint8_t> out;
    uint64_t pkt_idx = 0, dropped = 0;
    for (size_t m = 0; m < msgs.size(); m += per_pkt, ++pkt_idx) {
        size_t n = std::min(per_pkt, msgs.size() - m);
        if (drop.count(pkt_idx)) { seq += n; ++dropped; continue; }
        put_header(out, seq, uint16_t(n));
        for (size_t i = 0; i < n; ++i)
            out.insert(out.end(), in.begin() + long(msgs[m + i].first),
                       in.begin() + long(msgs[m + i].first + msgs[m + i].second));
        seq += n;
    }
    if (end_pkt) put_header(out, seq, 0xFFFF);

    std::FILE* fo = std::fopen(out_path, "wb");
    if (!fo) { std::fprintf(stderr, "cannot write %s\n", out_path); return 1; }
    std::fwrite(out.data(), 1, out.size(), fo);
    std::fclose(fo);
    std::printf("mold_wrap: %zu msgs -> %llu packets (%llu dropped), %zu bytes, final seq %llu\n",
                msgs.size(), (unsigned long long)pkt_idx - (unsigned long long)dropped,
                (unsigned long long)dropped, out.size(), (unsigned long long)seq);
    return 0;
}
