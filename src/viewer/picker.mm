// The file picker's helper: `cocaine --pick --session <name>`.
//
// A guest program's open and save panels are Leopard's, and Leopard's need
// daemons and a Finder that a session doesn't have. So the guest's cocaine
// process asks this helper instead: a short-lived process that shows this
// Mac's own picker, checks that what was picked is something the guest can
// reach, and answers with the path to hand the app. The messages are
// described in picker.h.
//
// It is a process of its own, not part of the guest's or the viewer's: this
// Mac's AppKit can't run inside a process holding a guest (its fault handlers,
// its blocked signals, its forks), and a guest's main thread is stuck for as
// long as the panel is up. The viewer only learns that a panel is up (a small
// file, see pick_flag_path) so it can keep the mouse and keys to itself.
//
// Tests and runs without a window never get a real panel: the COCAINE_PICK_*
// variables answer instead, through the same checks a real pick goes through.

#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "kernel/paths.h"
#include "viewer/picker.h"

// ---- Plain functions (tests/picker.mm includes this file and calls them) ----

// One line for COCAINE_PICK_LOG: `request`, then every field as it arrived,
// with tabs between. Tabs, line ends and backslashes inside a value are
// written as \t, \n, \r and \\ so a line is always one request.
static std::string pick_log_line(const PickFields& fields) {
  std::string line = "request";
  for (const auto& [key, value] : fields) {
    line += '\t';
    for (const char c : key + "=" + value) {
      if (c == '\t') line += "\\t";
      else if (c == '\n') line += "\\n";
      else if (c == '\r') line += "\\r";
      else if (c == '\\') line += "\\\\";
      else line += c;
    }
  }
  return line + "\n";
}

// A stand-in answer: lines that are each an absolute host path; none means
// cancel. An answer file (`delay_line`) may start with `delay N`, the seconds
// to take over it, which may be a fraction.
static bool pick_parse_answer(const std::string& text, bool delay_line, double& delay,
                              std::vector<std::string>& paths, std::string& error) {
  delay = 0;
  paths.clear();
  size_t at = 0;
  for (int number = 1; at < text.size(); number++) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(at, end - at);
    at = end + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (number == 1 && delay_line && line.compare(0, 5, "delay") == 0 &&
        (line.size() == 5 || line[5] == ' ')) {
      char* rest = nullptr;
      delay = strtod(line.c_str() + 5, &rest);
      while (*rest == ' ') rest++;
      if (rest == line.c_str() + 5 || *rest || !(delay >= 0) || !std::isfinite(delay)) {
        error = "the stand-in answer's delay is not a number of seconds: " + line;
        return false;
      }
      continue;
    }
    if (line.empty()) continue;
    if (line[0] != '/') {
      error = "the stand-in answer's line " + std::to_string(number) + " is not an absolute path: " + line;
      return false;
    }
    paths.push_back(line);
  }
  return true;
}

static std::string pick_lower(std::string text) {
  for (char& c : text) {
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  }
  return text;
}

// A name's extension as Cocoa sees it: what follows the last dot, unless the
// dot starts or ends the name or a space follows it ("Notes v. 2" has none).
static std::string pick_extension(const std::string& name) {
  const size_t dot = name.rfind('.');
  if (dot == std::string::npos || dot == 0 || dot + 1 == name.size()) return "";
  const std::string extension = name.substr(dot + 1);
  return extension.find(' ') == std::string::npos ? extension : "";
}

// What kind of entry a Leopard type list holds.
static bool pick_type_is_code(const std::string& type) {  // 'TEXT'
  return type.size() == 6 && type.front() == '\'' && type.back() == '\'';
}
static bool pick_type_is_identifier(const std::string& type) {  // public.plain-text
  return !pick_type_is_code(type) && type.find('.') != std::string::npos;
}
static bool pick_type_is_extension(const std::string& type) {
  return !type.empty() && type != "*" && !pick_type_is_code(type) && !pick_type_is_identifier(type);
}

