#include "itch_file_reader.hpp"

#include <cstdio>

namespace obsim {

std::vector<uint8_t> read_itch_file(const std::string& path) {
    std::vector<uint8_t> out;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz > 0) {
        out.resize(static_cast<size_t>(sz));
        size_t got = std::fread(out.data(), 1, out.size(), f);
        out.resize(got);
    }
    std::fclose(f);
    return out;
}

std::vector<RawMessage> split_messages(const std::vector<uint8_t>& s, size_t max_msgs) {
    std::vector<RawMessage> msgs;
    size_t i = 0;
    while (i + 2 <= s.size()) {
        uint16_t len = (uint16_t(s[i]) << 8) | uint16_t(s[i + 1]);
        if (len == 0 || i + 2 + len > s.size()) break;
        RawMessage m;
        m.body.assign(s.begin() + i + 2, s.begin() + i + 2 + len);
        msgs.push_back(std::move(m));
        i += 2 + len;
        if (max_msgs && msgs.size() >= max_msgs) break;
    }
    return msgs;
}

std::vector<uint8_t> reserialize(const std::vector<RawMessage>& msgs) {
    std::vector<uint8_t> out;
    for (const auto& m : msgs) {
        uint16_t len = static_cast<uint16_t>(m.body.size());
        out.push_back(uint8_t(len >> 8));
        out.push_back(uint8_t(len & 0xFF));
        out.insert(out.end(), m.body.begin(), m.body.end());
    }
    return out;
}

}  // namespace obsim
