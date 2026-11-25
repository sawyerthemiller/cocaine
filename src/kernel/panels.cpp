// Open and save panels, shown by this Mac's file picker.
//
// Leopard's own panel can't run here: building one loads its browser and
// sidebar, which want the Finder's services and Disk Arbitration, and the
// program dies before the panel is on the screen. So the panel a guest
// creates is left hollow, and running it asks this Mac's picker instead.
//
// Three things are taken over in AppKit's NSSavePanel (NSOpenPanel is a
// subclass that only forwards to it):
//
//  - Two steps of its init, the ones that load the browser and the sidebar.
//    The rest of the init runs as Apple wrote it, so the object is a real
//    NSSavePanel in every other way, with no window on the screen.
//  - The settings that lived in the browser or the nib's controls (may it
//    choose folders, the prompt, the folder to start in ...): kept here, by
//    panel, and read back from here.
//  - The three methods every way of running a panel ends in, and the three
//    every way of reading its answer ends in. Running sends one request to
//    the picker's helper (`cocaine --pick`, see viewer/picker.h) and waits
//    for the reply on the calling thread. A sheet or a modeless panel must
//    return first and answer later, so those schedule a method of cocaine's
//    own, -_cocainePickNow:, on the main thread's run loop.
//
// A handler here runs as the guest's own thread, between two of its
// instructions. Guest code it calls can throw, and Leopard's exceptions are
// longjmps: a throw caught further up never comes back to the handler. So
// nothing is held across a guest call, and what must happen is done first.
//
// COCAINE_GUEST_PANELS=1 leaves all of it alone (Leopard's own panels).

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "kernel/hooks.h"
#include "kernel/process.h"
#include "viewer/picker.h"

extern char** environ;

namespace {

// Guards everything below. Held for moments only, and never across a call
// into the guest. A forking thread takes it (panel_fork_lock).
std::mutex g_mu;
// One request to the picker at a time: a second guest thread waits its turn.
// A flag, not a lock, since a pick can last minutes and a fork must not wait
// for it.
std::condition_variable g_turn;
bool g_asking = false;

// Guest functions, selectors and classes, looked up once.
std::unordered_map<std::string, u32> g_names;

struct Panel {
  int mode = 0;  // 1 an open panel, 2 a save panel; 0 not looked at yet
  // What the browser and the nib's controls held in Leopard's panel.
  bool multi = false, files = false, dirs = false, aliases = false, packages = false;
  bool extension_hidden = false;
  // Guest objects, each retained while it is here.
  u32 directory = 0, prompt = 0, message = 0, label = 0, accessory = 0;

  // A sheet or modeless panel begun and not yet answered.
  bool begun = false;
  PickRequest request;
  u32 delegate = 0, selector = 0, context = 0;

  // The last answer: guest paths, none after a cancel.
  std::vector<std::string> paths;
  bool answered_folder = false;  // -directory gives the answer's folder, until setDirectory: is next called
};
std::unordered_map<u32, Panel> g_panels;

// ---- talking to the guest -----------------------------------------------------

u32 fn(GuestCall& c, const char* name) {
  const std::string key = std::string("fn:") + name;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (auto it = g_names.find(key); it != g_names.end()) return it->second;
  }
  const u32 v = c.symbol(name);
  if (!v) log_warn("the guest has no %s: its open and save panels will not work", name);
  std::lock_guard<std::mutex> lk(g_mu);
  g_names[key] = v;
  return v;
}

// A selector or a class by name. The name's bytes stay in guest memory for
// good: there is one per name.
u32 named(GuestCall& c, const char* function, const char* name) {
  const std::string key = std::string(function) + ":" + name;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (auto it = g_names.find(key); it != g_names.end()) return it->second;
  }
  const u32 at = c.scratch((u32)strlen(name) + 1);
  if (!at) return 0;
  memcpy(c.mem.host(at), name, strlen(name) + 1);
  const u32 v = c.call(fn(c, function), {at});
  std::lock_guard<std::mutex> lk(g_mu);
  g_names[key] = v;
  return v;
}
u32 sel(GuestCall& c, const char* name) { return named(c, "sel_registerName", name); }
u32 cls(GuestCall& c, const char* name) { return named(c, "objc_getClass", name); }

