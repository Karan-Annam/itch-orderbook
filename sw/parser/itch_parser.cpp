// SIMD batch header scan (out-of-line). Counts occurrences of a byte value
// using the widest vector ISA available: AVX-512 does 64 bytes/iteration via
// _mm512_cmpeq_epi8_mask, AVX2 does 32, scalar otherwise. All three return the
// same count; the scalar path is the reference.
#include "itch_parser.hpp"
#include "../util/simd.hpp"

namespace ob {

DecodedMessage ItchParser::decode_body(const uint8_t* b, size_t body_len) {
    DecodedMessage m;
    if (!b || body_len == 0) return m;
    m.type = static_cast<MsgType>(static_cast<char>(b[hdr::TYPE]));
    const size_t expected = msg_body_len(m.type);
    if (expected == 0 || body_len != expected) {
        m.type = MsgType::Unknown;
        return m;
    }
    m.stock_locate = be16(b + hdr::STOCK_LOC);
    m.stock_idx    = m.stock_locate;
    m.timestamp    = be64(b + hdr::TIMESTAMP);

    switch (m.type) {
        case MsgType::AddOrder:
        case MsgType::AddOrderMPID:
            m.order_ref = be64(b + off::ADD_REF);
            m.side      = static_cast<char>(b[off::ADD_SIDE]);
            m.shares    = be32(b + off::ADD_SHARES);
            copy8(m.stock, b + off::ADD_STOCK);
            m.price     = be32(b + off::ADD_PRICE);
            if (m.type == MsgType::AddOrderMPID)
                for (int i = 0; i < 4; ++i) m.mpid[i] = char(b[off::ADD_MPID + i]);
            break;
        case MsgType::OrderExecuted:
            m.order_ref    = be64(b + off::EXEC_REF);
            m.shares       = be32(b + off::EXEC_SHARES);
            m.match_number = be64(b + off::EXEC_MATCH);
            break;
        case MsgType::OrderExecutedPrice:
            m.order_ref    = be64(b + off::EXECP_REF);
            m.shares       = be32(b + off::EXECP_SHARES);
            m.match_number = be64(b + off::EXECP_MATCH);
            m.printable    = (char(b[off::EXECP_PRINTABLE]) == 'Y') ? 1 : 0;
            m.price        = be32(b + off::EXECP_PRICE);
            break;
        case MsgType::OrderCancel:
            m.order_ref = be64(b + off::CANCEL_REF);
            m.shares    = be32(b + off::CANCEL_SHARES);
            break;
        case MsgType::OrderDelete:
            m.order_ref = be64(b + off::DELETE_REF);
            break;
        case MsgType::OrderReplace:
            m.order_ref     = be64(b + off::REPL_ORIG_REF);
            m.new_order_ref = be64(b + off::REPL_NEW_REF);
            m.shares        = be32(b + off::REPL_SHARES);
            m.price         = be32(b + off::REPL_PRICE);
            break;
        case MsgType::Trade:
            m.order_ref    = be64(b + off::TRADE_REF);
            m.side         = static_cast<char>(b[off::TRADE_SIDE]);
            m.shares       = be32(b + off::TRADE_SHARES);
            copy8(m.stock, b + off::TRADE_STOCK);
            m.price        = be32(b + off::TRADE_PRICE);
            m.match_number = be64(b + off::TRADE_MATCH);
            break;
        case MsgType::SystemEvent:
            m.event_code = static_cast<char>(b[off::SYS_EVENT]);
            break;
        default:
            m.type = MsgType::Unknown;
            break;
    }
    return m;
}

size_t ItchParser::simd_count_type(const uint8_t* data, size_t len, uint8_t tb) {
    size_t count = 0;
    size_t i = 0;

#if defined(OB_SIMD_AVX512)
    const __m512i needle = _mm512_set1_epi8(static_cast<char>(tb));
    for (; i + 64 <= len; i += 64) {
        __m512i v = _mm512_loadu_si512(reinterpret_cast<const void*>(data + i));
        __mmask64 hits = _mm512_cmpeq_epi8_mask(v, needle);
        count += __builtin_popcountll(static_cast<unsigned long long>(hits));
    }
#elif defined(OB_SIMD_AVX2)
    const __m256i needle = _mm256_set1_epi8(static_cast<char>(tb));
    for (; i + 32 <= len; i += 32) {
        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
        unsigned m = static_cast<unsigned>(
            _mm256_movemask_epi8(_mm256_cmpeq_epi8(v, needle)));
        count += static_cast<size_t>(__builtin_popcount(m));
    }
#endif
    for (; i < len; ++i) if (data[i] == tb) ++count;
    return count;
}

}  // namespace ob
