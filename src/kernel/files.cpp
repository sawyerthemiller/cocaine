// File-system calls that need more than a one-line translation.

#include <sys/acl.h>
#include <sys/attr.h>
#include <sys/kauth.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>

#include "kernel/guest_struct.h"
#include "kernel/process.h"

namespace {

// How one attribute is laid out in an attribute buffer. The host and the
// 32-bit guest agree on everything except times, which are 16 bytes on the
// host and 8 for the guest, and (for PowerPC) byte order.
enum AttrType {
  kU32,      // one 32-bit value
  kU64,      // one 64-bit value
  kTime,     // a timespec
  kRef,      // offset and length of variable-length data placed after the fixed part
  kPair,     // two 32-bit values (a file-system or object identifier)
  kBytes16,  // opaque bytes, copied as they are
  kBytes32,
  kWords8,   // eight 32-bit values (volume capabilities)
  kWords10,  // ten 32-bit values (supported-attribute sets)
};

struct AttrDef {
  u32 bit;
  AttrType type;
};

const AttrDef kCommon[] = {
    {0x00000001, kRef},   {0x00000002, kU32},  {0x00000004, kPair},    {0x00000008, kU32},
    {0x00000010, kU32},   {0x00000020, kPair}, {0x00000040, kPair},    {0x00000080, kPair},
    {0x00000100, kU32},   {0x00000200, kTime}, {0x00000400, kTime},    {0x00000800, kTime},
    {0x00001000, kTime},  {0x00002000, kTime}, {0x00004000, kBytes32}, {0x00008000, kU32},
    {0x00010000, kU32},   {0x00020000, kU32},  {0x00040000, kU32},     {0x00200000, kU32},
    {0x00400000, kRef},   {0x00800000, kBytes16}, {0x01000000, kBytes16}, {0x02000000, kU64},
    {0x04000000, kU64},
};
const AttrDef kVolume[] = {
    {0x00000001, kU32}, {0x00000002, kU32}, {0x00000004, kU64},    {0x00000008, kU64},
    {0x00000010, kU64}, {0x00000020, kU64}, {0x00000040, kU64},    {0x00000080, kU32},
    {0x00000100, kU32}, {0x00000200, kU32}, {0x00000400, kU32},    {0x00000800, kU32},
    {0x00001000, kRef}, {0x00002000, kRef}, {0x00004000, kU32},    {0x00008000, kRef},
    {0x00010000, kU64}, {0x00020000, kWords8}, {0x40000000, kWords10},
};
const AttrDef kDir[] = {{0x1, kU32}, {0x2, kU32}, {0x4, kU32}};
const AttrDef kFile[] = {
    {0x0001, kU32}, {0x0002, kU64}, {0x0004, kU64}, {0x0008, kU32}, {0x0010, kU32},
    {0x0020, kU32}, {0x0040, kU32}, {0x0080, kU32}, {0x0200, kU64}, {0x0400, kU64},
    {0x1000, kU64}, {0x2000, kU64},
};

u32 host_size(AttrType t) {
  switch (t) {
    case kU32: return 4;
    case kU64: return 8;
    case kTime: return 16;
    case kRef: return 8;
    case kPair: return 8;
    case kBytes16: return 16;
    case kBytes32: return 32;
    case kWords8: return 32;
    default: return 40;
  }
}

template <size_t N>
bool known(u32 wanted, const AttrDef (&defs)[N]) {
  for (const AttrDef& d : defs) wanted &= ~d.bit;
  return wanted == 0;
}

// A file security record is how getattrlist and setattrlist carry an access
// control list: a magic number, the owner's and group's identifiers, the
// number of entries (-1 when there is no list at all), flags, and the
// entries, each an identifier and two words. Host and guest lay it out the
// same way; the numbers are in each side's byte order and the identifiers
// are plain bytes. This turns the numbers of a record with `entries` entries
// from one byte order into the other.
void flip_filesec_numbers(u8* record, u32 entries) {
  const auto flip = [&](size_t at) {
    u32 value;
    memcpy(&value, record + at, 4);
    value = __builtin_bswap32(value);
    memcpy(record + at, &value, 4);
  };
  flip(offsetof(struct kauth_filesec, fsec_magic));
  flip(offsetof(struct kauth_filesec, fsec_acl.acl_entrycount));
  flip(offsetof(struct kauth_filesec, fsec_acl.acl_flags));
  for (u32 i = 0; i < entries; i++) {
    const size_t entry = KAUTH_FILESEC_SIZE(i);
    flip(entry + offsetof(struct kauth_ace, ace_flags));
    flip(entry + offsetof(struct kauth_ace, ace_rights));
  }
}

struct AppleDouble {
  int fd = -1;
  std::array<u8, 32> finder = {};
  bool has_finder = false, has_fork = false;
  u32 fork_offset = 0, fork_size = 0;
  ~AppleDouble() { if (fd >= 0) close(fd); }
};

u32 be32(const u8* b) {
  return (u32)b[0] << 24 | (u32)b[1] << 16 | (u32)b[2] << 8 | b[3];
}

int read_appledouble(const std::string& host, AppleDouble& info) {
  const size_t slash = host.rfind('/');
  if (slash == std::string::npos) return ENOENT;
  const std::string side = host.substr(0, slash + 1) + "._" + host.substr(slash + 1);
  info.fd = open(side.c_str(), O_RDONLY);
  if (info.fd < 0) return errno;
  struct stat st;
  if (fstat(info.fd, &st) != 0) return errno;
  u8 header[26];
  if (pread(info.fd, header, sizeof header, 0) != sizeof header ||
      be32(header) != 0x00051607 || be32(header + 4) != 0x00020000) return EINVAL;
  const u32 count = (u32)header[24] << 8 | header[25];
  const u64 data_start = 26 + (u64)count * 12;
  if (st.st_size < 0 || data_start > (u64)st.st_size) return EINVAL;
  std::vector<std::pair<u32, u32>> ranges;
  for (u32 i = 0; i < count; i++) {
    u8 entry[12];
    if (pread(info.fd, entry, sizeof entry, 26 + (off_t)i * 12) != sizeof entry) return EIO;
    const u32 kind = be32(entry), offset = be32(entry + 4), length = be32(entry + 8);
    if ((length && offset < data_start) || (u64)offset + length > (u64)st.st_size) return EINVAL;
    if (length) ranges.emplace_back(offset, length);
    if (kind == 9) {
      if (info.has_finder || length < info.finder.size()) return EINVAL;
      if (pread(info.fd, info.finder.data(), info.finder.size(), offset) != (ssize_t)info.finder.size())
        return EIO;
      info.has_finder = true;
    } else if (kind == 2) {
      if (info.has_fork) return EINVAL;
      info.has_fork = true;
      info.fork_offset = offset;
      info.fork_size = length;
    }
  }
  std::sort(ranges.begin(), ranges.end());
  for (size_t i = 1; i < ranges.size(); i++)
    if ((u64)ranges[i - 1].first + ranges[i - 1].second > ranges[i].first) return EINVAL;
  return 0;
}

bool in_system_root(const std::string& host, const std::string& root) {
  return !root.empty() && host.size() > root.size() && host.compare(0, root.size(), root) == 0 &&
         host[root.size()] == '/';
}

bool system_root_file(const std::string& host, const std::string& root) {
  if (!in_system_root(host, root)) return false;
  char resolved[PATH_MAX];
  return realpath(host.c_str(), resolved) && in_system_root(resolved, root);
}

}  // namespace

