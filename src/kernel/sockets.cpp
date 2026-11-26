// Sockets. The host's socket calls and address structures have the same
// layout the guest uses (addresses are byte strings in network order), so
// these pass through. Two things are translated: paths of Unix-domain
// sockets, and option values, which are integers in the guest's byte order.

#include <netinet/in.h>
#include <libproc.h>
#include <sys/proc_info.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>

#include "kernel/process.h"
#include "kernel/route.h"

namespace {

SysRet result(long r) { return r < 0 ? SysRet::err(errno) : SysRet::ok((u32)r); }

// Options whose value is an address or raw bytes, already in network order,
// rather than a number: these pass through untouched even when they happen
// to be four or eight bytes long (an interface address, a multicast group
// and interface).
bool option_is_bytes(int level, int name) {
  return level == IPPROTO_IP && (name == IP_OPTIONS || name == IP_RETOPTS || name == IP_MULTICAST_IF ||
                                 name == IP_ADD_MEMBERSHIP || name == IP_DROP_MEMBERSHIP);
}

}  // namespace

int Process::route_socket(int fd) const {
  int type = 0;
  socklen_t len = sizeof type;
  if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) != 0 || type != SOCK_RAW) return 0;
  struct socket_fdinfo info = {};
  errno = 0;
  if (proc_pidfdinfo(getpid(), fd, PROC_PIDFDSOCKETINFO, &info, sizeof info) != sizeof info)
    return -(errno ? errno : EIO);
  return info.psi.soi_family == AF_ROUTE ? 1 : 0;
}

SysRet Process::sys_route_io(int fd, u32 buf, u32 len, bool writing) {
  if (!len) return result(writing ? write(fd, mem_.host(buf), 0) : read(fd, mem_.host(buf), 0));
  if (writing) {
    if (len > 65535) return SysRet::err(EMSGSIZE);
    std::vector<u8> message(len);
    if (!mem_.copy_in(message.data(), buf, len)) return SysRet::err(EFAULT);
    if (message.size() < 4 || (message[3] != RTM_GET && message[3] != kRouteGetSilent) ||
        !swap_route_message(message, true)) {
      log_warn("unsupported or malformed PowerPC routing message");
      return SysRet::err(EINVAL);
    }
    return result(write(fd, message.data(), message.size()));
  }
  std::vector<u8> message(65535);
  const ssize_t n = read(fd, message.data(), message.size());
  if (n < 0) return SysRet::err(errno);
  message.resize((size_t)n);
  if (!swap_route_message(message, false)) {
    log_warn("unsupported or malformed host routing message");
    return SysRet::err(EOPNOTSUPP);
  }
  const size_t copied = std::min<size_t>(len, message.size());
  if (!mem_.copy_out(buf, message.data(), copied)) return SysRet::err(EFAULT);
  return SysRet::ok((u32)copied);
}

