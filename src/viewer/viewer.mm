// The viewer: a real macOS window showing the guest system's display.
//
// The guest's window server draws into memory of its own. The cocaine process
// running it copies what changes into a small shared file (see
// Process::share_display) and counts the changes there, and this program,
// started alongside, maps the file and shows each new picture: it is handed
// to the window's layer, so this Mac's graphics hardware does the scaling.
// The mouse and the keyboard go back the other way down a pipe.

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "viewer/picker.h"
#include "viewer/viewer.h"

enum class ViewerScale { Actual, Integer, Fit };

static bool viewer_scale_options(ViewerScale& mode, CGFloat& legacyPixels, std::string& error) {
  mode = ViewerScale::Actual;
  legacyPixels = 0;
  if (const char* scale = getenv("COCAINE_VIEWER_SCALE")) {
    if (strcmp(scale, "actual") == 0) mode = ViewerScale::Actual;
    else if (strcmp(scale, "integer") == 0) mode = ViewerScale::Integer;
    else if (strcmp(scale, "fit") == 0) mode = ViewerScale::Fit;
    else {
      error = "COCAINE_VIEWER_SCALE must be actual, integer, or fit";
      return false;
    }
  } else if (const char* doublePixels = getenv("COCAINE_VIEWER_DOUBLE")) {
    if (strcmp(doublePixels, "0") != 0 && strcmp(doublePixels, "1") != 0) {
      error = "COCAINE_VIEWER_DOUBLE must be 0 or 1";
      return false;
    }
    legacyPixels = strcmp(doublePixels, "0") == 0 ? 1 : 2;
  }
  return true;
}

static NSSize default_guest_display(NSSize available) {
  const NSSize modes[] = {{640, 480}, {800, 600}, {1024, 768}, {1152, 864},
                          {1280, 800}, {1280, 1024}, {1360, 768}, {1440, 900},
                          {1600, 1200}, {1680, 1050}, {1920, 1080}, {1920, 1200},
                          {2560, 1440}, {2560, 1600}};
  NSSize best = modes[0];
  for (const NSSize mode : modes) {
    if (mode.width <= available.width && mode.height <= available.height &&
        mode.width * mode.height > best.width * best.height) best = mode;
  }
  return best;
}

bool configure_host_display(std::string& error) {
  ViewerScale mode;
  CGFloat legacyPixels;
  if (!viewer_scale_options(mode, legacyPixels, error)) return false;
  if (getenv("COCAINE_DISPLAY")) return true;
  @autoreleasepool {
    NSScreen* screen = NSScreen.mainScreen;
    const NSSize size = screen ? default_guest_display(
        NSMakeSize(screen.visibleFrame.size.width - 40, screen.visibleFrame.size.height - 60))
                               : NSMakeSize(1280, 800);
    if (!screen) fprintf(stderr, "cocaine: no host screen; using a 1280x800 guest display\n");
    char value[32];
    snprintf(value, sizeof value, "%.0fx%.0f", size.width, size.height);
    if (setenv("COCAINE_DISPLAY", value, 0) == 0) return true;
    error = std::string("cannot set COCAINE_DISPLAY: ") + strerror(errno);
    return false;
  }
}

static NSRect viewer_image_rect(uint32_t width, uint32_t height, NSRect bounds,
                                CGFloat backing, ViewerScale mode, CGFloat legacyPixels = 0) {
  if (!width || !height || bounds.size.width <= 0 || bounds.size.height <= 0) return NSZeroRect;
  const CGFloat fit = std::min(bounds.size.width / width, bounds.size.height / height);
  CGFloat scale = fit;
  if (mode == ViewerScale::Actual) scale = std::min(fit, legacyPixels ? legacyPixels / backing : 1);
  if (mode == ViewerScale::Integer && fit * backing >= 1)
    scale = std::floor(fit * backing) / backing;
  const CGFloat w = width * scale, h = height * scale;
  return NSMakeRect(NSMinX(bounds) + std::floor((bounds.size.width - w) * backing / 2) / backing,
                    NSMinY(bounds) + std::floor((bounds.size.height - h) * backing / 2) / backing, w, h);
}

static NSPoint viewer_guest_point(NSPoint point, NSRect image, uint32_t width, uint32_t height) {
  return NSMakePoint(std::clamp((point.x - NSMinX(image)) * width / image.size.width,
                                0.0, (double)width - 1),
                     std::clamp((NSMaxY(image) - point.y) * height / image.size.height,
                                0.0, (double)height - 1));
}