// A list that lets every file through: no list at all, or one naming the
// type every file is. (NSDocumentController hands TextEdit's list over with
// public.data and "*" in it.)
static bool pick_types_take_everything(const std::vector<std::string>& types) {
  if (types.empty()) return true;
  for (const std::string& type : types) {
    if (type.empty() || type == "*" || type == "public.data" || type == "public.item") return true;
  }
  return false;
}

static NSString* pick_string(const std::string& text) {
  return [NSString stringWithUTF8String:text.c_str()] ?: @"";
}

static NSURL* pick_url(const std::string& path) {
  return [NSURL fileURLWithPath:pick_string(path)];
}

static std::string pick_url_path(NSURL* url) {
  const char* path = url.path.UTF8String;
  return path ? path : "";
}

// How a panel treats what a URL names. A file package counts as a file,
// unless the app asked for packages to be opened like folders.
enum class PickItem { Missing, Folder, File };

// Through links and Finder aliases, to what would be opened.
static NSURL* pick_resolved(NSURL* url) {
  NSNumber* alias = nil;
  NSNumber* link = nil;
  [url getResourceValue:&link forKey:NSURLIsSymbolicLinkKey error:nil];
  if (link.boolValue) return url.URLByResolvingSymlinksInPath;
  [url getResourceValue:&alias forKey:NSURLIsAliasFileKey error:nil];
  if (alias.boolValue) {
    NSURL* target = [NSURL URLByResolvingAliasFileAtURL:url
                                                options:NSURLBookmarkResolutionWithoutUI |
                                                        NSURLBookmarkResolutionWithoutMounting
                                                  error:nil];
    if (target) return target;
  }
  return url;
}

static PickItem pick_item(const PickRequest& r, NSURL* url) {
  NSNumber* folder = nil;
  NSNumber* package = nil;
  if (![url getResourceValue:&folder forKey:NSURLIsDirectoryKey error:nil] || !folder) return PickItem::Missing;
  [url getResourceValue:&package forKey:NSURLIsPackageKey error:nil];
  return folder.boolValue && (r.packages || !package.boolValue) ? PickItem::Folder : PickItem::File;
}

// Whether a file is of one of the listed types: its extension is a plain
// entry (whatever the case), its HFS type code is a quoted entry, or its
// content type is, or conforms to, an entry that is a type identifier.
// This Mac's own type matching is not used for extensions: it turns jpg into
// jpeg and takes rtfd and app for something else.
static bool pick_type_matches(const std::vector<std::string>& types, NSURL* url) {
  if (pick_types_take_everything(types)) return true;
  const std::string extension = pick_lower(pick_extension(url.lastPathComponent.UTF8String ?: ""));
  bool looked = false;
  NSString* code = nil;
  UTType* content = nil;
  for (const std::string& type : types) {
    if (pick_type_is_extension(type)) {
      if (!extension.empty() && pick_lower(type) == extension) return true;
    } else if (pick_type_is_code(type)) {
      if (!looked) {
        looked = true;
        NSDictionary* attributes = [[NSFileManager defaultManager] attributesOfItemAtPath:url.path error:nil];
        const OSType hfs = (OSType)[attributes fileHFSTypeCode];
        if (hfs) code = NSFileTypeForHFSTypeCode(hfs);
      }
      if (code && [code isEqualToString:pick_string(type)]) return true;
    } else {
      if (!content) [url getResourceValue:&content forKey:NSURLContentTypeKey error:nil];
      if (!content) continue;
      NSString* identifier = pick_string(type);
      if ([content.identifier caseInsensitiveCompare:identifier] == NSOrderedSame) return true;
      UTType* wanted = [UTType typeWithIdentifier:identifier];
      if (wanted && [content conformsToType:wanted]) return true;
    }
  }
  return false;
}

// The open panel's filter (panel:shouldEnableURL:): folders always, so they
// can be gone into; files by type. `missing` is the answer for something
// that can't be looked at.
static bool pick_open_enables(const PickRequest& r, NSURL* url, bool missing) {
  NSURL* target = pick_resolved(url);
  switch (pick_item(r, target)) {
    case PickItem::Missing: return missing;
    case PickItem::Folder: return true;
    case PickItem::File: return pick_type_matches(r.types, target);
  }
  return missing;
}

