#include "viewer/clipboard.h"

#import <ImageIO/ImageIO.h>

#include <copyfile.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits.h>
#include <memory>
#include <vector>

extern char** environ;

namespace {
const char kGuestScript[] =
#include "clipboard_guest.inc"
;

bool ready(int fd, short events, std::chrono::steady_clock::time_point deadline) {
  for (;;) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    if (left <= 0) {
      errno = ETIMEDOUT;
      return false;
    }
    struct pollfd p = {fd, events, 0};
    const int n = poll(&p, 1, (int)left);
    if (n < 0 && errno == EINTR) continue;
    if (n == 0) errno = ETIMEDOUT;
    if (n <= 0) return false;
    if (p.revents & events) return true;
    errno = EPIPE;
    return false;
  }
}

NSDictionary* decode_plist(const std::string& reply, char prefix) {
  if (reply.empty() || reply[0] != prefix) return nil;
  NSString* encoded = [[NSString alloc] initWithBytes:reply.data() + 1
                                              length:reply.size() - 1
                                            encoding:NSASCIIStringEncoding];
  if (!encoded) return nil;
  NSData* data = [[NSData alloc] initWithBase64EncodedString:encoded options:0];
  if (!data) return nil;
  id plist = [NSPropertyListSerialization propertyListWithData:data options:NSPropertyListImmutable
                                                        format:nil error:nil];
  return [plist isKindOfClass:NSDictionary.class] ? plist : nil;
}

std::string encode_data(NSData* data) {
  return std::string([data base64EncodedStringWithOptions:0].UTF8String);
}

bool same_file(NSString* path, NSDictionary* identity) {
  NSString* device = identity[@"dev"];
  NSString* inode = identity[@"ino"];
  if (![device isKindOfClass:NSString.class] || ![inode isKindOfClass:NSString.class]) return false;
  NSScanner* devScanner = [NSScanner scannerWithString:device];
  NSScanner* inoScanner = [NSScanner scannerWithString:inode];
  unsigned long long dev, ino;
  if (![devScanner scanUnsignedLongLong:&dev] || !devScanner.isAtEnd || dev > UINT32_MAX ||
      ![inoScanner scanUnsignedLongLong:&ino] || !inoScanner.isAtEnd) return false;
  struct stat st;
  return stat(path.fileSystemRepresentation, &st) == 0 &&
         (uint32_t)st.st_dev == dev && st.st_ino == ino;
}

bool beneath(NSString* path, NSString* directory) {
  return [path isEqualToString:directory] ||
         [path hasPrefix:[directory stringByAppendingString:@"/"]];
}

bool safe_staged_link(const std::string& target) {
  if (target.empty() || target[0] == '/') return false;
  size_t start = 0;
  while (start < target.size()) {
    size_t end = target.find('/', start);
    if (end == std::string::npos) end = target.size();
    const std::string part = target.substr(start, end - start);
    if (part == "..") return false;
    start = end + 1;
  }
  return true;
}