static NSSize viewer_content_size(uint32_t width, uint32_t height, ViewerScale mode,
                                  CGFloat legacyPixels, NSScreen* screen) {
  const CGFloat backing = screen ? screen.backingScaleFactor : 1;
  const NSRect room = screen ? screen.visibleFrame : NSMakeRect(0, 0, 1280, 800);
  const NSRect available = NSMakeRect(0, 0, std::max(1.0, room.size.width - 40),
                                     std::max(1.0, room.size.height - 60));
  const NSRect image = viewer_image_rect(width, height, available, backing, mode, legacyPixels);
  return NSMakeSize(std::max(1.0, std::ceil(image.size.width)),
                     std::max(1.0, std::ceil(image.size.height)));
}

// The file picker's helper while it has a panel on the screen for this
// session, else 0: the helper leaves its process number in a file named
// after the session's root, and the panel is up for as long as that process
// lives (a helper that was killed leaves the file behind).
static pid_t viewer_picker_up(uint32_t root) {
  if (!root) return 0;
  FILE* f = fopen(pick_flag_path((int)root).c_str(), "r");
  if (!f) return 0;
  int pid = 0;
  if (fscanf(f, "%d", &pid) != 1) pid = 0;
  fclose(f);
  return pid > 0 && kill(pid, 0) == 0 ? (pid_t)pid : 0;
}

// What the guest isn't sent while the picker is up: the app that asked for
// it is waiting and would only save these up and act on them all afterwards.
// Presses, movement and the wheel are kept back; a key or a button let go
// still goes through, so nothing is left held down in the guest.
static bool viewer_kept_from_guest(uint32_t kind) {
  return kind == 1 || kind == 3 || (kind >= 5 && kind <= 7) || kind == 10 || kind == 22;
}

@interface CocaineDisplayView : NSView {
 @public
  const uint8_t* pixels;
  const DisplayFileHeader* header;
  size_t pixelCapacity;
  int width, height;
  ViewerScale scaleMode;
  CGFloat legacyPixelScale;
  CALayer* picture;
  bool bigEndian;
  pid_t writer;
  const volatile uint32_t* frame;  // the file's count of pictures
  uint32_t shown;                  // the count when the layer was last given one
  double scrollX, scrollY;         // scrolling not yet amounting to a whole line
  const volatile uint32_t* held;   // when the guest last held the pointer (DisplayFileHeader)
  bool holding;                    // the Mac's pointer is hidden and kept still for it
  bool manualCapture, suppressAutoHold, swallowCaptureKeyUp;
  bool skipMove;                   // the next movement includes putting the pointer back
  double moveX, moveY;             // movement not yet amounting to a whole point
  uint32_t root;                   // the session's root process (DisplayFileHeader)
  pid_t picker;                    // the file picker's helper while its panel is up, else 0
  bool pickerYielded;              // ... and it has been told it may come to the front
  bool sentPoint;                  // lastX and lastY are where the guest was last told the pointer is
  int32_t lastX, lastY;
}
- (void)show;
- (void)hold:(bool)hold;
- (NSRect)imageRect;
- (void)layoutPicture;
- (void)sizeToDisplay;
- (void)chooseScale:(NSMenuItem*)item;
- (void)checkPicker;
- (void)resignedKey;
@end

