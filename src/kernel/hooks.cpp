// Native stand-ins for guest library functions (see hooks.h), and calling
// guest code from native code.

#include "kernel/hooks.h"

#include <fcntl.h>
#include <sys/param.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "kernel/process.h"

namespace {

// Every function taken over, in host call order. Shared by the whole host
// process, which is one guest process.
struct Hook {
  std::string name;
  void (*fn)(GuestCall&);
  u32 original = 0;
};
std::mutex g_hooks_mu;
std::vector<Hook> g_hooks;
std::unordered_map<std::string, u32> g_hook_index;

// How long the Intel instruction at `p` is, for the few that start functions;
// 0 for any other.
u32 prologue_length(const u8* p) {
  switch (p[0]) {
    case 0x55: case 0x53: case 0x56: case 0x57: case 0x50: case 0x51: case 0x52: return 1;  // push
    case 0x89: case 0x8B:  // mov between registers, or with a small displacement from a register
      if ((p[1] & 0xC0) == 0xC0) return 2;
      if ((p[1] & 0xC7) == 0x45 || (p[1] & 0xC7) == 0x44) return (p[1] & 7) == 4 ? 4 : 3;
      return 0;
    case 0x83: return (p[1] & 0xF8) == 0xE8 || (p[1] & 0xF8) == 0xC0 ? 3 : 0;   // sub/add esp, imm8
    case 0x81: return (p[1] & 0xF8) == 0xE8 || (p[1] & 0xF8) == 0xC0 ? 6 : 0;   // sub/add esp, imm32
    case 0xE8: return 5;  // call, replaced by a push of its return address and a jump (see keep_original)
    default: return 0;
  }
}

u32 hook_for(const std::string& name, void (*fn)(GuestCall&)) {
  std::lock_guard<std::mutex> lk(g_hooks_mu);
  auto it = g_hook_index.find(name);
  if (it != g_hook_index.end()) return it->second;
  const u32 index = (u32)g_hooks.size();
  g_hooks.push_back({name, fn});
  g_hook_index[name] = index;
  return index;
}

u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
u32 le32(const u8* p) { return (u32)p[3] << 24 | (u32)p[2] << 16 | (u32)p[1] << 8 | p[0]; }

// An Objective-C method's symbol without its category:
// "-[NSSavePanel(NSSavePanelLayout) _initContentView]" is
// "-[NSSavePanel _initContentView]". Which category of a class holds a
// method is the library's own business, and may differ between builds.
std::string without_category(const std::string& name) {
  const size_t open = name.find('('), space = name.find(' ');
  if (open == std::string::npos || (space != std::string::npos && open > space)) return name;
  const size_t close = name.find(')', open);
  if (close == std::string::npos) return name;
  return name.substr(0, open) + name.substr(close + 1);
}

bool is_method_name(const char* name) { return (name[0] == '-' || name[0] == '+') && name[1] == '['; }

// The exported functions of the slice of `path` that starts at
// `slice_offset`: names without the leading underscore, unslid addresses.
// Also the __TEXT segment's address and file offset.
//
// Objective-C methods aren't exported: their symbols are local, named as
// written ("-[Class method:]"). AppKit alone has 24,000, so only the ones in
// `methods` are kept (named without the category), for the libraries whose
// methods cocaine takes over.
bool read_exports(const std::string& path, u64 slice_offset, bool big_endian, u32& text_vmaddr, u32& text_fileoff,
                  std::vector<std::pair<std::string, u32>>& out, const std::unordered_set<std::string>& methods) {
  const int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0) return false;
  auto read_at = [&](u64 at, u64 n, std::vector<u8>& buf) {
    buf.resize(n);
    u64 done = 0;
    while (done < n) {
      const ssize_t r = pread(fd, buf.data() + done, n - done, (off_t)(at + done));
      if (r <= 0) return false;
      done += (u64)r;
    }
    return true;
  };
  auto rd = [&](const u8* p) { return big_endian ? be32(p) : le32(p); };
  std::vector<u8> head;
  bool ok = read_at(slice_offset, 28, head);
  if (ok) {
    const u32 ncmds = rd(&head[16]), sizeofcmds = rd(&head[20]);
    std::vector<u8> cmds;
    ok = read_at(slice_offset + 28, sizeofcmds, cmds);
    u32 symoff = 0, nsyms = 0, stroff = 0, strsize = 0;
    bool have_text = false;
    for (u32 k = 0, off = 0; ok && k < ncmds && off + 8 <= cmds.size(); k++) {
      const u32 cmd = rd(&cmds[off]), size = rd(&cmds[off + 4]);
      if (cmd == 1 && off + 56 <= cmds.size() && !memcmp(&cmds[off + 8], "__TEXT", 7)) {  // LC_SEGMENT
        text_vmaddr = rd(&cmds[off + 24]);
        text_fileoff = rd(&cmds[off + 32]);
        have_text = true;
      } else if (cmd == 2 && off + 24 <= cmds.size()) {  // LC_SYMTAB
        symoff = rd(&cmds[off + 8]);
        nsyms = rd(&cmds[off + 12]);
        stroff = rd(&cmds[off + 16]);
        strsize = rd(&cmds[off + 20]);
      }
      if (size < 8) break;
      off += size;
    }
    std::vector<u8> syms, strs;
    ok = ok && have_text && nsyms && read_at(slice_offset + symoff, (u64)nsyms * 12, syms) &&
         read_at(slice_offset + stroff, strsize, strs);
    for (u32 k = 0; ok && k < nsyms; k++) {
      const u8* n = &syms[12 * k];
      const u32 strx = rd(n), value = rd(n + 8);
      const u8 type = n[4], sect = n[5];
      // Defined in the first section (__text), not a debugging entry.
      if (strx + 1 >= strs.size()) continue;
      const bool method = is_method_name((const char*)&strs[strx]);
      if (strs[strx] != '_' && !method) continue;
      if ((type & 0xE0) || (type & 0x0E) != 0x0E || sect != 1) continue;
      if (method && methods.empty()) continue;
      const u32 from = method ? strx : strx + 1;
      std::string name((const char*)&strs[from], strnlen((const char*)&strs[from], strs.size() - from));
      if (method) {
        name = without_category(name);
        if (!methods.count(name)) continue;
      } else if (!(type & 0x11) && name != "VFBSelectMode") {
        continue;  // a function must be external, or private to the library's own files
      }
      out.emplace_back(name, value);
    }
  }
  close(fd);
  return ok;
}

