// Path translation between the guest's view of the file system and the host's.
//
// The guest sees one file system made of two: its system directories come
// from the Mac OS X system root, and everything else (home directories,
// temporary files, whatever folder the program was started from) is the
// host's own. The exception inside a home directory is its Library and its
// dot-files, which the guest system keeps a private set of.

#include <fcntl.h>
#include <pwd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstdlib>

#include "kernel/paths.h"
#include "kernel/process.h"
#include "session/art.h"

namespace {

// Directories that belong to the guest's Mac OS X rather than to this Mac.
const char* const kSystemDirs[] = {
    "/System", "/usr", "/bin", "/sbin", "/Library", "/etc", "/private/etc",
    "/var/db", "/private/var/db", "/var/root", "/private/var/root",
    // Where system daemons keep their sockets. The guest must not reach this
    // Mac's own daemons, which speak newer protocols.
    "/var/run", "/private/var/run",
    // Per-user temporary and cache folders, which the directory helper
    // daemon makes on request. (One that this Mac already has for the user,
    // such as where TMPDIR points, is used as it is: see host_path.)
    "/var/folders", "/private/var/folders",
    "/var/log", "/private/var/log",
};
// Looked up in the system root first, and on the host if not found there, so
// that both the old system's applications and the user's own are reachable.
const char* const kOverlayDirs[] = {"/Applications"};

bool under(const std::string& path, const char* dir) {
  const size_t n = strlen(dir);
  return path.compare(0, n, dir) == 0 && (path.size() == n || path[n] == '/');
}

std::vector<std::string> split(const std::string& path) {
  std::vector<std::string> parts;
  size_t i = 0;
  while (i < path.size()) {
    const size_t j = path.find('/', i);
    const size_t end = j == std::string::npos ? path.size() : j;
    if (end > i) parts.push_back(path.substr(i, end - i));
    i = end + 1;
  }
  return parts;
}

// The guest's user list is the system root's, which has never heard of the
// person running cocaine. Old programs look themselves up in it (to find a
// home or a temporary folder), so they are shown a copy with one more line.
// Returns the copy's path, or an empty string to use the original.
std::string passwd_with_user(const std::string& original) {
  const uid_t uid = getuid();
  const struct passwd* me = getpwuid(uid);
  if (!me || !me->pw_name || uid == 0) return "";
  char tmp[PATH_MAX];
  if (!confstr(_CS_DARWIN_USER_TEMP_DIR, tmp, sizeof tmp)) return "";
  const std::string copy = std::string(tmp) + "cocaine.passwd." + std::to_string(uid);
  struct stat have, from;
  if (stat(original.c_str(), &from) != 0) return "";
  if (stat(copy.c_str(), &have) == 0 && have.st_mtime >= from.st_mtime) return copy;

  std::string text;
  if (FILE* f = fopen(original.c_str(), "r")) {
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    fclose(f);
  }
  const std::string key = std::string(me->pw_name) + ":";
  if (text.compare(0, key.size(), key) != 0 && text.find("\n" + key) == std::string::npos) {
    if (!text.empty() && text.back() != '\n') text += '\n';
    text += key + "*:" + std::to_string(uid) + ":" + std::to_string(me->pw_gid) + ":" +
            (me->pw_gecos ? me->pw_gecos : "") + ":" + (me->pw_dir ? me->pw_dir : "/") +
            ":/bin/bash\n";
  }
  // Written under another name and moved into place, so no process ever
  // reads half of it.
  const std::string fresh = copy + "." + std::to_string(getpid());
  FILE* out = fopen(fresh.c_str(), "w");
  if (!out) return "";
  const bool ok = fwrite(text.data(), 1, text.size(), out) == text.size();
  fclose(out);
  if (!ok || rename(fresh.c_str(), copy.c_str()) != 0) {
    unlink(fresh.c_str());
    return "";
  }
  return copy;
}

// The home folder of the person running cocaine, as this Mac has it (see
// real_home). Looked up once; a test may put another folder in its place.
std::string& home_slot() {
  static std::string home = [] {
    const struct passwd* me = getpwuid(getuid());
    std::string dir = me && me->pw_dir ? me->pw_dir : "";
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    return dir.size() > 1 ? dir : std::string();
  }();
  return home;
}

// A path with its ".", ".." and doubled slashes worked out, without looking
// at the disk.
std::string tidy(const std::string& path) {
  std::vector<std::string> kept;
  for (const std::string& part : split(path)) {
    if (part == ".") continue;
    if (part == "..") {
      if (!kept.empty()) kept.pop_back();
      continue;
    }
    kept.push_back(part);
  }
  std::string out;
  for (const std::string& part : kept) out += "/" + part;
  return out.empty() ? "/" : out;
}

std::string mounted_volume(unsigned long long device) {
  std::vector<struct statfs> mounts;
  const int error = mounted_volumes(mounts, MNT_NOWAIT);
  if (error) {
    log_warn("cannot list mounted volumes: %s", strerror(error));
    return "";
  }
  for (const struct statfs& fs : mounts) {
    struct stat st;
    if (stat(fs.f_mntonname, &st) == 0 && device == (unsigned long long)st.st_dev)
      return fs.f_mntonname;
  }
  return "";
}

}  // namespace