@implementation CocaineDisplayView
- (NSRect)imageRect {
  const CGFloat backing = self.window.backingScaleFactor ?: 1;
  return viewer_image_rect(width, height, self.bounds, backing, scaleMode, legacyPixelScale);
}
- (void)layoutPicture {
  const CGFloat backing = self.window.backingScaleFactor ?: 1;
  const NSRect rect = [self imageRect];
  const CGFloat physicalScale = width ? rect.size.width * backing / width : 0;
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  picture.frame = rect;
  picture.magnificationFilter = physicalScale >= 1 && std::abs(physicalScale - std::round(physicalScale)) < 0.001
                                   ? kCAFilterNearest : kCAFilterLinear;
  picture.minificationFilter = kCAFilterTrilinear;
  self.layer.contentsScale = backing;
  picture.contentsScale = backing;
  [CATransaction commit];
}
- (void)layout {
  [super layout];
  [self layoutPicture];
}
- (void)viewDidChangeBackingProperties {
  [super viewDidChangeBackingProperties];
  [self layoutPicture];
}
- (void)sizeToDisplay {
  NSWindow* window = self.window;
  NSScreen* screen = window.screen ?: NSScreen.mainScreen;
  const NSSize size = viewer_content_size(width, height, scaleMode, legacyPixelScale, screen);
  const NSRect old = window.frame;
  NSRect next = [window frameRectForContentRect:NSMakeRect(0, 0, size.width, size.height)];
  next.origin = NSMakePoint(NSMidX(old) - next.size.width / 2, NSMidY(old) - next.size.height / 2);
  if (screen) {
    const NSRect room = screen.visibleFrame;
    next.origin.x = std::clamp(next.origin.x, NSMinX(room), std::max(NSMinX(room), NSMaxX(room) - next.size.width));
    next.origin.y = std::clamp(next.origin.y, NSMinY(room), std::max(NSMinY(room), NSMaxY(room) - next.size.height));
  }
  [window setFrame:next display:YES];
  [self layoutPicture];
}
- (void)chooseScale:(NSMenuItem*)item {
  scaleMode = (ViewerScale)item.tag;
  legacyPixelScale = 0;
  moveX = moveY = 0;
  for (NSMenuItem* choice in item.menu.itemArray)
    choice.state = choice == item ? NSControlStateValueOn : NSControlStateValueOff;
  [self sizeToDisplay];
}
// Hands the layer the picture as it is now. The guest's pixels are 32 bits
// each with the unused byte first, in the guest CPU's byte order. They are
// copied first: the file goes on changing underneath. The frame number is
// odd while the writer is changing the picture, so a copy is kept only if
// the number was even before and the same after; otherwise the next tick
// tries again.
- (void)show {
  const uint32_t before = __atomic_load_n(frame, __ATOMIC_ACQUIRE);
  if (before & 1) return;
  const uint32_t w = __atomic_load_n(&header->width, __ATOMIC_RELAXED);
  const uint32_t h = __atomic_load_n(&header->height, __ATOMIC_RELAXED);
  const uint64_t bytes64 = (uint64_t)w * h * 4;
  if (!w || !h || bytes64 > pixelCapacity) return;
  const size_t bytes = (size_t)bytes64;
  CFDataRef copy = CFDataCreate(NULL, pixels, (CFIndex)bytes);
  // The copy's reads must be done before the number is read again. An
  // acquire load alone only holds back what comes after it, and this Mac's
  // CPU may otherwise finish the copy's reads later.
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  if (__atomic_load_n(frame, __ATOMIC_RELAXED) != before) {
    CFRelease(copy);
    return;
  }
  shown = before;
  if (width != (int)w || height != (int)h) {
    width = (int)w;
    height = (int)h;
    moveX = moveY = 0;
    [self sizeToDisplay];
  }
  CGDataProviderRef data = CGDataProviderCreateWithCFData(copy);
  CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
  CGImageRef image = CGImageCreate(width, height, 8, 32, (size_t)width * 4, space,
                                   (CGBitmapInfo)kCGImageAlphaNoneSkipFirst |
                                       (bigEndian ? kCGBitmapByteOrder32Big : kCGBitmapByteOrder32Little),
                                   data, NULL, false, kCGRenderingIntentDefault);
  [CATransaction begin];
  [CATransaction setDisableActions:YES];  // no cross-fade between pictures
  picture.contents = (__bridge id)image;
  [CATransaction commit];
  CGImageRelease(image);
  CGColorSpaceRelease(space);
  CGDataProviderRelease(data);
  CFRelease(copy);
}
// The mouse and keys go back to the guest down the pipe this program was
// started with as its output, six numbers an event: its kind (as Mac OS X
// numbers events: 1 and 2 the left button down and up, 10 a key down, and so
// on), where on the guest's display, then three that depend on the kind.
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { return YES; }
- (void)send:(uint32_t)kind event:(NSEvent*)event {
  if (picker && viewer_kept_from_guest(kind)) return;
  const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
  const NSRect image = [self imageRect];
  if (NSIsEmptyRect(image)) return;
  if (!holding && (kind == 1 || kind == 3 || kind == 22) && !NSPointInRect(p, image)) return;
  const NSPoint guest = viewer_guest_point(p, image, width, height);
  int32_t key = 0, extra = 0, more = 0;
  if (kind >= 10 && kind <= 12) {
    // The key code, which Macs have kept the same since, then the modifier
    // keys held (also unchanged) with the character typed below them, and
    // whether the key is repeating.
    key = event.keyCode;
    extra = (int32_t)(event.modifierFlags & 0xFFFF0000u);
    if (kind != 12) {
      if (event.charactersIgnoringModifiers.length) {
        extra |= [event.charactersIgnoringModifiers characterAtIndex:0] & 0xFFFF;
      }
      more = event.isARepeat;
    }
  } else if (kind >= 1 && kind <= 4) {
    more = (int32_t)event.clickCount;  // 2 for the second click of a double click
  } else if (kind >= 5 && kind <= 7) {
    // Scale movement with the displayed picture unless a game has captured
    // the pointer for camera controls; keep fractions between events.
    if (skipMove) {
      skipMove = false;
    } else {
      moveX += event.deltaX * (holding ? 1 : width / image.size.width);
      moveY += event.deltaY * (holding ? 1 : height / image.size.height);
    }
    key = (int32_t)moveX;
    extra = (int32_t)moveY;
    moveX -= key;
    moveY -= extra;
    more = 1;
  } else if (kind == 22) {
    // Whole lines, down then across; a trackpad's fine movement is saved up.
    const BOOL fine = event.hasPreciseScrollingDeltas;
    scrollY += fine ? event.scrollingDeltaY / 10 : event.scrollingDeltaY;
    scrollX += fine ? event.scrollingDeltaX / 10 : event.scrollingDeltaX;
    key = (int32_t)scrollY;
    more = (int32_t)scrollX;
    if (!key && !more) return;
    scrollY -= key;
    scrollX -= more;
  }
  const int32_t record[6] = {(int32_t)kind, (int32_t)guest.x, (int32_t)guest.y, key, extra, more};
  sentPoint = true;
  lastX = record[1];
  lastY = record[2];
  (void)write(STDOUT_FILENO, record, sizeof record);
}
- (void)keyDown:(NSEvent*)event {
  if (picker) return;
  const NSEventModifierFlags modifiers = event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
  if (event.keyCode == 46 && modifiers == (NSEventModifierFlagCommand | NSEventModifierFlagOption)) {
    if (!event.isARepeat) {
      manualCapture = !holding;
      suppressAutoHold = holding;
      [self hold:manualCapture];
    }
    swallowCaptureKeyUp = true;
    return;
  }
  [self send:10 event:event];
}
- (void)keyUp:(NSEvent*)event {
  if (event.keyCode == 46 && swallowCaptureKeyUp) {
    swallowCaptureKeyUp = false;
    return;
  }
  [self send:11 event:event];
}
- (void)flagsChanged:(NSEvent*)event { [self send:12 event:event]; }
// A click on the guest's picture while the picker is up is a person looking
// for the panel: this window is nearly the screen's size and may have
// covered it. The panel's program is brought to the front instead.
- (bool)raisePicker {
  if (!picker) return false;
  NSRunningApplication* helper = [NSRunningApplication runningApplicationWithProcessIdentifier:picker];
  if (helper) {
    if (@available(macOS 14.0, *)) [NSApp yieldActivationToApplication:helper];
    [helper activateWithOptions:0];
  }
  return true;
}
- (void)mouseDown:(NSEvent*)event {
  if (![self raisePicker]) [self send:1 event:event];
}
- (void)mouseUp:(NSEvent*)event { [self send:2 event:event]; }
- (void)rightMouseDown:(NSEvent*)event {
  if (![self raisePicker]) [self send:3 event:event];
}
- (void)rightMouseUp:(NSEvent*)event { [self send:4 event:event]; }
- (void)mouseMoved:(NSEvent*)event { [self send:5 event:event]; }
- (void)mouseDragged:(NSEvent*)event { [self send:6 event:event]; }
- (void)rightMouseDragged:(NSEvent*)event { [self send:7 event:event]; }
- (void)scrollWheel:(NSEvent*)event { [self send:22 event:event]; }
// While a program in the guest holds the pointer, as a game looking around
// with the mouse does, the Mac's pointer is hidden and kept still in the
// middle of the window, so it can't wander off and click another app; the
// mouse's movement still comes through. It comes back once the guest has
// let go for a second and a half, or this app isn't the one in front.
- (void)hold:(bool)hold {
  if (hold == holding) return;
  holding = hold;
  moveX = moveY = 0;
  skipMove = false;
  const int32_t record[6] = {(int32_t)kViewerCaptureChanged, hold ? 1 : 0, 0, 0, 0, 0};
  (void)write(STDOUT_FILENO, record, sizeof record);
  self.window.title = hold ? @"cocaine: Mac OS X 10.5 - mouse captured (Cmd-Option-M to release)"
                           : @"cocaine: Mac OS X 10.5";
  if (hold) {
    const NSRect r = [self.window convertRectToScreen:[self convertRect:self.bounds toView:nil]];
    const CGFloat top = NSScreen.screens.count ? NSMaxY(NSScreen.screens[0].frame) : 0;
    CGWarpMouseCursorPosition(CGPointMake(NSMidX(r), top - NSMidY(r)));
    CGAssociateMouseAndMouseCursorPosition(false);
    [NSCursor hide];
    skipMove = true;
  } else {
    CGAssociateMouseAndMouseCursorPosition(true);
    [NSCursor unhide];
  }
}
// Looked at about eight times a second.
- (void)checkPicker {
  const pid_t now = viewer_picker_up(root);
  if (now != picker) {
    picker = now;
    pickerYielded = false;
    if (now) {
      manualCapture = false;
      [self hold:false];
    }
  }
  if (picker && !pickerYielded) {
    // The helper asks to come to the front as it starts, which this Mac may
    // refuse while this program is the one in front, unless this program
    // says it may. The helper may not be known to the system just yet.
    NSRunningApplication* helper = [NSRunningApplication runningApplicationWithProcessIdentifier:picker];
    if (helper) {
      if (@available(macOS 14.0, *)) [NSApp yieldActivationToApplication:helper];
      pickerYielded = true;
    }
  }
}
// The window is no longer the one keys go to. A modifier key held at that
// moment (the Command of the Command-O that brought the picker up, say) is
// let go where this window can't see it, and the guest would go on thinking
// it held: so the guest is told that none is.
- (void)resignedKey {
  if (sentPoint) {
    const int32_t record[6] = {12, lastX, lastY, 0, 0, 0};
    (void)write(STDOUT_FILENO, record, sizeof record);
  }
  manualCapture = false;
  suppressAutoHold = false;
  [self hold:false];
}
- (void)tick:(NSTimer*)timer {
  if (__atomic_load_n(frame, __ATOMIC_ACQUIRE) != shown) [self show];
  const uint32_t at = __atomic_load_n(held, __ATOMIC_ACQUIRE);
  const uint32_t now = (uint32_t)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000);
  const bool active = NSApp.isActive && self.window.isKeyWindow;
  if (!active) {
    manualCapture = false;
    suppressAutoHold = false;
  }
  [self hold:active && !picker && (manualCapture || (!suppressAutoHold && at != 0 && now - at < 1500))];
  static int ticks;
  if (++ticks % 15 == 0) [self checkPicker];
  if (ticks % 60 == 0 && kill(writer, 0) != 0) {
    [self hold:false];
    [NSApp terminate:nil];
  }
}
@end

