#pragma once

#include <cstddef>
#include <utility>
#include <vector>

#include "common.h"

// Bits and port-right kinds of the Mach message format.
constexpr u32 kComplex = 0x80000000u;  // the message carries ports or out-of-line memory
constexpr u32 kMoveSend = 17;          // how a reply hands a port right to the guest

// Builds a message for the guest to receive, in the guest's byte order.
class Message {
 public:
  Message(bool big_endian, u32 local_port, s32 id, bool complex = false, std::size_t capacity = 40)
      : be_(big_endian) {
    bytes_.reserve(capacity);
    u32 header[6] = {0x1200 | (complex ? kComplex : 0), 0, 0, local_port, 0, (u32)id};
    append_words(header, sizeof header / sizeof header[0]);
  }

  void put(u32 v) {
    if (be_) v = __builtin_bswap32(v);
    const u8* p = (const u8*)&v;
    bytes_.insert(bytes_.end(), p, p + 4);
  }
  void put_words(const u32* words, std::size_t count) {
    if (!be_) {
      const u8* p = (const u8*)words;
      bytes_.insert(bytes_.end(), p, p + count * sizeof(*words));
      return;
    }
    const std::size_t offset = bytes_.size();
    bytes_.resize(offset + count * sizeof(*words));
    for (std::size_t i = 0; i < count; ++i) {
      const u32 value = __builtin_bswap32(words[i]);
      memcpy(bytes_.data() + offset + i * sizeof(value), &value, sizeof(value));
    }
  }
  void put64(u64 v) {
    if (be_) v = __builtin_bswap64(v);
    const u8* p = (const u8*)&v;
    bytes_.insert(bytes_.end(), p, p + sizeof v);
  }
  // Append uninterpreted data, padded to the message's 4-byte alignment.
  void put_bytes(const std::vector<u8>& data) {
    if (!data.empty()) bytes_.insert(bytes_.end(), data.begin(), data.end());
    static const u8 padding[3] = {};
    const std::size_t pad = (4 - data.size() % 4) % 4;
    if (pad) bytes_.insert(bytes_.end(), padding, padding + pad);
  }
  // The data-representation record that follows the header of simple messages.
  void ndr() {
    const u8 record[8] = {0, 0, 0, 0, (u8)(be_ ? 0 : 1), 0, 0, 0};
    bytes_.insert(bytes_.end(), record, record + 8);
  }
  // A port right carried in a complex message.
  void port(u32 name, u32 disposition) {
    const u8 tail[4] = {0, 0, (u8)disposition, 0};  // type 0: a port descriptor
    append_descriptor(name, 0, tail);
  }

  // Out-of-line memory carried in a complex message.
  void ool_data(u32 address, u32 size) {
    const u8 tail[4] = {0, 1, 0, 1};  // a virtual copy; type 1: out-of-line data
    append_descriptor(address, size, tail);
  }
  // An out-of-line array of port rights carried in a complex message.
  void ool_ports(u32 address, u32 count) {
    const u8 tail[4] = {0, 0, (u8)kMoveSend, 2};  // type 2: out-of-line ports
    append_descriptor(address, count, tail);
  }

  std::vector<u8> finish() {
    u32 size = (u32)bytes_.size();
    if (be_) size = __builtin_bswap32(size);
    memcpy(bytes_.data() + 4, &size, 4);
    return std::move(bytes_);
  }

 private:
  void append_words(u32* words, std::size_t count) {
    if (be_) {
      for (std::size_t i = 0; i < count; ++i) words[i] = __builtin_bswap32(words[i]);
    }
    const u8* p = (const u8*)words;
    bytes_.insert(bytes_.end(), p, p + count * sizeof(*words));
  }
  void append_descriptor(u32 first, u32 second, const u8 tail[4]) {
    u32 words[2] = {first, second};
    if (be_) {
      words[0] = __builtin_bswap32(words[0]);
      words[1] = __builtin_bswap32(words[1]);
    }
    u8 descriptor[12];
    memcpy(descriptor, words, sizeof words);
    memcpy(descriptor + sizeof words, tail, 4);
    bytes_.insert(bytes_.end(), descriptor, descriptor + sizeof descriptor);
  }

  bool be_;
  std::vector<u8> bytes_;
};