// Where the slice for this CPU starts in a file: 0 for a thin file.
bool slice_of(int fd, bool powerpc, u64& offset) {
  u8 h[8 + 20 * 8];
  const ssize_t n = pread(fd, h, sizeof h, 0);
  if (n < 8) return false;
  if (be32(h) != 0xCAFEBABE) {
    offset = 0;
    return true;
  }
  const u32 count = be32(h + 4);
  for (u32 k = 0; k < count && 8 + 20 * (k + 1) <= (u32)n; k++) {
    const u32 cpu = be32(h + 8 + 20 * k);
    if (cpu == (powerpc ? 18u : 7u)) {
      offset = be32(h + 8 + 20 * k + 8);
      return true;
    }
  }
  return false;
}

void not_done_yet(GuestCall& c);

// Words changed in a library's code as it is mapped, each checked first, so
// that a library built differently is left alone.
struct CodePatch {
  u32 addr;     // unslid
  u32 expect;   // the word there, as the guest reads it
  u32 replace;
};

constexpr u32 lfs(u32 fr, u32 ra, s32 d) { return 0xC0000000u | fr << 21 | ra << 16 | ((u32)d & 0xFFFF); }

// Leopard's window server makes its virtual display 1280 x 1024, in two
// places in CoreGraphics: the rectangle CGXConstructVirtualFramebufferSet
// gives the framebuffer (two float constants), and the display modes it
// offers (a table of 1280 x 1024 at 8, 16 and 32 bits a pixel, with each
// one's bytes to a row). Here both become the size cocaine wants: PowerPC's
// loads of the constants are pointed at the size cocaine keeps in the comm
// page, Intel's constants (immediates in the code) are replaced, and the
// table is rewritten in place.
std::vector<CodePatch> display_size_patches(bool powerpc, bool leopard_1056) {
  u32 w = 0, h = 0;
  guest_display_size(w, h);
  std::vector<CodePatch> out;
  if (powerpc) {
    const s32 W = (s32)(s16)(kPPCDisplaySizeAddr & 0xFFFF), H = W + 4;
    if (leopard_1056) {
      out = {{0xF4A20, lfs(0, 26, 22596), lfs(0, 0, H)},
             {0xF4A24, lfs(13, 25, 23136), lfs(13, 0, W)},
             {0xF4AAC, lfs(13, 2, 22596), lfs(13, 0, H)},
             {0xF4AB0, lfs(0, 9, 23136), lfs(0, 0, W)}};
    } else {
      out = {{0xF413C, lfs(0, 26, -4460), lfs(0, 0, H)},
             {0xF4140, lfs(13, 25, -4292), lfs(13, 0, W)},
             {0xF41C8, lfs(13, 2, -4460), lfs(13, 0, H)},
             {0xF41CC, lfs(0, 9, -4292), lfs(0, 0, W)}};
    }
  } else {
    const float fw = (float)w, fh = (float)h;
    u32 bw, bh;
    memcpy(&bw, &fw, 4);
    memcpy(&bh, &fh, 4);
    if (leopard_1056) {
      out = {{0x15449C, 0x44A00000u, bw}, {0x1544A3, 0x44800000u, bh}, {0x15452D, 0x44A00000u, bw},
             {0x154534, 0x44800000u, bh}};
    } else {
      out = {{0x154140, 0x44A00000u, bw}, {0x154147, 0x44800000u, bh}, {0x1541D1, 0x44A00000u, bw},
             {0x1541D8, 0x44800000u, bh}};
    }
  }
  const u32 table = leopard_1056 ? (powerpc ? 0x56FE78 : 0x693D50) : (powerpc ? 0x56C000 : 0x6904B0);
  const u32 rows[3] = {0x540, 0xA00, 0x1400}, now[3] = {w + 64, w * 2, w * 4};
  for (u32 k = 0; k < 3; k++) {
    const u32 entry = table + 0xC8 * k;
    out.push_back({entry, 1280, w});
    out.push_back({entry + 4, 1024, h});
    out.push_back({entry + 12, rows[k], now[k]});
  }
  if (powerpc && leopard_1056) {
    // The first three modes are 1x1 placeholders, not 640x480 modes.
    // Reuse two unsupported 8-bit slots for RGB555 and 32-bit 640x480;
    // keep the existing 800x600, 1024x768 and configured true-color modes.
    const u32 encodings[2][8] = {
        {0x2D525252, 0x52524747, 0x47474742, 0x42424242, 0, 0, 0, 0},
        {0x2D2D2D2D, 0x2D2D2D2D, 0x52525252, 0x52525252,
         0x47474747, 0x47474747, 0x42424242, 0x42424242}};
    for (u32 k = 0; k < 2; k++) {
      const u32 entry = table - (6 - 3 * k) * 0xC8;
      out.push_back({entry, k ? 1024u : 800u, 640});
      out.push_back({entry + 4, k ? 768u : 600u, 480});
      out.push_back({entry + 8, 2, k ? 4u : 3u});
      out.push_back({entry + 12, k ? 1088u : 832u, k ? 2560u : 1280u});
      out.push_back({entry + 16, 8, k ? 32u : 16u});
      out.push_back({entry + 20, 2, k ? 8u : 5u});
      for (u32 word = 0; word < 8; word++) {
        const u32 before = word < 2 ? 0x50505050 : 0;
        if (before != encodings[k][word])
          out.push_back({entry + 40 + word * 4, before, encodings[k][word]});
      }
      if (!k) out.push_back({entry + 184, 0x403, 0x407});
    }
  }
  return out;
}
constexpr const char* kCoreGraphics = "/CoreGraphics.framework/Versions/A/CoreGraphics";

