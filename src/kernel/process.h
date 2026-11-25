#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <initializer_list>
#include <map>
#include <set>
#include <vector>

#include "common.h"
#include "cpu/ppc/ppc.h"
#include "cpu/x86/x86.h"
#include "macho/macho.h"
#include "mem/guest_memory.h"

struct Options {
  std::string sysroot;      // Mac OS X system root: where dyld and system libraries come from
  bool trace = false;       // print every guest instruction
  bool trace_registers = false;  // ... along with the registers
  bool strace = false;      // print every system call
  bool stats = false;       // print instruction count and speed at exit
  u64 max_insns = ~0ull;    // stop after this many guest instructions
  std::string self_path;    // where the cocaine program itself is, for running guest exec()
  std::string argv0;        // what the guest sees as argv[0], if not its own path
  bool as_root = false;     // tell the guest it is root: system daemons insist on it
  std::string session;      // name of the session to join, if started by a guest of one
};

// One guest thread's CPU state. Only the member matching the process's
// architecture is used.
struct Thread {
  PPCState ppc;
  X86State x86;
  u32 self_port = 0;   // this thread's Mach port name
  std::atomic<bool> done{false};  // set when the thread has ended
  pthread_t host{};    // the host thread running it
  u32 host_port = 0;   // ... and that host thread's own Mach port
  int number = 0;      // 0 for the main thread, then counting up; shown in traces

  // Signals. The mask itself lives in the host thread, which has the same one.
  u32 altstack_sp = 0, altstack_size = 0;  // sigaltstack, if size is not zero
  bool on_altstack = false;
  bool suspended = false;  // just back from sigsuspend: deliver what its mask let in
  u32 suspend_mask = 0;

  // A guest function cocaine called (Process::call_guest) has returned.
  bool returned = false;
};

// What a system call produced, before it is written back in the guest's
// calling convention.
struct SysRet {
  enum Kind { Value, Value64, Error, NoReturn };
  Kind kind;
  u64 value;

  static SysRet ok(u32 v = 0) { return {Value, v}; }
  static SysRet ok64(u64 v) { return {Value64, v}; }
  static SysRet err(int e) { return {Error, (u64)e}; }
  static SysRet none() { return {NoReturn, 0}; }
};

// System call arguments as the guest passed them: a run of 32-bit words
// (registers on PowerPC, stack slots on Intel). A 64-bit argument takes two
// words, most significant first on PowerPC and last on Intel.
struct SysArgs {
  u32 w[12] = {};
  int next = 0;
  bool big_endian = false;

  u32 u() { return next < 12 ? w[next++] : 0; }
  s32 i() { return (s32)u(); }
  u64 q() {
    const u32 a = u(), b = u();
    return big_endian ? ((u64)a << 32) | b : ((u64)b << 32) | a;
  }
};

// A kernel object the guest reaches through a Mach port. Each is a real port
// of this host process, so its name can't clash with any other port's and the
// right can travel to another process in a message; what the guest sends to
// it is answered here (mach.cpp).
enum class KObject { Task, Host, Thread, Clock, IOKit, IOObject };

struct KPort {
  KObject kind = KObject::Task;
  Thread* thread = nullptr;  // for Thread ports
  u32 io = 0;                // for IOObject ports: which kind (iokit.cpp)
  u32 item = 0;              // for an IOKit iterator: the kind of object still to hand out
  u32 initial_item = 0;      // the object to restore when an iterator is reset
};

// Browsable host volumes mounted under /Volumes, or an errno on failure.
int mounted_volumes(std::vector<struct statfs>& volumes, int flags);

// A message taken off a port and turned into what the guest will see, along
// with what the kernel says about its sender.
struct Incoming {
  std::vector<u8> bytes;  // the message, in the guest's layout and byte order
  u32 port = 0;           // the port it arrived on
  u32 seqno = 0;
  u32 sender[2] = {};     // user and group of the sender
  u32 audit[8] = {};      // ... and the longer form of that, with its pid
};

