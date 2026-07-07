// itch_file_reader.hpp — load a raw length-prefixed ITCH 5.0 file into memory.
//
// The file is a concatenation of [2-byte BE length][body] records (exactly what
// tools/gen_itch.cpp writes and what a NASDAQ feed payload contains). This
// reader returns the full byte vector; the framer/parser handle record
// boundaries. A helper also splits the stream into individual message bodies for
// the reference model and the unit tests.
#pragma once

#include <cstdint>
#include <vector>
#include <string>

namespace obsim {

// Read an entire file into a byte vector. Returns empty on failure.
std::vector<uint8_t> read_itch_file(const std::string& path);

// A single message: its body bytes (without the 2-byte length prefix).
struct RawMessage {
    std::vector<uint8_t> body;
};

// Split a raw length-prefixed stream into message bodies. Stops if a truncated
// record is encountered. If max_msgs > 0, stops after that many messages.
std::vector<RawMessage> split_messages(const std::vector<uint8_t>& stream,
                                       size_t max_msgs = 0);

// Re-serialise a subset of messages back into a length-prefixed stream (used to
// drive a bounded number of messages through the RTL).
std::vector<uint8_t> reserialize(const std::vector<RawMessage>& msgs);

}  // namespace obsim