bool ends_with(const std::string& s, const char* suffix) {
  const size_t n = strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Every list of libraries whose functions cocaine supplies.
typedef const HookLibrary* (*HookList)(size_t&);
constexpr HookList kHookLists[] = {gl_hook_libraries, al_hook_libraries, audio_hook_libraries,
                                   display_hook_libraries, panel_hook_libraries};

// The library at `path`, if it is one of those.
const HookLibrary* hook_library_for(const std::string& path, bool powerpc) {
  const HookLibrary* lib = nullptr;
  for (HookList list : kHookLists) {
    size_t nlibs = 0;
    const HookLibrary* libs = list(nlibs);
    for (size_t k = 0; k < nlibs; k++) {
      if (!powerpc && !libs[k].intel) continue;
      if (ends_with(path, libs[k].suffix)) lib = &libs[k];
    }
  }
  return lib;
}

// The Objective-C methods cocaine takes over in the library at `path`.
std::unordered_set<std::string> wanted_methods(const std::string& path, bool powerpc) {
  std::unordered_set<std::string> out;
  if (const HookLibrary* lib = hook_library_for(path, powerpc)) {
    for (size_t k = 0; k < lib->count; k++) {
      if (is_method_name(lib->specs[k].name)) out.insert(without_category(lib->specs[k].name));
    }
  }
  return out;
}

}  // namespace