// [obj selector args...], every argument a word. Nil answers 0, as it would.
u32 msg(GuestCall& c, u32 obj, const char* selector, std::initializer_list<u32> args = {}) {
  if (!obj) return 0;
  const u32 send = fn(c, "objc_msgSend"), s = sel(c, selector);
  if (!send || !s) return 0;
  std::vector<Process::GuestArg> words = {{obj}, {s}};
  for (u32 a : args) words.push_back({a});
  return c.process.call_guest_args(c.thread, send, words);
}
// A BOOL answer: only its low byte means anything.
bool yes(GuestCall& c, u32 obj, const char* selector, std::initializer_list<u32> args = {}) {
  return (msg(c, obj, selector, args) & 0xFF) != 0;
}

// Bytes for one call, from the guest's own malloc; given back with guest_free.
u32 guest_alloc(GuestCall& c, u32 bytes) { return c.call(fn(c, "malloc"), {bytes ? bytes : 1}); }
void guest_free(GuestCall& c, u32 at) {
  if (at) c.call(fn(c, "free"), {at});
}

// A new NSString (autoreleased), or 0.
u32 ns_string(GuestCall& c, const std::string& s) {
  const u32 at = guest_alloc(c, (u32)s.size() + 1);
  if (!at || !c.mem.range_ok(at, s.size() + 1)) return 0;
  memcpy(c.mem.host(at), s.c_str(), s.size() + 1);
  const u32 string = msg(c, cls(c, "NSString"), "stringWithUTF8String:", {at});
  guest_free(c, at);
  return string;
}
// A new NSArray (autoreleased) of the strings, or 0 if one could not be
// made: a nil in an array is an exception.
u32 ns_array(GuestCall& c, const std::vector<std::string>& strings) {
  std::vector<u32> items;
  for (const std::string& s : strings) {
    const u32 string = ns_string(c, s);
    if (!string) return 0;
    items.push_back(string);
  }
  const u32 at = guest_alloc(c, 4 * (u32)items.size());
  if (!at || !c.mem.range_ok(at, 4 * items.size() + 1)) return 0;
  for (size_t k = 0; k < items.size(); k++) c.mem.w32(at + 4 * (u32)k, items[k]);
  const u32 array = msg(c, cls(c, "NSArray"), "arrayWithObjects:count:", {at, (u32)items.size()});
  guest_free(c, at);
  return array;
}

bool is_kind(GuestCall& c, u32 obj, const char* class_name) {
  const u32 k = cls(c, class_name);
  return obj && k && yes(c, obj, "isKindOfClass:", {k});
}
// An NSString's text; empty for nil or for anything else.
std::string text_of(GuestCall& c, u32 string) {
  std::string out;
  if (!is_kind(c, string, "NSString")) return out;
  const u32 at = msg(c, string, "UTF8String");
  if (at) c.mem.read_cstr(at, out);
  return out;
}
// An NSArray's strings.
std::vector<std::string> texts_of(GuestCall& c, u32 array) {
  std::vector<std::string> out;
  if (!is_kind(c, array, "NSArray")) return out;
  const u32 n = msg(c, array, "count");
  for (u32 k = 0; k < n && k < 4096; k++) {
    const u32 item = msg(c, array, "objectAtIndex:", {k});
    if (is_kind(c, item, "NSString")) out.push_back(text_of(c, item));
  }
  return out;
}

// A string the getters hand out when nothing was set, made once and kept.
u32 constant(GuestCall& c, const char* text) {
  const std::string key = std::string("text:") + text;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (auto it = g_names.find(key); it != g_names.end()) return it->second;
  }
  const u32 v = msg(c, ns_string(c, text), "retain");
  if (!v) return 0;
  std::lock_guard<std::mutex> lk(g_mu);
  auto [it, added] = g_names.emplace(key, v);
  return it->second;
}