// The guest display size, resolved at launch into COCAINE_DISPLAY from the
// host's logical screen size; 1280 x 800 without a host screen (mach.cpp).
void guest_display_size(u32& width, u32& height);
// Where the PowerPC comm page holds that size, as two floats, for the window
// server's own code to read (hooks.cpp patches it to).
constexpr u32 kPPCDisplaySizeAddr = 0xFFFF9FE0u;

// A running guest program: its memory, its threads, and the fake Mac OS X
// kernel that answers its system calls.
class Process {
 public:
  Process(Arch arch, GuestMemory& mem, const Options& opt);

  // Maps the executable (and dyld, if it needs one) and builds the initial
  // stack. `argv[0]` is the path the guest sees as its own.
  bool load(const MachImage& exe, const std::vector<std::string>& argv,
            const std::vector<std::string>& envp, std::string& err);

  // Runs until the guest exits or cannot continue. Returns the exit status.
  int run();

  // Called from the host signal handler when a memory access faults.
  void report_fault(int sig, const void* host_addr, const void* context = nullptr);
  bool guest_writable(u32 addr) const;  // whether the guest may write the page at addr
  bool guest_readable(u32 addr) const;  // or read it

  // ---- hooks.cpp: native stand-ins for guest library functions ----
  GuestMemory& memory() { return mem_; }
  // Runs a guest function from native code, on thread `t`, and returns its
  // integer result. The thread's registers are as they were afterwards.
  u32 call_guest(Thread& t, u32 fn, std::initializer_list<u32> args);
  // The same, for functions taking floats: each argument a word, marked if it
  // is a float's bits (they travel in floating point registers on PowerPC).
  struct GuestArg {
    u32 bits;
    bool is_float = false;
  };
  u32 call_guest_args(Thread& t, u32 fn, const std::vector<GuestArg>& args);
  // Puts the wallpaper behind every window: run in loginwindow once the
  // session is ready (see wallpaper.cpp).
  void draw_wallpaper(Thread& t);
  // The address of a function exported by a loaded guest library, or 0.
  u32 guest_symbol(const std::string& name);
  // Guest memory for cocaine's own use, never freed.
  u32 guest_scratch(u32 bytes);
  // The window server has just put an update on the screen: copy what
  // changed to the viewer (mach.cpp). Nothing in other processes.
  void publish_display();
  void set_display_format(u32 width, u32 height, u32 stride, u32 bits);

 private:
  // ---- process.cpp ----
  bool map_image(const MachImage& img, std::string& err);
  bool setup_stack(const std::vector<std::string>& argv, const std::vector<std::string>& envp,
                   bool push_header, u32 header_addr, u32& sp, std::string& err);
  int run_ppc(Thread& t);
  int run_x86(Thread& t);
  void dump_registers(const Thread& t) const;
  // The mapped library holding guest address `pc`, and how far into it, for crash reports.
  const char* image_of(u32 pc, u32& offset) const;

  // ---- syscalls.cpp: BSD system calls ----
  void ppc_syscall(Thread& t);
  void x86_syscall(Thread& t, bool sysenter, u8 vector);
  SysRet bsd_syscall(Thread& t, u32 num, SysArgs& a);
  SysRet sys_mmap(u32 addr, u32 len, u32 prot, u32 flags, s32 fd, u64 offset);
  SysRet sys_shm(u32 num, const std::string& name, int oflag, u32 mode);
  SysRet sys_stat(const std::string& path, u32 buf, bool follow, bool wide);
  SysRet sys_fstat(int fd, u32 buf, bool wide);
  SysRet sys_fcntl(int fd, u32 cmd, u32 arg);
  SysRet sys_sysctl(u32 name, u32 namelen, u32 oldp, u32 oldlenp, u32 newp, u32 newlen);
  // What the guest should be told about a process: itself, or another guest.
  struct GuestProcess {
    int pid = 0, ppid = 0, pgid = 0;
    u32 uid = 0, gid = 0;
    long started = 0;
    std::string name;  // the guest program's name, not "cocaine"
  };
  bool describe_process(int pid, GuestProcess& out) const;
  SysRet sys_getdirentries(int fd, u32 buf, u32 nbytes, u32 basep, bool wide);
  SysRet sys_statfs(const struct statfs& fs, u32 buf, bool wide);
  SysRet sys_getattrlist(const std::string& path, u32 list, u32 buf, u32 size, u32 options);
  SysRet sys_setattrlist(const std::string& path, u32 list, u32 buf, u32 size, u32 options);
  SysRet sys_access_extended(u32 entries, u32 size, u32 results);
  int open_appledouble_fork(const std::string& host, int flags);
  SysRet sys_socket_call(u32 num, SysArgs& a);
  void write_stat(u32 buf, const struct stat& st, bool wide);
  u32 machdep_call(Thread& t, u32 num, SysArgs& a);
  void ppc_fast_trap(Thread& t, u32 num);

