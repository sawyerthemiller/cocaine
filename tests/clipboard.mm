#import <AppKit/AppKit.h>

#include "viewer/clipboard.h"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

extern char** environ;

static std::string session_for(pid_t pid) {
  char tmp[1024];
  if (!confstr(_CS_DARWIN_USER_TEMP_DIR, tmp, sizeof tmp)) return {};
  const std::string prefix = "cocaine.session." + std::to_string(pid) + ".";
  DIR* dir = opendir(tmp);
  if (!dir) return {};
  std::string result;
  while (dirent* entry = readdir(dir)) {
    if (std::string(entry->d_name).find(prefix) == 0) {
      result = entry->d_name;
      break;
    }
  }
  closedir(dir);
  return result;
}

static bool exchange(FILE* in, FILE* out, const std::string& command, std::string& response) {
  if (fwrite(command.data(), 1, command.size(), in) != command.size() || fflush(in)) return false;
  response.clear();
  for (;;) {
    const int c = fgetc(out);
    if (c == EOF) return false;
    if (c == '\n') return true;
    response += (char)c;
  }
}

static std::string encode(NSString* value) {
  NSData* data = [value dataUsingEncoding:NSUTF8StringEncoding];
  return std::string([data base64EncodedStringWithOptions:0].UTF8String);
}

