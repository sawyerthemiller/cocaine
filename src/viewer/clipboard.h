#pragma once

#import <AppKit/AppKit.h>

#include <cstdint>
#include <string>
#include <sys/types.h>

class ClipboardBridge {
 public:
  ClipboardBridge() = default;
  ~ClipboardBridge();
  ClipboardBridge(const ClipboardBridge&) = delete;
  ClipboardBridge& operator=(const ClipboardBridge&) = delete;

  bool start(const std::string& self, const std::string& sysroot,
             const std::string& session, bool powerpc);
  bool sync(NSPasteboard* host);
  bool sync_host_if_changed(NSPasteboard* host);
  bool recoverable_error() const { return recoverable_; }

 private:
  bool request(const std::string& command, std::string& response);
  bool reject_host(NSInteger change);
  bool guest_identity(NSString* path, NSDictionary** identity);
  bool guest_path(NSString* path, NSString** result);
  bool host_path(NSDictionary* entry, NSString** result);
  bool stage_file(NSString* path, NSString** result);
  int input_ = -1, output_ = -1;
  pid_t child_ = -1;
  NSInteger host_change_ = -1;
  bool have_guest_ = false;
  NSDictionary* guest_content_ = nil;
  std::string sysroot_;
  std::string staging_;
  size_t stage_count_ = 0;
  bool staging_exported_ = false;
  bool connection_failed_ = false;
  bool recoverable_ = false;
};

void start_clipboard_bridge(const std::string& self, const std::string& sysroot,
                            const std::string& session, bool powerpc);
void stop_clipboard_bridge();
void send_viewer_input(const int32_t (&record)[6]);