  // ---- paths.cpp ----
  // Where a path the guest names lives on the host. System directories are
  // redirected into the system root.
  std::string host_path(const std::string& guest_path, bool follow_last = true) const;
  std::string bundled_wallpaper_path() const;
  // The reverse, for calls that hand a path back to the guest.
  std::string guest_path(const std::string& host_path) const;

  // ---- hooks.cpp ----
  // A file's code has been mapped at `at`: note the library, and take over
  // the functions cocaine supplies itself.
  void note_code_mapping(u32 at, u32 len, int fd, u64 offset);
  void run_hook(Thread& t, u32 index);
  u32 keep_original(u32 fn);
 public:
  void plant_host_call(u32 where, u32 id);
 private:
  struct MappedImage {
    std::string path;
    u64 slice_offset = 0;
    u32 at = 0, len = 0;  // where the mapping went, and its size
    u64 offset = 0;    // and from where in the file
    u32 slide = 0;
    bool read = false, usable = false;
    std::vector<std::pair<std::string, u32>> exports;  // name (no underscore), unslid address
  };
  bool read_image(MappedImage& img);
  std::vector<MappedImage> images_;
  mutable std::mutex images_mu_;
  u32 scratch_at_ = 0, scratch_left_ = 0;

  // ---- commpage.cpp ----
  bool setup_commpage(std::string& err);
  void host_call(Thread& t, u32 id);

  // ---- mach.cpp: Mach traps and the kernel side of Mach messages ----
  u32 mach_trap(Thread& t, u32 num, SysArgs& a);
  u32 guest_mach_msg(Thread& t, u32 msg, u32 option, u32 send_size, u32 rcv_size, u32 rcv_name,
                     u32 timeout, u32 rcv_msg);
  // Creates the ports every process starts with. Also run in a forked child,
  // which inherits none of its parent's.
  void setup_ports(Thread& t);
  u32 new_kport(KObject kind, Thread* thread = nullptr);  // call with kmu_ held
  void give_send(u32 name);  // one more reference to a kernel object's port, for the guest
  // Maps the memory behind a memory-entry port into the guest. Returns a
  // Mach result, and the address in `at`.
  // Starts copying the guest window server's display out for the viewer.
  void share_display(u32 addr, u64 bytes);
  bool display_shared_ = false;
  u8* display_file_ = nullptr;  // the shared file the viewer reads: header, then pixels
  u64 display_source_bytes_ = 0, display_file_pixels_ = 0;
  u32 display_w_ = 0, display_h_ = 0;
  u32 display_stride_ = 0, display_bits_ = 32;
  std::mutex publish_mu_;
  std::atomic<u64> hook_published_ns_{0};  // when publish_display last ran
  bool publish_changed();  // with publish_mu_ held
  u32 map_memory_entry(u32 entry, u64 addr, u64 size, u32 flags, u64 offset, bool copy, u32 prot,
                       u32 max_prot, u32 inherit, u32& at);
  // Answers what other processes send to this one's kernel-object ports.
  void serve_other_processes(u32 port_set);
  // Handles a message sent to a port the kernel layer implements. Returns
  // false if nobody handles it; otherwise `reply` holds the reply message
  // (empty if the routine sends none).
  bool kernel_rpc(Thread& t, u32 name, const KPort& port, u32 msg, u32 size, std::vector<u8>& reply);
  // The same for IOKit's ports (iokit.cpp).
  bool iokit_rpc(u32 name, const KPort& port, u32 msg, u32 size, std::vector<u8>& reply);
  // One call on a driver connection: numbers and bytes in, numbers and bytes out.
  u32 io_method(const KPort& port, u32 selector, const std::vector<u64>& in,
                const std::vector<u8>& bytes_in, std::vector<u64>& out, std::vector<u8>& bytes_out);
  // Memory a driver shares with its client, by connection and type.
  bool io_memory(const KPort& port, u32 type, u32& address, u32& size);