// Whether the panel saves (an NSSavePanel that is not an NSOpenPanel).
bool saves(GuestCall& c, u32 self) {
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (const int mode = g_panels[self].mode) return mode == 2;
  }
  const bool save = !is_kind(c, self, "NSOpenPanel");
  std::lock_guard<std::mutex> lk(g_mu);
  g_panels[self].mode = save ? 2 : 1;
  return save;
}

// ---- the init -----------------------------------------------------------------

// -_initContentView loads the browser's nibs; this is the step that starts
// the thread that dies. In its place: a fresh record for this panel. The
// init goes on to set every default through the setters below.
void init_content_view(GuestCall& c) {
  const u32 self = c.i();
  Panel old;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_panels.find(self);
    if (it != g_panels.end()) {
      // A panel that was here before, long gone: nothing says when a panel
      // is freed, so what it held is let go now.
      old = std::move(it->second);
      g_panels.erase(it);
    }
    g_panels[self] = Panel();
  }
  log_debug("panel 0x%x: made hollow", self);
  for (u32 held : {old.directory, old.prompt, old.message, old.label, old.accessory}) msg(c, held, "release");
  c.ret(0);
}

// -_initPanelCommon fills the sidebar and lays the window out.
void init_panel_common(GuestCall& c) { c.ret(1); }

// ---- settings -------------------------------------------------------------------

template <bool Panel::*Field>
void set_flag(GuestCall& c) {
  const u32 self = c.i();
  c.i();
  const bool on = (c.i() & 0xFF) != 0;
  std::lock_guard<std::mutex> lk(g_mu);
  g_panels[self].*Field = on;
  c.ret(0);
}
template <bool Panel::*Field>
void get_flag(GuestCall& c) {
  const u32 self = c.i();
  std::lock_guard<std::mutex> lk(g_mu);
  c.ret(g_panels[self].*Field ? 1 : 0);
}

// Keeps `value` (retained) in place of what was there. The old one is
// autoreleased, not released: the caller may still be using it.
void keep_object(GuestCall& c, u32 self, u32 Panel::*field, u32 value) {
  msg(c, value, "retain");
  u32 old;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    Panel& p = g_panels[self];
    old = p.*field;
    p.*field = value;
    if (field == &Panel::directory) p.answered_folder = false;
  }
  msg(c, old, "autorelease");
}
template <u32 Panel::*Field>
void set_object(GuestCall& c) {
  const u32 self = c.i();
  c.i();
  keep_object(c, self, Field, c.i());
  c.ret(0);
}
// A string setting where nil and "" both mean "the usual".
template <u32 Panel::*Field>
void set_text(GuestCall& c) {
  const u32 self = c.i();
  c.i();
  u32 value = c.i();
  if (value && (!is_kind(c, value, "NSString") || !msg(c, value, "length"))) value = 0;
  keep_object(c, self, Field, value);
  c.ret(0);
}
u32 held(u32 self, u32 Panel::*field) {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_panels[self].*field;
}
void get_accessory(GuestCall& c) { c.ret(held(c.i(), &Panel::accessory)); }
void get_prompt(GuestCall& c) {
  const u32 self = c.i();
  const u32 set = held(self, &Panel::prompt);
  c.ret(set ? set : constant(c, saves(c, self) ? "Save" : "Open"));
}
void get_label(GuestCall& c) {
  const u32 set = held(c.i(), &Panel::label);
  c.ret(set ? set : constant(c, "Save As:"));
}
void get_message(GuestCall& c) {
  const u32 set = held(c.i(), &Panel::message);
  c.ret(set ? set : constant(c, ""));
}

// ---- the answers --------------------------------------------------------------

std::vector<std::string> answer_of(u32 self) {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_panels[self].paths;
}

std::string folder_of(const std::string& path) {
  const size_t slash = path.rfind('/');
  if (slash == std::string::npos) return "";
  return slash ? path.substr(0, slash) : "/";
}