// The extensions a saved name may end in: the required one first, then the
// listed types that are extensions.
static std::vector<std::string> pick_save_extensions(const PickRequest& r) {
  std::vector<std::string> extensions;
  if (pick_type_is_extension(r.required)) extensions.push_back(r.required);
  for (const std::string& type : r.types) {
    if (pick_type_is_extension(type)) extensions.push_back(type);
  }
  return extensions;
}

// What the name field starts with: the suggested name, with the required
// extension if it has none. (NSDocument takes the extension off the name it
// suggests and relies on the panel to put it back.)
static std::string pick_prefill_name(const PickRequest& r) {
  if (r.name.empty() || !pick_type_is_extension(r.required) || !pick_extension(r.name).empty()) return r.name;
  return r.name + "." + r.required;
}

// What Leopard's OK button does to the name before the app sees it: a name
// whose extension isn't one of the allowed ones gets the first of them added;
// if the app allows other types, only a name with no extension at all does.
static std::string pick_finish_save_name(const PickRequest& r, const std::string& name) {
  const std::vector<std::string> allowed = pick_save_extensions(r);
  if (allowed.empty() || name.empty()) return name;
  const std::string extension = pick_lower(pick_extension(name));
  for (const std::string& one : allowed) {
    if (!extension.empty() && pick_lower(one) == extension) return name;
  }
  if (r.othertypes && !extension.empty()) return name;
  return name + "." + allowed[0];
}

// The same, for a whole path.
static std::string pick_finish_save_path(const PickRequest& r, const std::string& path) {
  const size_t slash = path.rfind('/');
  if (slash == std::string::npos) return path;
  return path.substr(0, slash + 1) + pick_finish_save_name(r, path.substr(slash + 1));
}

// One pick, as the panel or the stand-in gave it, to the path the guest is
// handed: a save name is finished first, then the path is checked.
static PickedPath pick_settle_one(const PickRequest& r, const std::string& pick, std::string& guest) {
  return guest_path_for_pick(r.sysroot, r.save ? pick_finish_save_path(r, pick) : pick, r.save, guest);
}

static bool pick_settle(const PickRequest& r, const std::vector<std::string>& picks,
                        std::vector<std::string>& guest, std::string& error) {
  guest.clear();
  for (const std::string& pick : picks) {
    std::string path;
    const PickedPath result = pick_settle_one(r, pick, path);
    if (result != PickedPath::Ok) {
      error = std::string(picked_path_refusal(result)) + ": " + pick;
      return false;
    }
    guest.push_back(path);
  }
  return true;
}

// Whether the real panel, set up as the request says, could have given this
// answer. A stand-in answer it could not give is a mistake in the test, and
// is refused rather than handed to the app.
static bool pick_answer_possible(const PickRequest& r, const std::vector<std::string>& picks,
                                 std::string& error) {
  if (r.save) {
    if (picks.size() != 1) {
      error = "a save panel gives one name, not " + std::to_string(picks.size());
      return false;
    }
    if (pick_item(r, pick_resolved(pick_url(picks[0]))) == PickItem::Folder) {
      error = "a save panel can't give a folder that is already there: " + picks[0];
      return false;
    }
    return true;
  }
  if (picks.size() > 1 && !r.multi) {
    error = "this open panel gives one item, not " + std::to_string(picks.size());
    return false;
  }
  for (const std::string& pick : picks) {
    NSURL* target = pick_resolved(pick_url(pick));
    switch (pick_item(r, target)) {
      case PickItem::Missing:
        error = "there is nothing at " + pick;
        return false;
      case PickItem::Folder:
        if (!r.dirs) {
          error = "this open panel doesn't give folders: " + pick;
          return false;
        }
        break;
      case PickItem::File:
        if (!r.files) {
          error = "this open panel gives folders only: " + pick;
          return false;
        }
        if (!pick_type_matches(r.types, target)) {
          error = "this open panel's types leave that file greyed out: " + pick;
          return false;
        }
        break;
    }
  }
  return true;
}