static bool guest_copy(const char* executable, const char* root, const std::string& session,
                       bool powerpc, NSString* value) {
  int input[2];
  if (pipe(input)) return false;
  std::vector<std::string> args = {executable, "--sysroot", root, "--arch",
                                   powerpc ? "ppc" : "i386", "--session", session,
                                   "--", std::string(root) + "/usr/bin/pbcopy"};
  std::vector<char*> child_args;
  for (std::string& arg : args) child_args.push_back(arg.data());
  child_args.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, input[0], 0);
  posix_spawn_file_actions_addclose(&actions, input[1]);
  pid_t pid;
  const int rc = posix_spawn(&pid, executable, &actions, nullptr, child_args.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(input[0]);
  if (rc) {
    close(input[1]);
    return false;
  }
  NSData* data = [value dataUsingEncoding:NSUTF8StringEncoding];
  const char* bytes = (const char*)data.bytes;
  size_t sent = 0;
  while (sent < data.length) {
    const ssize_t n = write(input[1], bytes + sent, data.length - sent);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    sent += (size_t)n;
  }
  close(input[1]);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  return sent == data.length && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool guest_client(const char* executable, const char* root, const std::string& session,
                         bool powerpc, NSString* value, const char* operation,
                         NSData* payload = nil, NSData** output_data = nullptr) {
  const char* script = R"PY(
import base64, ctypes, os, struct, sys
ptr = ctypes.c_void_p
objc = ctypes.CDLL("/usr/lib/libobjc.A.dylib")
ctypes.CDLL("/System/Library/Frameworks/AppKit.framework/AppKit")
cf = ctypes.CDLL("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")
objc.objc_getClass.argtypes = [ctypes.c_char_p]
objc.objc_getClass.restype = ptr
objc.sel_registerName.argtypes = [ctypes.c_char_p]
objc.sel_registerName.restype = ptr
cf.CFStringCreateWithCString.argtypes = [ptr, ctypes.c_char_p, ctypes.c_uint]
cf.CFStringCreateWithCString.restype = ptr
cf.CFDataCreate.argtypes = [ptr, ptr, ctypes.c_long]
cf.CFDataCreate.restype = ptr
cf.CFDataGetLength.argtypes = [ptr]
cf.CFDataGetLength.restype = ctypes.c_long
cf.CFDataGetBytePtr.argtypes = [ptr]
cf.CFDataGetBytePtr.restype = ptr
cf.CFPropertyListCreateFromXMLData.argtypes = [ptr, ptr, ctypes.c_uint, ptr]
cf.CFPropertyListCreateFromXMLData.restype = ptr
pb = ctypes.CDLL("/System/Library/Frameworks/ApplicationServices.framework/Frameworks/HIServices.framework/HIServices")
pb.PasteboardCreate.argtypes = [ptr, ctypes.POINTER(ptr)]
pb.PasteboardSynchronize.argtypes = [ptr]
pb.PasteboardGetItemIdentifier.argtypes = [ptr, ctypes.c_uint, ctypes.POINTER(ptr)]
pb.PasteboardCopyItemFlavorData.argtypes = [ptr, ptr, ptr, ctypes.POINTER(ptr)]
pb.PasteboardClear.argtypes = [ptr]
pb.PasteboardPutItemFlavor.argtypes = [ptr, ptr, ptr, ptr, ctypes.c_uint]
ls = ctypes.CDLL("/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/LaunchServices")
ls.UTTypeCreatePreferredIdentifierForTag.argtypes = [ptr, ptr, ptr]
ls.UTTypeCreatePreferredIdentifierForTag.restype = ptr
send0 = ctypes.CFUNCTYPE(ptr, ptr, ptr)(("objc_msgSend", objc))
send1 = ctypes.CFUNCTYPE(ptr, ptr, ptr, ptr)(("objc_msgSend", objc))
send2 = ctypes.CFUNCTYPE(ctypes.c_ubyte, ptr, ptr, ptr, ptr)(("objc_msgSend", objc))
send2ptr = ctypes.CFUNCTYPE(ptr, ptr, ptr, ptr, ptr)(("objc_msgSend", objc))
send3ptr = ctypes.CFUNCTYPE(ptr, ptr, ptr, ptr, ctypes.c_uint, ptr)(("objc_msgSend", objc))
send1uint = ctypes.CFUNCTYPE(ctypes.c_uint, ptr, ptr, ptr)(("objc_msgSend", objc))
send0float = ctypes.CFUNCTYPE(ctypes.c_float, ptr, ptr)(("objc_msgSend", objc))
sel = objc.sel_registerName
pool = send0(objc.objc_getClass("NSAutoreleasePool"), sel("new"))
board = send0(objc.objc_getClass("NSPasteboard"), sel("generalPasteboard"))
kind_name = "NSStringPboardType"
if sys.argv[1].startswith("image-"):
    kind_name = "NSTIFFPboardType"
    if sys.argv[1] == "image-png-read":
        kind_name = "Apple PNG pasteboard type"
elif sys.argv[1].startswith("files-"):
    kind_name = "NSFilenamesPboardType"
elif sys.argv[1].startswith("rtf-"):
    kind_name = "NSRTFPboardType"
elif sys.argv[1].startswith("html-"):
    kind_name = "NSHTMLPboardType"
kind = cf.CFStringCreateWithCString(None, kind_name, 0x08000100)
if sys.argv[1] in ("carbon", "binary", "image-carbon-read", "files-carbon-read", "image-png-write", "image-jpeg-write"):
    name = cf.CFStringCreateWithCString(None, "com.apple.pasteboard.clipboard", 0x08000100)
    pasteboard = ptr()
    if pb.PasteboardCreate(name, ctypes.byref(pasteboard)):
        raise RuntimeError("Carbon could not open clipboard")
    pb.PasteboardSynchronize(pasteboard)
    if sys.argv[1] in ("binary", "image-png-write", "image-jpeg-write"):
        flavor_name = {"binary": "cocaine.test.binary", "image-png-write": "public.png", "image-jpeg-write": "public.jpeg"}[sys.argv[1]]
        flavor = cf.CFStringCreateWithCString(None, flavor_name, 0x08000100)
        raw = "x" if sys.argv[1] == "binary" else base64.b64decode(sys.argv[2])
        buf = ctypes.create_string_buffer(raw, len(raw) + 1)
        data = cf.CFDataCreate(None, buf, len(raw))
        if pb.PasteboardClear(pasteboard) or pb.PasteboardPutItemFlavor(pasteboard, ptr(1), flavor, data, 0):
            raise RuntimeError("Carbon could not write clipboard data")
        print "ok"
    else:
        cls_name = "com.apple.nspboard-type" if sys.argv[1] == "image-carbon-read" else "com.apple.ostype"
        tag_name = {"image-carbon-read": "NSTIFFPboardType", "files-carbon-read": "furl"}.get(sys.argv[1], "utxt")
        cls = cf.CFStringCreateWithCString(None, cls_name, 0x08000100)
        tag = cf.CFStringCreateWithCString(None, tag_name, 0x08000100)
        flavor = ls.UTTypeCreatePreferredIdentifierForTag(cls, tag, None)
        item, data = ptr(), ptr()
        if pb.PasteboardGetItemIdentifier(pasteboard, 1, ctypes.byref(item)) or pb.PasteboardCopyItemFlavorData(pasteboard, item, flavor, ctypes.byref(data)):
            raise RuntimeError("Carbon could not read clipboard data")
        raw = ctypes.string_at(cf.CFDataGetBytePtr(data), cf.CFDataGetLength(data))
        print base64.b64encode(raw.decode("utf-16-be").encode("utf-8") if sys.argv[1] == "carbon" else raw)
elif sys.argv[1] in ("image-write", "files-write", "rtf-write", "html-write"):
    raw = base64.b64decode(sys.argv[2])
    buf = ctypes.create_string_buffer(raw, len(raw) + 1)
    data = cf.CFDataCreate(None, buf, len(raw))
    types = send1(objc.objc_getClass("NSArray"), sel("arrayWithObject:"), kind)
    send2(board, sel("declareTypes:owner:"), types, None)
    if sys.argv[1] == "files-write":
        value = cf.CFPropertyListCreateFromXMLData(None, data, 0, None)
        if not value or not send2(board, sel("setPropertyList:forType:"), value, kind):
            raise RuntimeError("Cocoa could not write file clipboard")
    elif not send2(board, sel("setData:forType:"), data, kind):
        raise RuntimeError("Cocoa could not write clipboard data")
    print "ok"
elif sys.argv[1] in ("image-read", "image-png-read", "files-read", "rtf-read", "html-read"):
    data = send1(board, sel("dataForType:"), kind)
    if not data:
        raise RuntimeError("Cocoa could not read clipboard data")
    if sys.argv[1] == "rtf-read":
        attributed = send0(objc.objc_getClass("NSAttributedString"), sel("alloc"))
        attributed = send2ptr(attributed, sel("initWithRTF:documentAttributes:"), data, None)
        font_key = cf.CFStringCreateWithCString(None, "NSFont", 0x08000100)
        color_key = cf.CFStringCreateWithCString(None, "NSColor", 0x08000100)
        font = send3ptr(attributed, sel("attribute:atIndex:effectiveRange:"), font_key, 0, None)
        color = send3ptr(attributed, sel("attribute:atIndex:effectiveRange:"), color_key, 0, None)
        manager = send0(objc.objc_getClass("NSFontManager"), sel("sharedFontManager"))
        space = cf.CFStringCreateWithCString(None, "NSDeviceRGBColorSpace", 0x08000100)
        color = send1(color, sel("colorUsingColorSpaceName:"), space)
        if (not attributed or not font or not color or not (send1uint(manager, sel("traitsOfFont:"), font) & 2) or
                send0float(color, sel("redComponent")) < 0.8 or send0float(color, sel("greenComponent")) > 0.2):
            raise RuntimeError("Cocoa could not restore rich text formatting")
    raw = ctypes.string_at(cf.CFDataGetBytePtr(data), cf.CFDataGetLength(data))
    print base64.b64encode(raw)
elif sys.argv[1] == "image-can-read":
    image_class = objc.objc_getClass("NSImage")
    if not send1(image_class, sel("canInitWithPasteboard:"), board):
        raise RuntimeError("NSImage cannot read the clipboard")
    image = send0(image_class, sel("alloc"))
    image = send1(image, sel("initWithPasteboard:"), board)
    if not image or not send0(image, sel("isValid")):
        raise RuntimeError("NSImage could not decode the clipboard")
    print "ok"
elif sys.argv[1] == "volume-root":
    libc = ctypes.CDLL("/usr/lib/libSystem.B.dylib")
    libc.getattrlist.argtypes = [ctypes.c_char_p, ptr, ptr, ctypes.c_ulong, ctypes.c_ulong]
    attrs = ctypes.create_string_buffer(struct.pack("=HHIIIII", 5, 0, 0xa0, 0, 0, 0, 0))
    def ids(path):
        data = ctypes.create_string_buffer(20)
        if libc.getattrlist(path, attrs, data, len(data), 0):
            raise RuntimeError("cannot read root volume object IDs")
        return struct.unpack("=IIIII", data.raw)[1:]
    root_id = ids("/")
    child_id = ids("/System")
    nested_id = ids("/System/Library")
    volume = "/.vol/%d/" % os.stat("/System").st_dev
    if (root_id != (2, 0, 2, 0) or ids(volume + "@") != root_id or ids(volume + "2") != root_id or
            child_id[0] == 2 or child_id[2:] != (2, 0) or nested_id[2] != child_id[0]):
        raise RuntimeError("inconsistent root volume object IDs")
    print "ok"
elif sys.argv[1] == "write":
    text = cf.CFStringCreateWithCString(None, base64.b64decode(sys.argv[2]), 0x08000100)
    types = send1(objc.objc_getClass("NSArray"), sel("arrayWithObject:"), kind)
    send2(board, sel("declareTypes:owner:"), types, None)
    if not send2(board, sel("setString:forType:"), text, kind):
        raise RuntimeError("Cocoa could not write clipboard text")
    print "ok"
else:
    text = send1(board, sel("stringForType:"), kind)
    if not text:
        raise RuntimeError("Cocoa did not find clipboard text")
    data = send0(text, sel("UTF8String"))
    print base64.b64encode(ctypes.string_at(data))
)PY";
  int output[2];
  if (pipe(output)) return false;
  std::vector<std::string> args = {executable, "--sysroot", root, "--arch",
                                   powerpc ? "ppc" : "i386", "--session", session, "--",
                                   std::string(root) + "/usr/bin/python2.5", "-c", script,
                                   operation, payload ? std::string([payload base64EncodedStringWithOptions:0].UTF8String) :
                                     strcmp(operation, "write") == 0 ? encode(value) : ""};
  std::vector<char*> child_args;
  for (std::string& arg : args) child_args.push_back(arg.data());
  child_args.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
  posix_spawn_file_actions_addclose(&actions, output[0]);
  pid_t pid;
  const int rc = posix_spawn(&pid, executable, &actions, nullptr, child_args.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(output[1]);
  if (rc) {
    close(output[0]);
    return false;
  }
  std::string result;
  char buf[4096];
  ssize_t n;
  while ((n = read(output[0], buf, sizeof buf)) > 0) result.append(buf, (size_t)n);
  close(output[0]);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  if (output_data) {
    if (result.empty() || result.back() != '\n') return false;
    result.pop_back();
    NSString* base64 = [[NSString alloc] initWithBytes:result.data() length:result.size()
                                               encoding:NSASCIIStringEncoding];
    *output_data = [[NSData alloc] initWithBase64EncodedString:base64 options:0];
    return n == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 && *output_data != nil;
  }
  return n == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
         result == ((strcmp(operation, "write") == 0 || strcmp(operation, "binary") == 0 ||
                     strcmp(operation, "image-write") == 0 || strcmp(operation, "files-write") == 0 ||
                     strcmp(operation, "rtf-write") == 0 || strcmp(operation, "html-write") == 0 ||
                     strcmp(operation, "image-png-write") == 0 || strcmp(operation, "image-jpeg-write") == 0 ||
                     strcmp(operation, "image-can-read") == 0 || strcmp(operation, "volume-root") == 0)
                    ? "ok\n" : encode(value) + "\n");
}

static NSData* plist_data(id value) {
  return [NSPropertyListSerialization dataWithPropertyList:value
    format:NSPropertyListXMLFormat_v1_0 options:0 error:nil];
}

static id plist_value(NSData* data) {
  return data ? [NSPropertyListSerialization propertyListWithData:data options:NSPropertyListImmutable
                                                           format:nil error:nil] : nil;
}

static NSString* temporary_folder(NSString* parent) {
  NSString* pattern = [parent stringByAppendingPathComponent:@"cocaine-clipboard-test.XXXXXX"];
  std::vector<char> path(strlen(pattern.fileSystemRepresentation) + 1);
  memcpy(path.data(), pattern.fileSystemRepresentation, path.size());
  char* result = mkdtemp(path.data());
  return result ? [NSString stringWithUTF8String:result] : nil;
}

static bool same_file(NSString* first, NSString* second) {
  struct stat a, b;
  return first && second && stat(first.fileSystemRepresentation, &a) == 0 &&
         stat(second.fileSystemRepresentation, &b) == 0 && a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

static bool preview_clipboard(const char* executable, const char* root, bool powerpc) {
  NSString* folder = temporary_folder(@"/private/tmp");
  if (!folder) return false;
  NSString* imagePath = [folder stringByAppendingPathComponent:@"image.png"];
  NSString* beforePath = [folder stringByAppendingPathComponent:@"before.raw"];
  NSString* afterPath = [folder stringByAppendingPathComponent:@"after.raw"];
  NSString* closedPath = [folder stringByAppendingPathComponent:@"closed.raw"];
  NSString* action = [NSString stringWithFormat:
    @"wait 20; shot %@; down 136 12; wait 1; dragto 170 34; wait 1; up 170 34; wait 12; shot %@; "
     "click 19 32; wait 6; shot %@", beforePath, afterPath, closedPath];
  setenv("COCAINE_NO_VIEWER", "1", 1);
  setenv("COCAINE_DISPLAY", "1024x768", 1);
  setenv("COCAINE_SCRIPT", action.UTF8String, 1);

  NSBitmapImageRep* bitmap = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nil
    pixelsWide:1200 pixelsHigh:900 bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES
    isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
  NSUInteger pixel[] = {20, 220, 40, 255};
  for (NSInteger y = 0; y < bitmap.pixelsHigh; y++)
    for (NSInteger x = 0; x < bitmap.pixelsWide; x++) [bitmap setPixel:pixel atX:x y:y];
  NSData* png = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
  bool ok = png && [png writeToFile:imagePath atomically:YES];

  std::vector<std::string> args = {executable, "--sysroot", root, "--arch", powerpc ? "ppc" : "i386",
    "--", std::string(root) + "/Applications/Preview.app/Contents/MacOS/Preview"};
  std::vector<char*> child_args;
  for (std::string& arg : args) child_args.push_back(arg.data());
  child_args.push_back(nullptr);
  pid_t preview = -1;
  if (ok && posix_spawn(&preview, executable, nullptr, nullptr, child_args.data(), environ)) ok = false;
  std::string session;
  if (ok) {
    for (int i = 0; i < 100 && session.empty(); i++) {
      usleep(100000);
      session = session_for(preview);
    }
    ok = !session.empty();
  }
  NSPasteboard* host = [NSPasteboard pasteboardWithUniqueName];
  {
    ClipboardBridge bridge;
    ok = ok && bridge.start(executable, root, session, powerpc);
    [host clearContents];
    ok = ok && [host writeObjects:@[[NSURL fileURLWithPath:imagePath]]] && bridge.sync(host);
    if (ok) {
      for (int i = 0; i < 550; i++) {
        NSDictionary* attrs = [NSFileManager.defaultManager attributesOfItemAtPath:closedPath error:nil];
        if ([attrs fileSize] == 1024 * 768 * 4) break;
        usleep(100000);
      }
      NSData* before = [NSData dataWithContentsOfFile:beforePath];
      NSData* after = [NSData dataWithContentsOfFile:afterPath];
      NSData* closed = [NSData dataWithContentsOfFile:closedPath];
      ok = before.length == 1024 * 768 * 4 && after.length == before.length && closed.length == before.length;
      if (ok) {
        const unsigned char* a = (const unsigned char*)before.bytes;
        const unsigned char* b = (const unsigned char*)after.bytes;
        const unsigned char* d = (const unsigned char*)closed.bytes;
        const size_t rgb = powerpc ? 1 : 0;
        size_t greenBefore = 0, greenAfter = 0, greenClosed = 0;
        for (size_t i = 0; i < before.length; i += 4) {
          if (a[i + rgb] < 80 && a[i + rgb + 1] > 150 && a[i + rgb + 2] < 100) greenBefore++;
          if (b[i + rgb] < 80 && b[i + rgb + 1] > 150 && b[i + rgb + 2] < 100) greenAfter++;
          if (d[i + rgb] < 80 && d[i + rgb + 1] > 150 && d[i + rgb + 2] < 100) greenClosed++;
        }
        ok = greenAfter > greenBefore + 10000 && greenClosed < greenAfter - 10000;
        if (!ok) fprintf(stderr, "Preview image pixels: before %zu, after %zu, closed %zu\n",
                         greenBefore, greenAfter, greenClosed);
      } else {
        fprintf(stderr, "Preview clipboard screenshots are missing or incomplete\n");
      }
    }
  }
  [host releaseGlobally];
  if (preview > 0) {
    int status;
    pid_t waited;
    do { waited = waitpid(preview, &status, WNOHANG); } while (waited < 0 && errno == EINTR);
    if (waited == 0) {
      kill(preview, SIGKILL);
      while (waitpid(preview, &status, 0) < 0 && errno == EINTR) {}
    } else {
      fprintf(stderr, "Preview exited while closing the clipboard image\n");
      ok = false;
    }
  }
  NSError* error = nil;
  if (![NSFileManager.defaultManager removeItemAtPath:folder error:&error]) {
    fprintf(stderr, "cannot remove Preview test folder: %s\n", error.localizedDescription.UTF8String);
    ok = false;
  }
  if (!ok) fprintf(stderr, "Preview clipboard test failed for %s\n", powerpc ? "ppc" : "i386");
  return ok;
}

int main(int argc, char** argv) {
  if (argc != 4) return 1;
  @autoreleasepool {
    signal(SIGPIPE, SIG_IGN);
    NSString* python = [NSString stringWithFormat:@"%s/usr/bin/python2.5", argv[2]];
    if (access(python.fileSystemRepresentation, X_OK) != 0) return 77;
    if (strcmp(argv[3], "--preview") == 0) {
      NSString* preview = [NSString stringWithFormat:@"%s/Applications/Preview.app/Contents/MacOS/Preview", argv[2]];
      if (access(preview.fileSystemRepresentation, X_OK) != 0) return 77;
      return preview_clipboard(argv[1], argv[2], true) && preview_clipboard(argv[1], argv[2], false) ? 0 : 1;
    }
    std::ifstream source(argv[3]);
    std::string script((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
    if (script.empty()) return 1;

    for (bool powerpc : {true, false}) {
      int in[2], out[2];
      if (pipe(in) || pipe(out)) return 1;
      std::vector<std::string> args = {argv[1], "--sysroot", argv[2], "--arch",
                                       powerpc ? "ppc" : "i386", "--env",
                                       "PYTHONDONTWRITEBYTECODE=1", "--",
                                       python.fileSystemRepresentation, "-c", script};
      std::vector<char*> child_args;
      for (std::string& arg : args) child_args.push_back(arg.data());
      child_args.push_back(nullptr);
      posix_spawn_file_actions_t actions;
      posix_spawn_file_actions_init(&actions);
      posix_spawn_file_actions_adddup2(&actions, in[0], 0);
      posix_spawn_file_actions_adddup2(&actions, out[1], 1);
      posix_spawn_file_actions_addclose(&actions, in[1]);
      posix_spawn_file_actions_addclose(&actions, out[0]);
      pid_t pid;
      const int rc = posix_spawn(&pid, argv[1], &actions, nullptr, child_args.data(), environ);
      posix_spawn_file_actions_destroy(&actions);
      close(in[0]);
      close(out[1]);
      if (rc) return 1;
      FILE* writer = fdopen(in[1], "w");
      FILE* reader = fdopen(out[0], "r");
      std::string response;
      bool ok = writer && reader && exchange(writer, reader, "G\n", response) && response == "E";
      const std::string session = session_for(pid);
      NSPasteboard* host = [NSPasteboard pasteboardWithUniqueName];
      NSString* stageRoot = nil;
      bool exportedStage = false;
      {
        ClipboardBridge bridge;
        ok = ok && !session.empty() && bridge.start(argv[1], argv[2], session, powerpc);
        ok = ok && guest_client(argv[1], argv[2], session, powerpc, @"", "volume-root");
        NSString* first = @"caf\u00e9 \U0001f34d";
        [host clearContents];
        ok = ok && [host setString:first forType:NSPasteboardTypeString] &&
             bridge.sync_host_if_changed(host);
        ok = ok && exchange(writer, reader, "G\n", response) && response == "T" + encode(first);
        ok = ok && guest_client(argv[1], argv[2], session, powerpc, first, "read");
        ok = ok && guest_client(argv[1], argv[2], session, powerpc, first, "carbon");
        if (!ok) fprintf(stderr, "host-to-guest text failed\n");

        NSString* second = @"guest \u6f22\u5b57 \U0001f680";
        ok = ok && exchange(writer, reader, "S" + encode(second) + "\n", response) && response == "O";
        ok = ok && bridge.sync(host) && [[host stringForType:NSPasteboardTypeString] isEqualToString:second];
        if (!ok) fprintf(stderr, "guest-to-host text failed\n");
        NSString* cocoaText = @"Cocoa \u00e9 \u6f22 \U0001f34d";
        ok = ok && guest_client(argv[1], argv[2], session, powerpc, cocoaText, "write");
        ok = ok && bridge.sync_host_if_changed(host) && bridge.sync(host) &&
             [[host stringForType:NSPasteboardTypeString] isEqualToString:cocoaText];
        if (!ok) fprintf(stderr, "guest Cocoa copy failed\n");
        const NSInteger change = host.changeCount;
        ok = ok && bridge.sync(host) && host.changeCount == change;
        ok = ok && guest_client(argv[1], argv[2], session, powerpc, @"", "binary");
        ok = ok && bridge.sync(host) &&
             [[host stringForType:NSPasteboardTypeString] isEqualToString:cocoaText];
        if (!ok) fprintf(stderr, "guest non-text clipboard failed\n");

        [host clearContents];
        NSData* binary = [NSData dataWithBytes:"x" length:1];
        ok = ok && [host setData:binary forType:@"cocaine.test.binary"] && bridge.sync(host);
        ok = ok && exchange(writer, reader, "G\n", response) && response == "E";
        ok = ok && bridge.sync(host) && [host dataForType:@"cocaine.test.binary"] != nil;
        if (!ok) fprintf(stderr, "non-text clipboard failed\n");

        NSMutableString* large = [NSMutableString stringWithCapacity:131072];
        for (int i = 0; i < 8192; i++) [large appendString:@"0123456789abcdef"];
        [host clearContents];
        ok = ok && [host setString:large forType:NSPasteboardTypeString] && bridge.sync(host);
        ok = ok && exchange(writer, reader, "G\n", response) && response == "T" + encode(large);
        ok = ok && guest_client(argv[1], argv[2], session, powerpc, large, "read");
        if (!ok) fprintf(stderr, "large host-to-guest text failed\n");

        [host clearContents];
        ok = ok && [host setString:@"" forType:NSPasteboardTypeString] && bridge.sync(host);
        ok = ok && exchange(writer, reader, "G\n", response) && response == "T";
        ok = ok && guest_client(argv[1], argv[2], session, powerpc, @"", "read");
        if (!ok) fprintf(stderr, "empty text failed\n");

        NSFont* bold = [NSFont boldSystemFontOfSize:17];
        NSAttributedString* formatted = [[NSAttributedString alloc] initWithString:@"Bold and plain"
          attributes:@{NSFontAttributeName: bold, NSForegroundColorAttributeName: NSColor.redColor}];
        NSData* rtf = [formatted RTFFromRange:NSMakeRange(0, formatted.length) documentAttributes:@{}];
        NSData* html = [@"<html><body><b>Bold</b> and plain</body></html>" dataUsingEncoding:NSUTF8StringEncoding];
        for (NSArray* rich in @[@[@"rtf", NSPasteboardTypeRTF, rtf],
                                @[@"html", NSPasteboardTypeHTML, html]]) {
          NSString* kind = rich[0];
          NSData* original = rich[2];
          [host clearContents];
          ok = ok && [host setString:@"Bold and plain" forType:NSPasteboardTypeString] &&
               [host setData:original forType:rich[1]] && bridge.sync(host);
          ok = ok && guest_client(argv[1], argv[2], session, powerpc, @"Bold and plain", "read");
          NSData* receivedRich = nil;
          std::string readOperation = std::string(kind.UTF8String) + "-read";
          std::string writeOperation = std::string(kind.UTF8String) + "-write";
          ok = ok && guest_client(argv[1], argv[2], session, powerpc, @"",
                                  readOperation.c_str(), nil, &receivedRich) &&
               [receivedRich isEqualToData:original];
          ok = ok && guest_client(argv[1], argv[2], session, powerpc, @"",
                                  writeOperation.c_str(), original) && bridge.sync(host) &&
               [[host dataForType:rich[1]] isEqualToData:original];
          if ([kind isEqualToString:@"rtf"]) {
            NSAttributedString* restored = [[NSAttributedString alloc]
              initWithRTF:[host dataForType:NSPasteboardTypeRTF] documentAttributes:nil];
            NSFont* font = [restored attribute:NSFontAttributeName atIndex:0 effectiveRange:nil];
            NSColor* color = [[restored attribute:NSForegroundColorAttributeName atIndex:0 effectiveRange:nil]
              colorUsingColorSpace:NSColorSpace.genericRGBColorSpace];
            ok = ok && [restored.string isEqualToString:formatted.string] &&
                 ([[NSFontManager sharedFontManager] traitsOfFont:font] & NSBoldFontMask) &&
                 color && color.redComponent > 0.8 && color.greenComponent < 0.2;
          }
          if (!ok) fprintf(stderr, "%s formatted clipboard failed\n", kind.UTF8String);
        }

        bool copied = ok && guest_copy(argv[1], argv[2], session, powerpc, large);
        bool synced = copied && bridge.sync(host);
        NSString* copiedText = [host stringForType:NSPasteboardTypeString];
        if (!copied || !synced || ![copiedText isEqualToString:large])
          fprintf(stderr, "pbcopy result: copied %d, synced %d, length %lu\n",
                  copied, synced, (unsigned long)copiedText.length);
        ok = copied && synced && [copiedText isEqualToString:large];
        if (!ok) fprintf(stderr, "guest pbcopy text failed\n");

        NSBitmapImageRep* bitmap = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nil
          pixelsWide:128 pixelsHigh:128 bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES
          isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
        for (NSInteger i = 0; i < bitmap.bytesPerRow * bitmap.pixelsHigh; i++)
          bitmap.bitmapData[i] = (i % 4 == 3) ? 255 : (unsigned char)((i * 13 + i / 127) & 255);
        NSData* png = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        NSData* tiff = [bitmap representationUsingType:NSBitmapImageFileTypeTIFF properties:@{}];
        NSData* jpeg = [bitmap representationUsingType:NSBitmapImageFileTypeJPEG properties:@{}];
        [host clearContents];
        ok = ok && png && tiff && jpeg && [host setData:png forType:NSPasteboardTypePNG] &&
             [host setString:first forType:NSPasteboardTypeString] && bridge.sync(host) &&
             exchange(writer, reader, "G\n", response) && response == "T" + encode(first);
        NSData* received = nil;
        ok = ok && guest_client(argv[1], argv[2], session, powerpc, @"", "image-read", nil, &received);
        NSBitmapImageRep* converted = received ? [NSBitmapImageRep imageRepWithData:received] : nil;
        ok = ok && converted && converted.pixelsWide == bitmap.pixelsWide &&
             converted.pixelsHigh == bitmap.pixelsHigh;
        for (NSInteger y = 0; ok && y < bitmap.pixelsHigh; y++) {
          for (NSInteger x = 0; ok && x < bitmap.pixelsWide; x++) {
            NSUInteger expected[4] = {}, actual[4] = {};
            [bitmap getPixel:expected atX:x y:y];
            [converted getPixel:actual atX:x y:y];
            ok = memcmp(expected, actual, sizeof expected) == 0;
          }
        }
        received = nil;
        ok = ok && guest_client(argv[1], argv[2], session, powerpc, @"", "image-carbon-read", nil, &received) &&
             [NSBitmapImageRep imageRepWithData:received] != nil;
        if (!ok) fprintf(stderr, "host-to-guest image failed\n");
        ok = ok && guest_client(argv[1], argv[2], session, powerpc, @"", "image-write", tiff);
        ok = ok && bridge.sync(host) && [[host dataForType:NSPasteboardTypeTIFF] isEqualToData:tiff];
        ok = ok && guest_client(argv[1], argv[2], session, powerpc, @"", "image-png-write", png);
        ok = ok && bridge.sync(host) && [[host dataForType:NSPasteboardTypePNG] isEqualToData:png] &&
             [NSBitmapImageRep imageRepWithData:[host dataForType:NSPasteboardTypeTIFF]] != nil;
        [host clearContents];
        ok = ok && [host setData:jpeg forType:@"public.jpeg"] && bridge.sync(host);
        received = nil;
        ok = ok && guest_client(argv[1], argv[2], session, powerpc, @"", "image-read", nil, &received) &&
             [NSBitmapImageRep imageRepWithData:received] != nil;
        ok = ok && guest_client(argv[1], argv[2], session, powerpc, @"", "image-jpeg-write", jpeg) &&
             bridge.sync(host) && [[host dataForType:@"public.jpeg"] isEqualToData:jpeg] &&
             [NSBitmapImageRep imageRepWithData:[host dataForType:NSPasteboardTypeTIFF]] != nil;
        if (!ok) fprintf(stderr, "guest-to-host image failed\n");

        NSFileManager* manager = NSFileManager.defaultManager;
        NSString* folder = temporary_folder(@"/private/tmp");
        NSString* build = [@(argv[1]) stringByDeletingLastPathComponent];
        NSString* protectedFolder = temporary_folder(build);
        NSString* one = [folder stringByAppendingPathComponent:@"one file.txt"];
        NSString* two = [folder stringByAppendingPathComponent:@"caf\u00e9.txt"];
        NSString* privateFile = [protectedFolder stringByAppendingPathComponent:@"private.txt"];
        NSString* privateDirectory = [protectedFolder stringByAppendingPathComponent:@"folder"];
        NSString* nested = [privateDirectory stringByAppendingPathComponent:@"nested.txt"];
        NSData* fileData = [@"clipboard file contents" dataUsingEncoding:NSUTF8StringEncoding];
        ok = ok && folder && protectedFolder && [fileData writeToFile:one atomically:YES] &&
             [fileData writeToFile:two atomically:YES] && [fileData writeToFile:privateFile atomically:YES];
        for (NSArray* entry in @[@[@"image.png", png], @[@"image.tiff", tiff], @[@"image.jpg", jpeg]]) {
          NSString* imageFile = [folder stringByAppendingPathComponent:entry[0]];
          ok = ok && [entry[1] writeToFile:imageFile atomically:YES];
          [host clearContents];
          ok = ok && [host writeObjects:@[[NSURL fileURLWithPath:imageFile]]] && bridge.sync(host);
          ok = ok && guest_client(argv[1], argv[2], session, powerpc, @"", "image-can-read");
          received = nil;
          ok = ok && guest_client(argv[1], argv[2], session, powerpc, @"", "image-read", nil, &received);
          NSBitmapImageRep* pastedFileImage = received ? [NSBitmapImageRep imageRepWithData:received] : nil;
          ok = ok && pastedFileImage && pastedFileImage.pixelsWide == bitmap.pixelsWide &&
               pastedFileImage.pixelsHigh == bitmap.pixelsHigh;
          if ([entry[0] isEqualToString:@"image.png"]) {
            received = nil;
            ok = ok && guest_client(argv[1], argv[2], session, powerpc, @"", "image-png-read", nil, &received) &&
                 [received isEqualToData:png];
          }
          received = nil;
          ok = ok && guest_client(argv[1], argv[2], session, powerpc, @"", "files-read", nil, &received);
          NSArray* imagePaths = plist_value(received);
          ok = ok && imagePaths.count == 1 && same_file(imagePaths[0], imageFile);
        }
        if (!ok) fprintf(stderr, "image file clipboard failed\n");
        ok = ok && [manager createDirectoryAtPath:privateDirectory withIntermediateDirectories:NO
                                       attributes:nil error:nil] && [fileData writeToFile:nested atomically:YES];
        NSString* internalLink = [privateDirectory stringByAppendingPathComponent:@"link"];
        ok = ok && symlink("nested.txt", internalLink.fileSystemRepresentation) == 0;
        [host clearContents];
        ok = ok && [host writeObjects:@[[NSURL fileURLWithPath:one], [NSURL fileURLWithPath:two],
                                        [NSURL fileURLWithPath:privateFile], [NSURL fileURLWithPath:privateDirectory]]] &&
             bridge.sync(host);
        received = nil;
        ok = ok && guest_client(argv[1], argv[2], session, powerpc, @"", "files-read", nil, &received);
        NSArray* paths = plist_value(received);
        ok = ok && [paths isKindOfClass:NSArray.class] && paths.count == 4 &&
             same_file(paths[0], one) && same_file(paths[1], two) &&
             [[NSData dataWithContentsOfFile:paths[2]] isEqualToData:fileData] &&
             [[NSData dataWithContentsOfFile:[paths[3] stringByAppendingPathComponent:@"nested.txt"]] isEqualToData:fileData] &&
             [[NSData dataWithContentsOfFile:[paths[3] stringByAppendingPathComponent:@"link"]] isEqualToData:fileData];
        if ([protectedFolder hasPrefix:[NSHomeDirectory() stringByAppendingString:@"/Library/"]]) {
          ok = ok && !same_file(paths[2], privateFile) && !same_file(paths[3], privateDirectory);
          if (ok) stageRoot = [[paths[2] stringByDeletingLastPathComponent] stringByDeletingLastPathComponent];
        }
        received = nil;
        ok = ok && guest_client(argv[1], argv[2], session, powerpc, @"", "files-carbon-read", nil, &received);
        NSString* fileURL = [[NSString alloc] initWithData:received encoding:NSUTF8StringEncoding];
        ok = ok && same_file([NSURL URLWithString:fileURL].path, one);
        if (!ok) fprintf(stderr, "host-to-guest files failed\n");
        NSData* fileList = plist_data(@[one, two]);
        ok = ok && guest_client(argv[1], argv[2], session, powerpc, @"", "files-write", fileList);
        ok = ok && bridge.sync(host);
        NSArray<NSURL*>* urls = [host readObjectsForClasses:@[NSURL.class]
          options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
        ok = ok && urls.count == 2 && same_file(urls[0].path, one) && same_file(urls[1].path, two);
        if (!ok) fprintf(stderr, "guest-to-host files failed\n");
        NSString* systemFile = [@(argv[2]) stringByAppendingPathComponent:@"usr/bin/pbpaste"];
        [host clearContents];
        ok = ok && [host writeObjects:@[[NSURL fileURLWithPath:systemFile]]] && bridge.sync(host);
        received = nil;
        ok = ok && guest_client(argv[1], argv[2], session, powerpc, @"", "files-read", nil, &received);
        NSArray* systemPaths = plist_value(received);
        ok = ok && systemPaths.count == 1 && [systemPaths[0] isEqualToString:@"/usr/bin/pbpaste"];
        ok = ok && guest_client(argv[1], argv[2], session, powerpc, @"", "files-write",
                                plist_data(@[@"/usr/bin/pbpaste"])) && bridge.sync(host);
        NSArray<NSURL*>* systemURLs = [host readObjectsForClasses:@[NSURL.class]
          options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
        ok = ok && systemURLs.count == 1 && same_file(systemURLs[0].path, systemFile);
        if (!ok) fprintf(stderr, "system-root file mapping failed\n");
        if ([protectedFolder hasPrefix:[NSHomeDirectory() stringByAppendingString:@"/Library/"]]) {
          if (powerpc && ok) {
            NSData* stagedList = plist_data(@[paths[2]]);
            ok = guest_client(argv[1], argv[2], session, powerpc, @"", "files-write", stagedList) &&
                 bridge.sync(host);
            NSArray<NSURL*>* stagedURLs = [host readObjectsForClasses:@[NSURL.class]
              options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
            ok = ok && stagedURLs.count == 1 && same_file(stagedURLs[0].path, paths[2]);
            exportedStage = ok;
            if (!ok) fprintf(stderr, "staged file export failed\n");
          }
          NSString* unsafe = [privateDirectory stringByAppendingPathComponent:@"unsafe"];
          ok = ok && symlink("../../outside", unsafe.fileSystemRepresentation) == 0;
          [host clearContents];
          ok = ok && [host writeObjects:@[[NSURL fileURLWithPath:privateDirectory]]] &&
               !bridge.sync(host) && bridge.recoverable_error() &&
               exchange(writer, reader, "G\n", response) && response == "E";
          [host clearContents];
          ok = ok && [host setString:first forType:NSPasteboardTypeString] && bridge.sync(host) &&
               exchange(writer, reader, "G\n", response) && response == "T" + encode(first);
          if (!ok) fprintf(stderr, "unsafe staged link recovery failed\n");
        }
        NSError* cleanupError = nil;
        if (folder && ![manager removeItemAtPath:folder error:&cleanupError]) {
          fprintf(stderr, "cannot remove test folder: %s\n", cleanupError.localizedDescription.UTF8String);
          ok = false;
        }
        if (protectedFolder && ![manager removeItemAtPath:protectedFolder error:&cleanupError]) {
          fprintf(stderr, "cannot remove test folder: %s\n", cleanupError.localizedDescription.UTF8String);
          ok = false;
        }
      }
      if (stageRoot) {
        NSFileManager* manager = NSFileManager.defaultManager;
        const bool exists = [manager fileExistsAtPath:stageRoot];
        if (exists != exportedStage) {
          fprintf(stderr, "clipboard staging lifetime is incorrect\n");
          ok = false;
        }
        if (exists && ![manager removeItemAtPath:stageRoot error:nil]) {
          fprintf(stderr, "cannot remove retained test staging folder\n");
          ok = false;
        }
      }
      [host releaseGlobally];
      if (writer) fclose(writer); else close(in[1]);
      if (reader) fclose(reader); else close(out[0]);
      int status;
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
      if (!ok || !WIFEXITED(status) || WEXITSTATUS(status)) {
        fprintf(stderr, "clipboard bridge test failed for %s\n", powerpc ? "ppc" : "i386");
        return 1;
      }
      fprintf(stderr, "clipboard bridge ok for %s\n", powerpc ? "ppc" : "i386");
    }
  }
  return 0;
}
