#pragma once

#include <cstdint>
#include <string>

// Resolve the default once, before starting the session, so every process
// inherits the same guest display even if the viewer later changes screens.
bool configure_host_display(std::string& error);

// The start of the file a guest's display is copied into for the viewer.
constexpr uint32_t kDisplayFileMagic = 0x636F6344;  // "cocD"
constexpr uint32_t kViewerCaptureChanged = 0xFFFF0001;
struct DisplayFileHeader {
  uint32_t magic;
  uint32_t width, height;  // current mode, protected by frame's sequence count
  uint32_t big_endian;  // the pixels are in a PowerPC guest's byte order
  uint32_t writer;      // the process keeping the picture up to date
  uint32_t frame;       // goes up each time the picture in the file changes
  // When a program in the guest last held the pointer, putting it back in
  // place again and again as a game looking around with the mouse does: the
  // time since startup in milliseconds (CLOCK_UPTIME_RAW), 0 if never. While
  // that's recent the viewer hides the Mac's pointer and keeps it still.
  uint32_t pointer_held;
  uint32_t reserved;
};

// Shows the display in that file in a window, until the window is closed or
// the writer goes away.
int run_viewer(const char* path, const std::string& session,
               const std::string& self, const std::string& sysroot);