// -filename: the first path picked, nil if nothing was.
void filename(GuestCall& c) {
  const std::vector<std::string> paths = answer_of(c.i());
  c.ret(paths.empty() ? 0 : ns_string(c, paths[0]));
}
// -URL, which Leopard builds from -filename and would throw for nil.
void url(GuestCall& c) {
  const std::vector<std::string> paths = answer_of(c.i());
  const u32 path = paths.empty() ? 0 : ns_string(c, paths[0]);
  c.ret(path ? msg(c, cls(c, "NSURL"), "fileURLWithPath:", {path}) : 0);
}
// -_filenamesWithAlwaysIncludingDirectories:, behind -filenames and -URLs.
void filenames(GuestCall& c) { c.ret(ns_array(c, answer_of(c.i()))); }
// -directory: where the answer is; before one, the folder last set.
void directory(GuestCall& c) {
  const u32 self = c.i();
  std::string folder;
  u32 set;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    const Panel& p = g_panels[self];
    set = p.directory;
    if (p.answered_folder && !p.paths.empty()) folder = folder_of(p.paths[0]);
  }
  c.ret(folder.empty() ? set : ns_string(c, folder));
}

// ---- running ------------------------------------------------------------------

// Everything the picker is told, read from the panel and the run method's
// arguments. Guest calls throughout: nothing of cocaine's is held.
PickRequest request_for(GuestCall& c, u32 self, u32 directory, u32 file, u32 types) {
  PickRequest r;
  r.save = saves(c, self);
  Panel p;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    p = g_panels[self];
  }
  r.title = text_of(c, msg(c, self, "title"));
  // The title Leopard gives a panel nobody named says nothing, and today's
  // picker would show it as a message.
  if (r.title == (r.save ? "Save" : "Open")) r.title.clear();
  r.prompt = text_of(c, p.prompt);
  r.message = text_of(c, p.message);
  r.label = text_of(c, p.label);
  const u32 folder = is_kind(c, directory, "NSString") ? directory : p.directory;
  r.dir = c.process.picker_start_folder(text_of(c, msg(c, folder, "stringByExpandingTildeInPath")));
  // Leopard's name field holds a POSIX name; this Mac's shows ':' for '/'.
  r.name = text_of(c, file);
  if (const size_t slash = r.name.rfind('/'); slash != std::string::npos) r.name.erase(0, slash + 1);
  for (char& ch : r.name) {
    if (ch == ':') ch = '/';
  }
  if (r.save) {
    r.types = texts_of(c, msg(c, self, "allowedFileTypes"));
    r.required = text_of(c, msg(c, self, "requiredFileType"));
    r.othertypes = yes(c, self, "allowsOtherFileTypes");
    r.canhideext = yes(c, self, "canSelectHiddenExtension");
    r.exthidden = p.extension_hidden;
  } else {
    // An open panel filters by the run method's types; its allowedFileTypes
    // mean something else in Leopard.
    r.types = texts_of(c, types);
    r.multi = p.multi;
    r.files = p.files;
    r.dirs = p.dirs;
  }
  r.mkdir = yes(c, self, "canCreateDirectories");
  r.packages = p.packages;
  r.aliases = p.aliases;
  r.hidden = yes(c, self, "showsHiddenFiles");
  return r;
}

// Asks the picker and keeps its answer for the getters. 1 for a pick.
u32 run(GuestCall& c, u32 self, PickRequest& request) {
  PickReply reply;
  const bool picked = c.process.ask_file_picker(c.thread, request, reply) && !reply.paths.empty();
  std::lock_guard<std::mutex> lk(g_mu);
  Panel& p = g_panels[self];
  p.paths = picked ? reply.paths : std::vector<std::string>();
  p.answered_folder = picked;
  if (picked && request.save) p.extension_hidden = reply.exthidden;
  return picked ? 1 : 0;
}

// -runModalForDirectory:file:types:
void run_modal(GuestCall& c) {
  const u32 self = c.i();
  c.i();
  const u32 directory = c.i(), file = c.i(), types = c.i();
  PickRequest request = request_for(c, self, directory, file, types);
  c.ret(run(c, self, request));
}