int run_viewer(const char* path) {
  const int fd = open(path, O_RDONLY);
  struct stat st;
  if (fd < 0 || fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(DisplayFileHeader)) return 1;
  const uint8_t* map = (const uint8_t*)mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED) return 1;
  const DisplayFileHeader* head = (const DisplayFileHeader*)map;
  if (head->magic != kDisplayFileMagic || !head->width || !head->height ||
      head->width > 4096 || head->height > 4096 ||
      (off_t)(sizeof *head + (size_t)head->width * head->height * 4) > st.st_size) {
    fprintf(stderr, "invalid viewer display file\n");
    return 1;
  }
  unlink(path);  // it lives on for as long as the two programs have it mapped

  @autoreleasepool {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    NSScreen* screen = NSScreen.mainScreen;
    const NSRect room = screen ? screen.visibleFrame : NSMakeRect(0, 0, 1280, 800);
    ViewerScale mode;
    CGFloat legacyPixels;
    std::string error;
    if (!viewer_scale_options(mode, legacyPixels, error)) {
      fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
    const NSSize size = viewer_content_size(head->width, head->height, mode, legacyPixels, screen);
    const CGFloat w = size.width, h = size.height;
    const NSRect frame = NSMakeRect(room.origin.x + (room.size.width - w) / 2,
                                    room.origin.y + (room.size.height - h) / 2, w, h);
    NSWindow* window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    window.title = @"cocaine: Mac OS X 10.5";
    CocaineDisplayView* view = [[CocaineDisplayView alloc] initWithFrame:frame];
    view->pixels = map + sizeof *head;
    view->header = head;
    view->pixelCapacity = (size_t)st.st_size - sizeof *head;
    view->width = (int)head->width;
    view->height = (int)head->height;
    view->scaleMode = mode;
    view->legacyPixelScale = legacyPixels;
    view->bigEndian = head->big_endian != 0;
    view->writer = (pid_t)head->writer;
    view->frame = &head->frame;
    view->held = &head->pointer_held;
    view->root = head->root;
    view.layer = [CALayer layer];  // a layer of the view's own, which AppKit then leaves alone
    view.wantsLayer = YES;
    view.layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
    view.layer.masksToBounds = YES;
    view->picture = [CALayer layer];
    [view.layer addSublayer:view->picture];
    window.contentView = view;
    [view layoutPicture];
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@""];
    NSMenuItem* appItem = [menu addItemWithTitle:@"cocaine" action:nil keyEquivalent:@""];
    NSMenu* appMenu = [[NSMenu alloc] initWithTitle:@"cocaine"];
    NSMenuItem* quit = [appMenu addItemWithTitle:@"Quit cocaine" action:@selector(performClose:) keyEquivalent:@"q"];
    quit.target = window;
    appItem.submenu = appMenu;
    NSMenuItem* viewItem = [menu addItemWithTitle:@"View" action:nil keyEquivalent:@""];
    NSMenu* viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
    NSArray<NSString*>* titles = @[@"Actual Size", @"Integer Scale", @"Fit"];
    for (NSInteger index = 0; index < (NSInteger)titles.count; index++) {
      NSMenuItem* item = [viewMenu addItemWithTitle:titles[index] action:@selector(chooseScale:)
                                     keyEquivalent:[NSString stringWithFormat:@"%ld", (long)index + 1]];
      item.target = view;
      item.tag = index;
      item.state = index == (NSInteger)mode ? NSControlStateValueOn : NSControlStateValueOff;
    }
    viewItem.submenu = viewMenu;
    NSApp.mainMenu = menu;
    [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowDidChangeScreenNotification
                                                      object:window queue:nil
                                                  usingBlock:^(NSNotification* note) {
                                                    [view sizeToDisplay];
                                                  }];
    [view show];
    window.acceptsMouseMovedEvents = YES;
    [window makeFirstResponder:view];
    signal(SIGPIPE, SIG_IGN);
    [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowWillCloseNotification
                                                      object:window
                                                       queue:nil
                                                  usingBlock:^(NSNotification* note) {
                                                    // Closing the window ends the whole run:
                                                    // say so (kind 0) before going.
                                                    [view hold:false];
                                                    const int32_t closed[6] = {0, 0, 0, 0, 0, 0};
                                                    (void)write(STDOUT_FILENO, closed, sizeof closed);
                                                    [NSApp terminate:nil];
                                                  }];
    [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowDidResignKeyNotification
                                                      object:window
                                                       queue:nil
                                                  usingBlock:^(NSNotification* note) {
                                                    [view resignedKey];
                                                  }];
    // Twice a screen's usual refresh rate; a tick with nothing new costs nothing.
    NSTimer* timer = [NSTimer timerWithTimeInterval:1.0 / 120 target:view selector:@selector(tick:) userInfo:nil repeats:YES];
    // In the common modes, so that the picture keeps moving while a mouse
    // button is held down in the window.
    [[NSRunLoop currentRunLoop] addTimer:timer forMode:NSRunLoopCommonModes];
    // For checking the viewer without looking at it: COCAINE_VIEWER_SNAPSHOT
    // names a file to write what the window shows to, twelve seconds in.
    if (const char* snapshot = getenv("COCAINE_VIEWER_SNAPSHOT")) {
      NSString* to = [NSString stringWithUTF8String:snapshot];
      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 12 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        const NSSize size = view.bounds.size;
        NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL
                                                                        pixelsWide:(NSInteger)size.width
                                                                        pixelsHigh:(NSInteger)size.height
                                                                     bitsPerSample:8
                                                                   samplesPerPixel:4
                                                                          hasAlpha:YES
                                                                          isPlanar:NO
                                                                    colorSpaceName:NSDeviceRGBColorSpace
                                                                       bytesPerRow:0
                                                                      bitsPerPixel:0];
        NSGraphicsContext* context = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
        [view.layer renderInContext:context.CGContext];
        [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:to atomically:YES];
      });
    }
    [window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [NSApp run];
  }
  return 0;
}