int Process::open_resolver_config(const std::string& host, int flags) {
  if (opt_.sysroot.empty() || host != opt_.sysroot + "/private/var/run/resolv.conf" ||
      (flags & ~O_CLOEXEC) != O_RDONLY) {
    errno = ENOENT;
    return -1;
  }
  // A dangling guest symlink is an explicit configuration, not a missing file.
  struct stat st;
  if (lstat(host.c_str(), &st) == 0) {
    errno = ENOENT;
    return -1;
  }
  if (errno != ENOENT) return -1;
  int source = open("/var/run/resolv.conf", O_RDONLY);
  if (source < 0) {
    log_warn("cannot open host resolver configuration: %s", strerror(errno));
    return -1;
  }
  char tmp[PATH_MAX];
  if (!confstr(_CS_DARWIN_USER_TEMP_DIR, tmp, sizeof tmp)) {
    close(source);
    errno = EIO;
    return -1;
  }
  std::string name = std::string(tmp) + "cocaine-resolv-XXXXXX";
  int fd = mkstemp(name.data());
  if (fd < 0) {
    const int err = errno;
    close(source);
    errno = err;
    return -1;
  }
  auto fail = [&](int err) {
    close(source);
    close(fd);
    if (unlink(name.c_str()) != 0)
      log_warn("cannot remove temporary resolver configuration %s: %s", name.c_str(), strerror(errno));
    errno = err;
    return -1;
  };
  std::array<char, 4096> data;
  for (;;) {
    const ssize_t n = read(source, data.data(), data.size());
    if (n < 0) return fail(errno);
    if (n == 0) break;
    size_t written = 0;
    while (written < (size_t)n) {
      const ssize_t w = write(fd, data.data() + written, (size_t)n - written);
      if (w <= 0) return fail(w < 0 ? errno : EIO);
      written += (size_t)w;
    }
  }
  int reader = open(name.c_str(), O_RDONLY | (flags & O_CLOEXEC));
  if (reader < 0) return fail(errno);
  struct stat written, opened;
  if (fstat(fd, &written) != 0 || fstat(reader, &opened) != 0) {
    const int err = errno; close(reader); return fail(err);
  }
  if (written.st_dev != opened.st_dev || written.st_ino != opened.st_ino) {
    close(reader); return fail(EIO);
  }
  if (unlink(name.c_str()) != 0) {
    const int err = errno; close(reader); close(source); close(fd); errno = err; return -1;
  }
  close(source);
  close(fd);
  return reader;
}