// ---- arguments ----------------------------------------------------------------
// The Mac OS X PowerPC convention: integer arguments in r3-r10 and then on
// the stack, 24 bytes above the stack pointer, one word each; floating point
// ones in f1-f13, though each still uses up the integer words it would have
// taken (one for a float, two for a double). The Intel one: everything on
// the stack, just above the return address.

GuestCall::GuestCall(Process& p, Thread& t)
    : mem(p.memory()), process(p), thread(t), powerpc(p.memory().big_endian()) {}

u32 GuestCall::word(int n) const {
  if (!powerpc) return mem.r32(thread.x86.reg[X86State::ESP] + 4 + 4 * (u32)n);
  if (n < 8) return thread.ppc.gpr[3 + n];
  return mem.r32(thread.ppc.gpr[1] + 24 + 4 * (u32)n);
}
u32 GuestCall::i() { return word(word_++); }
float GuestCall::f() {
  if (!powerpc) {
    const u32 bits = word(word_++);
    float v;
    memcpy(&v, &bits, 4);
    return v;
  }
  word_ += 1;
  return (float)thread.ppc.fpr[1 + fpr_++].d;
}
double GuestCall::d() {
  if (!powerpc) {
    const u64 bits = (u64)word(word_ + 1) << 32 | word(word_);
    word_ += 2;
    double v;
    memcpy(&v, &bits, 8);
    return v;
  }
  word_ += 2;
  return thread.ppc.fpr[1 + fpr_++].d;
}
void GuestCall::ret(u32 v) {
  if (powerpc) {
    thread.ppc.gpr[3] = v;
  } else {
    thread.x86.reg[X86State::EAX] = v;
  }
}
void GuestCall::ret_f(double v) {
  if (powerpc) thread.ppc.fpr[1].d = v;
  // (Intel returns floating point values on the x87 stack; nothing taken over needs it yet.)
}
u32 GuestCall::call(u32 fn, std::initializer_list<u32> args) { return process.call_guest(thread, fn, args); }
u32 GuestCall::symbol(const char* name) { return process.guest_symbol(name); }
u32 GuestCall::scratch(u32 bytes) { return process.guest_scratch(bytes); }

namespace {
void not_done_yet(GuestCall& c) {
  c.ret(0);
}
}  // namespace

// ---- taking functions over --------------------------------------------------