// The whole reply for a stand-in answer.
static PickReply pick_stand_in_reply(const PickRequest& r, const std::vector<std::string>& picks) {
  PickReply reply;
  reply.pid = (int)getpid();
  reply.exthidden = r.exthidden;
  if (picks.empty()) {
    reply.status = "cancel";
  } else if (pick_answer_possible(r, picks, reply.error) && pick_settle(r, picks, reply.paths, reply.error)) {
    reply.status = "ok";
  } else {
    reply.status = "error";
    reply.paths.clear();
  }
  return reply;
}

// Runs the panel with anything Objective-C throws caught: the panel talks to
// a service in another process, and reports some of its troubles that way.
static NSModalResponse pick_run_guarded(NSModalResponse (^run)(void), std::string& thrown) {
  @try {
    return run();
  } @catch (NSException* exception) {
    const char* reason = (exception.reason ?: exception.name).UTF8String;
    thrown = reason && *reason ? reason : "an exception";
  } @catch (id) {
    thrown = "an exception";
  }
  return NSModalResponseAbort;
}

// ---- The process ----

static const int kRequestFd = 3;

static bool pick_read_whole(const char* path, std::string& text) {
  const int fd = open(path, O_RDONLY);
  if (fd < 0) return false;
  char buf[4096];
  ssize_t n;
  while ((n = read(fd, buf, sizeof buf)) > 0 || (n < 0 && errno == EINTR)) {
    if (n > 0) text.append(buf, (size_t)n);
  }
  const bool ok = n == 0;
  const int error = errno;
  close(fd);
  errno = error;
  return ok;
}

// Called when the requester's end has something to say. It has nothing more
// to send, so anything but waiting means it has gone.
static bool pick_requester_gone() {
  char buf[256];
  const ssize_t n = read(kRequestFd, buf, sizeof buf);
  if (n == 0) return true;
  return n < 0 && errno != EINTR && errno != EAGAIN;
}

static void pick_remove_flag(int root) {
  const std::string path = pick_flag_path(root);
  std::string text;
  if (root > 0 && pick_read_whole(path.c_str(), text) && atoi(text.c_str()) == (int)getpid()) unlink(path.c_str());
}

static void pick_write_flag(int root) {
  if (root <= 0) return;
  const int fd = open(pick_flag_path(root).c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return;
  const std::string text = std::to_string((int)getpid()) + "\n";
  (void)write(fd, text.data(), text.size());
  close(fd);
}

[[noreturn]] static void pick_send(const PickReply& reply) {
  const std::string message = pick_encode_reply(reply);
  size_t sent = 0;
  while (sent < message.size()) {
    const ssize_t n = write(kRequestFd, message.data() + sent, message.size() - sent);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    sent += (size_t)n;
  }
  _exit(0);
}

// Waits out a stand-in's delay, leaving at once if the requester hangs up:
// a guest killed while "the panel is up" must not leave a helper behind.
static void pick_wait(double seconds) {
  const uint64_t until = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + (uint64_t)(seconds * 1e9);
  for (;;) {
    const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (now >= until) return;
    struct pollfd watched = {kRequestFd, POLLIN, 0};
    const int n = poll(&watched, 1, (int)std::min<uint64_t>((until - now) / 1000000 + 1, 1000));
    if (n > 0 && ((watched.revents & (POLLHUP | POLLERR | POLLNVAL)) || pick_requester_gone())) _exit(0);
  }
}

// The same while the real panel is up: the process ends, panel and all, when
// the requester hangs up or its process ends. Not on the main queue, which
// is busy with the panel.
static void pick_watch_requester(int root, pid_t requester) {
  static dispatch_source_t hung, gone;
  dispatch_queue_t queue = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
  hung = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)kRequestFd, 0, queue);
  dispatch_source_set_event_handler(hung, ^{
    if (!pick_requester_gone()) return;
    pick_remove_flag(root);
    _exit(0);
  });
  dispatch_resume(hung);
  if (requester <= 1) return;
  gone = dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC, (uintptr_t)requester, DISPATCH_PROC_EXIT, queue);
  dispatch_source_set_event_handler(gone, ^{
    pick_remove_flag(root);
    _exit(0);
  });
  dispatch_resume(gone);
  if (kill(requester, 0) != 0 && errno == ESRCH) {  // it went before the watch was set
    pick_remove_flag(root);
    _exit(0);
  }
}