SysRet Process::sys_socket_call(u32 num, SysArgs& a) {
  // Reads a socket address from the guest. A Unix-domain path goes through
  // path translation, which keeps the guest away from this Mac's own system
  // daemons: their sockets live in directories that belong to the guest's
  // system root, where nobody is listening.
  auto address_in = [&](u32 addr, u32 len, struct sockaddr_storage& out, socklen_t& out_len) -> bool {
    if (len > sizeof out || !mem_.copy_in(&out, addr, len)) return false;
    out_len = len;
    if (len >= 2 && out.ss_family == AF_UNIX) {
      struct sockaddr_un* un = (struct sockaddr_un*)&out;
      const size_t max = len - offsetof(struct sockaddr_un, sun_path);
      const std::string path = host_path(std::string(un->sun_path, strnlen(un->sun_path, max)));
      if (path.size() >= sizeof un->sun_path) return false;
      memset(un->sun_path, 0, sizeof un->sun_path);
      memcpy(un->sun_path, path.c_str(), path.size());
      out_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path.size() + 1);
      un->sun_len = (u8)out_len;
    }
    return true;
  };
  // Writes an address back, with its length through a guest pointer.
  auto address_out = [&](u32 addr, u32 len_ptr, const struct sockaddr_storage& from, socklen_t from_len) {
    if (!addr || !len_ptr || !mem_.range_ok(len_ptr, 4)) return;
    const u32 room = mem_.r32(len_ptr);
    mem_.copy_out(addr, &from, from_len < room ? from_len : room);
    mem_.w32(len_ptr, from_len);
  };

  struct sockaddr_storage ss;
  socklen_t ss_len = sizeof ss;
  switch (num) {
    case 97: {  // socket
      const int domain = a.i(), type = a.i(), protocol = a.i();
      return result(socket(domain, type, protocol));
    }
    case 98:
    case 409:
    case 104: {  // connect, bind
      const int fd = a.i();
      const u32 addr = a.u(), len = a.u();
      if (!address_in(addr, len, ss, ss_len)) return SysRet::err(EINVAL);
      return result(num == 104 ? bind(fd, (struct sockaddr*)&ss, ss_len)
                               : connect(fd, (struct sockaddr*)&ss, ss_len));
    }
    case 106: {  // listen
      const int fd = a.i(), backlog = a.i();
      return result(listen(fd, backlog));
    }
    case 30:
    case 404: {  // accept
      const int fd = a.i();
      const u32 addr = a.u(), len_ptr = a.u();
      const int r = accept(fd, (struct sockaddr*)&ss, &ss_len);
      if (r >= 0) address_out(addr, len_ptr, ss, ss_len);
      return result(r);
    }
    case 31:
    case 32: {  // getpeername, getsockname
      const int fd = a.i();
      const u32 addr = a.u(), len_ptr = a.u();
      const int r = num == 31 ? getpeername(fd, (struct sockaddr*)&ss, &ss_len)
                              : getsockname(fd, (struct sockaddr*)&ss, &ss_len);
      if (r == 0) address_out(addr, len_ptr, ss, ss_len);
      return result(r);
    }
    case 133:
    case 413: {  // sendto
      const int fd = a.i();
      const u32 buf = a.u(), len = a.u();
      const int flags = a.i();
      const u32 to = a.u(), to_len = a.u();
      if (!mem_.range_ok(buf, len)) return SysRet::err(EFAULT);
      if (!to) return result(send(fd, mem_.host(buf), len, flags));
      if (!address_in(to, to_len, ss, ss_len)) return SysRet::err(EINVAL);
      return result(sendto(fd, mem_.host(buf), len, flags, (struct sockaddr*)&ss, ss_len));
    }
    case 29:
    case 403: {  // recvfrom
      const int fd = a.i();
      const u32 buf = a.u(), len = a.u();
      const int flags = a.i();
      const u32 from = a.u(), from_len = a.u();
      if (!mem_.range_ok(buf, len)) return SysRet::err(EFAULT);
      const ssize_t r = recvfrom(fd, mem_.host(buf), len, flags, (struct sockaddr*)&ss, &ss_len);
      if (r >= 0) address_out(from, from_len, ss, ss_len);
      return result(r);
    }
    case 27:
    case 401:
    case 28:
    case 402: {  // recvmsg, sendmsg (descriptor, message, flags)
      // A message here is a record of pointers: an optional address, a list
      // of buffers, and "control" data, which is how one process hands
      // another its open files. Each piece is translated to the host's
      // layout and back.
      const bool sending = num == 28 || num == 402;
      const int fd = a.i();
      const u32 mh = a.u();
      const int flags = a.i();
      if (!mem_.range_ok(mh, 28)) return SysRet::err(EFAULT);
      const u32 name = mem_.r32(mh), name_len = mem_.r32(mh + 4), iov = mem_.r32(mh + 8),
                iov_count = mem_.r32(mh + 12), control = mem_.r32(mh + 16),
                control_len = mem_.r32(mh + 20);
      if (iov_count > 1024 || !mem_.range_ok(iov, 8ull * iov_count) ||
          (control_len && !mem_.range_ok(control, control_len)) || control_len > 65536) {
        return SysRet::err(EFAULT);
      }
      std::vector<struct iovec> vecs(iov_count);
      for (u32 i = 0; i < iov_count; i++) {
        const u32 base = mem_.r32(iov + 8 * i), len = mem_.r32(iov + 8 * i + 4);
        if (len && !mem_.range_ok(base, len)) return SysRet::err(EFAULT);
        vecs[i].iov_base = mem_.host(base);
        vecs[i].iov_len = len;
      }
      struct msghdr h = {};
      struct sockaddr_storage ss;
      socklen_t ss_len = 0;
      if (sending && name && name_len) {
        if (!address_in(name, name_len, ss, ss_len)) return SysRet::err(EFAULT);
        h.msg_name = &ss;
        h.msg_namelen = ss_len;
      } else if (!sending && name) {
        h.msg_name = &ss;
        h.msg_namelen = sizeof ss;
      }
      h.msg_iov = vecs.data();
      h.msg_iovlen = (int)iov_count;
      // Control records have the same shape on both systems (a length, two
      // numbers, then data, in units of four bytes); only the byte order of
      // a PowerPC guest's needs fixing, and passed files are just numbers.
      std::vector<u8> ctl(control_len);
      const bool be = mem_.big_endian();
      auto swap_control = [&](u8* p, size_t n, bool lengths_are_guest) {
        size_t off = 0;
        while (be && off + 12 <= n) {
          u32 len;
          memcpy(&len, p + off, 4);
          const u32 real = lengths_are_guest ? __builtin_bswap32(len) : len;
          for (size_t k = off; k + 4 <= off + real && k + 4 <= n; k += 4) {
            u32 v;
            memcpy(&v, p + k, 4);
            v = __builtin_bswap32(v);
            memcpy(p + k, &v, 4);
          }
          if (real < 12) break;
          off += (real + 3) & ~(size_t)3;
        }
      };
      if (control_len) {
        if (sending) {
          memcpy(ctl.data(), mem_.host(control), control_len);
          swap_control(ctl.data(), control_len, true);
        }
        h.msg_control = ctl.data();
        h.msg_controllen = control_len;
      }
      const ssize_t n = sending ? sendmsg(fd, &h, flags) : recvmsg(fd, &h, flags);
      if (n < 0) return SysRet::err(errno);
      if (!sending) {
        if (control_len) {
          swap_control(ctl.data(), h.msg_controllen, false);
          memcpy(mem_.host(control), ctl.data(), h.msg_controllen);
        }
        mem_.w32(mh + 20, (u32)h.msg_controllen);
        mem_.w32(mh + 24, (u32)h.msg_flags);
        if (name) {
          mem_.w32(mh + 4, name_len);
          address_out(name, mh + 4, ss, h.msg_namelen);
        }
      }
      return SysRet::ok((u32)n);
    }
    case 134: {  // shutdown
      const int fd = a.i(), how = a.i();
      return result(shutdown(fd, how));
    }
    case 135: {  // socketpair
      const int domain = a.i(), type = a.i(), protocol = a.i();
      const u32 out = a.u();
      int fds[2];
      if (!mem_.range_ok(out, 8)) return SysRet::err(EFAULT);
      if (socketpair(domain, type, protocol, fds) != 0) return SysRet::err(errno);
      mem_.w32(out, (u32)fds[0]);
      mem_.w32(out + 4, (u32)fds[1]);
      return SysRet::ok();
    }
    case 105: {  // setsockopt
      const int fd = a.i(), level = a.i(), name = a.i();
      const u32 val = a.u(), len = a.u();
      if (!mem_.range_ok(val, len)) return SysRet::err(EFAULT);
      if (level == SOL_SOCKET && (name == SO_SNDTIMEO || name == SO_RCVTIMEO) && len == 8) {
        const struct timeval tv = {(time_t)mem_.r32(val), (suseconds_t)mem_.r32(val + 4)};
        return result(setsockopt(fd, level, name, &tv, sizeof tv));
      }
      if ((len == 4 || len == 8) && !option_is_bytes(level, name)) {  // one integer, or a pair such as struct linger
        u32 words[2] = {mem_.r32(val), len == 8 ? mem_.r32(val + 4) : 0};
        return result(setsockopt(fd, level, name, words, len));
      }
      return result(setsockopt(fd, level, name, mem_.host(val), len));
    }
    case 118: {  // getsockopt
      const int fd = a.i(), level = a.i(), name = a.i();
      const u32 val = a.u(), len_ptr = a.u();
      if (!mem_.range_ok(len_ptr, 4)) return SysRet::err(EFAULT);
      if (level == SOL_SOCKET && (name == SO_SNDTIMEO || name == SO_RCVTIMEO) && mem_.r32(len_ptr) >= 8) {
        // The host's timeval is sixteen bytes, the guest's eight.
        struct timeval tv = {};
        socklen_t tv_len = sizeof tv;
        if (getsockopt(fd, level, name, &tv, &tv_len) != 0) return SysRet::err(errno);
        if (!mem_.range_ok(val, 8)) return SysRet::err(EFAULT);
        mem_.w32(val, (u32)tv.tv_sec);
        mem_.w32(val + 4, (u32)tv.tv_usec);
        mem_.w32(len_ptr, 8);
        return SysRet::ok();
      }
      u32 words[8] = {};
      socklen_t len = mem_.r32(len_ptr) < sizeof words ? mem_.r32(len_ptr) : sizeof words;
      if (getsockopt(fd, level, name, words, &len) != 0) return SysRet::err(errno);
      if (!mem_.range_ok(val, len)) return SysRet::err(EFAULT);
      if ((len == 4 || len == 8) && !option_is_bytes(level, name)) {
        mem_.w32(val, words[0]);
        if (len == 8) mem_.w32(val + 4, words[1]);
      } else {
        mem_.copy_out(val, words, len);
      }
      mem_.w32(len_ptr, len);
      return SysRet::ok();
    }
  }
  return SysRet::err(ENOSYS);
}