void Process::note_code_mapping(u32 at, u32 len, int fd, u64 offset) {
  const bool powerpc = arch_ == Arch::PPC;
  char path[MAXPATHLEN];
  if (fcntl(fd, F_GETPATH, path) != 0) return;
  u64 slice = 0;
  if (!slice_of(fd, powerpc, slice)) return;
  MappedImage img;
  img.path = path;
  img.slice_offset = slice;
  img.at = at;
  img.len = len;
  img.offset = offset;

  bool known_ppc_1056 = false;
  if (ends_with(img.path, kCoreGraphics) && read_image(img)) {
    u32 done = 0;
    size_t count = display_size_patches(powerpc, false).size();
    for (bool leopard_1056 : {false, true}) {
      const std::vector<CodePatch> patches = display_size_patches(powerpc, leopard_1056);
      bool matches = true;
      for (const CodePatch& p : patches) {
        const u32 where = p.addr + img.slide;
        if (where < at || (u64)where + 4 > (u64)at + len || mem_.r32(where) != p.expect) {
          matches = false;
          break;
        }
      }
      if (!matches) continue;
      count = patches.size();
      for (const CodePatch& p : patches) {
        mem_.w32(p.addr + img.slide, p.replace);
        done++;
      }
      known_ppc_1056 = powerpc && leopard_1056;
      break;
    }
    if (done != count) {
      log_warn("this CoreGraphics is not the one cocaine knows: its display may stay 1280 x 1024");
    }
    if (const char* f = getenv("COCAINE_NOTE_PATCHES")) {
      if (FILE* out = fopen(f, "a")) {
        fprintf(out, "%d %s: CoreGraphics at slide 0x%x: %u of %zu display patches\n", getpid(), exe_path_.c_str(), img.slide, done, count);
        fclose(out);
      }
    }
  } else if (powerpc && ends_with(img.path, "/DrawSprocket.framework/Versions/A/DrawSprocket") &&
             read_image(img)) {
    // Leopard's depth search advances by two, missing 32-bit modes when
    // only 16/32-bit entries remain. Check the whole loop before fixing
    // its stride; the virtual driver's first three slots must stay 1x1.
    const u32 start = img.slide + 0x1E18;
    const u32 loop[] = {0x7F83D800, 0x409C0014, 0x3BDE0002,
                        0x7F9EE800, 0x419CFFDC, 0x3BC0FFFF};
    if (start >= at && (u64)start + sizeof loop <= (u64)at + len) {
      bool matches = true;
      for (u32 k = 0; k < sizeof loop / sizeof loop[0]; k++) {
        if (mem_.r32(start + 4 * k) != loop[k]) matches = false;
      }
      if (matches) mem_.w32(start + 8, 0x3BDE0001);  // addi r30, r30, 1
      else log_warn("this DrawSprocket is not the one cocaine knows: its depth search is unchanged");
    }
  }

  // Is it one whose functions cocaine supplies?
  const HookLibrary* lib = hook_library_for(img.path, powerpc);

  if (lib) {
    if (!read_image(img) || !img.usable) return;
    std::unordered_map<std::string, void (*)(GuestCall&)> mine;
    std::unordered_set<std::string> keep;
    for (size_t k = 0; k < lib->count; k++) {
      const std::string name = without_category(lib->specs[k].name);
      mine[name] = lib->specs[k].fn;
      if (lib->specs[k].keep) keep.insert(name);
    }
    // A library taken over whole or not at all: its stand-ins only make
    // sense together, so each must be there, once, inside this mapping.
    bool plant = true;
    if (lib->whole) {
      std::unordered_map<std::string, int> found;
      for (const auto& [name, addr] : img.exports) {
        const u32 where = addr + img.slide;
        if (mine.count(name) && where >= at && (u64)where + 8 <= (u64)at + len) found[name]++;
      }
      for (size_t k = 0; k < lib->count && plant; k++) {
        const std::string name = without_category(lib->specs[k].name);
        const int n = found.count(name) ? found[name] : 0;
        if (n == 1) continue;
        plant = false;
        static std::atomic<bool> said{false};
        if (!said.exchange(true)) {
          log_warn("this %s is not the one cocaine knows (%s is %s): %s", strrchr(lib->suffix, '/') + 1, name.c_str(),
                   n ? "there more than once" : "missing", lib->whole);
        }
      }
    }
    const size_t pl = strlen(lib->prefix);
    u32 taken = 0;
    for (const auto& [name, addr] : img.exports) {
      if (!plant) break;
      auto it = mine.find(name);
      if (it == mine.end() && (!pl || name.compare(0, pl, lib->prefix) != 0)) continue;
      const u32 where = addr + img.slide;
      if (where < at || where + 4 > at + len) continue;
      // The private mode record layout below is known only for this build.
      if (name == "VFBSelectMode" &&
          (!known_ppc_1056 || addr != 0xF5618 || mem_.r32(where) != 0x7C0802A6)) continue;
      const u32 index = hook_for(name, it != mine.end() ? it->second : nullptr);
      if (keep.count(name)) {
        const u32 kept = keep_original(where);
        if (!kept) log_warn("can't keep the guest's own %s; calls cocaine doesn't handle will fail", name.c_str());
        std::lock_guard<std::mutex> lk(g_hooks_mu);
        g_hooks[index].original = kept;
      }
      plant_host_call(where, kHookCallBase + index);
      taken++;
    }
    log_debug("took over %u functions of %s", taken, img.path.c_str());
  }
  std::lock_guard<std::mutex> lk(images_mu_);
  images_.push_back(std::move(img));
}