const std::string& real_home() { return home_slot(); }
void set_real_home_for_tests(const std::string& dir) { home_slot() = dir; }

// The part of a home folder the guest system keeps to itself. A home folder
// is this Mac's own, so that an old program can open the documents kept
// there. But its Library is where programs keep their settings, caches and
// keychains, under names that haven't changed in twenty years: an old program
// let loose in the real one reads settings it can't understand and writes
// over the ones a current program of the same name depends on. The same goes
// for the dot-files of shells and tools. So those live in the system root,
// at the same path. Returns where `path` (inside the home folder) really is,
// or an empty string if it is one of the shared parts.
std::string private_home_path(const std::string& root, const std::string& path) {
  const std::string& home = real_home();
  if (home.empty() || path.size() <= home.size() + 1 || !under(path, home.c_str())) return "";
  const std::string rest = path.substr(home.size() + 1);
  const std::string first = rest.substr(0, rest.find('/'));
  if (first != "Library" && !(first.size() > 1 && first[0] == '.')) return "";
  static const bool made = [&] {
    std::string dir = root;
    for (const std::string& part : split(home + "/Library")) mkdir((dir += "/" + part).c_str(), 0755);
    for (const char* sub : {"/Preferences", "/Caches", "/Application Support", "/Logs"}) {
      mkdir((dir + sub).c_str(), 0700);
    }
    return true;
  }();
  (void)made;
  return root + home + "/" + rest;
}

int mounted_volumes(std::vector<struct statfs>& volumes, int flags) {
  int count = getfsstat(nullptr, 0, flags);
  if (count < 0) return errno;
  std::vector<struct statfs> mounts;
  int got;
  do {
    mounts.resize((size_t)count + 16);
    got = getfsstat(mounts.data(), (int)(mounts.size() * sizeof(struct statfs)), flags);
    if (got < 0) return errno;
    count = got;
  } while ((size_t)got == mounts.size());
  volumes.clear();
  for (int i = 0; i < got; i++) {
    const struct statfs& fs = mounts[(size_t)i];
    if (strncmp(fs.f_mntonname, "/Volumes/", 9) == 0 && !(fs.f_flags & MNT_DONTBROWSE))
      volumes.push_back(fs);
  }
  return 0;
}

std::string bundled_wallpaper_path(const std::string& self_path) {
  std::string dir = self_path;
  for (int up = 0; up < 2 && dir.rfind('/') != std::string::npos; up++) dir.erase(dir.rfind('/'));
  return dir + "/images/linen.jpeg";
}