  // ---- ipc.cpp: guest messages over real host ports ----
  // Sends the guest message at `g`, whose descriptors point into guest memory.
  u32 ipc_send(const u8* g, u32 size, u32 option, u32 timeout);
  u32 ipc_receive(u32 buf, u32 option, u32 rcv_size, u32 rcv_name, u32 timeout);
  bool ipc_decode(const void* host_message, Incoming& in);
  u32 ipc_deliver(Incoming&& in, u32 buf, u32 option, u32 rcv_size);
  void ipc_discard(const Incoming& in);
  u32 place_out_of_line(u64 host_address, u64 size);
  // Starts this session's server (the stand-in for launchd) if the bootstrap
  // port is still the unanswered placeholder a first process begins with.
  void ensure_session_server();

  Arch arch_;
  GuestMemory& mem_;
  Options opt_;
  // ---- threads.cpp ----
  // Every guest thread runs on a host thread of its own. The interpreters
  // run unlocked; the Mach side of the kernel (ports, semaphores, the thread
  // list) is guarded by `kmu_`, and anything that waits does so on `kcv_`.
  Thread* new_thread();          // call with kmu_ held
  void start_thread(Thread* t);  // begins running it
  int run_thread(Thread& t);
  Thread* thread_for_port(u32 port);
  SysRet sys_bsdthread_create(Thread& t, u32 func, u32 arg, u32 stack, u32 pthread, u32 flags);

  // ---- processes.cpp ----
  SysRet sys_fork(Thread& t);
  void lock_for_fork(bool take);  // in commpage.cpp, next to one of the locks
  // `prefer` is the CPU to run a universal program on, if it has that half.
  SysRet sys_execve(const std::string& path, u32 argv, u32 envp,
                    std::optional<Arch> prefer = std::nullopt);
  SysRet sys_posix_spawn(u32 pid_out, const std::string& path, u32 desc, u32 argv, u32 envp);

  // ---- signals.cpp ----
  SysRet sys_sigaction(int sig, u32 act, u32 old);
  SysRet sys_sigprocmask(int how, u32 set, u32 old);
  SysRet sys_sigsuspend(Thread& t, u32 mask);
  SysRet sys_sigaltstack(Thread& t, u32 ss, u32 old);
  SysRet sys_sigreturn(Thread& t, u32 uctx, u32 style);
  SysRet sys_sigwait(u32 set, u32 sig_out);
  SysRet sys_setitimer(int which, u32 value, u32 old, bool set);
  // Starts the handler for every signal that has arrived and isn't blocked.
  void deliver_signals(Thread& t);
  bool signal_arrived() const;    // cheap: has any signal arrived at all?
  bool signal_deliverable() const;  // ... that this thread would take now?
  // Should a call that a signal interrupted be run again after the handler?
  bool restart_after_signal(u32 num) const;
  void forget_pending_signals();
  void push_signal_frame_x86(Thread& t, int sig, u32 handler, u32 tramp, u32 flags, u32 mask);
  void push_signal_frame_ppc(Thread& t, int sig, u32 handler, u32 tramp, u32 flags, u32 mask);
  SysRet sys_wait4(int pid, u32 status, int options, u32 rusage);
  // Two results in the two result registers, first one in the first.
  SysRet two_results(u32 first, u32 second) const;