// Keeps the start of a function cocaine is about to overwrite, followed by a
// jump back into the rest of it, so that the guest's version stays callable.
u32 Process::keep_original(u32 fn) {
  const u32 at = guest_scratch(32);
  if (!at) return 0;
  if (arch_ == Arch::PPC) {
    const u32 first = mem_.r32(fn);
    const u32 op = first >> 26;
    if (op == 16 || op == 18) return 0;  // a relative branch can't move
    const u32 back = fn + 4;
    mem_.w32(at, first);
    mem_.w32(at + 4, 0x3D800000u | (back >> 16));          // lis r12, hi
    mem_.w32(at + 8, 0x618C0000u | (back & 0xFFFF));        // ori r12, r12, lo
    mem_.w32(at + 12, 0x7D8903A6u);                         // mtctr r12
    mem_.w32(at + 16, 0x4E800420u);                         // bctr
    return at;
  }
  // n walks the original, d the copy: they part once a call is rewritten.
  u32 n = 0, d = 0;
  while (n < 4) {
    const u32 len = prologue_length(mem_.host(fn + n));
    if (!len) return 0;
    if (mem_.r8(fn + n) == 0xE8) {
      // A call moved as it is would push a return address inside the copy,
      // and code that reads its own location from that address (call next;
      // pop ebx, or a get_pc_thunk helper) would get the copy's. So push the
      // return address the original would have pushed, and jump to the
      // target. The call returns into the original function past the bytes
      // overwritten (a call here always ends beyond the first four), so
      // nothing more is copied and no jump back is needed.
      const u32 ret = fn + n + 5;
      const u32 target = ret + mem_.r32(fn + n + 1);
      mem_.w8(at + d, 0x68);  // push imm32
      mem_.w32(at + d + 1, ret);
      mem_.w8(at + d + 5, 0xE9);  // jmp rel32
      mem_.w32(at + d + 6, target - (at + d + 10));
      return at;
    }
    memcpy(mem_.host(at + d), mem_.host(fn + n), len);
    n += len;
    d += len;
  }
  // At most three bytes, one instruction of up to six and this jump: well
  // within the 32 bytes taken.
  mem_.w8(at + d, 0xE9);  // jmp to the rest
  mem_.w32(at + d + 1, (fn + n) - (at + d + 5));
  return at;
}

