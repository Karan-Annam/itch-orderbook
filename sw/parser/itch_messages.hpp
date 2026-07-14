// NASDAQ ITCH 5.0 message definitions and wire layout.
//
// Single source of truth for the byte layout, shared by the software parser,
// the synthetic generator, and (mirrored in SystemVerilog) the RTL decoder.
// SW and HW can only produce identical book state if they agree here.
//
// Layout convention:
//   * Every message body begins with a 13-byte common header:
//       [0]      message type        char
//       [1:2]    stock_locate        uint16 BE
//       [3:4]    tracking_number     uint16 BE
//       [5:12]   timestamp           uint64 BE  (ns since midnight; we use 8B,
//                real ITCH wire format is 6B — see data/README.md)
//   * Type-specific fields follow at body offset 13.
//   * On the wire each body is preceded by a 2-byte BE length prefix
//     (framing), so a message occupies (2 + body_len) bytes.
//   * All integers are big-endian (network byte order).
#pragma once

#include <cstdint>
#include <cstddef>

namespace ob {

// ---- message type codes ---------------------------------------------------
enum class MsgType : char {
    AddOrder            = 'A',
    AddOrderMPID        = 'F',
    OrderExecuted       = 'E',
    OrderExecutedPrice  = 'C',
    OrderCancel         = 'X',
    OrderDelete         = 'D',
    OrderReplace        = 'U',
    Trade               = 'P',
    SystemEvent         = 'S',
    Unknown             = '?',
};

inline const char* msg_type_name(MsgType t) {
    switch (t) {
        case MsgType::AddOrder:           return "Add";
        case MsgType::AddOrderMPID:       return "AddMPID";
        case MsgType::OrderExecuted:      return "Execute";
        case MsgType::OrderExecutedPrice: return "ExecutePrice";
        case MsgType::OrderCancel:        return "Cancel";
        case MsgType::OrderDelete:        return "Delete";
        case MsgType::OrderReplace:       return "Replace";
        case MsgType::Trade:              return "Trade";
        case MsgType::SystemEvent:        return "System";
        default:                          return "Unknown";
    }
}

// Index used for per-type counter/histogram arrays. Order matters: it is mirrored
// by the RTL perf_counters msg_count[8] layout (A,F,E,C,X,D,U,P) plus System.
enum MsgTypeIdx {
    IDX_ADD = 0, IDX_ADD_MPID, IDX_EXEC, IDX_EXEC_PRICE,
    IDX_CANCEL, IDX_DELETE, IDX_REPLACE, IDX_TRADE, IDX_SYSTEM,
    IDX_COUNT
};

inline int msg_type_idx(MsgType t) {
    switch (t) {
        case MsgType::AddOrder:           return IDX_ADD;
        case MsgType::AddOrderMPID:       return IDX_ADD_MPID;
        case MsgType::OrderExecuted:      return IDX_EXEC;
        case MsgType::OrderExecutedPrice: return IDX_EXEC_PRICE;
        case MsgType::OrderCancel:        return IDX_CANCEL;
        case MsgType::OrderDelete:        return IDX_DELETE;
        case MsgType::OrderReplace:       return IDX_REPLACE;
        case MsgType::Trade:              return IDX_TRADE;
        case MsgType::SystemEvent:        return IDX_SYSTEM;
        default:                          return IDX_COUNT - 1;
    }
}

// ---- wire layout constants ------------------------------------------------
// Common 13-byte header field offsets (within the message body).
namespace hdr {
    constexpr size_t TYPE       = 0;
    constexpr size_t STOCK_LOC  = 1;   // u16
    constexpr size_t TRACK_NUM  = 3;   // u16
    constexpr size_t TIMESTAMP  = 5;   // u64 (8 bytes in this format)
    constexpr size_t END        = 13;  // type-specific fields start here
}

// Per-type body lengths (bytes). Max is Trade ('P') at 46 bytes, which is why
// the RTL decoder uses a 48-byte shift register.
namespace blen {
    constexpr size_t ADD        = 38;  // hdr + ref(8)+side(1)+shares(4)+stock(8)+price(4)
    constexpr size_t ADD_MPID   = 42;  // ADD + mpid(4)
    constexpr size_t EXEC       = 33;  // hdr + ref(8)+exec_shares(4)+match(8)
    constexpr size_t EXEC_PRICE = 38;  // hdr + ref(8)+exec_shares(4)+match(8)+printable(1)+price(4)
    constexpr size_t CANCEL     = 25;  // hdr + ref(8)+cancelled_shares(4)
    constexpr size_t DELETE     = 21;  // hdr + ref(8)
    constexpr size_t REPLACE    = 37;  // hdr + orig_ref(8)+new_ref(8)+shares(4)+price(4)
    constexpr size_t TRADE      = 46;  // hdr + ref(8)+side(1)+shares(4)+stock(8)+price(4)+match(8)
    constexpr size_t SYSTEM     = 14;  // hdr + event_code(1)
    constexpr size_t MAX        = 48;
}

// Exact body length for a supported internal message type. Returning zero
// means the type is unsupported. Keeping this next to the layout constants
// lets every parser validate a body before touching type-specific fields.
inline constexpr size_t msg_body_len(MsgType t) {
    switch (t) {
        case MsgType::AddOrder:           return blen::ADD;
        case MsgType::AddOrderMPID:       return blen::ADD_MPID;
        case MsgType::OrderExecuted:      return blen::EXEC;
        case MsgType::OrderExecutedPrice: return blen::EXEC_PRICE;
        case MsgType::OrderCancel:        return blen::CANCEL;
        case MsgType::OrderDelete:        return blen::DELETE;
        case MsgType::OrderReplace:       return blen::REPLACE;
        case MsgType::Trade:              return blen::TRADE;
        case MsgType::SystemEvent:        return blen::SYSTEM;
        default:                          return 0;
    }
}

// Field offsets within each message body (after the 13-byte header).
namespace off {
    // Add / AddMPID ('A','F')
    constexpr size_t ADD_REF    = 13;  // u64
    constexpr size_t ADD_SIDE   = 21;  // char
    constexpr size_t ADD_SHARES = 22;  // u32
    constexpr size_t ADD_STOCK  = 26;  // char[8]
    constexpr size_t ADD_PRICE  = 34;  // u32
    constexpr size_t ADD_MPID   = 38;  // char[4]  (F only)

