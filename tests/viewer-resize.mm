#include "../src/viewer/viewer.mm"

#include <sys/wait.h>

#include <cstdio>
#include <vector>

int main() {
  @autoreleasepool {
    const struct { NSSize room, expected; } displays[] = {
        {{1920, 1080}, {1920, 1080}}, {{1472, 922}, {1440, 900}},
        {{1280, 800}, {1280, 800}}, {{1000, 750}, {800, 600}}, {{400, 300}, {640, 480}}};
    for (const auto& test : displays) {
      if (!NSEqualSizes(default_guest_display(test.room), test.expected)) {
        fprintf(stderr, "wrong default display for %.0fx%.0f logical points\n",
                test.room.width, test.room.height);
        return 1;
      }
    }
    const struct { ViewerScale mode; CGFloat backing, legacy; NSRect expected; } layouts[] = {
        {ViewerScale::Actual, 1, 0, {180, 160, 640, 480}},
        {ViewerScale::Actual, 2, 0, {180, 160, 640, 480}},
        {ViewerScale::Integer, 1, 0, {180, 160, 640, 480}},
        {ViewerScale::Integer, 2, 0, {20, 40, 960, 720}},
        {ViewerScale::Fit, 1, 0, {0, 25, 1000, 750}},
        {ViewerScale::Fit, 2, 0, {0, 25, 1000, 750}},
        {ViewerScale::Actual, 2, 1, {340, 280, 320, 240}},
        {ViewerScale::Actual, 2, 2, {180, 160, 640, 480}}};
    for (const auto& test : layouts) {
      const NSRect image = viewer_image_rect(640, 480, NSMakeRect(0, 0, 1000, 800),
                                             test.backing, test.mode, test.legacy);
      if (!NSEqualRects(image, test.expected) ||
          !NSEqualPoints(viewer_guest_point(NSMakePoint(500, 400), image, 640, 480), NSMakePoint(320, 240)) ||
          !NSEqualPoints(viewer_guest_point(NSMakePoint(-10, 900), image, 640, 480), NSMakePoint(0, 0)) ||
          !NSEqualPoints(viewer_guest_point(NSMakePoint(1010, -10), image, 640, 480), NSMakePoint(639, 479))) {
        fprintf(stderr, "incorrect letterbox layout or mouse mapping at %.0fx backing\n", test.backing);
        return 1;
      }
    }
    for (ViewerScale mode : {ViewerScale::Actual, ViewerScale::Integer, ViewerScale::Fit}) {
      if (!NSEqualRects(viewer_image_rect(1280, 800, NSMakeRect(0, 0, 640, 480), 1, mode),
                         NSMakeRect(0, 40, 640, 400))) {
        fprintf(stderr, "oversized display does not fit\n");
        return 1;
      }
    }
    std::string error;
    ViewerScale selected;
    CGFloat legacyPixels;
    if (unsetenv("COCAINE_VIEWER_SCALE") != 0 || unsetenv("COCAINE_VIEWER_DOUBLE") != 0 ||
        !viewer_scale_options(selected, legacyPixels, error) ||
        selected != ViewerScale::Actual || legacyPixels != 0) return 1;
    if (setenv("COCAINE_VIEWER_DOUBLE", "0", 1) != 0 ||
        !viewer_scale_options(selected, legacyPixels, error) || legacyPixels != 1) return 1;
    if (setenv("COCAINE_VIEWER_DOUBLE", "1", 1) != 0 ||
        !viewer_scale_options(selected, legacyPixels, error) || legacyPixels != 2) return 1;
    const struct { const char* name; ViewerScale mode; } choices[] = {
        {"actual", ViewerScale::Actual}, {"integer", ViewerScale::Integer}, {"fit", ViewerScale::Fit}};
    for (const auto& choice : choices) {
      if (setenv("COCAINE_VIEWER_SCALE", choice.name, 1) != 0 ||
          !viewer_scale_options(selected, legacyPixels, error) ||
          selected != choice.mode || legacyPixels != 0) {
        fprintf(stderr, "viewer scale does not override legacy doubling\n");
        return 1;
      }
    }
    if (setenv("COCAINE_VIEWER_SCALE", "bad", 1) != 0 ||
        configure_host_display(error) || error.find("COCAINE_VIEWER_SCALE") == std::string::npos) {
      fprintf(stderr, "invalid viewer mode was not rejected before guest startup\n");
      return 1;
    }
    if (unsetenv("COCAINE_VIEWER_SCALE") != 0 || setenv("COCAINE_VIEWER_DOUBLE", "bad", 1) != 0 ||
        viewer_scale_options(selected, legacyPixels, error) ||
        error.find("COCAINE_VIEWER_DOUBLE") == std::string::npos) return 1;
    if (unsetenv("COCAINE_VIEWER_DOUBLE") != 0) return 1;
    if (setenv("COCAINE_DISPLAY", "800x600", 1) != 0 || !configure_host_display(error) ||
        strcmp(getenv("COCAINE_DISPLAY"), "800x600") != 0) {
      fprintf(stderr, "explicit display override was not preserved: %s\n", error.c_str());
      return 1;
    }
    [NSApplication sharedApplication];
    NSScreen* screen = NSScreen.mainScreen;
    if (!screen) {
      fprintf(stderr, "viewer resize test skipped: no screen\n");
      return 77;
    }
    const NSRect room = screen.visibleFrame;
    NSWindow* window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(NSMidX(room) - 200, NSMidY(room) - 150, 400, 300)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    CocaineDisplayView* view = [[CocaineDisplayView alloc] initWithFrame:window.contentView.bounds];
    view.wantsLayer = YES;
    view.layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
    view.layer.masksToBounds = YES;
    window.contentView = view;
    DisplayFileHeader header = {kDisplayFileMagic, 800, 600, 1, (uint32_t)getpid(), 0, 0, 0};
    std::vector<uint8_t> pixels(1280 * 768 * 4, 0x7f);
    view->pixels = pixels.data();
    view->header = &header;
    view->pixelCapacity = pixels.size();
    view->width = 800;
    view->height = 600;
    view->picture = [CALayer layer];
    [view.layer addSublayer:view->picture];
    view->bigEndian = true;
    view->frame = &header.frame;

    const uint32_t modes[][2] = {{1024, 768}, {640, 480}, {1280, 720}, {800, 600}};
    for (CGFloat legacy : {0.0, 2.0, 1.0}) {
      view->scaleMode = ViewerScale::Actual;
      view->legacyPixelScale = legacy;
      for (const auto& mode : modes) {
        header.width = mode[0];
        header.height = mode[1];
        header.frame += 2;
        [view show];
        NSScreen* current = window.screen ?: NSScreen.mainScreen;
        const NSRect visible = current.visibleFrame;
        const CGFloat scale = current.backingScaleFactor;
        const CGFloat factor = legacy ? legacy / scale : 1;
        const CGFloat w = mode[0] * factor, h = mode[1] * factor;
        const CGFloat fit = std::min({1.0, std::max(1.0, visible.size.width - 40) / w,
                                     std::max(1.0, visible.size.height - 60) / h});
        const NSSize actual = [window contentRectForFrameRect:window.frame].size;
        if (view->width != (int)mode[0] || view->height != (int)mode[1] ||
            std::abs(actual.width - std::max(1.0, std::ceil(w * fit))) > 1 ||
            std::abs(actual.height - std::max(1.0, std::ceil(h * fit))) > 1 ||
            std::abs(view.bounds.size.width - actual.width) > 1 ||
            std::abs(view.bounds.size.height - actual.height) > 1 ||
            NSMinX(window.frame) < NSMinX(visible) || NSMaxX(window.frame) > NSMaxX(visible) ||
            NSMinY(window.frame) < NSMinY(visible) || NSMaxY(window.frame) > NSMaxY(visible)) {
          fprintf(stderr, "wrong window size for %ux%u with legacy scale %.0f: %.1fx%.1f\n",
                  mode[0], mode[1], legacy, actual.width, actual.height);
          return 1;
        }
      }
    }
    if (unsetenv("COCAINE_DISPLAY") != 0 || !configure_host_display(error)) {
      fprintf(stderr, "cannot configure host display: %s\n", error.c_str());
      return 1;
    }
    const NSSize automatic = default_guest_display(
        NSMakeSize(screen.visibleFrame.size.width - 40, screen.visibleFrame.size.height - 60));
    char expected[32];
    snprintf(expected, sizeof expected, "%.0fx%.0f", automatic.width, automatic.height);
    if (strcmp(getenv("COCAINE_DISPLAY"), expected) != 0) {
      fprintf(stderr, "automatic display does not match the host's logical space\n");
      return 1;
    }

    header.width = 640;
    header.height = 480;
    header.frame += 2;
    [view show];
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@"View"];
    for (ViewerScale mode : {ViewerScale::Actual, ViewerScale::Integer, ViewerScale::Fit}) {
      NSMenuItem* item = [menu addItemWithTitle:@"scale" action:nil keyEquivalent:@""];
      item.tag = (NSInteger)mode;
      [view chooseScale:item];
      [window setContentSize:NSMakeSize(1000, 800)];
      [view layoutSubtreeIfNeeded];
      const NSRect image = [view imageRect];
      if (header.width != 640 || header.height != 480 || view->legacyPixelScale != 0 ||
          !NSEqualRects(view->picture.frame, image) || !view->picture.contents ||
          view->scaleMode != mode) {
        fprintf(stderr, "viewer resizing or scale selection changed the guest or lost its picture\n");
        return 1;
      }
      const bool integral = std::abs(image.size.width * window.backingScaleFactor / 640 -
                                    std::round(image.size.width * window.backingScaleFactor / 640)) < 0.001;
      if (![view->picture.magnificationFilter isEqualToString:integral ? kCAFilterNearest : kCAFilterLinear]) {
        fprintf(stderr, "incorrect image filter for the effective scale\n");
        return 1;
      }
      std::vector<uint8_t> rendered(1000 * 800 * 4);
      CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
      CGContextRef bitmap = CGBitmapContextCreate(rendered.data(), 1000, 800, 8, 4000, space,
          (CGBitmapInfo)kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
      [view.layer renderInContext:bitmap];
      CGContextRelease(bitmap);
      CGColorSpaceRelease(space);
      const size_t center = (400 * 1000 + 500) * 4, border = (5 * 1000 + 500) * 4;
      if (rendered[center] != 0x7f || rendered[center + 1] != 0x7f || rendered[center + 2] != 0x7f ||
          rendered[border] || rendered[border + 1] || rendered[border + 2]) {
        fprintf(stderr, "viewer did not render the guest image with black letterboxing\n");
        return 1;
      }
    }
    int input[2];
    if (pipe(input) != 0) return 1;
    const int output = dup(STDOUT_FILENO);
    if (output < 0 || dup2(input[1], STDOUT_FILENO) < 0 ||
        fcntl(input[0], F_SETFL, O_NONBLOCK) < 0) return 1;
    close(input[1]);
    auto send = [&](NSEventType type, uint32_t kind, NSPoint point) {
      NSEvent* event = [NSEvent mouseEventWithType:type location:[view convertPoint:point toView:nil]
                                    modifierFlags:0 timestamp:0 windowNumber:window.windowNumber
                                          context:nil eventNumber:1 clickCount:1 pressure:1];
      [view send:kind event:event];
    };
    int32_t record[6];
    send(NSEventTypeLeftMouseDown, 1, NSMakePoint(500, 400));
    bool inputOK = read(input[0], record, sizeof record) == sizeof record &&
                   record[0] == 1 && record[1] == 320 && record[2] == 240;
    send(NSEventTypeLeftMouseDown, 1, NSMakePoint(500, 5));
    inputOK = inputOK && read(input[0], record, sizeof record) == -1 && errno == EAGAIN;
    // A drag released in the border must not leave the guest's button held.
    send(NSEventTypeLeftMouseUp, 2, NSMakePoint(500, 5));
    inputOK = inputOK && read(input[0], record, sizeof record) == sizeof record &&
              record[0] == 2 && record[1] == 320 && record[2] == 479;
    CGEventRef movement = CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved, CGPointZero, kCGMouseButtonLeft);
    CGEventSetIntegerValueField(movement, kCGMouseEventDeltaX, 20);
    CGEventSetIntegerValueField(movement, kCGMouseEventDeltaY, 10);
    NSEvent* event = [NSEvent eventWithCGEvent:movement];
    [view send:5 event:event];
    inputOK = inputOK && read(input[0], record, sizeof record) == sizeof record &&
              record[3] == 12 && record[4] == 6;
    view->holding = true;
    view->moveX = view->moveY = 0;
    [view send:5 event:event];
    inputOK = inputOK && read(input[0], record, sizeof record) == sizeof record &&
              record[3] == 20 && record[4] == 10;
    view->holding = false;
    // While the file picker's panel is up the guest is sent no presses,
    // movement or scrolling, but still hears of keys and buttons let go.
    auto nothing = [&] { return read(input[0], record, sizeof record) == -1 && errno == EAGAIN; };
    auto key = [&](NSEventType type) {
      return [NSEvent keyEventWithType:type location:NSZeroPoint modifierFlags:NSEventModifierFlagCommand
                             timestamp:0 windowNumber:window.windowNumber context:nil characters:@"o"
           charactersIgnoringModifiers:@"o" isARepeat:NO keyCode:31];
    };
    view->picker = getpid();
    send(NSEventTypeLeftMouseDown, 1, NSMakePoint(500, 400));
    send(NSEventTypeRightMouseDown, 3, NSMakePoint(500, 400));
    for (uint32_t kind : {5u, 6u, 7u, 22u}) [view send:kind event:event];
    [view send:10 event:key(NSEventTypeKeyDown)];
    bool pickerOK = inputOK && nothing();
    send(NSEventTypeLeftMouseUp, 2, NSMakePoint(250, 400));
    pickerOK = pickerOK && read(input[0], record, sizeof record) == sizeof record &&
               record[0] == 2 && record[1] == 160 && record[2] == 240;
    send(NSEventTypeRightMouseUp, 4, NSMakePoint(250, 400));
    pickerOK = pickerOK && read(input[0], record, sizeof record) == sizeof record && record[0] == 4;
    [view send:11 event:key(NSEventTypeKeyUp)];
    pickerOK = pickerOK && read(input[0], record, sizeof record) == sizeof record &&
               record[0] == 11 && record[3] == 31;
    [view send:12 event:key(NSEventTypeFlagsChanged)];
    pickerOK = pickerOK && read(input[0], record, sizeof record) == sizeof record &&
               record[0] == 12 && (uint32_t)record[4] == NSEventModifierFlagCommand;
    view->picker = 0;
    send(NSEventTypeLeftMouseDown, 1, NSMakePoint(250, 400));
    pickerOK = pickerOK && read(input[0], record, sizeof record) == sizeof record && record[0] == 1;
    // Losing the keyboard tells the guest no modifier key is held, where the
    // pointer last was; a viewer that never sent anything says nothing.
    [view resignedKey];
    pickerOK = pickerOK && read(input[0], record, sizeof record) == sizeof record &&
               record[0] == 12 && record[1] == 160 && record[2] == 240 &&
               record[3] == 0 && record[4] == 0 && record[5] == 0 && nothing();
    view->sentPoint = false;
    [view resignedKey];
    pickerOK = pickerOK && nothing();
    // The panel is up while the helper's flag file names a living process.
    // A stand-in process here: not this one, which the viewer would then try
    // to hand the screen to.
    const uint32_t session = (uint32_t)getpid();
    const std::string flag = pick_flag_path((int)session);
    auto setFlag = [&](const std::string& text) {
      FILE* f = fopen(flag.c_str(), "w");
      if (f) {
        fputs(text.c_str(), f);
        fclose(f);
      }
    };
    unlink(flag.c_str());
    const pid_t helper = fork();
    if (helper == 0) {
      pause();
      _exit(0);
    }
    pickerOK = pickerOK && helper > 0 && viewer_picker_up(0) == 0 && viewer_picker_up(session) == 0;
    setFlag("not a number\n");
    pickerOK = pickerOK && viewer_picker_up(session) == 0;
    setFlag(std::to_string(helper) + "\n");
    pickerOK = pickerOK && viewer_picker_up(session) == helper;
    view->root = session;
    view->manualCapture = true;
    [view checkPicker];
    pickerOK = pickerOK && view->picker == helper && !view->manualCapture;
    send(NSEventTypeLeftMouseDown, 1, NSMakePoint(500, 400));
    pickerOK = pickerOK && nothing();
    if (helper > 0) {
      kill(helper, SIGKILL);
      waitpid(helper, nullptr, 0);
    }
    pickerOK = pickerOK && viewer_picker_up(session) == 0;  // the file is still there; its process is not
    [view checkPicker];
    pickerOK = pickerOK && view->picker == 0;
    send(NSEventTypeLeftMouseDown, 1, NSMakePoint(500, 400));
    pickerOK = pickerOK && read(input[0], record, sizeof record) == sizeof record && record[0] == 1;
    unlink(flag.c_str());
    view->root = 0;
    for (uint32_t kind : {1u, 3u, 5u, 6u, 7u, 10u, 22u}) pickerOK = pickerOK && viewer_kept_from_guest(kind);
    for (uint32_t kind : {0u, 2u, 4u, 11u, 12u, (uint32_t)kViewerCaptureChanged})
      pickerOK = pickerOK && !viewer_kept_from_guest(kind);
    CFRelease(movement);
    const bool restored = dup2(output, STDOUT_FILENO) >= 0;
    close(output);
    close(input[0]);
    if (!inputOK || !restored) {
      fprintf(stderr, "letterboxed viewer forwarded incorrect mouse input\n");
      return 1;
    }
    if (!pickerOK) {
      fprintf(stderr, "viewer mishandled input, or the helper's flag, while the file picker was up\n");
      return 1;
    }
  }
  return 0;
}