std::string guest_to_host_path(const std::string& root, const std::string& guest, bool follow_last,
                               const std::string& self_path) {
  if (root.empty() || guest.empty()) return guest;
  if (guest == "/.cocaine/linen.jpeg") return bundled_wallpaper_path(self_path);
  if (guest[0] != '/') {
    // A relative path is this Mac's business, except one that leads into
    // the private part of the home folder from the folder above it.
    if (guest.compare(0, 7, "Library") != 0 && guest[0] != '.') return guest;
    if (guest == "." || guest == ".." || guest.compare(0, 2, "./") == 0 || guest.compare(0, 3, "../") == 0) {
      if (guest.find("Library") == std::string::npos) return guest;
    }
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) return guest;
    const std::string kept = private_home_path(root, tidy(host_to_guest_path(root, cwd) + "/" + guest));
    return kept.empty() ? guest : kept;
  }

  std::string path = guest;
  if (under(path, real_home().c_str())) {
    const std::string kept = private_home_path(root, tidy(path));
    if (!kept.empty()) return kept;
  }
  // The guest should keep this Mac's time zone. Its own zone files are in the
  // system root, so point its /etc/localtime at the one with the host's name.
  if (path == "/etc/localtime" || path == "/private/etc/localtime") {
    char zone[PATH_MAX];
    const ssize_t n = readlink("/etc/localtime", zone, sizeof zone - 1);
    if (n > 0) {
      zone[n] = 0;
      if (const char* name = strstr(zone, "zoneinfo/")) {
        const std::string in_root = root + "/usr/share/zoneinfo/" + (name + 9);
        if (access(in_root.c_str(), R_OK) == 0) return in_root;
      }
    }
  }
  // Leopard's art file, with cocaine's logo in the menu bar (session/art.cpp).
  static const std::string kArtName = "/SArtFile.bin";
  if (path.size() > kArtName.size() && path.compare(path.size() - kArtName.size(), kArtName.size(), kArtName) == 0) {
    const std::string t = tidy(path);
    if (t == "/System/Library/PrivateFrameworks/CoreUI.framework/Resources/SArtFile.bin" ||
        t == "/System/Library/PrivateFrameworks/CoreUI.framework/Versions/A/Resources/SArtFile.bin") {
      const std::string made = menu_logo_file(root);
      if (access(made.c_str(), R_OK) == 0) return made;
    }
  }
  // Carbon's file code names a file by volume number, folder number and
  // name: "/.vol/V/F/name". This Mac understands such paths too, but would
  // walk its own folders, not the guest's view of them. So the folder is
  // found first (number 2 is the root), turned back into the path the guest
  // knows it by, and the result goes through the usual translation.
  if (path.compare(0, 6, "/.vol/") == 0) {
    const std::vector<std::string> parts = split(path.substr(6));
    if (parts.size() >= 2) {
      if (parts[1] == "@" || parts[1] == "2") {
        // Carbon uses @ or folder ID 2 for a volume root.
        char* end = nullptr;
        errno = 0;
        const unsigned long long device = strtoull(parts[0].c_str(), &end, 10);
        const bool valid = !parts[0].empty() && parts[0][0] >= '0' && parts[0][0] <= '9' &&
                           !errno && !*end;
        struct stat st;
        if (valid && stat(root.c_str(), &st) == 0 && device == (unsigned long long)st.st_dev) {
          std::string suffix;
          for (size_t i = 2; i < parts.size(); i++) suffix += "/" + parts[i];
          return suffix.empty() ? root : guest_to_host_path(root, suffix, follow_last, self_path);
        }
        if (valid) {
          std::string volume = mounted_volume(device);
          if (!volume.empty()) {
            for (size_t i = 2; i < parts.size(); i++) volume += "/" + parts[i];
            return guest_to_host_path(root, tidy(volume), follow_last, self_path);
          }
        }
        return path;
      }
      std::string folder;
      const int fd = open(("/.vol/" + parts[0] + "/" + parts[1]).c_str(), O_RDONLY);
      char real[PATH_MAX];
      const bool found = fd >= 0 && fcntl(fd, F_GETPATH, real) == 0;
      if (fd >= 0) close(fd);
      if (!found) return path;
      folder = host_to_guest_path(root, real);
      if (folder == "/") folder = "";
      for (size_t i = 2; i < parts.size(); i++) folder += "/" + parts[i];
      return guest_to_host_path(root, folder.empty() ? "/" : folder, follow_last, self_path);
    }
  }
  if (path == "/etc/passwd" || path == "/private/etc/passwd") {
    const std::string copy = passwd_with_user(root + "/private/etc/passwd");
    if (!copy.empty()) return copy;
  }
  for (const char* folders : {"/var/folders", "/private/var/folders"}) {
    if (!under(path, folders)) continue;
    const std::vector<std::string> parts = split(path.substr(strlen(folders)));
    if (parts.size() >= 2 &&
        access(("/private/var/folders/" + parts[0] + "/" + parts[1]).c_str(), F_OK) == 0) {
      return path;
    }
  }
  for (int restarts = 0; restarts < 32; restarts++) {
    bool system = false, overlay = false;
    for (const char* d : kSystemDirs) system |= under(path, d);
    for (const char* d : kOverlayDirs) overlay |= under(path, d);
    if (!system && !overlay) return path;

    // Walk the path inside the system root, following symbolic links the way
    // the guest's own kernel would: an absolute link target means the guest's
    // root, not the host's.
    std::vector<std::string> todo = split(path);
    std::string done;  // resolved so far, guest-absolute; empty means "/"
    bool restarted = false;
    for (size_t i = 0; i < todo.size() && !restarted; i++) {
      const std::string& part = todo[i];
      if (part == ".") continue;
      if (part == "..") {
        const size_t slash = done.rfind('/');
        if (slash != std::string::npos) done.erase(slash);
        continue;
      }
      const std::string next = done + "/" + part;
      const bool last = i + 1 == todo.size();
      struct stat st;
      char target[PATH_MAX];
      ssize_t n;
      if ((!last || follow_last) && lstat((root + next).c_str(), &st) == 0 && S_ISLNK(st.st_mode) &&
          (n = readlink((root + next).c_str(), target, sizeof target - 1)) > 0) {
        target[n] = 0;
        std::string rest;
        for (size_t k = i + 1; k < todo.size(); k++) rest += "/" + todo[k];
        // Splice the target in and start over with the new path.
        path = (target[0] == '/' ? std::string(target) : done + "/" + target) + rest;
        restarted = true;
        break;
      }
      done = next;
    }
    if (restarted) continue;

    const std::string in_root = root + done;
    if (overlay && !system && access(in_root.c_str(), F_OK) != 0) return path;
    return in_root;
  }
  return path;  // a symbolic link loop; let the host report it
}