void Process::run_hook(Thread& t, u32 index) {
  void (*fn)(GuestCall&) = nullptr;
  std::string name;
  u32 original = 0;
  {
    std::lock_guard<std::mutex> lk(g_hooks_mu);
    if (index >= g_hooks.size()) return;
    const Hook& h = g_hooks[index];
    fn = h.fn;
    original = h.original;
    if (!fn) name = h.name;
  }
  GuestCall c(*this, t);
  c.original_ = original;
  if (fn) {
    fn(c);
  } else {
    static std::mutex said_mu;
    static std::unordered_set<std::string> said;
    std::lock_guard<std::mutex> lk(said_mu);
    if (said.insert(name).second) log_warn("%s is not passed through to the Mac yet", name.c_str());
    not_done_yet(c);
  }
  // The function's first instruction was the host call: return from it.
  if (arch_ == Arch::PPC) {
    t.ppc.pc = t.ppc.lr & ~3u;
  } else {
    X86State& s = t.x86;
    s.eip = mem_.r32(s.reg[X86State::ESP]);
    s.reg[X86State::ESP] += 4;
  }
}

// A host call instruction at `where`: one word on PowerPC, `int 0xFE` and the
// number (four bytes) on Intel.
void Process::plant_host_call(u32 where, u32 id) {
  if (arch_ == Arch::PPC) {
    mem_.w32(where, kPPCHostCall | id);
  } else {
    const u8 code[4] = {0xCD, kX86HostCallVector, (u8)id, (u8)(id >> 8)};
    memcpy(mem_.host(where), code, 4);
  }
}

// The window server's CGXUpdateDisplay puts each update on the screen. When
// it returns, the display holds a whole picture, and it is handed to the
// viewer then, on the window server's own thread, before the next update can
// start: no picture of a window half redrawn.
namespace {
void display_updated(GuestCall& c) {
  u32 w[8];
  for (u32& v : w) v = c.i();
  const u32 r = c.original() ? c.call(c.original(), {w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]}) : 0;
  c.process.publish_display();
  c.ret(r);
}
void display_mode_selected(GuestCall& c) {
  u32 w[8];
  for (u32& v : w) v = c.i();
  const u32 r = c.call(c.original(), {w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]});
  bool found = false;
  if (!r && w[0] <= UINT32_MAX - 160 && c.mem.range_ok(w[0] + 160, 4)) {
    const u32 obj = c.mem.r32(w[0] + 160);
    if (obj <= UINT32_MAX - 448 && c.mem.range_ok(obj + 448, 4) && w[1] < 16) {
      const u32 table = c.mem.r32(obj + 448);
      const u64 desc64 = (u64)table + 200 * w[1];
      if (desc64 <= UINT32_MAX && c.mem.range_ok((u32)desc64, 28)) {
        const u32 desc = (u32)desc64;
        c.process.set_display_format(c.mem.r32(desc + 8), c.mem.r32(desc + 12),
                                     c.mem.r32(desc + 20), c.mem.r32(desc + 24));
        found = true;
      }
    }
  }
  if (!r && !found) log_warn("cannot read the virtual display's selected mode");
  c.ret(r);
}
const HookSpec kDisplay[] = {{"CGXUpdateDisplay", display_updated, true},
                             {"VFBSelectMode", display_mode_selected, true}};
const HookLibrary kDisplayLibraries[] = {
    {"/CoreGraphics.framework/Versions/A/CoreGraphics", "", kDisplay, 2, true},
};
}  // namespace
const HookLibrary* display_hook_libraries(size_t& n) {
  n = 1;
  return kDisplayLibraries;
}

u32 native_function(Process& p, const char* name, void (*fn)(GuestCall&)) {
  const u32 index = hook_for(name, fn);
  const u32 at = p.guest_scratch(16);
  if (at) p.plant_host_call(at, kHookCallBase + index);
  return at;
}

// Reads a mapped library's exported functions, the first time they are wanted.
bool Process::read_image(MappedImage& img) {
  if (img.read) return img.usable;
  img.read = true;
  u32 text_vmaddr = 0, text_fileoff = 0;
  const bool powerpc = arch_ == Arch::PPC;
  if (!read_exports(img.path, img.slice_offset, powerpc, text_vmaddr, text_fileoff, img.exports,
                    wanted_methods(img.path, powerpc)))
    return false;
  // Only the mapping of __TEXT says where the library went.
  img.usable = img.offset == img.slice_offset + text_fileoff;
  img.slide = img.at - text_vmaddr;
  return img.usable;
}