// ---- The real panel ----

@interface CocainePickDelegate : NSObject <NSOpenSavePanelDelegate> {
 @public
  const PickRequest* request;
}
@end

@implementation CocainePickDelegate
- (BOOL)panel:(id)sender shouldEnableURL:(NSURL*)url {
  return request->save || pick_open_enables(*request, url, true);
}
// The path check happens here, while the panel is still up, so it can say
// why and stay on the same folder: places a guest can't reach (iCloud Drive
// among them) are a click away in the sidebar.
- (BOOL)panel:(id)sender validateURL:(NSURL*)url error:(NSError* __autoreleasing*)error {
  std::string guest;
  const PickedPath result = pick_settle_one(*request, pick_url_path(url), guest);
  if (result == PickedPath::Ok) return YES;
  if (error) {
    // The panel shows the first part in bold and the rest below it.
    NSString* text = pick_string(picked_path_refusal(result));
    const NSRange dash = [text rangeOfString:@" - "];
    NSMutableDictionary* info = [NSMutableDictionary dictionary];
    if (dash.location == NSNotFound) {
      info[NSLocalizedDescriptionKey] = text;
    } else {
      info[NSLocalizedDescriptionKey] = [text substringToIndex:dash.location];
      info[NSLocalizedRecoverySuggestionErrorKey] = [text substringFromIndex:NSMaxRange(dash)];
    }
    *error = [NSError errorWithDomain:@"cocaine.picker" code:1 userInfo:info];
  }
  return NO;
}
@end