bool copy_staged(int source, int destination, unsigned depth) {
  if (depth > 256) {
    fprintf(stderr, "cocaine: clipboard folder nesting exceeds 256 levels\n");
    return false;
  }
  struct stat st;
  if (fstat(source, &st) != 0) {
    fprintf(stderr, "cocaine: cannot inspect clipboard item: %s\n", strerror(errno));
    return false;
  }
  if (S_ISREG(st.st_mode)) {
    if (fcopyfile(source, destination, nullptr, COPYFILE_DATA | COPYFILE_XATTR | COPYFILE_STAT) == 0)
      return true;
    fprintf(stderr, "cocaine: cannot copy clipboard file: %s\n", strerror(errno));
    return false;
  }
  if (!S_ISDIR(st.st_mode)) {
    fprintf(stderr, "cocaine: cannot stage clipboard symbolic links or special files\n");
    return false;
  }
  const int dupfd = dup(source);
  if (dupfd < 0) {
    fprintf(stderr, "cocaine: cannot inspect clipboard folder: %s\n", strerror(errno));
    return false;
  }
  DIR* dir = fdopendir(dupfd);
  if (!dir) {
    fprintf(stderr, "cocaine: cannot inspect clipboard folder: %s\n", strerror(errno));
    close(dupfd);
    return false;
  }
  bool ok = true;
  for (;;) {
    errno = 0;
    dirent* entry = readdir(dir);
    if (!entry) {
      if (errno) {
        fprintf(stderr, "cocaine: cannot read clipboard folder: %s\n", strerror(errno));
        ok = false;
      }
      break;
    }
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
    struct stat linkInfo;
    if (fstatat(source, entry->d_name, &linkInfo, AT_SYMLINK_NOFOLLOW) != 0) {
      fprintf(stderr, "cocaine: cannot inspect clipboard folder item: %s\n", strerror(errno));
      ok = false;
      break;
    }
    if (S_ISLNK(linkInfo.st_mode)) {
      char target[PATH_MAX];
      const ssize_t length = readlinkat(source, entry->d_name, target, sizeof target);
      if (length <= 0 || (size_t)length == sizeof target ||
          !safe_staged_link(std::string(target, (size_t)length))) {
        fprintf(stderr, "cocaine: cannot stage an absolute or parent-relative clipboard link\n");
        ok = false;
        break;
      }
      target[length] = 0;
      if (symlinkat(target, destination, entry->d_name) != 0) {
        fprintf(stderr, "cocaine: cannot stage clipboard link: %s\n", strerror(errno));
        ok = false;
        break;
      }
      continue;
    }
    int from = openat(source, entry->d_name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (from < 0) {
      fprintf(stderr, "cocaine: cannot open clipboard folder item: %s\n", strerror(errno));
      ok = false;
      break;
    }
    struct stat child = {};
    if (fstat(from, &child) != 0) {
      fprintf(stderr, "cocaine: cannot inspect clipboard folder item: %s\n", strerror(errno));
      close(from);
      ok = false;
      break;
    }
    int to = -1;
    if (S_ISREG(child.st_mode)) {
      to = openat(destination, entry->d_name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    } else if (S_ISDIR(child.st_mode)) {
      if (mkdirat(destination, entry->d_name, 0700) == 0)
        to = openat(destination, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    } else {
      errno = EINVAL;
    }
    if (to < 0) {
      fprintf(stderr, "cocaine: cannot stage clipboard folder item: %s\n", strerror(errno));
      close(from);
      ok = false;
      break;
    }
    ok = copy_staged(from, to, depth + 1);
    close(to);
    close(from);
    if (!ok) break;
  }
  closedir(dir);
  if (ok && fchmod(destination, (st.st_mode & 0777) | 0700) != 0) {
    fprintf(stderr, "cocaine: cannot preserve clipboard folder permissions: %s\n", strerror(errno));
    return false;
  }
  return ok;
}
}  // namespace

ClipboardBridge::~ClipboardBridge() {
  if (input_ >= 0) close(input_);
  if (output_ >= 0) close(output_);
  if (child_ > 0) {
    int status;
    if (waitpid(child_, &status, WNOHANG) == 0) {
      kill(child_, SIGKILL);
      while (waitpid(child_, &status, 0) < 0 && errno == EINTR) {}
    }
    if (!staging_.empty() && !staging_exported_) {
      NSError* error = nil;
      if (![[NSFileManager defaultManager] removeItemAtPath:@(staging_.c_str()) error:&error])
        fprintf(stderr, "cocaine: cannot remove clipboard staging folder: %s\n", error.localizedDescription.UTF8String);
    } else if (staging_exported_) {
      fprintf(stderr, "cocaine: retained temporary clipboard files at %s\n", staging_.c_str());
    }
  }
}

bool ClipboardBridge::start(const std::string& self, const std::string& sysroot,
                            const std::string& session, bool powerpc) {
  char resolved[PATH_MAX];
  if (!realpath(sysroot.c_str(), resolved)) {
    fprintf(stderr, "cocaine: cannot resolve clipboard system root: %s\n", strerror(errno));
    return false;
  }
  sysroot_ = resolved;
  const std::string python = sysroot + "/usr/bin/python2.5";
  if (access(python.c_str(), X_OK) != 0) {
    fprintf(stderr, "cocaine: clipboard bridge needs Leopard's Python 2.5 at %s\n", python.c_str());
    return false;
  }
  int in[2] = {-1, -1}, out[2] = {-1, -1};
  if (pipe(in) != 0 || pipe(out) != 0) {
    fprintf(stderr, "cocaine: clipboard bridge pipe: %s\n", strerror(errno));
    for (int fd : {in[0], in[1], out[0], out[1]}) if (fd >= 0) close(fd);
    return false;
  }
  std::vector<std::string> args = {self, "--sysroot", sysroot, "--arch", powerpc ? "ppc" : "i386",
                                   "--env", "PYTHONDONTWRITEBYTECODE=1"};
  if (!session.empty()) {
    args.push_back("--session");
    args.push_back(session);
  }
  args.insert(args.end(), {"--", python, "-c", kGuestScript});
  std::vector<char*> argv;
  for (std::string& arg : args) argv.push_back(arg.data());
  argv.push_back(nullptr);

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, in[0], STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, STDERR_FILENO, STDERR_FILENO);
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
  const int rc = posix_spawn(&child_, self.c_str(), &actions, &attr, argv.data(), environ);
  posix_spawnattr_destroy(&attr);
  posix_spawn_file_actions_destroy(&actions);
  close(in[0]);
  close(out[1]);
  if (rc != 0) {
    fprintf(stderr, "cocaine: cannot start clipboard bridge: %s\n", strerror(rc));
    close(in[1]);
    close(out[0]);
    child_ = -1;
    return false;
  }
  input_ = in[1];
  output_ = out[0];
  if (fcntl(input_, F_SETFL, O_NONBLOCK) < 0 || fcntl(output_, F_SETFL, O_NONBLOCK) < 0) {
    fprintf(stderr, "cocaine: clipboard bridge pipe setup: %s\n", strerror(errno));
    return false;
  }
  fcntl(input_, F_SETFD, FD_CLOEXEC);
  fcntl(output_, F_SETFD, FD_CLOEXEC);
  return true;
}

bool ClipboardBridge::request(const std::string& command, std::string& response) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  size_t sent = 0;
  while (sent < command.size()) {
    if (!ready(input_, POLLOUT, deadline)) break;
    const ssize_t n = write(input_, command.data() + sent, command.size() - sent);
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
    if (n <= 0) break;
    sent += (size_t)n;
  }
  response.clear();
  if (sent == command.size()) {
    while (ready(output_, POLLIN, deadline)) {
      char buf[16384];
      const ssize_t n = read(output_, buf, sizeof buf);
      if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
      if (n <= 0) { errno = EPIPE; break; }
      response.append(buf, (size_t)n);
      if (response.back() == '\n') {
        response.pop_back();
        return true;
      }
    }
  }
  fprintf(stderr, "cocaine: clipboard bridge %c communication failed: %s\n",
          command.empty() ? '?' : command[0], strerror(errno));
  connection_failed_ = true;
  int status;
  if (child_ > 0 && waitpid(child_, &status, WNOHANG) == child_) {
    if (WIFEXITED(status))
      fprintf(stderr, "cocaine: clipboard helper exited with status %d\n", WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
      fprintf(stderr, "cocaine: clipboard helper was killed by signal %d\n", WTERMSIG(status));
    child_ = -1;
  }
  return false;
}

bool ClipboardBridge::guest_identity(NSString* path, NSDictionary** identity) {
  NSData* data = [path dataUsingEncoding:NSUTF8StringEncoding];
  if (!data) {
    fprintf(stderr, "cocaine: cannot encode clipboard file path\n");
    return false;
  }
  std::string response;
  if (!request("V" + encode_data(data) + "\n", response)) return false;
  if (response == "N") {
    *identity = nil;
    return true;
  }
  *identity = decode_plist(response, 'V');
  if (!*identity) {
    fprintf(stderr, "cocaine: invalid guest file identity\n");
    return false;
  }
  return true;
}

bool ClipboardBridge::reject_host(NSInteger change) {
  std::string response;
  if (!request("C\n", response) || response != "O" ||
      !request("P\n", response) || !(guest_content_ = decode_plist(response, 'P'))) {
    fprintf(stderr, "cocaine: cannot clear the guest clipboard after a failed transfer\n");
    return false;
  }
  host_change_ = change;
  have_guest_ = true;
  recoverable_ = true;
  return false;
}

bool ClipboardBridge::stage_file(NSString* path, NSString** result) {
  if (staging_.empty()) {
    char folder[] = "/private/tmp/cocaine-clipboard.XXXXXX";
    if (!mkdtemp(folder)) {
      fprintf(stderr, "cocaine: cannot create clipboard staging folder: %s\n", strerror(errno));
      return false;
    }
    staging_ = folder;
  }
  const int source = open(path.fileSystemRepresentation, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
  if (source < 0) {
    fprintf(stderr, "cocaine: cannot open clipboard item for staging: %s\n", strerror(errno));
    return false;
  }
  struct stat st;
  if (fstat(source, &st) != 0 || (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))) {
    fprintf(stderr, "cocaine: clipboard staging requires a regular file or folder\n");
    close(source);
    return false;
  }
  NSString* directory = [@(staging_.c_str()) stringByAppendingPathComponent:
                         [NSString stringWithFormat:@"%zu", stage_count_++]];
  if (mkdir(directory.fileSystemRepresentation, 0700) != 0) {
    fprintf(stderr, "cocaine: cannot create clipboard staging directory: %s\n", strerror(errno));
    close(source);
    return false;
  }
  NSString* copy = [directory stringByAppendingPathComponent:path.lastPathComponent];
  int destination = -1;
  if (S_ISDIR(st.st_mode)) {
    if (mkdir(copy.fileSystemRepresentation, 0700) == 0)
      destination = open(copy.fileSystemRepresentation, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  } else {
    destination = open(copy.fileSystemRepresentation, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  }
  if (destination < 0) {
    fprintf(stderr, "cocaine: cannot create staged clipboard item: %s\n", strerror(errno));
    close(source);
    return false;
  }
  const bool ok = copy_staged(source, destination, 0);
  close(destination);
  close(source);
  if (!ok) return false;
  *result = copy;
  return true;
}

bool ClipboardBridge::guest_path(NSString* path, NSString** result) {
  NSString* canonical = path.stringByResolvingSymlinksInPath;
  NSString* root = @(sysroot_.c_str());
  NSString* candidate = beneath(canonical, root) ? [canonical substringFromIndex:root.length] : canonical;
  if (!candidate.length) candidate = @"/";
  NSDictionary* identity = nil;
  if (!guest_identity(candidate, &identity)) return false;
  if (identity && same_file(canonical, identity)) {
    *result = candidate;
    return true;
  }
  if (!stage_file(canonical, result)) return false;
  if (!guest_identity(*result, &identity)) return false;
  if (!identity || !same_file(*result, identity)) {
    fprintf(stderr, "cocaine: staged clipboard file is not accessible to the guest\n");
    return false;
  }
  return true;
}

bool ClipboardBridge::host_path(NSDictionary* entry, NSString** result) {
  NSString* path = entry[@"path"];
  NSDictionary* identity = entry[@"identity"];
  if (![path isKindOfClass:NSString.class] || !path.isAbsolutePath ||
      ![identity isKindOfClass:NSDictionary.class]) {
    fprintf(stderr, "cocaine: guest clipboard file is inaccessible or invalid\n");
    return false;
  }
  NSString* rootPath = [@(sysroot_.c_str()) stringByAppendingString:path];
  for (NSString* candidate in @[rootPath, path]) {
    if (same_file(candidate, identity)) {
      *result = candidate;
      if (!staging_.empty()) {
        char resolved[PATH_MAX];
        if (realpath(candidate.fileSystemRepresentation, resolved) &&
            beneath(@(resolved), @(staging_.c_str())))
          staging_exported_ = true;
      }
      return true;
    }
  }
  fprintf(stderr, "cocaine: cannot map guest clipboard file to the host\n");
  return false;
}

bool ClipboardBridge::sync(NSPasteboard* host) {
  recoverable_ = false;
  const NSInteger change = host.changeCount;
  std::string response;
  if (change != host_change_) {
    NSMutableDictionary* content = [NSMutableDictionary dictionary];
    NSString* text = [host stringForType:NSPasteboardTypeString];
    if (text) content[@"text"] = text;
    for (NSArray* pair in @[@[@"rtf", NSPasteboardTypeRTF], @[@"html", NSPasteboardTypeHTML]]) {
      NSData* data = [host dataForType:pair[1]];
      if (data) content[pair[0]] = data;
    }
    for (NSArray* pair in @[@[@"tiff", NSPasteboardTypeTIFF], @[@"png", NSPasteboardTypePNG],
                            @[@"jpeg", @"public.jpeg"]]) {
      NSData* data = [host dataForType:pair[1]];
      if (data) content[pair[0]] = data;
    }
    NSArray<NSURL*>* urls = [host readObjectsForClasses:@[NSURL.class]
                                                options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
    if (urls.count) {
      NSMutableArray* paths = [NSMutableArray arrayWithCapacity:urls.count];
      for (NSURL* url in urls) {
        NSString* path = nil;
        if (!url.fileURL) {
          fprintf(stderr, "cocaine: macOS clipboard contains an invalid file URL\n");
          return reject_host(change);
        }
        if (!guest_path(url.path, &path)) {
          return connection_failed_ ? false : reject_host(change);
        }
        [paths addObject:path];
      }
      content[@"files"] = paths;
    }
    if (urls.count == 1 && !content[@"tiff"] && !content[@"png"] && !content[@"jpeg"]) {
      NSURL* url = urls[0];
      CGImageSourceRef source = CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
      if (source) {
        NSString* type = (__bridge NSString*)CGImageSourceGetType(source);
        NSString* key = [@{@"public.tiff": @"tiff", @"public.png": @"png",
                           @"public.jpeg": @"jpeg"} objectForKey:type];
        if (key) {
          NSError* error = nil;
          NSData* data = [NSData dataWithContentsOfURL:url options:NSDataReadingMappedIfSafe error:&error];
          if (data) content[key] = data;
          else fprintf(stderr, "cocaine: cannot read clipboard image file: %s\n", error.localizedDescription.UTF8String);
        }
        CFRelease(source);
      }
    }
    if (!content[@"tiff"] && (content[@"png"] || content[@"jpeg"])) {
      NSImage* image = [[NSImage alloc] initWithData:content[@"png"] ?: content[@"jpeg"]];
      NSData* tiff = image.TIFFRepresentation;
      if (!tiff) {
        fprintf(stderr, "cocaine: cannot convert macOS clipboard image to TIFF\n");
        return reject_host(change);
      }
      content[@"tiff"] = tiff;
    }
    if (host.changeCount != change) return true;
    NSError* error = nil;
    NSData* plist = [NSPropertyListSerialization dataWithPropertyList:content
      format:NSPropertyListXMLFormat_v1_0 options:0 error:&error];
    if (!plist) {
      fprintf(stderr, "cocaine: cannot encode macOS clipboard: %s\n", error.localizedDescription.UTF8String);
      return false;
    }
    if (!request("W" + encode_data(plist) + "\n", response) || response != "O") {
      fprintf(stderr, "cocaine: guest clipboard update failed\n");
      return false;
    }
    host_change_ = change;
    if (!request("P\n", response) || !(guest_content_ = decode_plist(response, 'P'))) {
      fprintf(stderr, "cocaine: cannot read the guest clipboard\n");
      return false;
    }
    have_guest_ = true;
    return true;
  }

  if (!request("P\n", response)) return false;
  if (response == "U") return true;
  NSDictionary* content = decode_plist(response, 'P');
  if (!content) {
    fprintf(stderr, "cocaine: cannot read the guest clipboard\n");
    return false;
  }
  if (host.changeCount != change) return true;
  if (have_guest_ && content.count && ![content isEqual:guest_content_]) {
    NSArray* files = content[@"files"];
    if (files && ![files isKindOfClass:NSArray.class]) {
      fprintf(stderr, "cocaine: invalid guest clipboard file list\n");
      return false;
    }
    NSMutableArray<NSPasteboardItem*>* items = [NSMutableArray array];
    for (id entry in files) {
      NSString* path = nil;
      if (![entry isKindOfClass:NSDictionary.class] || !host_path(entry, &path)) {
        fprintf(stderr, "cocaine: cannot transfer a guest clipboard file\n");
        recoverable_ = true;
        return false;
      }
      NSPasteboardItem* item = [[NSPasteboardItem alloc] init];
      if (![item setString:[NSURL fileURLWithPath:path].absoluteString forType:NSPasteboardTypeFileURL]) {
        fprintf(stderr, "cocaine: cannot encode guest clipboard file URL\n");
        return false;
      }
      [items addObject:item];
    }
    if (!items.count) [items addObject:[[NSPasteboardItem alloc] init]];
    NSPasteboardItem* first = items[0];
    NSString* text = content[@"text"];
    if (text && (![text isKindOfClass:NSString.class] || ![first setString:text forType:NSPasteboardTypeString])) {
      fprintf(stderr, "cocaine: invalid guest clipboard text\n");
      return false;
    }
    for (NSArray* pair in @[@[@"rtf", NSPasteboardTypeRTF], @[@"html", NSPasteboardTypeHTML]]) {
      NSData* data = content[pair[0]];
      if (data && (![data isKindOfClass:NSData.class] || ![first setData:data forType:pair[1]])) {
        fprintf(stderr, "cocaine: invalid guest clipboard formatted text\n");
        return false;
      }
    }
    for (NSArray* pair in @[@[@"tiff", NSPasteboardTypeTIFF], @[@"png", NSPasteboardTypePNG],
                            @[@"jpeg", @"public.jpeg"]]) {
      NSData* data = content[pair[0]];
      if (data && (![data isKindOfClass:NSData.class] || ![first setData:data forType:pair[1]])) {
        fprintf(stderr, "cocaine: invalid guest clipboard image\n");
        return false;
      }
    }
    if (!content[@"tiff"] && (content[@"png"] || content[@"jpeg"])) {
      NSImage* image = [[NSImage alloc] initWithData:content[@"png"] ?: content[@"jpeg"]];
      NSData* tiff = image.TIFFRepresentation;
      if (!tiff || ![first setData:tiff forType:NSPasteboardTypeTIFF]) {
        fprintf(stderr, "cocaine: cannot convert guest clipboard image to TIFF\n");
        recoverable_ = true;
        return false;
      }
    }
    [host clearContents];
    if (![host writeObjects:items]) {
      fprintf(stderr, "cocaine: cannot write macOS clipboard\n");
      return false;
    }
    host_change_ = host.changeCount;
  }
  guest_content_ = content;
  have_guest_ = true;
  return true;
}

bool ClipboardBridge::sync_host_if_changed(NSPasteboard* host) {
  for (int attempts = 0; attempts < 3 && host.changeCount != host_change_; ++attempts) {
    if (!sync(host)) return false;
  }
  if (host.changeCount == host_change_) return true;
  fprintf(stderr, "cocaine: macOS clipboard kept changing during synchronization\n");
  return false;
}

namespace {
struct InputRecord { int32_t words[6]; };
struct BridgeState {
  std::unique_ptr<ClipboardBridge> bridge;
};
dispatch_queue_t input_queue;
dispatch_source_t input_timer;
BridgeState* bridge_state;
}

void start_clipboard_bridge(const std::string& self, const std::string& sysroot,
                            const std::string& session, bool powerpc) {
  // Serialize clipboard work and input so a paste cannot overtake a host update.
  input_queue = dispatch_queue_create("cocaine.clipboard-input", DISPATCH_QUEUE_SERIAL);
  bridge_state = new BridgeState;
  dispatch_async(input_queue, ^{
    @autoreleasepool {
      bridge_state->bridge = std::make_unique<ClipboardBridge>();
      if (!bridge_state->bridge->start(self, sysroot, session, powerpc))
        bridge_state->bridge.reset();
    }
  });
  input_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, input_queue);
  dispatch_source_set_timer(input_timer, DISPATCH_TIME_NOW, 200 * NSEC_PER_MSEC, 20 * NSEC_PER_MSEC);
  dispatch_source_set_event_handler(input_timer, ^{
    @autoreleasepool {
      if (bridge_state && bridge_state->bridge &&
          !bridge_state->bridge->sync(NSPasteboard.generalPasteboard) &&
          !bridge_state->bridge->recoverable_error())
        bridge_state->bridge.reset();
    }
  });
  dispatch_resume(input_timer);
}

void stop_clipboard_bridge() {
  if (!input_queue) return;
  dispatch_source_cancel(input_timer);
  dispatch_semaphore_t done = dispatch_semaphore_create(0);
  dispatch_async(input_queue, ^{
    bridge_state->bridge.reset();
    delete bridge_state;
    bridge_state = nullptr;
    dispatch_semaphore_signal(done);
  });
  if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC)) != 0)
    fprintf(stderr, "cocaine: clipboard bridge did not finish cleanup before exit\n");
}

void send_viewer_input(const int32_t (&record)[6]) {
  if (!input_queue) {
    (void)write(STDOUT_FILENO, record, sizeof record);
    return;
  }
  InputRecord copy;
  memcpy(copy.words, record, sizeof copy.words);
  dispatch_async(input_queue, ^{
    @autoreleasepool {
      if (bridge_state && bridge_state->bridge &&
          !bridge_state->bridge->sync_host_if_changed(NSPasteboard.generalPasteboard) &&
          !bridge_state->bridge->recoverable_error())
        bridge_state->bridge.reset();
      (void)write(STDOUT_FILENO, copy.words, sizeof copy.words);
    }
  });
}