u32 Process::guest_symbol(const std::string& name) {
  std::lock_guard<std::mutex> lk(images_mu_);
  for (MappedImage& img : images_) {
    if (!read_image(img)) continue;
    for (const auto& [n, addr] : img.exports) {
      if (n == name) return addr + img.slide;
    }
  }
  return 0;
}

u32 Process::guest_scratch(u32 bytes) {
  std::lock_guard<std::mutex> lk(images_mu_);
  bytes = (bytes + 15) & ~15u;
  if (bytes > scratch_left_) {
    const u32 size = bytes > 0x100000 ? (bytes + 0xFFFF) & ~0xFFFFu : 0x100000;
    const u32 at = mem_.find_free(size, 0x10000000u, 0xC0000000u);
    if (!at || !mem_.map(at, size, GuestMemory::kRead | GuestMemory::kWrite | GuestMemory::kExec)) return 0;  // code too: stubs, trampolines
    scratch_at_ = at;
    scratch_left_ = size;
  }
  const u32 at = scratch_at_;
  scratch_at_ += bytes;
  scratch_left_ -= bytes;
  return at;
}

// ---- calling the guest ------------------------------------------------------

u32 Process::call_guest(Thread& t, u32 fn, std::initializer_list<u32> args) {
  std::vector<GuestArg> words;
  for (u32 a : args) words.push_back({a});
  return call_guest_args(t, fn, words);
}

u32 Process::call_guest_args(Thread& t, u32 fn, const std::vector<GuestArg>& args) {
  if (!fn) return 0;
  if (arch_ != Arch::PPC) {  // the arguments on the stack, and a return address that comes back here
    const X86State saved = t.x86;
    X86State& s = t.x86;
    const u32 n = (u32)args.size();
    const u32 base = (saved.reg[X86State::ESP] - 1024) & ~15u;
    const u32 args_at = base - ((4 * n + 15) & ~15u);  // sixteen-byte aligned, as the Mac's convention wants
    u32 k = 0;
    for (const GuestArg& a : args) mem_.w32(args_at + 4 * k++, a.bits);
    s.reg[X86State::ESP] = args_at - 4;
    mem_.w32(args_at - 4, kX86GuestReturnAddr);
    s.eip = fn;
    t.returned = false;
    run_x86(t);
    const u32 result = s.reg[X86State::EAX];
    const u64 icount = s.icount;
    t.x86 = saved;
    t.x86.icount = icount;
    return result;
  }
  const PPCState saved = t.ppc;
  PPCState& s = t.ppc;
  // A fresh frame well below the caller's, past the 224 bytes below the
  // stack pointer that a PowerPC function may use without moving it.
  const u32 extra = args.size() > 8 ? (u32)(args.size() - 8) * 4 : 0;
  const u32 sp = (saved.gpr[1] - 1024 - extra) & ~15u;
  mem_.w32(sp, saved.gpr[1]);
  s.gpr[1] = sp;
  // Integers in r3 to r10; floats in f1 to f13, each still using up an
  // integer register's place, as Apple's PowerPC convention has it.
  int k = 0, f = 0;
  for (const GuestArg& a : args) {
    if (a.is_float) {
      float v;
      memcpy(&v, &a.bits, 4);
      if (f < 13) s.fpr[1 + f++].d = v;
    } else if (k < 8) {
      s.gpr[3 + k] = a.bits;
    }
    if (k >= 8) mem_.w32(sp + 24 + 4 * (u32)k, a.bits);
    k++;
  }
  s.lr = kPPCGuestReturnAddr;
  s.pc = fn & ~3u;
  // A call through a pointer arrives with the function's own address in
  // r12, and code built to run at any address (an Objective-C method, called
  // by objc_msgSend that way) finds its data from it.
  s.gpr[12] = fn & ~3u;
  t.returned = false;
  run_ppc(t);
  const u32 result = s.gpr[3];
  const u64 icount = s.icount;
  t.ppc = saved;
  t.ppc.icount = icount;
  return result;
}