// A main menu with an Edit menu, so Command-V, C, X, A and Z work in the
// name field: key equivalents are looked up in the menu bar, and a program
// with no application bundle has none.
static NSMenu* pick_menu() {
  NSMenu* bar = [[NSMenu alloc] initWithTitle:@""];
  NSMenuItem* app = [bar addItemWithTitle:@"cocaine" action:nil keyEquivalent:@""];
  app.submenu = [[NSMenu alloc] initWithTitle:@"cocaine"];
  NSMenuItem* item = [bar addItemWithTitle:@"Edit" action:nil keyEquivalent:@""];
  NSMenu* edit = [[NSMenu alloc] initWithTitle:@"Edit"];
  [edit addItemWithTitle:@"Undo" action:NSSelectorFromString(@"undo:") keyEquivalent:@"z"];
  [edit addItemWithTitle:@"Redo" action:NSSelectorFromString(@"redo:") keyEquivalent:@"Z"];
  [edit addItem:[NSMenuItem separatorItem]];
  [edit addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
  [edit addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
  [edit addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
  [edit addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
  item.submenu = edit;
  return bar;
}

static NSSavePanel* pick_build_panel(const PickRequest& r) {
  NSSavePanel* panel;
  if (r.save) {
    panel = [NSSavePanel savePanel];
    const std::string name = pick_prefill_name(r);
    if (!name.empty()) panel.nameFieldStringValue = pick_string(name);
    if (!r.label.empty()) panel.nameFieldLabel = pick_string(r.label);
    panel.canSelectHiddenExtension = r.canhideext;
    panel.extensionHidden = r.exthidden;
  } else {
    NSOpenPanel* open = [NSOpenPanel openPanel];
    open.allowsMultipleSelection = r.multi;
    open.canChooseFiles = r.files;
    open.canChooseDirectories = r.dirs;
    open.resolvesAliases = r.aliases;
    panel = open;
  }
  panel.canCreateDirectories = r.mkdir;
  panel.treatsFilePackagesAsDirectories = r.packages;
  panel.showsHiddenFiles = r.hidden;
  if (!r.title.empty()) panel.title = pick_string(r.title);
  if (!r.prompt.empty()) panel.prompt = pick_string(r.prompt);
  // Today's open panel shows no title, so with no message the title goes there.
  std::string message = r.message.empty() ? r.title : r.message;
  if (!r.note.empty()) message += (message.empty() ? "" : "\n") + r.note;
  if (!message.empty()) panel.message = pick_string(message);
  if (!r.dir.empty()) panel.directoryURL = [NSURL fileURLWithPath:pick_string(r.dir) isDirectory:YES];
  // The types are not handed to the panel (see pick_type_matches); the
  // delegate greys files out, and a saved name is finished afterwards.
  //
  // The viewer's window is nearly as big as the screen and takes a click
  // without being in front, so the panel must not hide, or sink to an
  // ordinary window's level, when this program stops being the active one.
  panel.hidesOnDeactivate = NO;
  panel.collectionBehavior = NSWindowCollectionBehaviorMoveToActiveSpace |
                             NSWindowCollectionBehaviorFullScreenAuxiliary;
  return panel;
}

static PickReply pick_with_panel(const PickRequest& r, pid_t requester) {
  PickReply reply;
  reply.pid = (int)getpid();
  reply.status = "error";
  reply.exthidden = r.exthidden;
  @autoreleasepool {
    [NSApplication sharedApplication];
    // No Dock icon and no menu bar of its own until it is in front.
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    NSApp.mainMenu = pick_menu();
    // Kept for good: the panel doesn't hold on to its delegate, and a local
    // one may be let go as soon as this function last mentions it.
    static CocainePickDelegate* delegate;
    delegate = [[CocainePickDelegate alloc] init];
    delegate->request = &r;
    NSSavePanel* panel = pick_build_panel(r);
    panel.delegate = delegate;
    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    // Coming back to the front (the viewer sends its clicks this way while
    // the panel is up) makes the panel the key window again.
    id active = [center addObserverForName:NSApplicationDidBecomeActiveNotification
                                    object:NSApp
                                     queue:nil
                                usingBlock:^(NSNotification* note) {
                                  if (panel.isVisible) [panel makeKeyAndOrderFront:nil];
                                }];
    // AppKit drops a modal panel to an ordinary window's level when its
    // program is no longer the active one; this runs after it has.
    id resigned = [center addObserverForName:NSApplicationDidResignActiveNotification
                                      object:NSApp
                                       queue:nil
                                  usingBlock:^(NSNotification* note) {
                                    if (!panel.isVisible) return;
                                    panel.level = NSFloatingWindowLevel;
                                    panel.hidesOnDeactivate = NO;
                                    [panel orderFrontRegardless];
                                  }];
    pick_write_flag(r.root);
    pick_watch_requester(r.root, requester);
    // Asked for both ways: the newer call alone can be refused when another
    // program is in front, as the viewer is.
    if (@available(macOS 14.0, *)) [NSApp activate];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [NSApp activateIgnoringOtherApps:YES];
#pragma clang diagnostic pop
    for (;;) {
      std::string thrown;
      const NSModalResponse result = pick_run_guarded(^{ return [panel runModal]; }, thrown);
      if (!thrown.empty()) {
        reply.error = "this Mac's file picker failed: " + thrown;
        break;
      }
      if (result == NSModalResponseCancel) {
        reply.status = "cancel";
        break;
      }
      if (result != NSModalResponseOK) {
        reply.error = "this Mac's file picker ended with result " + std::to_string((long)result);
        break;
      }
      std::vector<std::string> picks;
      if (r.save) {
        const std::string pick = pick_url_path(panel.URL);
        const std::string finished = pick_finish_save_path(r, pick);
        struct stat st;
        if (finished != pick && stat(finished.c_str(), &st) == 0) {
          // The name gained its extension and that name is taken. The panel
          // asked nothing about it, so it is shown again with the full name
          // and asks its own question about replacing the file.
          const size_t slash = finished.rfind('/');
          std::string leaf = finished.substr(slash + 1);
          for (char& c : leaf) {
            if (c == ':') c = '/';  // the name field shows a path's colon as a slash
          }
          panel.directoryURL = [NSURL fileURLWithPath:pick_string(finished.substr(0, slash ? slash : 1))
                                          isDirectory:YES];
          panel.nameFieldStringValue = pick_string(leaf);
          continue;
        }
        picks.push_back(pick);
        reply.exthidden = panel.isExtensionHidden;
      } else {
        for (NSURL* url in ((NSOpenPanel*)panel).URLs) picks.push_back(pick_url_path(url));
      }
      // The delegate has already passed each of these; this is where the
      // paths for the guest are made.
      if (pick_settle(r, picks, reply.paths, reply.error)) reply.status = "ok";
      else reply.paths.clear();
      break;
    }
    [center removeObserver:active];
    [center removeObserver:resigned];
    pick_remove_flag(r.root);
  }
  return reply;
}

int run_picker(const std::string& session) {
  const pid_t requester = getppid();
  std::string bytes;
  size_t length;
  while (!(length = pick_message_length(bytes))) {
    char buf[4096];
    const ssize_t n = read(kRequestFd, buf, sizeof buf);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0 || bytes.size() > (1u << 20)) {
      fprintf(stderr, "cocaine: --pick is for cocaine itself: it takes a request on descriptor 3\n");
      return 125;
    }
    bytes.append(buf, (size_t)n);
  }
  bytes.resize(length);
  PickRequest r;
  PickReply reply;
  reply.pid = (int)getpid();
  const bool understood = pick_parse_request(bytes, r, reply.error);
  if (r.root <= 0) sscanf(session.c_str(), "cocaine.session.%d.", &r.root);
  reply.exthidden = r.exthidden;

  // Nothing of this program's own goes to the terminal or the launcher's
  // log: its troubles travel in the reply, and what this Mac's AppKit prints
  // goes with the daemons' output.
  signal(SIGPIPE, SIG_IGN);
  const int null = open("/dev/null", O_WRONLY);
  if (null >= 0) {
    char tmp[PATH_MAX] = "/tmp/";
    confstr(_CS_DARWIN_USER_TEMP_DIR, tmp, sizeof tmp);
    const std::string log = std::string(tmp) + "cocaine-session-" + std::to_string(r.root) + ".log";
    const int session_log = r.root > 0 ? open(log.c_str(), O_WRONLY | O_APPEND) : -1;
    dup2(null, STDOUT_FILENO);
    dup2(session_log >= 0 ? session_log : null, STDERR_FILENO);
    if (session_log >= 0) close(session_log);
    close(null);
  }

  if (const char* file = getenv("COCAINE_PICK_LOG")) {
    const int fd = open(file, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd >= 0) {
      const std::string line = pick_log_line(pick_fields(bytes));
      (void)write(fd, line.data(), line.size());
      close(fd);
    }
  }
  if (!understood) {
    reply.status = "error";
    pick_send(reply);
  }

  // The stand-in, decided before this Mac's AppKit is started at all.
  const char* answer = getenv("COCAINE_PICK_ANSWER");
  const char* list = getenv(r.save ? "COCAINE_PICK_SAVE" : "COCAINE_PICK_OPEN");
  if (answer || list) {
    std::string text = list ? list : "";
    double delay = 0;
    std::vector<std::string> picks;
    bool ok = true;
    if (answer) {
      text.clear();
      ok = pick_read_whole(answer, text);
      if (!ok) reply.error = std::string("cannot read the stand-in answer ") + answer + ": " + strerror(errno);
    }
    ok = ok && pick_parse_answer(text, answer != nullptr, delay, picks, reply.error);
    if (!answer) {
      const char* seconds = getenv("COCAINE_PICK_DELAY");
      delay = seconds ? atof(seconds) : 0;
    }
    if (!ok) {
      reply.status = "error";
      pick_send(reply);
    }
    if (delay > 0 && std::isfinite(delay)) pick_wait(delay);
    pick_send(pick_stand_in_reply(r, picks));
  }
  const char* real = getenv("COCAINE_PICK_REAL");
  if ((getenv("COCAINE_NO_VIEWER") || getenv("COCAINE_SCRIPT")) && !(real && strcmp(real, "1") == 0)) {
    // A run nobody is looking at must never put a panel on the screen and
    // wait for a person who isn't there.
    reply.status = "cancel";
    reply.error = "no viewer - file picker answered cancel";
    pick_send(reply);
  }
  pick_send(pick_with_panel(r, requester));
  return 0;
}