// -_cocainePickNow:, the method cocaine adds to NSSavePanel: the answer to
// a sheet or a modeless panel, on the main thread, after begin... returned.
void pick_now(GuestCall& c) {
  const u32 self = c.i();
  PickRequest request;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    request = g_panels[self].request;
  }
  const u32 code = run(c, self, request);
  u32 delegate, selector, context;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    Panel& p = g_panels[self];
    p.begun = false;
    delegate = p.delegate;
    selector = p.selector;
    context = p.context;
    p.delegate = p.selector = p.context = 0;
  }
  c.ret(0);
  // An event to follow the delegate's call. Leopard ends a sheet from a
  // click, and a delegate that stops a modal session counts on an event
  // coming round for the session to notice; from here none would. Made
  // before the delegate runs, posted after it.
  // [NSEvent otherEventWithType:NSApplicationDefined location:(0,0)
  //   modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:0
  //   data1:0 data2:0]: the time is a double, and zero is the one double
  // that travels as plain words on both CPUs.
  const u32 send = fn(c, "objc_msgSend");
  const u32 app = msg(c, cls(c, "NSApplication"), "sharedApplication");
  const u32 make = sel(c, "otherEventWithType:location:modifierFlags:timestamp:windowNumber:context:subtype:data1:data2:");
  u32 event = 0;
  if (send && make) {
    event = c.process.call_guest_args(c.thread, send,
                                      {{cls(c, "NSEvent")}, {make}, {15}, {0}, {0}, {0}, {0, true}, {0}, {0}, {0}, {0}, {0}, {0}});
  }
  msg(c, event, "retain");
  // The delegate last of what matters: the answer is stored and nothing is
  // held, so it may run its own panels, or throw.
  if (delegate && selector && send) {
    c.process.call_guest_args(c.thread, send, {{delegate}, {selector}, {self}, {code}, {context}});
  }
  if (event) {
    msg(c, app, "postEvent:atStart:", {event, 1});
    msg(c, event, "release");
  }
}

// Has the main thread's run loop call -_cocainePickNow: on the panel, in the
// modes a modal session and a tracking loop run in too.
void begin(GuestCall& c, u32 self, u32 directory, u32 file, u32 types, u32 delegate, u32 selector, u32 context) {
  PickRequest request = request_for(c, self, directory, file, types);
  const u32 pick = sel(c, "_cocainePickNow:");
  // Added to the class once. No lock is held for it (a guest call): two
  // threads here at the same moment both add, and the runtime refuses the
  // second.
  static std::atomic<bool> added{false};
  if (!added) {
    const u32 imp = native_function(c.process, "-[NSSavePanel _cocainePickNow:]", pick_now);
    const u32 types_at = c.scratch(8);
    if (!imp || !types_at || !pick) return;
    memcpy(c.mem.host(types_at), "v@:@", 5);
    c.call(fn(c, "class_addMethod"), {cls(c, "NSSavePanel"), pick, imp, types_at});
    added = true;
  }
  {
    std::lock_guard<std::mutex> lk(g_mu);
    Panel& p = g_panels[self];
    if (p.begun) {
      log_warn("a panel was begun again before its first answer; the second is not shown");
      return;
    }
    p.begun = true;
    p.request = std::move(request);
    p.delegate = delegate;
    p.selector = selector;
    p.context = context;
  }
  const u32 modes = ns_array(c, {"kCFRunLoopDefaultMode", "NSModalPanelRunLoopMode", "NSEventTrackingRunLoopMode"});
  if (!modes) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_panels[self].begun = false;
    return;
  }
  msg(c, self, "performSelectorOnMainThread:withObject:waitUntilDone:modes:", {pick, 0, 0, modes});
}

