#pragma once

#include <net/route.h>
#include <sys/socket.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "common.h"

// Leopard's 32-bit rt_msghdr has the same layout as the host's, but its
// PowerPC fields (and the native-order fields of some sockaddrs) differ.
constexpr u8 kRouteGetSilent = 0x11;  // Leopard's route lookup used by scutil.
inline bool swap_route_message(std::vector<u8>& bytes, bool from_guest) {
  constexpr size_t header_size = 92;
  static_assert(sizeof(struct rt_msghdr) == header_size);
  if (bytes.size() < header_size || bytes[2] != RTM_VERSION ||
      !((bytes[3] >= RTM_ADD && bytes[3] <= RTM_RESOLVE) || bytes[3] == kRouteGetSilent))
    return false;
  u16 length;
  u32 addresses;
  memcpy(&length, bytes.data(), sizeof length);
  memcpy(&addresses, bytes.data() + 12, sizeof addresses);
  if (from_guest) {
    length = __builtin_bswap16(length);
    addresses = __builtin_bswap32(addresses);
  }
  if (length != bytes.size() || (addresses & ~((1u << RTAX_MAX) - 1))) return false;

  size_t offset = header_size;
  for (int i = 0; i < RTAX_MAX; i++) {
    if (!(addresses & (1u << i))) continue;
    if (offset >= bytes.size()) return false;
    const size_t len = bytes[offset];
    const size_t rounded = len ? (len + 3) & ~(size_t)3 : 4;
    if (rounded > bytes.size() - offset) return false;
    if (len >= 4 && bytes[offset + 1] == AF_LINK) {
      std::swap(bytes[offset + 2], bytes[offset + 3]);
    } else if (len >= 28 && bytes[offset + 1] == AF_INET6) {
      std::swap(bytes[offset + 24], bytes[offset + 27]);
      std::swap(bytes[offset + 25], bytes[offset + 26]);
    }
    offset += rounded;
  }
  std::swap(bytes[0], bytes[1]);
  std::swap(bytes[4], bytes[5]);
  for (size_t i = 8; i < header_size; i += 4) {
    std::swap(bytes[i], bytes[i + 3]);
    std::swap(bytes[i + 1], bytes[i + 2]);
  }
  return true;
}
