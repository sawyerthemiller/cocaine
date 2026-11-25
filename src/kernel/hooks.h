#pragma once
// Functions cocaine supplies natively in place of a guest library's own.
//
// Some libraries can't do their job as guest code: OpenGL needs a graphics
// card, and Leopard's drivers are for cards no Apple Silicon Mac has. So when
// such a library's code is mapped, cocaine overwrites the first instruction
// of each function it takes over with a host call (see ppc.h). Whoever calls
// the function, the game or another library, lands in cocaine's version,
// which reads the arguments as the guest passed them and does the work with
// the Mac's own library. See hooks.cpp, and src/gl/ for OpenGL.

#include <cstddef>
#include <initializer_list>

#include "common.h"

class Process;
class GuestMemory;
struct Thread;

// Host call numbers (see ppc.h): one to come back from a guest function
// cocaine called, and from here up, one per function taken over.
constexpr u32 kGuestReturnCall = 0x3FFF;
constexpr u32 kHookCallBase = 0x4000;
// Where in the comm page the "come back" host call sits, for each CPU.
constexpr u32 kPPCGuestReturnAddr = 0xFFFF9FF0u;
constexpr u32 kX86GuestReturnAddr = 0xFFFF1FF0u;

// One call into a native replacement: the guest's arguments, read in order
// by type, and somewhere to put the result. Both CPUs' conventions.
class GuestCall {
 public:
  GuestCall(Process& p, Thread& t);

  u32 i();     // the next integer or pointer argument
  float f();   // the next float
  double d();  // the next double
  void ret(u32 v);
  void ret_f(double v);

  GuestMemory& mem;
  Process& process;
  Thread& thread;
  bool powerpc;

  // Runs a guest function with integer arguments and returns its result.
  u32 call(u32 fn, std::initializer_list<u32> args);
  // A function in a loaded guest library, by its C name ("GetPortBounds"),
  // or 0 if no loaded library has it.
  u32 symbol(const char* name);
  // Guest memory that stays put for the life of the process, for strings
  // and small buffers handed to the guest. Never freed.
  u32 scratch(u32 bytes);
  // The guest's own version of the function taken over, for passing on
  // calls cocaine doesn't handle itself (0 if it couldn't be kept; see
  // HookSpec::keep). Call it with the same arguments.
  u32 original() const { return original_; }
  u32 original_ = 0;

 private:
  u32 word(int n) const;
  int word_ = 0, fpr_ = 0;
};

struct HookSpec {
  // A C function without the leading underscore, or an Objective-C method as
  // written, "-[Class method:]" (the category, if given, is passed over).
  const char* name;
  void (*fn)(GuestCall&);
  bool keep = false;  // keep the guest's own version callable (GuestCall::original)
};

// A library whose functions are taken over: every exported function whose
// name starts with `prefix` is, the ones in `specs` by those handlers and
// the rest by one that says it isn't done yet and returns 0. Explicit specs
// are also taken over when their names don't start with the prefix. An empty
// prefix takes over only the functions in `specs`.
struct HookLibrary {
  const char* suffix;  // how the library's path ends
  const char* prefix;
  const HookSpec* specs;
  size_t count;
  bool intel = false;  // also for Intel guests (the handlers mind the byte order)
  // Set for a library whose stand-ins only work together: unless every one
  // of `specs` is found, none is planted, and this is what the warning says
  // happens instead.
  const char* whole = nullptr;
};

// A native function the guest can call, made on the spot: its address in
// guest memory, for handing to the guest as a callback or a thread's start.
u32 native_function(Process& p, const char* name, void (*fn)(GuestCall&));

// src/gl/gl.cpp and src/al/al.cpp
const HookLibrary* gl_hook_libraries(size_t& n);
const HookLibrary* al_hook_libraries(size_t& n);
// src/audio/coreaudio.cpp
const HookLibrary* audio_hook_libraries(size_t& n);
// src/kernel/hooks.cpp: the window server's screen updates
const HookLibrary* display_hook_libraries(size_t& n);
// src/kernel/panels.cpp: AppKit's open and save panels, shown by this Mac's file picker
const HookLibrary* panel_hook_libraries(size_t& n);
// The panels' lock, for Process::lock_for_fork, and what a forked child must forget.
void panel_fork_lock(bool take);
void panel_fork_child();
