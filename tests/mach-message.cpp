#include "kernel/mach_message.h"

#include <cstdio>
#include <vector>

namespace {

void append_word(std::vector<u8>& bytes, u32 value, bool big_endian) {
  for (int i = 0; i < 4; ++i) {
    const int shift = big_endian ? 24 - i * 8 : i * 8;
    bytes.push_back((u8)(value >> shift));
  }
}

bool check_raw_bytes() {
  for (bool big_endian : {false, true}) {
    for (size_t size = 0; size <= 9; ++size) {
      std::vector<u8> data(size);
      for (size_t i = 0; i < size; ++i) data[i] = (u8)(0xa0 + i);
      const size_t padded_size = (size + 3) & ~size_t(3);

      Message message(big_endian, 0x11223344, 7, false, 24 + padded_size);
      message.put_bytes(data);
      const std::vector<u8> actual = message.finish();

      std::vector<u8> expected;
      append_word(expected, 0x1200, big_endian);
      append_word(expected, (u32)(24 + padded_size), big_endian);
      append_word(expected, 0, big_endian);
      append_word(expected, 0x11223344, big_endian);
      append_word(expected, 0, big_endian);
      append_word(expected, 7, big_endian);
      expected.insert(expected.end(), data.begin(), data.end());
      expected.resize(24 + padded_size, 0);

      if (actual != expected) {
        std::fprintf(stderr, "raw byte reply mismatch: %s-endian, %zu bytes\n",
                     big_endian ? "big" : "little", size);
        return false;
      }
    }
  }
  return true;
}

bool check_word_array() {
  for (bool big_endian : {false, true}) {
    const u32 words[] = {0x11223344, 0x55667788, 0x90abcdef};
    for (size_t count = 0; count <= sizeof words / sizeof words[0]; ++count) {
      Message message(big_endian, 0x1234, 9);
      message.put_words(words, count);
      const std::vector<u8> actual = message.finish();

      std::vector<u8> expected;
      append_word(expected, 0x1200, big_endian);
      append_word(expected, (u32)(24 + count * 4), big_endian);
      append_word(expected, 0, big_endian);
      append_word(expected, 0x1234, big_endian);
      append_word(expected, 0, big_endian);
      append_word(expected, 9, big_endian);
      for (size_t i = 0; i < count; ++i) append_word(expected, words[i], big_endian);
      if (actual != expected) {
        std::fprintf(stderr, "word array mismatch: %s-endian, %zu words\n",
                     big_endian ? "big" : "little", count);
        return false;
      }
    }
  }
  return true;
}

}  // namespace

int main() {
  return check_raw_bytes() && check_word_array() ? 0 : 1;
}