    // Order Executed ('E')
    constexpr size_t EXEC_REF    = 13; // u64
    constexpr size_t EXEC_SHARES = 21; // u32
    constexpr size_t EXEC_MATCH  = 25; // u64

    // Order Executed With Price ('C')
    constexpr size_t EXECP_REF       = 13; // u64
    constexpr size_t EXECP_SHARES    = 21; // u32
    constexpr size_t EXECP_MATCH     = 25; // u64
    constexpr size_t EXECP_PRINTABLE = 33; // char ('Y'/'N')
    constexpr size_t EXECP_PRICE     = 34; // u32

    // Order Cancel ('X')
    constexpr size_t CANCEL_REF    = 13; // u64
    constexpr size_t CANCEL_SHARES = 21; // u32

    // Order Delete ('D')
    constexpr size_t DELETE_REF = 13;    // u64

    // Order Replace ('U')
    constexpr size_t REPL_ORIG_REF = 13; // u64
    constexpr size_t REPL_NEW_REF  = 21; // u64
    constexpr size_t REPL_SHARES   = 29; // u32
    constexpr size_t REPL_PRICE    = 33; // u32

    // Trade ('P')
    constexpr size_t TRADE_REF    = 13;  // u64 (usually 0)
    constexpr size_t TRADE_SIDE   = 21;  // char
    constexpr size_t TRADE_SHARES = 22;  // u32
    constexpr size_t TRADE_STOCK  = 26;  // char[8]
    constexpr size_t TRADE_PRICE  = 34;  // u32
    constexpr size_t TRADE_MATCH  = 38;  // u64

    // System Event ('S')
    constexpr size_t SYS_EVENT = 13;     // char
}

// Side codes.
constexpr char SIDE_BUY  = 'B';
constexpr char SIDE_SELL = 'S';

// System event codes (subset).
constexpr char EVT_START_MESSAGES = 'O';
constexpr char EVT_START_HOURS    = 'Q';  // market open
constexpr char EVT_END_HOURS      = 'M';  // market close
constexpr char EVT_END_MESSAGES   = 'C';

// ---- decoded message (a flat, tagged superset for the SPSC queue) ----------
// Fat-but-flat: ~64 bytes, trivially copyable, switch on `type`. Fields not
// relevant to a given type are left zero-initialised.
struct DecodedMessage {
    MsgType  type        = MsgType::Unknown;
    char     side        = 0;       // A/F/P
    char     event_code  = 0;       // S
    uint8_t  printable   = 0;       // C
    uint16_t stock_locate = 0;      // raw locate code
    uint16_t stock_idx    = 0;      // resolved symbol index
    uint32_t shares       = 0;      // A/F add, X cancel, E/C exec, U new, P trade
    uint32_t price        = 0;      // A/F/U/P price; C execution price
    uint64_t timestamp    = 0;      // ns since midnight
    uint64_t order_ref    = 0;      // primary ref (original ref for Replace)
    uint64_t new_order_ref = 0;     // Replace only
    uint64_t match_number  = 0;     // E/C/P
    char     mpid[4]       = {0,0,0,0}; // F
    char     stock[8]      = {0};   // A/F/P symbol
};

}  // namespace ob