// -beginSheetForDirectory:file:types:modalForWindow:modalDelegate:didEndSelector:contextInfo:
void begin_sheet(GuestCall& c) {
  u32 w[9];
  for (u32& v : w) v = c.i();
  c.ret(0);
  begin(c, w[0], w[2], w[3], w[4], w[6], w[7], w[8]);
}
// -beginForDirectory:file:types:modelessDelegate:didEndSelector:contextInfo:
void begin_modeless(GuestCall& c) {
  u32 w[8];
  for (u32& v : w) v = c.i();
  c.ret(0);
  begin(c, w[0], w[2], w[3], w[4], w[5], w[6], w[7]);
}

// ---- Carbon -------------------------------------------------------------------

// Navigation Services is a thin layer over AppKit's panels in Leopard: its
// NavCreate...Dialog calls land in these functions, which build a panel that
// insists on a real window. Until there is a stand-in for those too, the
// dialog is one the person cancelled (userCanceledErr), where it used to
// take the program down.
template <int Words>
void nav_create(GuestCall& c) {
  u32 w[Words];
  for (u32& v : w) v = c.i();
  const u32 out = w[Words - 1];  // NavDialogRef*, always last
  if (out && c.mem.range_ok(out, 4)) c.mem.w32(out, 0);
  static std::atomic<bool> said{false};
  if (!said.exchange(true)) log_warn("Carbon file dialogs are not shown yet: answered as cancelled");
  c.ret((u32)-128);
}

#define PANEL "-[NSSavePanel "
const HookSpec kPanels[] = {
    {PANEL "_initContentView]", init_content_view},
    {PANEL "_initPanelCommon]", init_panel_common},
    {PANEL "setAllowsMultipleSelection:]", set_flag<&Panel::multi>},
    {PANEL "allowsMultipleSelection]", get_flag<&Panel::multi>},
    {PANEL "setCanChooseFiles:]", set_flag<&Panel::files>},
    {PANEL "canChooseFiles]", get_flag<&Panel::files>},
    {PANEL "setCanChooseDirectories:]", set_flag<&Panel::dirs>},
    {PANEL "canChooseDirectories]", get_flag<&Panel::dirs>},
    {PANEL "setResolvesAliases:]", set_flag<&Panel::aliases>},
    {PANEL "resolvesAliases]", get_flag<&Panel::aliases>},
    {PANEL "setTreatsFilePackagesAsDirectories:]", set_flag<&Panel::packages>},
    {PANEL "treatsFilePackagesAsDirectories]", get_flag<&Panel::packages>},
    {PANEL "setExtensionHidden:]", set_flag<&Panel::extension_hidden>},
    {PANEL "isExtensionHidden]", get_flag<&Panel::extension_hidden>},
    {PANEL "setDirectory:]", set_object<&Panel::directory>},
    {PANEL "directory]", directory},
    {PANEL "setPrompt:]", set_text<&Panel::prompt>},
    {PANEL "prompt]", get_prompt},
    {PANEL "setMessage:]", set_object<&Panel::message>},
    {PANEL "message]", get_message},
    {PANEL "setNameFieldLabel:]", set_text<&Panel::label>},
    {PANEL "nameFieldLabel]", get_label},
    {PANEL "setAccessoryView:]", set_object<&Panel::accessory>},
    {PANEL "accessoryView]", get_accessory},
    {PANEL "runModalForDirectory:file:types:]", run_modal},
    {PANEL "beginSheetForDirectory:file:types:modalForWindow:modalDelegate:didEndSelector:contextInfo:]", begin_sheet},
    {PANEL "beginForDirectory:file:types:modelessDelegate:didEndSelector:contextInfo:]", begin_modeless},
    {PANEL "filename]", filename},
    {PANEL "URL]", url},
    {PANEL "_filenamesWithAlwaysIncludingDirectories:]", filenames},
    {"_NSNavCreateGetFileDialog", nav_create<7>},
    {"_NSNavCreatePutFileDialog", nav_create<6>},
    {"_NSNavCreateChooseFileDialog", nav_create<7>},
    {"_NSNavCreateChooseFolderDialog", nav_create<5>},
    {"_NSNavCreateChooseVolumeDialog", nav_create<5>},
    {"_NSNavCreateChooseObjectDialog", nav_create<6>},
};
#undef PANEL