int Process::open_appledouble_fork(const std::string& host, int flags) {
  static const std::string suffix = "/..namedfork/rsrc";
  if (!in_system_root(host, opt_.sysroot) || host.size() <= suffix.size() ||
      host.compare(host.size() - suffix.size(), suffix.size(), suffix) != 0 ||
      (flags & ~O_CLOEXEC) != O_RDONLY) {
    errno = ENOENT;
    return -1;
  }
  AppleDouble info;
  const std::string base = host.substr(0, host.size() - suffix.size());
  struct stat st;
  if (!system_root_file(base, opt_.sysroot) || stat(base.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
    errno = ENOENT;
    return -1;
  }
  if (getxattr(base.c_str(), XATTR_RESOURCEFORK_NAME, nullptr, 0, 0, 0) >= 0) {
    errno = ENOENT;
    return -1;
  }
  if (errno != ENOATTR && errno != ENOTSUP) return -1;
  const int e = read_appledouble(base, info);
  if (e || !info.has_fork) {
    const int err = e ? e : ENOENT;
    if (err != ENOENT) log_warn("cannot read AppleDouble metadata for %s: %s", base.c_str(), strerror(err));
    errno = err;
    return -1;
  }
  char tmp[PATH_MAX];
  if (!confstr(_CS_DARWIN_USER_TEMP_DIR, tmp, sizeof tmp)) { errno = EIO; return -1; }
  std::string name = std::string(tmp) + "cocaine-fork-XXXXXX";
  int fd = mkstemp(name.data());
  if (fd < 0) return -1;
  auto fail = [&](int err) {
    close(fd);
    if (unlink(name.c_str()) != 0) log_warn("cannot remove temporary resource fork %s: %s", name.c_str(), strerror(errno));
    errno = err;
    return -1;
  };
  std::array<u8, 16384> data;
  u32 done = 0;
  while (done < info.fork_size) {
    const size_t size = std::min<size_t>(data.size(), info.fork_size - done);
    const ssize_t n = pread(info.fd, data.data(), size, (off_t)info.fork_offset + done);
    if (n <= 0) return fail(n < 0 ? errno : EIO);
    size_t written = 0;
    while (written < (size_t)n) {
      const ssize_t w = write(fd, data.data() + written, (size_t)n - written);
      if (w <= 0) return fail(w < 0 ? errno : EIO);
      written += (size_t)w;
    }
    done += (u32)n;
  }
  int reader = open(name.c_str(), O_RDONLY | (flags & O_CLOEXEC));
  if (reader < 0) return fail(errno);
  struct stat written, opened;
  if (fstat(fd, &written) != 0 || fstat(reader, &opened) != 0) {
    const int err = errno; close(reader); return fail(err);
  }
  if (written.st_dev != opened.st_dev || written.st_ino != opened.st_ino) {
    close(reader); return fail(EIO);
  }
  if (unlink(name.c_str()) != 0) {
    const int err = errno; close(reader); close(fd); errno = err; return -1;
  }
  close(fd);
  return reader;
}

// access_extended: several permission questions in one call, about one file
// or more. Carbon asks this way before it replaces a file (FSReplaceObject,
// behind every Cocoa Save onto an existing document: "may the original be
// written?"). The request is an array of 16-byte entries (name offset, flags,
// two unused words) followed by the names. An offset of zero means "the file
// of the entry before", and the lowest name offset is where the array ends.
// Every entry is answered with an error number of its own; the call itself
// fails only for a malformed request or a name that can't be looked up at
// all, and then writes no answers. The flags are access()'s three, or a set of
// finer rights ("delete", say, which is up to the folder holding the file).
// This Mac has the same call with the same flags, so each file's questions
// go to its kernel, with the name translated; the checks on the request are
// Leopard's own, since translating moves the names about.
SysRet Process::sys_access_extended(u32 entries, u32 size, u32 results) {
  const u32 kEntry = sizeof(struct accessx_descriptor);  // the same 16 bytes for the guest
  if (size > ACCESSX_MAX_TABLESIZE) return SysRet::err(ENOMEM);
  if (size < kEntry + 2) return SysRet::err(EINVAL);  // one entry and a one-letter name
  if (!mem_.range_ok(entries, size)) return SysRet::err(EFAULT);
  // The kernel ends the request with a zero byte of its own, so that a name
  // can't run off the end.
  std::string request((const char*)mem_.host(entries), size);
  request[size - 1] = 0;
  const auto offset = [&](u32 i) { return mem_.r32(entries + i * kEntry); };
  const auto flags = [&](u32 i) { return mem_.r32(entries + i * kEntry + 4); };

  const u32 most = (size - 2) / kEntry;
  u32 count = most;
  for (u32 i = 0; i < count; i++) {
    // A name offset, counted in entries: the array can't reach past it.
    const u32 end = offset(i) / kEntry;
    if (end > most || offset(i) >= size || (end != 0 && end <= i)) return SysRet::err(EINVAL);
    if (end == 0) {
      if (i == 0) return SysRet::err(EINVAL);  // the first entry has no file before it
      continue;
    }
    if (end < count) count = end;
  }
  if (count > ACCESSX_MAX_DESCRIPTORS) return SysRet::err(ENOMEM);

  std::vector<u32> answers(count);
  for (u32 first = 0, n; first < count; first += n) {
    // One named entry and the entries after it that mean the same file.
    for (n = 1; first + n < count && offset(first + n) == 0; n++) {}
    const std::string host = host_path(request.c_str() + offset(first));
    // (Two bytes past the name: the host wants room for a name of one letter
    // even when this one is empty, which it answers as "no such file".)
    std::vector<u8> ask(n * kEntry + host.size() + 2);
    if (ask.size() > ACCESSX_MAX_TABLESIZE) return SysRet::err(ENAMETOOLONG);
    auto* table = (struct accessx_descriptor*)ask.data();
    for (u32 i = 0; i < n; i++) table[i].ad_flags = (int)flags(first + i);
    table[0].ad_name_offset = n * kEntry;
    memcpy(ask.data() + n * kEntry, host.c_str(), host.size());
    // Leopard's kernel ignores the user argument and answers for the caller.
    std::vector<int> errors(n);
    if (accessx_np(table, ask.size(), errors.data(), getuid()) != 0) return SysRet::err(errno);
    for (u32 i = 0; i < n; i++) answers[first + i] = (u32)errors[i];
  }
  if (!mem_.range_ok(results, count * 4)) return SysRet::err(EFAULT);
  for (u32 i = 0; i < count; i++) mem_.w32(results + i * 4, answers[i]);
  return SysRet::ok();
}

SysRet Process::sys_setattrlist(const std::string& path, u32 list, u32 buf, u32 size, u32 options) {
  if (!mem_.range_ok(list, 24) || !mem_.range_ok(buf, size)) return SysRet::err(EFAULT);
  struct attrlist al = {};
  al.bitmapcount = mem_.r16(list);
  al.commonattr = mem_.r32(list + 4);
  al.volattr = mem_.r32(list + 8);
  al.dirattr = mem_.r32(list + 12);
  al.fileattr = mem_.r32(list + 16);
  al.forkattr = mem_.r32(list + 20);
  if (al.bitmapcount != 5) return SysRet::err(EINVAL);
  const u32 supported = ATTR_CMN_SCRIPT | ATTR_CMN_CRTIME | ATTR_CMN_MODTIME |
                        ATTR_CMN_CHGTIME | ATTR_CMN_ACCTIME | ATTR_CMN_BKUPTIME |
                        ATTR_CMN_FNDRINFO | ATTR_CMN_OWNERID | ATTR_CMN_GRPID |
                        ATTR_CMN_ACCESSMASK | ATTR_CMN_FLAGS | ATTR_CMN_EXTENDED_SECURITY |
                        ATTR_CMN_UUID | ATTR_CMN_GRPUUID;
  if ((al.commonattr & ~supported) || al.volattr || al.dirattr || al.fileattr || al.forkattr) {
    log_warn("setattrlist asked for attributes that aren't translated yet "
             "(common %x volume %x dir %x file %x fork %x)",
             al.commonattr, al.volattr, al.dirattr, al.fileattr, al.forkattr);
    return SysRet::err(ENOTSUP);
  }
  std::vector<u8> data;
  // An access control list isn't in the fixed part: a reference there says
  // where its file security record is, counted from the reference, and how
  // long. Carbon sends one whenever it replaces a file (every Cocoa Save
  // onto an existing document), to give the new file the old one's list;
  // for a file that had none, the record says "no list".
  std::vector<u8> acl;
  size_t acl_ref = 0;  // where the reference goes in `data`
  u32 at = 0;
  for (const AttrDef& d : kCommon) {
    if (!(al.commonattr & d.bit)) continue;
    const u32 guest_size = d.type == kTime ? 8 : host_size(d.type);
    if (guest_size > size - at) return SysRet::err(EINVAL);
    const size_t start = data.size();
    data.resize(start + host_size(d.type));
    if (d.bit == ATTR_CMN_EXTENDED_SECURITY) {
      const s32 offset = (s32)mem_.r32(buf + at);
      const u32 length = mem_.r32(buf + at + 4);
      const u32 room = size - at;  // from the reference to the end of the buffer
      // The kernel's own checks: a whole record inside the buffer, the magic
      // number, and a length that agrees with the number of entries.
      if (offset < 0 || (u32)offset > room || room - (u32)offset < KAUTH_FILESEC_SIZE(0)) {
        return SysRet::err(EINVAL);
      }
      const u32 record = buf + at + (u32)offset;
      const u32 kCount = offsetof(struct kauth_filesec, fsec_acl.acl_entrycount);
      const u32 listed = mem_.r32(record + kCount);
      const u32 entries = listed == KAUTH_FILESEC_NOACL ? 0 : listed;
      if (mem_.r32(record) != KAUTH_FILESEC_MAGIC || entries > KAUTH_ACL_MAX_ENTRIES ||
          length != KAUTH_FILESEC_SIZE(entries) || length > room - (u32)offset) {
        return SysRet::err(EINVAL);
      }
      acl.assign(mem_.host(record), mem_.host(record) + length);
      if (mem_.big_endian()) flip_filesec_numbers(acl.data(), entries);
      acl_ref = start;
    } else if (d.type == kTime) {
      const s64 sec = (s32)mem_.r32(buf + at);
      const s64 nsec = (s32)mem_.r32(buf + at + 4);
      memcpy(data.data() + start, &sec, 8);
      memcpy(data.data() + start + 8, &nsec, 8);
    } else if (d.type == kU32) {
      const u32 value = mem_.r32(buf + at);
      memcpy(data.data() + start, &value, 4);
    } else {
      memcpy(data.data() + start, mem_.host(buf + at), guest_size);
    }
    at += guest_size;
  }
  if (!acl.empty()) {
    const attrreference_t ref = {(int32_t)(data.size() - acl_ref), (u_int32_t)acl.size()};
    memcpy(data.data() + acl_ref, &ref, sizeof ref);
    data.insert(data.end(), acl.begin(), acl.end());
  }
  const std::string host = host_path(path, !(options & 1));
  if (setattrlist(host.c_str(), &al, data.data(), data.size(), options) != 0) return SysRet::err(errno);
  return SysRet::ok();
}

// getattrlist: asks for an arbitrary set of a file's attributes in one call.
// Carbon's File Manager is built on it. The host has the same call, so the
// request goes straight through and the answer is re-packed for the guest.
SysRet Process::sys_getattrlist(const std::string& path, u32 list, u32 buf, u32 size, u32 options) {
  if (!mem_.range_ok(list, 24) || !mem_.range_ok(buf, size)) return SysRet::err(EFAULT);
  struct attrlist al = {};
  al.bitmapcount = mem_.r16(list);
  al.commonattr = mem_.r32(list + 4);
  al.volattr = mem_.r32(list + 8);
  al.dirattr = mem_.r32(list + 12);
  al.fileattr = mem_.r32(list + 16);
  al.forkattr = mem_.r32(list + 20);
  if (al.bitmapcount != 5) return SysRet::err(EINVAL);
  if (al.forkattr || !known(al.commonattr, kCommon) || !known(al.volattr & ~0x80000000u, kVolume) ||
      !known(al.dirattr, kDir) || !known(al.fileattr, kFile)) {
    log_warn("getattrlist asked for attributes that aren't translated yet "
             "(common %x volume %x dir %x file %x fork %x)",
             al.commonattr, al.volattr, al.dirattr, al.fileattr, al.forkattr);
    return SysRet::err(EINVAL);
  }

  const bool follow = !(options & 1);  // FSOPT_NOFOLLOW
  const std::string host = host_path(path, follow);
  std::vector<u8> in(16384);
  if (getattrlist(host.c_str(), &al, in.data(), in.size(), options) != 0) return SysRet::err(errno);
  // Carbon walks IDs through /.vol; the imported root is volume root ID 2.
  const bool imported_root = !opt_.sysroot.empty() && host == opt_.sysroot;
  const bool root_child = !opt_.sysroot.empty() &&
                          host.rfind('/') == opt_.sysroot.size() &&
                          host.compare(0, opt_.sysroot.size(), opt_.sysroot) == 0;
  if ((imported_root || root_child) &&
      (al.commonattr & (ATTR_CMN_OBJID | ATTR_CMN_PAROBJID))) {
    size_t at = 4;
    for (const AttrDef& d : kCommon) {
      if (!(al.commonattr & d.bit)) continue;
      if ((imported_root && d.bit == ATTR_CMN_OBJID) ||
          d.bit == ATTR_CMN_PAROBJID) {
        const u32 root_id[2] = {2, 0};
        memcpy(in.data() + at, root_id, sizeof root_id);
      }
      at += host_size(d.type);
    }
  }
  struct stat st;
  const bool have_stat = (follow ? stat(host.c_str(), &st) : lstat(host.c_str(), &st)) == 0;
  const bool is_dir = have_stat && S_ISDIR(st.st_mode);
  bool root_entry = !opt_.sysroot.empty() && (path == "/" || host == opt_.sysroot);
  if (!opt_.sysroot.empty() && !root_entry && have_stat &&
      ((al.commonattr & ATTR_CMN_NAME) || (al.volattr & ATTR_VOL_INFO))) {
    struct stat slash;
    if (stat("/", &slash) != 0) return SysRet::err(errno);
    root_entry = st.st_dev == slash.st_dev && st.st_ino == slash.st_ino;
  }
  bool root_volume = root_entry;
  if (!opt_.sysroot.empty() && !root_volume && (al.volattr & ATTR_VOL_INFO)) {
    if (!have_stat) return SysRet::err(errno);
    struct stat root_stat;
    if (stat(opt_.sysroot.c_str(), &root_stat) != 0) return SysRet::err(errno);
    root_volume = st.st_dev == root_stat.st_dev;
  }
  if (have_stat && S_ISREG(st.st_mode) && system_root_file(host, opt_.sysroot) &&
      ((al.commonattr & ATTR_CMN_FNDRINFO) || (al.fileattr & (ATTR_FILE_RSRCLENGTH | ATTR_FILE_RSRCALLOCSIZE)))) {
    bool finder_missing = false, fork_missing = false;
    if (al.commonattr & ATTR_CMN_FNDRINFO) {
      if (getxattr(host.c_str(), XATTR_FINDERINFO_NAME, nullptr, 0, 0, 0) < 0) {
        if (errno != ENOATTR && errno != ENOTSUP) return SysRet::err(errno);
        finder_missing = true;
      }
    }
    if (al.fileattr & (ATTR_FILE_RSRCLENGTH | ATTR_FILE_RSRCALLOCSIZE)) {
      if (getxattr(host.c_str(), XATTR_RESOURCEFORK_NAME, nullptr, 0, 0, 0) < 0) {
        if (errno != ENOATTR && errno != ENOTSUP) return SysRet::err(errno);
        fork_missing = true;
      }
    }
    if (finder_missing || fork_missing) {
      AppleDouble info;
      const int error = read_appledouble(host, info);
      if (error && error != ENOENT) {
        log_warn("cannot read AppleDouble metadata for %s: %s", host.c_str(), strerror(error));
        return SysRet::err(error);
      }
      if (!error) {
        size_t at = 4;
        for (const AttrDef& d : kCommon) {
          if (!(al.commonattr & d.bit)) continue;
          if (d.bit == ATTR_CMN_FNDRINFO && finder_missing && info.has_finder)
            memcpy(in.data() + at, info.finder.data(), info.finder.size());
          at += host_size(d.type);
        }
        for (const AttrDef& d : kVolume) if (al.volattr & d.bit) at += host_size(d.type);
        for (const AttrDef& d : kFile) {
          if (!(al.fileattr & d.bit)) continue;
          if ((d.bit == ATTR_FILE_RSRCLENGTH || d.bit == ATTR_FILE_RSRCALLOCSIZE) &&
              fork_missing && info.has_fork) {
            const u64 length = info.fork_size;
            memcpy(in.data() + at, &length, sizeof length);
          }
          at += host_size(d.type);
        }
      }
    }
  }

  // The attributes present, in the order both sides lay them out. Directory
  // attributes come back only for directories, file attributes only for files.
  struct Field {
    AttrType type;
    bool root_name;
  };
  std::vector<Field> fields;
  size_t acl_field = SIZE_MAX;  // which of them is the access control list, if asked for
  for (const AttrDef& d : kCommon) {
    if (!(al.commonattr & d.bit)) continue;
    if (d.bit == ATTR_CMN_EXTENDED_SECURITY) acl_field = fields.size();
    fields.push_back({d.type, d.bit == ATTR_CMN_NAME &&
                               ((al.volattr & ATTR_VOL_INFO) ? root_volume : root_entry)});
  }
  for (const AttrDef& d : kVolume) {
    if (al.volattr & d.bit) fields.push_back({d.type, d.bit == ATTR_VOL_NAME && root_volume});
  }
  if (is_dir) {
    for (const AttrDef& d : kDir) if (al.dirattr & d.bit) fields.push_back({d.type, false});
  } else {
    for (const AttrDef& d : kFile) if (al.fileattr & d.bit) fields.push_back({d.type, false});
  }

  const bool be = mem_.big_endian();
  std::vector<u8> out(4);  // the leading length word, filled in last
  auto put32 = [&](std::vector<u8>& v, size_t at, u32 x) {
    if (be) x = __builtin_bswap32(x);
    if (v.size() < at + 4) v.resize(at + 4);
    memcpy(v.data() + at, &x, 4);
  };
  auto get32 = [&](size_t at) -> u32 {
    u32 x = 0;
    if (at + 4 <= in.size()) memcpy(&x, in.data() + at, 4);
    return x;
  };

  struct Pending {
    size_t guest_ref;  // where the guest's offset/length pair sits
    size_t host_data;  // where the host put the data
    u32 length;
    bool root_name;
  };
  std::vector<Pending> pending;
  size_t acl_ref = SIZE_MAX;  // the access control list's reference, in `out`
  size_t hp = 4, index = 0;
  for (const Field& field : fields) {
    const AttrType t = field.type;
    const size_t gp = out.size();
    if (index++ == acl_field) acl_ref = gp;
    switch (t) {
      case kU32:
        put32(out, gp, get32(hp));
        break;
      case kU64:
      case kPair: {
        // A 64-bit value keeps its halves in the guest's order; a pair is
        // just two separate words.
        const u32 a = get32(hp), b = get32(hp + 4);
        const bool swap_halves = t == kU64 && be;
        put32(out, gp, swap_halves ? b : a);
        put32(out, gp + 4, swap_halves ? a : b);
        break;
      }
      case kTime: {
        s64 sec, nsec;
        memcpy(&sec, in.data() + hp, 8);
        memcpy(&nsec, in.data() + hp + 8, 8);
        put32(out, gp, (u32)sec);
        put32(out, gp + 4, (u32)nsec);
        break;
      }
      case kRef: {
        const s32 offset = (s32)get32(hp);
        pending.push_back({gp, (size_t)((s64)hp + offset), get32(hp + 4), field.root_name});
        put32(out, gp, 0);
        put32(out, gp + 4, 0);
        break;
      }
      case kBytes16:
      case kBytes32:
        out.insert(out.end(), in.begin() + (long)hp, in.begin() + (long)hp + host_size(t));
        break;
      case kWords8:
      case kWords10:
        for (u32 i = 0; i < host_size(t); i += 4) put32(out, gp + i, get32(hp + i));
        break;
    }
    hp += host_size(t);
  }
  // Variable-length data follows the fixed part, each piece padded to 4 bytes.
  for (const Pending& p : pending) {
    const size_t at = out.size();
    constexpr char kRootName[] = "Macintosh HD";
    const u32 length = p.root_name ? sizeof kRootName : p.length;
    if (p.root_name) {
      out.insert(out.end(), kRootName, kRootName + sizeof kRootName);
    } else if (p.host_data + p.length <= in.size()) {
      out.insert(out.end(), in.begin() + (long)p.host_data, in.begin() + (long)(p.host_data + p.length));
      // A PowerPC guest reads the list's numbers the other way round.
      if (be && p.guest_ref == acl_ref && p.length >= KAUTH_FILESEC_SIZE(0)) {
        u32 entries = get32(p.host_data + offsetof(struct kauth_filesec, fsec_acl.acl_entrycount));
        if (entries == KAUTH_FILESEC_NOACL) entries = 0;
        if (p.length >= KAUTH_FILESEC_SIZE((u64)entries)) flip_filesec_numbers(out.data() + at, entries);
      }
    }
    while (out.size() % 4) out.push_back(0);
    put32(out, p.guest_ref, (u32)(at - p.guest_ref));
    put32(out, p.guest_ref + 4, length);
  }
  put32(out, 0, (u32)out.size());
  memcpy(mem_.host(buf), out.data(), out.size() < size ? out.size() : size);
  return SysRet::ok();
}