std::string host_to_guest_path(const std::string& root, const std::string& host) {
  if (!root.empty() && host.compare(0, root.size(), root) == 0 &&
      (host.size() == root.size() || host[root.size()] == '/')) {
    return host.size() == root.size() ? "/" : host.substr(root.size());
  }
  return host;
}

std::string Process::bundled_wallpaper_path() const { return ::bundled_wallpaper_path(opt_.self_path); }

std::string Process::host_path(const std::string& guest, bool follow_last) const {
  return guest_to_host_path(opt_.sysroot, guest, follow_last, opt_.self_path);
}

std::string Process::guest_path(const std::string& host) const {
  return host_to_guest_path(opt_.sysroot, host);
}

// ---- Files picked on this Mac for a guest (the file picker's helper) ----

namespace {

bool same_file(const struct stat& a, const struct stat& b) {
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

// Whether the guest's list of volumes (getfsstat: the system root's volume
// and the browsable ones under /Volumes) holds the one with this number.
// Carbon's file code can't name anything on a volume outside that list.
bool guest_lists_device(const std::string& sysroot, dev_t device) {
  struct stat st;
  if (stat(sysroot.empty() ? "/" : sysroot.c_str(), &st) == 0 && st.st_dev == device) return true;
  std::vector<struct statfs> mounts;
  if (mounted_volumes(mounts, MNT_NOWAIT) != 0) return false;
  for (const struct statfs& fs : mounts) {
    if (stat(fs.f_mntonname, &st) == 0 && st.st_dev == device) return true;
  }
  return false;
}

}  // namespace

PickedPath guest_path_for_pick(const std::string& sysroot, const std::string& picked, bool save,
                               std::string& guest) {
  std::string pick = picked;
  while (pick.size() > 1 && pick.back() == '/') pick.pop_back();
  if (pick.empty() || pick[0] != '/') return PickedPath::Missing;
  char real[PATH_MAX];
  struct stat picked_st, st;
  const bool exists = stat(pick.c_str(), &picked_st) == 0;

  if (!save) {
    // An existing item. The guest is handed the one spelling this Mac's
    // kernel gives back for it (links followed, the stored case and accents),
    // which is also what Carbon's file code will later call it; and only if
    // that path, read the way a guest's path is, leads to the same file. A
    // file reached through a link into the real Library is not one: the
    // guest's path for it lands on the system root's twin.
    if (!exists || !realpath(pick.c_str(), real)) return PickedPath::Missing;
    const std::string candidate = host_to_guest_path(sysroot, real);
    const std::string back = guest_to_host_path(sysroot, candidate);
    if (stat(back.c_str(), &st) != 0 || !same_file(st, picked_st)) return PickedPath::Unreachable;
    if (!guest_lists_device(sysroot, picked_st.st_dev)) return PickedPath::HiddenDisk;
    // Carbon gives "/" as the path of this Mac's top-level folders (/Users,
    // /Volumes, /opt), so an app asked for one of those would open the root.
    if (S_ISDIR(picked_st.st_mode) && candidate.size() > 1 && candidate.rfind('/') == 0 &&
        back == candidate) {
      return PickedPath::TopFolder;
    }
    guest = candidate;
    return PickedPath::Ok;
  }

  // A name to save under: the folder must exist, the name need not.
  const size_t slash = pick.rfind('/');
  const std::string parent = slash == 0 ? "/" : pick.substr(0, slash);
  std::string leaf = pick.substr(slash + 1);
  struct stat parent_st;
  if (leaf.empty() || stat(parent.c_str(), &parent_st) != 0 || !S_ISDIR(parent_st.st_mode) ||
      !realpath(parent.c_str(), real)) {
    return PickedPath::Missing;
  }
  const std::string folder = strcmp(real, "/") == 0 ? "" : real;
  // A file already there keeps the spelling it is stored under, unless the
  // name is a link: saving onto a link must not turn into saving onto what
  // it points at.
  struct stat link_st;
  if (exists && lstat(pick.c_str(), &link_st) == 0 && !S_ISLNK(link_st.st_mode) &&
      realpath(pick.c_str(), real)) {
    const std::string stored = real;
    const size_t at = stored.rfind('/');
    if (stored.compare(0, at, folder) == 0 && at == folder.size()) leaf = stored.substr(at + 1);
  }
  const std::string candidate = host_to_guest_path(sysroot, folder + "/" + leaf);
  // The whole name is translated, not only its folder: a dot-name in the home
  // folder, or a new name in the system root's /Applications, sits in a
  // folder the guest shares with this Mac and still lands somewhere else.
  const std::string back = guest_to_host_path(sysroot, candidate);
  const size_t back_slash = back.rfind('/');
  if (back_slash == std::string::npos) return PickedPath::Unreachable;
  const std::string back_parent = back_slash == 0 ? "/" : back.substr(0, back_slash);
  if (stat(back_parent.c_str(), &st) != 0 || !same_file(st, parent_st)) return PickedPath::Unreachable;
  const bool back_exists = stat(back.c_str(), &st) == 0;
  if (back_exists != exists || (exists && !same_file(st, picked_st))) return PickedPath::Unreachable;
  if (!guest_lists_device(sysroot, parent_st.st_dev)) return PickedPath::HiddenDisk;
  guest = candidate;
  return PickedPath::Ok;
}

const char* picked_path_refusal(PickedPath result) {
  switch (result) {
    case PickedPath::Ok: return "";
    case PickedPath::Missing: return "that file or folder isn't there";
    case PickedPath::Unreachable:
      return "this app can't reach that place - it has its own Library and system folders. "
             "pick something in your home folder or on another disk";
    case PickedPath::HiddenDisk: return "this disk isn't visible to the app";
    case PickedPath::TopFolder:
      return "this app can't open that folder itself - pick a folder inside it";
  }
  return "";
}