const HookLibrary kLibraries[] = {
    {"/AppKit.framework/Versions/C/AppKit", "", kPanels, sizeof kPanels / sizeof kPanels[0], true,
     "open and save panels stay Leopard's own, which do not work here"},
};

}  // namespace

const HookLibrary* panel_hook_libraries(size_t& n) {
  const char* own = getenv("COCAINE_GUEST_PANELS");
  n = own && *own && strcmp(own, "0") != 0 ? 0 : 1;
  return kLibraries;
}

void panel_fork_lock(bool take) {
  if (take) {
    g_mu.lock();
  } else {
    g_mu.unlock();
  }
}

// In a forked child the thread that was asking the picker, if any, is gone.
void panel_fork_child() {
  std::lock_guard<std::mutex> lk(g_mu);
  g_asking = false;
}

// ---- asking the helper ----------------------------------------------------------

// Where a panel should start, as a host path: the guest's folder made whole
// against its working directory. Empty leaves it to the picker.
std::string Process::picker_start_folder(const std::string& guest_folder) const {
  if (guest_folder.empty()) return "";
  std::string folder = guest_folder;
  if (folder[0] != '/') {
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) return "";
    folder = guest_path(cwd) + "/" + folder;
  }
  const std::string host = host_path(folder);
  struct stat st;
  if (stat(host.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return "";
  return host;
}

// Sends one request to the picker's helper and waits for its reply, on the
// calling guest thread. False, with a line in the log, for anything but a
// reply the guest can use; a cancel is true with no paths.
bool Process::ask_file_picker(Thread& t, PickRequest& request, PickReply& reply) {
  reply = PickReply();
  reply.status = "cancel";
  std::string session = session_name_.empty() ? opt_.session : session_name_;
  int root = 0;
  if (sscanf(session.c_str(), "cocaine.session.%d.", &root) != 1 || root <= 0) {
    root = (int)getpid();
    session = "cocaine.session." + std::to_string(root) + ".0";
  }
  request.root = root;
  request.sysroot = opt_.sysroot;
  if (opt_.self_path.empty()) {
    log_warn("no file picker: cocaine does not know where its own program is");
    return false;
  }

  // One at a time.
  {
    std::unique_lock<std::mutex> lk(g_mu);
    while (g_asking) {
      if (exited_ || t.done) return false;
      g_turn.wait_for(lk, std::chrono::milliseconds(100));
    }
    g_asking = true;
  }
  auto done = [] {
    {
      std::lock_guard<std::mutex> lk(g_mu);
      g_asking = false;
    }
    g_turn.notify_one();
  };

  // The helper: this same program, with one end of a socket pair as its
  // descriptor 3 and nothing else of the guest's. The kernel lock is held
  // from making the pair to closing the helper's end, so a guest thread
  // forking meanwhile can't take a copy of that end along (the reply's end
  // would then never read as closed).
  std::string self = opt_.self_path, pick = "--pick", flag = "--session";
  char* argv[] = {self.data(), pick.data(), flag.data(), session.data(), nullptr};
  char tmp[PATH_MAX] = "/tmp/";
  confstr(_CS_DARWIN_USER_TEMP_DIR, tmp, sizeof tmp);
  std::string log = std::string(tmp) + "cocaine-session-" + std::to_string(root) + ".log";
  if (access(log.c_str(), W_OK) != 0) log = "/dev/null";
  int ends[2] = {-1, -1};
  pid_t pid = 0;
  int error = 0;
  {
    std::lock_guard<std::mutex> lk(kmu_);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, ends) != 0) {
      error = errno;
    } else {
      const int on = 1;
      fcntl(ends[0], F_SETFD, FD_CLOEXEC);
      fcntl(ends[1], F_SETFD, FD_CLOEXEC);
      setsockopt(ends[0], SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
      posix_spawn_file_actions_t actions;
      posix_spawn_file_actions_init(&actions);
      posix_spawn_file_actions_adddup2(&actions, ends[1], 3);
      posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
      posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
      posix_spawn_file_actions_addopen(&actions, 2, log.c_str(), O_WRONLY | O_APPEND, 0);
      posix_spawnattr_t attributes;
      posix_spawnattr_init(&attributes);
      sigset_t none, all;
      sigemptyset(&none);
      sigfillset(&all);
      posix_spawnattr_setsigmask(&attributes, &none);
      posix_spawnattr_setsigdefault(&attributes, &all);
      posix_spawnattr_setflags(&attributes,
                               POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
      error = posix_spawn(&pid, self.c_str(), &actions, &attributes, argv, environ);
      posix_spawnattr_destroy(&attributes);
      posix_spawn_file_actions_destroy(&actions);
      close(ends[1]);
      if (error) close(ends[0]);
    }
  }
  if (error) {
    log_warn("could not start the file picker: %s", strerror(error));
    done();
    return false;
  }

  // The request, then the wait: in short slices, so that a signal for the
  // guest (which interrupts the wait and is delivered once the panel has
  // its answer) is no answer, and the process ending is noticed.
  const std::string message = pick_encode_request(request);
  std::string bytes;
  const char* failure = nullptr;
  for (size_t sent = 0; sent < message.size() && !failure;) {
    const ssize_t n = write(ends[0], message.data() + sent, message.size() - sent);
    if (n > 0) sent += (size_t)n;
    else if (errno != EINTR && errno != EAGAIN) failure = "the file picker went away before it was asked";
  }
  bool leaving = false;
  while (!failure && !pick_message_length(bytes)) {
    if (exited_ || t.done) {
      leaving = true;
      break;
    }
    struct pollfd wait = {ends[0], POLLIN, 0};
    const int ready = poll(&wait, 1, 100);
    if (ready < 0 && errno != EINTR && errno != EAGAIN) failure = "waiting for the file picker failed";
    if (ready <= 0) continue;
    char buf[4096];
    const ssize_t n = read(ends[0], buf, sizeof buf);
    if (n > 0) bytes.append(buf, (size_t)n);
    else if (n == 0 || (errno != EINTR && errno != EAGAIN)) failure = "the file picker went away without an answer";
    if (bytes.size() > (4u << 20)) failure = "the file picker's answer makes no sense";
  }
  // Closing this end is what tells the helper to go, panel and all. It ends
  // by itself after replying; one that hasn't soon is made to.
  close(ends[0]);
  if (leaving || failure) kill(pid, SIGTERM);
  for (int tries = 0;; tries++) {
    int status;
    const pid_t got = waitpid(pid, &status, WNOHANG);
    if (got == pid || (got < 0 && errno != EINTR)) break;  // gone, or the guest's own wait took it (ECHILD)
    if (tries == 200) kill(pid, SIGKILL);
    usleep(10000);
  }
  done();
  if (leaving) return false;

  std::string trouble;
  if (!failure) {
    bytes.resize(pick_message_length(bytes));
    if (!pick_parse_reply(bytes, reply, trouble)) failure = trouble.c_str();
  }
  if (failure) {
    log_warn("%s; the panel answers cancel", failure);
    reply = PickReply();
    reply.status = "cancel";
    return false;
  }
  if (reply.status != "ok") {
    if (!reply.error.empty()) log_warn("the file picker: %s", reply.error.c_str());
    const bool cancelled = reply.status == "cancel";
    reply.paths.clear();
    reply.status = "cancel";
    return cancelled;
  }
  // The helper has checked that each path leads the guest to the file
  // picked. Once more, as a safety net: what the guest is handed must be
  // there (for a save, the folder it goes in).
  for (const std::string& path : reply.paths) {
    struct stat st;
    const std::string host = host_path(request.save ? path.substr(0, path.rfind('/') + 1) : path);
    if (path.empty() || path[0] != '/' || stat(host.c_str(), &st) != 0) {
      log_warn("the file picker gave a path this program can't reach (%s); the panel answers cancel", path.c_str());
      reply.paths.clear();
      reply.status = "cancel";
      return false;
    }
  }
  return true;
}