  Thread main_;
  std::vector<std::unique_ptr<Thread>> threads_;
  std::mutex kmu_;
  std::condition_variable kcv_;
  u32 thread_start_ = 0;   // where the C library wants new threads to begin
  u32 pthread_size_ = 0;   // size of its per-thread structure
  std::atomic<bool> exited_{false};
  int exit_code_ = 0;
  std::set<u32> warned_bsd_, warned_mach_, warned_msg_;

  std::map<u32, KPort> kports_;  // the ports that are kernel objects, by name
  u32 kport_set_ = 0;            // all of them, for the thread that answers other processes
  // The input system's event queue, shared with the window server, and the
  // port it is told on when there are events.
  u32 hid_memory_ = 0, hid_memory_size_ = 0, hid_notify_ = 0;
  u32 hid_element_ = 96;  // bytes per slot of the event ring
  u32 hid_event_number_ = 0;
  s32 hid_x_ = 0, hid_y_ = 0;  // where the pointer last was
  // When the window server last put the pointer somewhere itself, and until
  // when the pointer moves by the mouse's own movement from there rather than
  // following the Mac's pointer (uptime in nanoseconds). See set_pointer.
  u64 hid_last_warp_ = 0, hid_relative_until_ = 0;
  bool hid_viewer_captured_ = false;
  u32 hid_buttons_ = 0;        // the mouse buttons held, the left one in bit 0
  // For working on cocaine: until this time, every message this process
  // sends is noted, to see what the window server does with an input event.
  std::atomic<u64> note_sends_until_{0}, note_from_{0};
  std::mutex hid_mu_;
  // The window server moving the pointer itself (CGWarpMouseCursorPosition
  // and the like): where it is now, on the display.
  void set_pointer(s32 x, s32 y);
  // Puts one input event (a type from the HID system's list, at a place on
  // the display, top-left origin) into the queue and tells the window server.
  // For a key, `key` is its Mac key code (unchanged since 10.5) and `extra`
  // the modifier flags with the character typed in its low 16 bits.
  void post_input_event(u32 type, s32 x, s32 y, u32 key = 0, u32 extra = 0, s32 more = 0);
  void run_input_script(const std::string& script);
  u32 display_at_ = 0;  // where the display's pixels are in this process, if it is the window server
  u32 task_port_ = 0, host_port_ = 0, clock_port_ = 0, iokit_port_ = 0;
  u32 calendar_port_ = 0;  // the clock that tells the time of day, not time since startup
  // The port to this session's stand-in for launchd. A session's first
  // process starts out owning the port itself, with nobody answering it; the
  // server is only started, and given the port, once something needs it.
  u32 bootstrap_port_ = 0;
  bool owns_bootstrap_ = false;
  std::string session_name_;  // empty until the session has a server
  // For a guest told it is root: the user and group it has since switched
  // itself to (real and effective of each). A daemon drops to a lesser user
  // and takes root back, and expects both to work.
  std::atomic<u32> ids_[4] = {};
  std::mutex session_mu_;
  // Messages received but too big for the buffer the guest offered, kept for
  // its next try, by the port they arrived on.
  std::map<u32, std::deque<Incoming>> stash_;

  std::vector<std::string> argv_, envp_;
  std::string exe_path_;
  mutable std::mutex proc_name_mu_;
  std::string proc_name_;
  bool is_login_window_ = false;  // this program is the system's loginwindow, yet to say the session is ready
  bool has_windows_ = false;      // an application, by the look of it: it will want the window server

  // What the guest wants done with each signal. 0 and 1 as the handler mean
  // "default" and "ignore"; `tramp` is the C library routine that calls it.
  struct GuestSigaction {
    u32 handler = 0, tramp = 0, mask = 0, flags = 0;
  };
  GuestSigaction sigactions_[32];
  // In a process forked from a thread other than the main one, that thread:
  // the only one left, so the process ends when it does.
  std::atomic<Thread*> sole_thread_{nullptr};
};
