// OpenGL for PowerPC guests, done by the Mac's own OpenGL.
//
// Leopard's OpenGL drives graphics cards no Apple Silicon Mac has, so cocaine
// takes over the guest's libGL and AGL (see kernel/hooks.h) and does each
// call with the host's OpenGL instead. Three things differ from simply
// forwarding the calls:
//
//  - Byte order. Guest memory is big-endian. Single values arrive in
//    registers and need nothing, but arrays (matrices, vertex arrays,
//    indices, glGet results) are byte-swapped on the way in or out. Pixel
//    data needs no copy: the host's GL_UNPACK_SWAP_BYTES/GL_PACK_SWAP_BYTES
//    are kept the opposite of what the guest asked for, which reads and
//    writes multi-byte pixels the guest's way and leaves bytes alone.
//
//  - Vertex arrays live in guest memory, where the host can't read them as
//    they are. The pointers are noted, and at each draw the elements it uses
//    are converted into host buffers.
//
//  - Where the picture goes. Each guest context renders into an offscreen
//    framebuffer on the host. aglSwapBuffers reads the frame back and copies
//    it into the guest window's own pixels (the QuickDraw port given to
//    aglSetDrawable), then asks the guest's window server to show it, the
//    way a game that drew in software would. The window server, the viewer
//    and window moves then all just work.

#define GL_SILENCE_DEPRECATION 1

#include <CoreGraphics/CoreGraphics.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <OpenGL/glu.h>

#include <cmath>
#include <cctype>
#include <ctime>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "gl/vertex_copy.h"
#include "kernel/display_pixels.h"
#include "kernel/hooks.h"
#include "kernel/process.h"
#include "session/pacing.h"

namespace {

// ---- guest memory -------------------------------------------------------------

// Whether the guest is big-endian (PowerPC). Intel guests share this Mac's
// byte order, and nothing of theirs needs swapping. One CPU per process.
bool g_big = true;

inline u8* gp(GuestCall& c, u32 addr) {
  g_big = c.powerpc;
  return addr ? c.mem.host(addr) : nullptr;
}

inline u16 rd16(const u8* p) { return g_big ? (u16)(p[0] << 8 | p[1]) : (u16)(p[1] << 8 | p[0]); }
inline void wr16(u8* p, u16 v) {
  p[g_big ? 0 : 1] = (u8)(v >> 8);
  p[g_big ? 1 : 0] = (u8)v;
}
inline u32 rd32(const u8* p) {
  return g_big ? (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]
               : (u32)p[3] << 24 | (u32)p[2] << 16 | (u32)p[1] << 8 | p[0];
}
inline u64 rd64(const u8* p) { return g_big ? (u64)rd32(p) << 32 | rd32(p + 4) : (u64)rd32(p + 4) << 32 | rd32(p); }
inline void wr32(u8* p, u32 v) {
  if (!g_big) {
    memcpy(p, &v, 4);
    return;
  }
  p[0] = (u8)(v >> 24);
  p[1] = (u8)(v >> 16);
  p[2] = (u8)(v >> 8);
  p[3] = (u8)v;
}
inline void wr64(u8* p, u64 v) {
  if (!g_big) {
    memcpy(p, &v, 8);
    return;
  }
  wr32(p, (u32)(v >> 32));
  wr32(p + 4, (u32)v);
}
inline float rdf(const u8* p) {
  const u32 v = rd32(p);
  float f;
  memcpy(&f, &v, 4);
  return f;
}
inline double rdd(const u8* p) {
  const u64 v = rd64(p);
  double d;
  memcpy(&d, &v, 8);
  return d;
}

// `n` floats (or doubles, or ints) from the guest.
template <int N>
struct Floats {
  GLfloat v[N] = {};
  Floats(GuestCall& c, u32 addr, int n = N) {
    const u8* p = gp(c, addr);
    for (int k = 0; p && k < n && k < N; k++) v[k] = rdf(p + 4 * k);
  }
};
template <int N>
struct Doubles {
  GLdouble v[N] = {};
  Doubles(GuestCall& c, u32 addr, int n = N) {
    const u8* p = gp(c, addr);
    for (int k = 0; p && k < n && k < N; k++) v[k] = rdd(p + 8 * k);
  }
};

// ---- contexts -----------------------------------------------------------------

struct Array {
  bool on = false;
  GLboolean normalized = GL_FALSE;
  GLint size = 4;
  GLenum type = GL_FLOAT;
  GLsizei stride = 0;
  u32 ptr = 0;
  std::vector<u8> host;
};

struct ClientArrays {
  Array vertex, normal, color;
  Array tex[8];
  std::vector<Array> attribs;
  int unit = 0;
  GLbitfield mask = 0;
  bool unpack_swap = false, pack_swap = false;
};

struct SurfaceFunctions {
  u32 window_context = 0, color_space = 0, provider = 0, image_create = 0, draw = 0, flush = 0;
  u32 release_context = 0, release_color = 0, release_provider = 0, release_image = 0, window_bounds = 0;
  u32 display_context = 0;
};

struct TextureMipRange {
  GLint requested = 1000;
  GLint applied = 1000;
};

struct SharedTextures {
  std::mutex mutex;
  std::unordered_map<GLuint, TextureMipRange> mip_ranges;
};

struct Context {
  u32 handle = 0;
  u32 refs = 1;
  CGLContextObj cgl = nullptr;
  GLuint fbo = 0, color = 0, auxiliary = 0, depth = 0;
  int width = 0, height = 0;
  u32 port = 0;
  u32 connection = 0, window = 0, surface = 0;
  u32 display = 0;
  u32 private_client_storage = 0;
  u32 surface_color_space = 0, surface_provider = 0;
  u32 surface_rect = 0, window_rect = 0, guest_frame = 0, guest_frame_size = 0;
  float surface_x = 0, surface_y = 0;
  SurfaceFunctions surface_functions;
  bool unpack_swap = false, pack_swap = false;  // what the guest asked for
  Array vertex, normal, color_array;
  Array tex[8];
  std::vector<Array> attribs;
  bool attribs_ready = false;
  int client_unit = 0;
  std::vector<ClientArrays> client_stack;
  std::vector<u8> frame;
  GLuint readback = 0;
  size_t readback_size = 0;
  u32 frames = 0;
  std::shared_ptr<SharedTextures> textures = std::make_shared<SharedTextures>();
};

std::mutex g_mu;
std::unordered_map<u32, Context*> g_contexts;
struct PixelFormat {
  CGLPixelFormatObj cgl = nullptr;
  u32 refs = 1;
};
std::unordered_map<u32, PixelFormat> g_formats;
u32 g_next_handle = 0x00C0C000;
thread_local Context* t_current = nullptr;
thread_local CGLError t_agl_error = kCGLNoError;

struct RendererQuery {
  CGLRendererInfoObj info = nullptr;
  std::vector<u32> handles;
  ~RendererQuery() {
    if (info) {
      const CGLError error = CGLDestroyRendererInfo(info);
      if (error != kCGLNoError) log_warn("OpenGL: cannot release renderer information (%d)", (int)error);
    }
  }
};
struct RendererEntry {
  std::shared_ptr<RendererQuery> query;
  GLint index;
};
std::unordered_map<u32, RendererEntry> g_renderers;

void agl_error(CGLError error, const char* operation) {
  t_agl_error = error;
  if (error != kCGLNoError) log_warn("OpenGL: %s failed (%d)", operation, (int)error);
}

std::vector<std::string> g_said;
void say_once(const std::string& what) {
  std::lock_guard<std::mutex> lk(g_mu);
  for (const auto& s : g_said) {
    if (s == what) return;
  }
  g_said.push_back(what);
  log_info("%s", what.c_str());
}

Context* context_of(u32 handle) {
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_contexts.find(handle);
  return it == g_contexts.end() ? nullptr : it->second;
}

// The framebuffer the guest draws into, sized to its window.
void size_framebuffer(Context* x, int w, int h) {
  if (w <= 0 || h <= 0) w = h = 1;
  if (x->fbo && w == x->width && h == x->height) return;
  if (!x->fbo) {
    glGenFramebuffersEXT(1, &x->fbo);
    glGenRenderbuffersEXT(1, &x->color);
    glGenRenderbuffersEXT(1, &x->auxiliary);
    glGenRenderbuffersEXT(1, &x->depth);
  }
  glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, x->fbo);
  glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, x->color);
  glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, w, h);
  glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, x->color);
  glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, x->auxiliary);
  glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, w, h);
  glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT1_EXT, GL_RENDERBUFFER_EXT, x->auxiliary);
  glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, x->depth);
  glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH24_STENCIL8_EXT, w, h);
  glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, x->depth);
  glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, x->depth);
  const GLenum st = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
  if (st != GL_FRAMEBUFFER_COMPLETE_EXT) log_warn("OpenGL: the offscreen framebuffer is incomplete (0x%x)", st);
  glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT);
  glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
  x->width = w;
  x->height = h;
  x->frame.resize((size_t)w * h * 4);
}

// The guest window's content size, from its port.
bool port_size(GuestCall& c, u32 port, int& w, int& h) {
  static u32 rect = 0;
  if (!rect) rect = c.scratch(16);
  const u32 fn = c.symbol("GetPortBounds");
  if (!port || !fn || !rect) return false;
  c.call(fn, {port, rect});
  const u8* r = gp(c, rect);
  const int top = (s16)rd16(r), left = (s16)rd16(r + 2), bottom = (s16)rd16(r + 4), right = (s16)rd16(r + 6);
  w = right - left;
  h = bottom - top;
  return w > 0 && h > 0;
}

bool read_frame(Context* x) {
  if (!x->readback) glGenBuffers(1, &x->readback);
  if (!x->readback) {
    log_warn("OpenGL: cannot create frame readback buffer");
    return false;
  }
  GLint pack = 0;
  glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack);
  glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  glPixelStorei(GL_PACK_ROW_LENGTH, 0);
  glPixelStorei(GL_PACK_SKIP_ROWS, 0);
  glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
  glPixelStorei(GL_PACK_SWAP_BYTES, GL_FALSE);
  GLint read = 0;
  glGetIntegerv(GL_READ_BUFFER, &read);
  glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
  // A pack buffer lets the GPU untile the image instead of doing it on the CPU.
  // Map this frame immediately: no extra frame of presentation latency.
  glBindBuffer(GL_PIXEL_PACK_BUFFER, x->readback);
  if (x->readback_size != x->frame.size()) {
    glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)x->frame.size(), nullptr, GL_STREAM_READ);
    GLint allocated = 0;
    glGetBufferParameteriv(GL_PIXEL_PACK_BUFFER, GL_BUFFER_SIZE, &allocated);
    x->readback_size = allocated > 0 ? (size_t)allocated : 0;
  }
  bool ok = false;
  if (x->readback_size != x->frame.size()) {
    log_warn("OpenGL: cannot allocate frame readback buffer (%zu bytes)", x->frame.size());
  } else {
    // A R G B on PowerPC, B G R A on Intel.
    glReadPixels(0, 0, x->width, x->height, GL_BGRA,
                 g_big ? GL_UNSIGNED_INT_8_8_8_8 : GL_UNSIGNED_INT_8_8_8_8_REV, nullptr);
    const void* pixels = glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
    if (pixels) {
      memcpy(x->frame.data(), pixels, x->frame.size());
      ok = glUnmapBuffer(GL_PIXEL_PACK_BUFFER) == GL_TRUE;
      if (!ok) log_warn("OpenGL: frame readback buffer became invalid");
    } else {
      log_warn("OpenGL: cannot map frame readback buffer");
    }
  }
  glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack);
  glReadBuffer((GLenum)read);
  glPopClientAttrib();
  return ok;
}

void frame_shown(Context* x, const char* kind) {
  if (++x->frames == 1) say_once(std::string("OpenGL: first ") + kind + "frame shown");
  if (x->frames % 300 == 0) log_info("OpenGL: %u %sframes shown", x->frames, kind);
  if (const char* e = getenv("COCAINE_GL_FRAME")) {
    if (strtoul(e, nullptr, 10) == x->frames) {
      if (const char* colon = strchr(e, ':')) {
        if (FILE* f = fopen(colon + 1, "wb")) {
          fwrite(x->frame.data(), 1, x->frame.size(), f);
          fclose(f);
        }
      }
    }
  }
}

// The frame, into the guest window's pixels, and onto the screen.
void present(GuestCall& c, Context* x) {
  if (!x->port || !x->fbo) return;
  if (!read_frame(x)) return;
  static u32 lock, unlock, pixmap, base, rowbytes, pixdepth, pixbounds, flush, dirty, rect, whole;
  if (!lock) {
    lock = c.symbol("LockPortBits");
    unlock = c.symbol("UnlockPortBits");
    pixmap = c.symbol("GetPortPixMap");
    base = c.symbol("GetPixBaseAddr");
    rowbytes = c.symbol("GetPixRowBytes");
    pixdepth = c.symbol("GetPixDepth");
    pixbounds = c.symbol("GetPixBounds");
    flush = c.symbol("QDFlushPortBuffer");
    dirty = c.symbol("QDAddRectToDirtyRegion");
    rect = c.scratch(16);
    whole = c.scratch(16);
  }
  if (!lock || !unlock || !pixmap || !base || !rowbytes || !pixdepth || !flush) {
    say_once("OpenGL: can't find QuickDraw's port functions, so nothing is shown");
    return;
  }
  c.call(lock, {x->port});
  const u32 pm = c.call(pixmap, {x->port});
  const u32 pixels = c.call(base, {pm});
  const u32 stride = c.call(rowbytes, {pm}) & 0x3FFF;
  const u32 depth = c.call(pixdepth, {pm});
  int left = 0, top = 0, pw = 0, ph = 0;
  if (pixbounds && rect) {
    c.call(pixbounds, {pm, rect});
    const u8* r = gp(c, rect);
    top = (s16)rd16(r);
    left = (s16)rd16(r + 2);
    ph = (s16)rd16(r + 4) - top;
    pw = (s16)rd16(r + 6) - left;
  }
  // The pixels start at the pixmap's corner, which for a window takes in
  // its title bar; the port's own bounds are where the content is.
  int ctop = 0, cleft = 0;
  if (const u32 portbounds = c.symbol("GetPortBounds"); portbounds && rect) {
    c.call(portbounds, {x->port, rect});
    const u8* r = gp(c, rect);
    ctop = (s16)rd16(r);
    cleft = (s16)rd16(r + 2);
  }
  const int dy = ctop - top, dx = cleft - left;
  if (depth != 16 && depth != 32) {
    say_once("OpenGL: unsupported QuickDraw pixel depth " + std::to_string(depth));
    c.call(unlock, {x->port});
    return;
  }
  const u32 bytes_per_pixel = depth / 8;
  if (pixels && stride >= bytes_per_pixel && dx >= 0 && dy >= 0) {
    const int w = std::min(pw > 0 ? std::min(pw - dx, x->width) : x->width,
                           (int)(stride / bytes_per_pixel) - dx);
    const int h = ph > 0 ? std::min(ph - dy, x->height) : x->height;
    for (int y = 0; w > 0 && y < h; y++) {
      const u8* src = x->frame.data() + (size_t)(x->height - 1 - y) * x->width * 4;
      const u64 at = (u64)pixels + (u64)(y + dy) * stride + (u64)dx * bytes_per_pixel;
      const u64 length = (u64)w * bytes_per_pixel;
      if (at > UINT32_MAX || !c.mem.range_ok((u32)at, length, GuestMemory::kWrite)) {
        say_once("OpenGL: QuickDraw pixel buffer is not writable");
        break;
      }
      u8* dst = c.mem.host((u32)at);
      if (depth == 16) display_to_rgb555(src, dst, (u32)w, c.powerpc);
      else memcpy(dst, src, (size_t)length);
    }
  }
  top = ctop;
  left = cleft;
  c.call(unlock, {x->port});
  // QuickDraw shows only what it knows changed, and it didn't see these
  // pixels change: say the whole window did.
  if (dirty && whole) {
    u8* r = gp(c, whole);
    const s16 box[4] = {(s16)top, (s16)left, (s16)(top + x->height), (s16)(left + x->width)};
    for (int k = 0; k < 4; k++) wr16(r + 2 * k, (u16)box[k]);
    c.call(dirty, {x->port, whole});
  }
  c.call(flush, {x->port, 0});
  frame_shown(x, "");
}

// ---- AGL ------------------------------------------------------------------------

void aglGetVersion(GuestCall& c) {
  const u32 major = c.i(), minor = c.i();
  if (!c.original()) {
    log_warn("OpenGL: the guest's aglGetVersion is unavailable");
    return;
  }
  c.call(c.original(), {major, minor});
}

u32 choose_pixel_format(GuestCall& c, u32 attribs, const char* api) {
  g_big = c.powerpc;
  std::string list;
  if (attribs) {
    bool ended = false;
    for (u32 i = 0; i < 128; i++) {
      const u64 at = (u64)attribs + 4 * i;
      if (at > UINT32_MAX || !c.mem.range_ok((u32)at, 4, GuestMemory::kRead)) {
        agl_error(kCGLBadAddress, "reading pixel format attributes");
        return 0;
      }
      const u32 value = c.mem.r32((u32)at);
      if (!value) {
        ended = true;
        break;
      }
      if (list.size() < 200) list += " " + std::to_string(value);
    }
    if (!ended) {
      agl_error(kCGLBadAttribute, "reading pixel format attributes");
      return 0;
    }
    static std::once_flag warned;
    std::call_once(warned, [] {
      log_warn("OpenGL: requested pixel format attributes are not yet enforced");
    });
  }
  log_debug("%s:%s", api, list.c_str());
  // Keep the existing offscreen context format; requested AGL attributes are
  // not yet used to choose it.
  const CGLPixelFormatAttribute attrs[] = {
      kCGLPFAAccelerated, kCGLPFAColorSize, (CGLPixelFormatAttribute)24, kCGLPFAAlphaSize,
      (CGLPixelFormatAttribute)8, kCGLPFADepthSize, (CGLPixelFormatAttribute)24, kCGLPFAStencilSize,
      (CGLPixelFormatAttribute)8, kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_Legacy,
      (CGLPixelFormatAttribute)0};
  CGLPixelFormatObj pf = nullptr;
  GLint n = 0;
  CGLError error = CGLChoosePixelFormat(attrs, &pf, &n);
  if (error != kCGLNoError || !pf || n <= 0) {
    if (pf) CGLReleasePixelFormat(pf);
    agl_error(error != kCGLNoError ? error : kCGLBadPixelFormat, "choosing a pixel format");
    return 0;
  }
  std::lock_guard<std::mutex> lk(g_mu);
  const u32 handle = g_next_handle += 0x10;
  g_formats[handle] = {pf, 1};
  agl_error(kCGLNoError, "choosing a pixel format");
  return handle;
}

void aglChoosePixelFormat(GuestCall& c) {
  c.i();
  c.i();
  const u32 attribs = c.i();
  c.ret(choose_pixel_format(c, attribs, "aglChoosePixelFormat"));
}

CGLError release_pixel_format(u32 handle) {
  CGLPixelFormatObj pf = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_formats.find(handle);
    if (it == g_formats.end()) return kCGLBadPixelFormat;
    if (--it->second.refs) return kCGLNoError;
    pf = it->second.cgl;
    g_formats.erase(it);
  }
  CGLReleasePixelFormat(pf);
  return kCGLNoError;
}

void aglDestroyPixelFormat(GuestCall& c) {
  agl_error(release_pixel_format(c.i()), "destroying a pixel format");
}

void aglDescribePixelFormat(GuestCall& c) {
  const u32 handle = c.i(), attr = c.i(), out = c.i();
  if (!out || !c.mem.range_ok(out, 4, GuestMemory::kWrite)) {
    agl_error(kCGLBadAddress, "describing a pixel format");
    return c.ret(0);
  }
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_formats.find(handle);
  if (it == g_formats.end()) {
    agl_error(kCGLBadPixelFormat, "describing a pixel format");
    return c.ret(0);
  }
  GLint value = 0;
  CGLError error;
  switch (attr) {
    case 4:  // AGL_RGBA
      value = 1;
      error = kCGLNoError;
      break;
    case 5: case 6: case 7: case 11: case 12: case 13:
    case 51: case 52: case 53: case 54: case 55: case 56:
    case 57: case 58: case 59: case 60: case 61: case 70:
    case 71: case 72: case 73: case 74: case 75: case 76:
    case 78: case 80: case 81: case 83: case 84: case 90:
    case 91: case 96:
      error = CGLDescribePixelFormat(it->second.cgl, 0, (CGLPixelFormatAttribute)attr, &value);
      break;
    default:
      error = kCGLBadAttribute;
      break;
  }
  agl_error(error, "describing a pixel format");
  if (error != kCGLNoError) return c.ret(0);
  if (attr == 7) value = 1;
  c.mem.w32(out, (u32)value);
  c.ret(1);
}

void aglQueryRendererInfo(GuestCall& c) {
  const u32 devices = c.i();
  const s32 count = (s32)c.i();
  if (count < 0 || count > 64 || (devices && !c.mem.range_ok(devices, (u64)count * 4, GuestMemory::kRead))) {
    agl_error(kCGLBadDisplay, "querying renderers");
    return c.ret(0);
  }
  if (devices && count) {
    const u32 get_main_device = c.symbol("GetMainDevice");
    const u32 main_device = get_main_device ? c.call(get_main_device, {}) : 0;
    if (!main_device) {
      agl_error(kCGLBadDisplay, "finding the guest display");
      return c.ret(0);
    }
    for (s32 i = 0; i < count; i++) {
      if (c.mem.r32(devices + (u32)i * 4) != main_device) {
        agl_error(kCGLBadDisplay, "querying renderers for an unknown display");
        return c.ret(0);
      }
    }
  }
  const GLuint mask = CGDisplayIDToOpenGLDisplayMask(CGMainDisplayID());
  if (!mask) {
    agl_error(kCGLBadDisplay, "querying renderers");
    return c.ret(0);
  }
  auto query = std::make_shared<RendererQuery>();
  GLint n = 0;
  CGLError error = CGLQueryRendererInfo(mask, &query->info, &n);
  if (error != kCGLNoError || !query->info || n <= 0) {
    agl_error(error != kCGLNoError ? error : kCGLBadRendererInfo, "querying renderers");
    return c.ret(0);
  }
  std::lock_guard<std::mutex> lk(g_mu);
  for (GLint i = 0; i < n; i++) {
    const u32 handle = g_next_handle += 0x10;
    query->handles.push_back(handle);
    g_renderers.emplace(handle, RendererEntry{query, i});
  }
  agl_error(kCGLNoError, "querying renderers");
  c.ret(query->handles.front());
}

void aglNextRendererInfo(GuestCall& c) {
  const u32 handle = c.i();
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_renderers.find(handle);
  if (it == g_renderers.end()) {
    agl_error(kCGLBadRendererInfo, "finding the next renderer");
    return c.ret(0);
  }
  const auto& entry = it->second;
  const size_t next = (size_t)entry.index + 1;
  agl_error(kCGLNoError, "finding the next renderer");
  c.ret(next < entry.query->handles.size() ? entry.query->handles[next] : 0);
}

void aglDescribeRenderer(GuestCall& c) {
  const u32 handle = c.i(), property = c.i(), out = c.i();
  if (!out || !c.mem.range_ok(out, 4, GuestMemory::kWrite)) {
    agl_error(kCGLBadAddress, "describing a renderer");
    return c.ret(0);
  }
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_renderers.find(handle);
  if (it == g_renderers.end()) {
    agl_error(kCGLBadRendererInfo, "describing a renderer");
    return c.ret(0);
  }
  GLint value = 0;
  CGLError error = kCGLNoError;
  if (property != 101 && property != 102) {
    error = CGLDescribeRenderer(it->second.query->info, it->second.index, (CGLRendererProperty)property, &value);
  }
  agl_error(error, "describing a renderer");
  if (error != kCGLNoError) return c.ret(0);
  c.mem.w32(out, (u32)value);
  c.ret(1);
}

void aglDestroyRendererInfo(GuestCall& c) {
  const u32 handle = c.i();
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_renderers.find(handle);
  if (it == g_renderers.end()) return agl_error(kCGLBadRendererInfo, "destroying renderer information");
  const auto query = it->second.query;
  for (u32 h : query->handles) g_renderers.erase(h);
  agl_error(kCGLNoError, "destroying renderer information");
}

u32 create_context(GuestCall& c, u32 format, u32 share) {
  g_big = c.powerpc;
  CGLPixelFormatObj pf = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_formats.find(format);
    if (it != g_formats.end()) pf = CGLRetainPixelFormat(it->second.cgl);
  }
  if (!pf) {
    agl_error(kCGLBadPixelFormat, "creating a context");
    return 0;
  }
  Context* shared = share ? context_of(share) : nullptr;
  if (share && !shared) {
    CGLReleasePixelFormat(pf);
    agl_error(kCGLBadMatch, "creating a context");
    return 0;
  }
  auto* x = new Context;
  if (shared) x->textures = shared->textures;
  const CGLError e = CGLCreateContext(pf, shared ? shared->cgl : nullptr, &x->cgl);
  CGLReleasePixelFormat(pf);
  if (e != kCGLNoError) {
    agl_error(e, "creating a context");
    delete x;
    return 0;
  }
  std::lock_guard<std::mutex> lk(g_mu);
  x->handle = g_next_handle += 0x10;
  g_contexts[x->handle] = x;
  agl_error(kCGLNoError, "creating a context");
  return x->handle;
}

void aglCreateContext(GuestCall& c) {
  const u32 format = c.i(), share = c.i();
  c.ret(create_context(c, format, share));
}

void make_current(Context* x) {
  t_current = x;
  CGLSetCurrentContext(x ? x->cgl : nullptr);
  if (!x) return;
  if (!x->fbo) {
    size_framebuffer(x, x->width, x->height);
    glPixelStorei(GL_UNPACK_SWAP_BYTES, g_big);  // big-endian pixels read the guest's way
    glPixelStorei(GL_PACK_SWAP_BYTES, g_big);
  }
  if (!x->attribs_ready) {
    GLint max_attribs = 0;
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS_ARB, &max_attribs);
    if (max_attribs > 0) x->attribs.resize((size_t)max_attribs);
    else log_warn("OpenGL: cannot determine the vertex attribute limit");
    x->attribs_ready = true;
  }
  glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, x->fbo);
}

void release_surface_provider(GuestCall& c, Context* x) {
  if (!x->surface_provider) return;
  c.call(x->surface_functions.release_provider, {x->surface_provider});
  x->surface_provider = 0;
}

void aglSetCurrentContext(GuestCall& c) {
  const u32 h = c.i();
  Context* x = h ? context_of(h) : nullptr;
  make_current(x);
  c.ret(h == 0 || x != nullptr);
}

void aglGetCurrentContext(GuestCall& c) { c.ret(t_current ? t_current->handle : 0); }

CGLError release_context(GuestCall& c, u32 handle) {
  Context* x;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    const auto it = g_contexts.find(handle);
    if (it == g_contexts.end()) return kCGLBadContext;
    x = it->second;
    if (--x->refs) return kCGLNoError;
    g_contexts.erase(it);
  }
  if (t_current == x) make_current(nullptr);
  if (x->fbo) {
    const CGLContextObj before = CGLGetCurrentContext();
    CGLSetCurrentContext(x->cgl);
    glDeleteFramebuffersEXT(1, &x->fbo);
    glDeleteRenderbuffersEXT(1, &x->color);
    glDeleteRenderbuffersEXT(1, &x->auxiliary);
    glDeleteRenderbuffersEXT(1, &x->depth);
    if (x->readback) glDeleteBuffers(1, &x->readback);
    CGLSetCurrentContext(before);
  }
  release_surface_provider(c, x);
  if (x->surface_color_space)
    c.call(x->surface_functions.release_color, {x->surface_color_space});
  const CGLError error = CGLDestroyContext(x->cgl);
  if (x->guest_frame) c.mem.unmap(x->guest_frame, x->guest_frame_size);
  delete x;
  return error;
}

void aglDestroyContext(GuestCall& c) {
  const CGLError error = release_context(c, c.i());
  agl_error(error, "destroying a context");
  c.ret(error == kCGLNoError);
}

void resize_context(Context* x, int w, int h) {
  CGLContextObj before = CGLGetCurrentContext();
  CGLSetCurrentContext(x->cgl);
  size_framebuffer(x, w, h);
  // The byte swapping is this context's own state, and it is the current one
  // just now, whichever was before: make_current won't set it once the
  // framebuffer exists.
  glPixelStorei(GL_UNPACK_SWAP_BYTES, g_big != x->unpack_swap);
  glPixelStorei(GL_PACK_SWAP_BYTES, g_big != x->pack_swap);
  CGLSetCurrentContext(before);
  if (t_current) glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, t_current->fbo);
}

void resize_to_port(GuestCall& c, Context* x) {
  int w = 0, h = 0;
  if (port_size(c, x->port, w, h)) resize_context(x, w, h);
}

void aglSetDrawable(GuestCall& c) {
  Context* x = context_of(c.i());
  const u32 port = c.i();
  if (x) {
    x->port = port;
    if (port) resize_to_port(c, x);
    log_debug("aglSetDrawable: port 0x%x, %d x %d", port, x->width, x->height);
  }
  c.ret(x != nullptr);
}

void aglGetDrawable(GuestCall& c) {
  Context* x = context_of(c.i());
  agl_error(x ? kCGLNoError : kCGLBadContext, "getting a drawable");
  c.ret(x ? x->port : 0);
}

void aglUpdateContext(GuestCall& c) {
  Context* x = context_of(c.i());
  if (x && x->port) resize_to_port(c, x);
  c.ret(x != nullptr);
}

bool low_latency_pacing(GuestCall& c) {
  static const bool enabled = initial_low_latency_pacing();
  return c.process.low_latency_pacing(enabled);
}

// Pace before presentation by default. In low-latency mode, show the frame
// first and wait before returning to the game, so it can sample newer input.
thread_local u64 pacing_next = 0;

void pace(bool after = false) {
  static const double interval = [] {
    double fps = 120;
    if (const char* e = getenv("COCAINE_GL_FPS")) fps = atof(e);
    return fps >= 1 ? 1e9 / fps : 0.0;
  }();
  if (interval <= 0) return;
  const u64 now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  if (after && !pacing_next) pacing_next = now + (u64)interval;
  if (pacing_next > now && pacing_next - now <= 1000000000ull) {
    const u64 delay = pacing_next - now;
    struct timespec ts = {(time_t)(delay / 1000000000ull), (long)(delay % 1000000000ull)};
    nanosleep(&ts, nullptr);
    pacing_next += (u64)interval;
  } else {
    pacing_next = now + (u64)interval;  // late, or the first frame: start the beat from here
  }
}

void pace_before_presentation(bool after) {
  static thread_local bool initialized = false, previous_after = false;
  if (initialized && after != previous_after) {
    if (after) pace();
    else pacing_next = 0;
  }
  initialized = true;
  previous_after = after;
  if (!after) pace();
}

void aglSwapBuffers(GuestCall& c) {
  Context* x = context_of(c.i());
  if (x && x == t_current) {
    const bool after = low_latency_pacing(c);
    pace_before_presentation(after);
    present(c, x);
    if (after) pace(true);
  }
  c.ret(0);
}

void aglSetFullScreen(GuestCall& c) {
  say_once("OpenGL: full screen is not done yet; set the game to play in a window");
  c.ret(0);
}

void aglDisable(GuestCall& c) {
  Context* x = context_of(c.i());
  const u32 name = c.i();
  if (!x) {
    agl_error(kCGLBadContext, "disabling an AGL option");
    return c.ret(0);
  }
  // Buffer rectangles are disabled by default; enabling them is not yet
  // implemented, so disabling them leaves the full drawable in use.
  if (name != 202) {
    agl_error(kCGLBadEnumeration, "disabling an AGL option");
    return c.ret(0);
  }
  agl_error(kCGLNoError, "disabling an AGL option");
  c.ret(1);
}

void agl_true(GuestCall& c) { c.ret(1); }
void aglGetError(GuestCall& c) {
  const CGLError error = t_agl_error;
  t_agl_error = kCGLNoError;
  c.ret((u32)error);
}

void aglErrorString(GuestCall& c) {
  const u32 error = c.i();
  static std::mutex mu;
  static std::unordered_map<u32, u32> strings;
  std::lock_guard<std::mutex> lk(mu);
  auto found = strings.find(error);
  if (found != strings.end()) return c.ret(found->second);
  const char* text = CGLErrorString((CGLError)error);
  if (!text) {
    log_warn("OpenGL: no error string for code %u", error);
    return c.ret(0);
  }
  const size_t size = strlen(text) + 1;
  const u32 at = c.scratch((u32)size);
  if (!at) {
    log_warn("OpenGL: cannot allocate error string");
    return c.ret(0);
  }
  memcpy(c.mem.host(at), text, size);
  strings[error] = at;
  c.ret(at);
}

// ---- CGL ----------------------------------------------------------------------

void cglChoosePixelFormat(GuestCall& c) {
  const u32 attributes = c.i(), out = c.i(), count = c.i();
  if (!out || !count || !c.mem.range_ok(out, 4, GuestMemory::kWrite) ||
      !c.mem.range_ok(count, 4, GuestMemory::kWrite)) return c.ret(kCGLBadAddress);
  c.mem.w32(out, 0);
  c.mem.w32(count, 0);
  const u32 format = choose_pixel_format(c, attributes, "CGLChoosePixelFormat");
  if (!format) return c.ret(t_agl_error);
  c.mem.w32(out, format);
  c.mem.w32(count, 1);
  c.ret(kCGLNoError);
}

void cglDescribePixelFormat(GuestCall& c) {
  const u32 format = c.i();
  const s32 index = (s32)c.i();
  const u32 attribute = c.i(), out = c.i();
  if (!out || !c.mem.range_ok(out, 4, GuestMemory::kWrite)) return c.ret(kCGLBadAddress);
  std::lock_guard<std::mutex> lk(g_mu);
  const auto it = g_formats.find(format);
  if (it == g_formats.end()) return c.ret(kCGLBadPixelFormat);
  GLint value = 0;
  const CGLError error = CGLDescribePixelFormat(it->second.cgl, index, (CGLPixelFormatAttribute)attribute, &value);
  if (error == kCGLNoError && attribute == kCGLPFAAuxBuffers) value = 1;
  if (error == kCGLNoError) c.mem.w32(out, (u32)value);
  c.ret(error);
}

void cglDestroyPixelFormat(GuestCall& c) {
  c.ret(release_pixel_format(c.i()));
}

void cglRetainPixelFormat(GuestCall& c) {
  const u32 handle = c.i();
  std::lock_guard<std::mutex> lk(g_mu);
  const auto it = g_formats.find(handle);
  if (it == g_formats.end()) {
    log_warn("OpenGL: cannot retain invalid pixel format 0x%x", handle);
    return c.ret(0);
  }
  ++it->second.refs;
  c.ret(handle);
}

void cglReleasePixelFormat(GuestCall& c) {
  const CGLError error = release_pixel_format(c.i());
  if (error != kCGLNoError) log_warn("OpenGL: cannot release pixel format (%d)", error);
}

void cglCreateContext(GuestCall& c) {
  const u32 format = c.i(), share = c.i(), out = c.i();
  if (!out || !c.mem.range_ok(out, 4, GuestMemory::kWrite)) return c.ret(kCGLBadAddress);
  const u32 context = create_context(c, format, share);
  c.mem.w32(out, context);
  c.ret(context ? kCGLNoError : t_agl_error);
}

void cglDestroyContext(GuestCall& c) {
  const u32 handle = c.i();
  c.ret(release_context(c, handle));
}

void cglRetainContext(GuestCall& c) {
  const u32 handle = c.i();
  std::lock_guard<std::mutex> lk(g_mu);
  const auto it = g_contexts.find(handle);
  if (it == g_contexts.end()) {
    log_warn("OpenGL: cannot retain invalid context 0x%x", handle);
    return c.ret(0);
  }
  ++it->second->refs;
  c.ret(handle);
}

void cglReleaseContext(GuestCall& c) {
  const CGLError error = release_context(c, c.i());
  if (error != kCGLNoError) log_warn("OpenGL: cannot release context (%d)", error);
}

void cglSetCurrentContext(GuestCall& c) {
  const u32 handle = c.i();
  Context* x = handle ? context_of(handle) : nullptr;
  if (handle && !x) return c.ret(kCGLBadContext);
  make_current(x);
  c.ret(kCGLNoError);
}

void cglGetCurrentContext(GuestCall& c) { c.ret(t_current ? t_current->handle : 0); }

void cglClearDrawable(GuestCall& c) {
  Context* x = context_of(c.i());
  if (!x) return c.ret(kCGLBadContext);
  x->connection = x->window = x->surface = 0;
  x->display = 0;
  c.ret(kCGLNoError);
}

bool resize_to_display(GuestCall& c, Context* x) {
  const u32 width = c.symbol("CGDisplayPixelsWide"), height = c.symbol("CGDisplayPixelsHigh");
  if (!width || !height) {
    say_once("OpenGL: cannot find captured display dimensions");
    return false;
  }
  const u32 w = c.call(width, {x->display}), h = c.call(height, {x->display});
  if (!w || !h || w > 16384 || h > 16384) {
    log_warn("OpenGL: invalid captured display size %u x %u", w, h);
    return false;
  }
  resize_context(x, (int)w, (int)h);
  return true;
}

void set_full_screen(GuestCall& c, Context* x, u32 mask) {
  if (!x) return c.ret(kCGLBadContext);
  const u32 main_display = c.symbol("CGMainDisplayID"), display_mask = c.symbol("CGDisplayIDToOpenGLDisplayMask");
  if (!main_display || !display_mask) {
    say_once("OpenGL: cannot find the guest fullscreen display");
    return c.ret(kCGLBadDisplay);
  }
  const u32 display = c.call(main_display, {});
  if (mask && mask != c.call(display_mask, {display})) return c.ret(kCGLBadDisplay);
  const u32 previous = x->display;
  x->display = display;
  if (!resize_to_display(c, x)) {
    x->display = previous;
    return c.ret(kCGLBadDrawable);
  }
  x->connection = x->window = x->surface = 0;
  c.ret(kCGLNoError);
}

void cglSetFullScreen(GuestCall& c) {
  Context* x = context_of(c.i());
  set_full_screen(c, x, 0);
}

void cglSetFullScreenOnDisplay(GuestCall& c) {
  Context* x = context_of(c.i());
  const u32 mask = c.i();
  if (!mask) return c.ret(x ? kCGLBadDisplay : kCGLBadContext);
  set_full_screen(c, x, mask);
}

bool resize_to_surface(GuestCall& c, Context* x) {
  const u32 get_bounds = c.symbol("CGSGetSurfaceBounds");
  if (!get_bounds) {
    say_once("OpenGL: cannot find CGSGetSurfaceBounds");
    return false;
  }
  if (!x->surface_rect) x->surface_rect = c.scratch(16);
  if (!x->surface_rect) {
    say_once("OpenGL: cannot allocate a surface rectangle");
    return false;
  }
  const u32 error = c.call(get_bounds, {x->connection, x->window, x->surface, x->surface_rect});
  if (error) {
    log_warn("OpenGL: cannot get Cocoa surface bounds (%u)", error);
    return false;
  }
  const u8* rect = gp(c, x->surface_rect);
  const float left = rdf(rect), top = rdf(rect + 4), w = rdf(rect + 8), h = rdf(rect + 12);
  if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(w) || !std::isfinite(h) ||
      w < 1 || h < 1 || w > 16384 || h > 16384) {
    log_warn("OpenGL: invalid Cocoa surface bounds %g,%g %gx%g", left, top, w, h);
    return false;
  }
  x->surface_x = left;
  x->surface_y = top;
  resize_context(x, (int)w, (int)h);
  log_debug("OpenGL: Cocoa surface %g,%g %d x %d", left, top, x->width, x->height);
  return true;
}

bool present_surface(GuestCall& c, Context* x) {
  if ((!x->display && (!x->window || !x->surface)) || !x->fbo) {
    say_once("OpenGL: Cocoa context has no drawable");
    return false;
  }
  SurfaceFunctions& fn = x->surface_functions;
  if (!fn.window_context) {
    SurfaceFunctions found;
    found.window_context = c.symbol("CGWindowContextCreate");
    found.color_space = c.symbol("CGColorSpaceCreateDeviceRGB");
    found.provider = c.symbol("CGDataProviderCreateWithData");
    found.image_create = c.symbol("CGImageCreate");
    found.draw = c.symbol("CGContextDrawImage");
    found.flush = c.symbol("CGContextFlush");
    found.release_context = c.symbol("CGContextRelease");
    found.release_color = c.symbol("CGColorSpaceRelease");
    found.release_provider = c.symbol("CGDataProviderRelease");
    found.release_image = c.symbol("CGImageRelease");
    found.window_bounds = c.symbol("CGSGetWindowBounds");
    found.display_context = c.symbol("CGDisplayGetDrawingContext");
    if (!found.window_context || !found.color_space || !found.provider || !found.image_create ||
        !found.draw || !found.flush || !found.release_context || !found.release_color ||
        !found.release_provider || !found.release_image || !found.window_bounds || !found.display_context) {
      say_once("OpenGL: cannot find CoreGraphics functions for a Cocoa surface");
      return false;
    }
    fn = found;
  }
  const u64 bytes = (u64)x->width * x->height * 4;
  if (bytes > UINT32_MAX) {
    say_once("OpenGL: Cocoa surface is too large");
    return false;
  }
  if (bytes > x->guest_frame_size) {
    if (x->guest_frame) {
      release_surface_provider(c, x);
      c.mem.unmap(x->guest_frame, x->guest_frame_size);
    }
    x->guest_frame = c.mem.map_alone(bytes, GuestMemory::kRead | GuestMemory::kWrite, 0x10000000u, 0xC0000000u);
    x->guest_frame_size = x->guest_frame ? (u32)bytes : 0;
  }
  if (!x->guest_frame) {
    say_once("OpenGL: cannot allocate Cocoa surface pixels");
    return false;
  }
  if (!read_frame(x)) return false;
  const size_t row = (size_t)x->width * 4;
  for (int y = 0; y < x->height; y++)
    memcpy(c.mem.host(x->guest_frame) + (size_t)y * row,
           x->frame.data() + (size_t)(x->height - 1 - y) * row, row);

  float window_height = (float)x->height;
  if (!x->display) {
    if (!x->window_rect) x->window_rect = c.scratch(16);
    const u32 bounds = x->window_rect;
    if (!bounds || c.call(fn.window_bounds, {x->connection, x->window, bounds})) {
      say_once("OpenGL: cannot get Cocoa window bounds");
      return false;
    }
    window_height = rdf(gp(c, bounds) + 12);
  }
  const u32 ctx = x->display ? c.call(fn.display_context, {x->display})
                             : c.call(fn.window_context, {x->connection, x->window, 0});
  if (!x->surface_color_space) x->surface_color_space = c.call(fn.color_space, {});
  if (!x->surface_provider) x->surface_provider = c.call(fn.provider, {0, x->guest_frame, (u32)bytes, 0});
  const u32 space = x->surface_color_space;
  const u32 source = x->surface_provider;
  u32 image = 0;
  if (ctx && space && source) {
    const u32 bitmap = (u32)kCGImageAlphaNoneSkipFirst |
                       (u32)(c.powerpc ? kCGBitmapByteOrderDefault : kCGBitmapByteOrder32Little);
    image = c.process.call_guest_args(c.thread, fn.image_create,
        {{(u32)x->width}, {(u32)x->height}, {8}, {32}, {(u32)row}, {space}, {bitmap}, {source}, {0}, {0}, {0}});
  }
  if (image) {
    auto bits = [](float value) { u32 word; memcpy(&word, &value, 4); return word; };
    c.call(fn.draw, {ctx, bits(x->display ? 0 : x->surface_x),
                  bits(x->display ? 0 : window_height - x->surface_y - x->height),
                  bits((float)x->width), bits((float)x->height), image});
    c.call(fn.flush, {ctx});
  } else {
    say_once("OpenGL: cannot create an image for a Cocoa surface");
  }
  if (image) c.call(fn.release_image, {image});
  // CGDisplayGetDrawingContext lends the display's context; only window
  // contexts created above belong to us.
  if (ctx && !x->display) c.call(fn.release_context, {ctx});
  if (image) frame_shown(x, x->display ? "fullscreen " : "Cocoa ");
  return image != 0;
}

void cglSetSurface(GuestCall& c) {
  Context* x = context_of(c.i());
  const u32 connection = c.i(), window = c.i(), surface = c.i();
  if (!x) return c.ret(kCGLBadContext);
  x->connection = connection;
  x->window = window;
  x->surface = surface;
  x->display = 0;
  log_debug("CGLSetSurface: connection 0x%x window 0x%x surface 0x%x", connection, window, surface);
  c.ret(resize_to_surface(c, x) ? kCGLNoError : kCGLBadDrawable);
}

void cglUpdateContext(GuestCall& c) {
  Context* x = context_of(c.i());
  if (!x) return c.ret(kCGLBadContext);
  if (x->display) return c.ret(resize_to_display(c, x) ? kCGLNoError : kCGLBadDrawable);
  c.ret(!x->surface || resize_to_surface(c, x) ? kCGLNoError : kCGLBadDrawable);
}

constexpr u32 kCGLCPPrivateGuestStorage = 1236;

void cglSetParameter(GuestCall& c) {
  const u32 handle = c.i();
  const u32 parameter = c.i(), address = c.i();
  std::lock_guard<std::mutex> lk(g_mu);
  const auto it = g_contexts.find(handle);
  if (it == g_contexts.end()) return c.ret(kCGLBadContext);
  if (!address || !c.mem.range_ok(address, 4, GuestMemory::kRead)) return c.ret(kCGLBadAddress);
  const u32 value = c.mem.r32(address);
  if (parameter == kCGLCPPrivateGuestStorage) {
    it->second->private_client_storage = value;
    return c.ret(kCGLNoError);
  }
  if (parameter == kCGLCPSwapInterval) return c.ret(kCGLNoError);
  log_warn("OpenGL: CGL context parameter %u is not supported (value %d)", parameter, (GLint)value);
  c.ret(kCGLBadEnumeration);
}

void cglGetParameter(GuestCall& c) {
  const u32 handle = c.i();
  const u32 parameter = c.i(), address = c.i();
  std::lock_guard<std::mutex> lk(g_mu);
  const auto it = g_contexts.find(handle);
  if (it == g_contexts.end()) return c.ret(kCGLBadContext);
  if (!address || !c.mem.range_ok(address, 4, GuestMemory::kWrite)) return c.ret(kCGLBadAddress);
  if (parameter == kCGLCPPrivateGuestStorage) {
    c.mem.w32(address, it->second->private_client_storage);
    return c.ret(kCGLNoError);
  }
  log_warn("OpenGL: CGL context parameter %u is not supported", parameter);
  c.ret(kCGLBadEnumeration);
}

void cglFlushDrawable(GuestCall& c) {
  Context* x = context_of(c.i());
  if (!x || x != t_current) return c.ret(kCGLBadContext);
  const bool after = low_latency_pacing(c);
  pace_before_presentation(after);
  const bool shown = present_surface(c, x);
  if (after) pace(true);
  c.ret(shown ? kCGLNoError : kCGLBadDrawable);
}

// ---- GL: state ----------------------------------------------------------------

void glEnable_(GuestCall& c) { glEnable(c.i()); }
void glDisable_(GuestCall& c) { glDisable(c.i()); }
void glDepthRange_(GuestCall& c) { const double a = c.d(), b = c.d(); glDepthRange(a, b); }
void glClearDepth_(GuestCall& c) { glClearDepth(c.d()); }
void glLightModeli_(GuestCall& c) { const u32 p = c.i(); glLightModeli(p, (GLint)c.i()); }
void glMaterialf_(GuestCall& c) { const u32 a = c.i(), b = c.i(); glMaterialf(a, b, c.f()); }
void glPointSize_(GuestCall& c) { glPointSize(c.f()); }
void glShadeModel_(GuestCall& c) { glShadeModel(c.i()); }
void gluOrtho2D(GuestCall& c) {
  const double left = c.d(), right = c.d(), bottom = c.d(), top = c.d();
  glOrtho(left, right, bottom, top, -1.0, 1.0);
}
void gluPerspective_(GuestCall& c) {
  const double fovy = c.d(), aspect = c.d(), near = c.d(), far = c.d();
  gluPerspective(fovy, aspect, near, far);
}
void gluLookAt_(GuestCall& c) {
  const double eye_x = c.d(), eye_y = c.d(), eye_z = c.d();
  const double center_x = c.d(), center_y = c.d(), center_z = c.d();
  const double up_x = c.d(), up_y = c.d(), up_z = c.d();
  gluLookAt(eye_x, eye_y, eye_z, center_x, center_y, center_z, up_x, up_y, up_z);
}
std::unordered_map<u32, GLUquadric*> g_quadrics;
GLUquadric* quadric_of(u32 handle) {
  std::lock_guard<std::mutex> lk(g_mu);
  const auto it = g_quadrics.find(handle);
  return it == g_quadrics.end() ? nullptr : it->second;
}
void gluNewQuadric_(GuestCall& c) {
  GLUquadric* q = gluNewQuadric();
  if (!q) {
    log_warn("OpenGL: cannot create GLU quadric");
    return c.ret(0);
  }
  std::lock_guard<std::mutex> lk(g_mu);
  const u32 handle = g_next_handle += 0x10;
  g_quadrics[handle] = q;
  c.ret(handle);
}
void gluDeleteQuadric_(GuestCall& c) {
  const u32 handle = c.i();
  std::lock_guard<std::mutex> lk(g_mu);
  const auto it = g_quadrics.find(handle);
  if (it == g_quadrics.end()) {
    log_warn("OpenGL: invalid GLU quadric 0x%x", handle);
    return;
  }
  gluDeleteQuadric(it->second);
  g_quadrics.erase(it);
}
void gluQuadricNormals_(GuestCall& c) {
  GLUquadric* q = quadric_of(c.i());
  const GLenum normal = c.i();
  if (!q) return log_warn("OpenGL: invalid GLU quadric");
  gluQuadricNormals(q, normal);
}
void gluQuadricTexture_(GuestCall& c) {
  GLUquadric* q = quadric_of(c.i());
  const GLboolean texture = (GLboolean)c.i();
  if (!q) return log_warn("OpenGL: invalid GLU quadric");
  gluQuadricTexture(q, texture);
}
void gluQuadricOrientation_(GuestCall& c) {
  GLUquadric* q = quadric_of(c.i());
  const GLenum orientation = c.i();
  if (!q) return log_warn("OpenGL: invalid GLU quadric");
  gluQuadricOrientation(q, orientation);
}
void gluSphere_(GuestCall& c) {
  GLUquadric* q = quadric_of(c.i());
  const double radius = c.d();
  const int slices = (s32)c.i(), stacks = (s32)c.i();
  if (!q) return log_warn("OpenGL: invalid GLU quadric");
  gluSphere(q, radius, slices, stacks);
}
void gluCylinder_(GuestCall& c) {
  GLUquadric* q = quadric_of(c.i());
  const double base = c.d(), top = c.d(), height = c.d();
  const int slices = (s32)c.i(), stacks = (s32)c.i();
  if (!q) return log_warn("OpenGL: invalid GLU quadric");
  gluCylinder(q, base, top, height, slices, stacks);
}
void gluDisk_(GuestCall& c) {
  GLUquadric* q = quadric_of(c.i());
  const double inner = c.d(), outer = c.d();
  const int slices = (s32)c.i(), loops = (s32)c.i();
  if (!q) return log_warn("OpenGL: invalid GLU quadric");
  gluDisk(q, inner, outer, slices, loops);
}
void gluPartialDisk_(GuestCall& c) {
  GLUquadric* q = quadric_of(c.i());
  const double inner = c.d(), outer = c.d();
  const int slices = (s32)c.i(), loops = (s32)c.i();
  const double start = c.d(), sweep = c.d();
  if (!q) return log_warn("OpenGL: invalid GLU quadric");
  gluPartialDisk(q, inner, outer, slices, loops, start, sweep);
}
int pixel_size(GLenum format, GLenum type) {
  int channels = 0, component_size = 0, packed_size = 0;
  switch (format) {
    case GL_COLOR_INDEX: case GL_STENCIL_INDEX: case GL_DEPTH_COMPONENT:
    case GL_RED: case GL_GREEN: case GL_BLUE: case GL_ALPHA: case GL_LUMINANCE: channels = 1; break;
    case GL_LUMINANCE_ALPHA: channels = 2; break;
    case GL_RGB: case GL_BGR: channels = 3; break;
    case GL_RGBA: case GL_BGRA: channels = 4; break;
  }
  switch (type) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: component_size = 1; break;
    case GL_SHORT: case GL_UNSIGNED_SHORT: component_size = 2; break;
    case GL_INT: case GL_UNSIGNED_INT: case GL_FLOAT: component_size = 4; break;
    case GL_UNSIGNED_BYTE_3_3_2: case GL_UNSIGNED_BYTE_2_3_3_REV: packed_size = 1; break;
    case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_5_6_5_REV:
    case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_4_4_4_4_REV:
    case GL_UNSIGNED_SHORT_5_5_5_1: case GL_UNSIGNED_SHORT_1_5_5_5_REV: packed_size = 2; break;
    case GL_UNSIGNED_INT_8_8_8_8: case GL_UNSIGNED_INT_8_8_8_8_REV:
    case GL_UNSIGNED_INT_10_10_10_2: case GL_UNSIGNED_INT_2_10_10_10_REV: packed_size = 4; break;
  }
  return channels ? (packed_size ? packed_size : channels * component_size) : 0;
}
void gluBuild2DMipmaps_(GuestCall& c) {
  const GLenum target = c.i();
  const GLint components = (GLint)c.i();
  const GLsizei width = (GLsizei)c.i(), height = (GLsizei)c.i();
  const GLenum format = c.i(), type = c.i();
  const u32 data = c.i();
  const int bytes_per_pixel = pixel_size(format, type);
  if (!bytes_per_pixel) {
    log_warn("OpenGL: unsupported GLU mipmap format 0x%x type 0x%x", format, type);
    return c.ret(GLU_INVALID_ENUM);
  }
  if (width <= 0 || height <= 0) return c.ret(GLU_INVALID_VALUE);
  GLint row_length = 0, skip_rows = 0, skip_pixels = 0, alignment = 0;
  glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
  glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_rows);
  glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_pixels);
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
  const int limit = 1 << 20;
  if (row_length < 0 || row_length > limit || skip_rows < 0 || skip_rows > limit ||
      skip_pixels < 0 || skip_pixels > limit || width > limit || height > limit || alignment <= 0) {
    log_warn("OpenGL: invalid GLU mipmap unpack state");
    return c.ret(GLU_INVALID_VALUE);
  }
  const u64 row_bytes = (u64)(row_length ? row_length : width) * bytes_per_pixel;
  const u64 stride = (row_bytes + alignment - 1) / alignment * alignment;
  const u64 bytes = (u64)skip_rows * stride + (u64)skip_pixels * bytes_per_pixel +
                    (u64)(height - 1) * stride + (u64)width * bytes_per_pixel;
  if (!data || bytes > UINT32_MAX || !c.mem.range_ok(data, bytes, GuestMemory::kRead)) {
    log_warn("OpenGL: invalid GLU mipmap data");
    return c.ret(GLU_INVALID_VALUE);
  }
  c.ret((u32)gluBuild2DMipmaps(target, components, width, height, format, type, gp(c, data)));
}
void glIsEnabled_(GuestCall& c) { c.ret(glIsEnabled(c.i())); }
void glClear_(GuestCall& c) { glClear(c.i()); }
void glClearColor_(GuestCall& c) {
  const float r = c.f(), g = c.f(), b = c.f(), a = c.f();
  glClearColor(r, g, b, a);
}
void glViewport_(GuestCall& c) {
  const s32 x = (s32)c.i(), y = (s32)c.i(), w = (s32)c.i(), h = (s32)c.i();
  glViewport(x, y, w, h);
}
void glScissor_(GuestCall& c) {
  const GLint x = (GLint)c.i(), y = (GLint)c.i();
  const GLsizei w = (GLsizei)c.i(), h = (GLsizei)c.i();
  glScissor(x, y, w, h);
}
void glColorMaterial_(GuestCall& c) {
  const GLenum face = c.i(), mode = c.i();
  glColorMaterial(face, mode);
}
void glBlendFunc_(GuestCall& c) {
  const GLenum s = c.i(), d = c.i();
  glBlendFunc(s, d);
}
void glBlendEquation_(GuestCall& c) { glBlendEquation(c.i()); }
void glAlphaFunc_(GuestCall& c) {
  const GLenum f = c.i();
  glAlphaFunc(f, c.f());
}
void glCullFace_(GuestCall& c) { glCullFace(c.i()); }
void glFrontFace_(GuestCall& c) { glFrontFace(c.i()); }
void glDepthFunc_(GuestCall& c) { glDepthFunc(c.i()); }
void glDepthMask_(GuestCall& c) { glDepthMask((GLboolean)c.i()); }
void glColorMask_(GuestCall& c) {
  const GLboolean r = (GLboolean)c.i(), g = (GLboolean)c.i(), b = (GLboolean)c.i(), a = (GLboolean)c.i();
  glColorMask(r, g, b, a);
}
void glStencilFunc_(GuestCall& c) {
  const GLenum f = c.i();
  const GLint ref = (GLint)c.i();
  glStencilFunc(f, ref, c.i());
}
void glStencilMask_(GuestCall& c) { glStencilMask(c.i()); }
void glStencilOp_(GuestCall& c) {
  const GLenum a = c.i(), b = c.i(), d = c.i();
  glStencilOp(a, b, d);
}
void glPolygonMode_(GuestCall& c) {
  const GLenum f = c.i();
  glPolygonMode(f, c.i());
}
void glPolygonOffset_(GuestCall& c) {
  const float f = c.f(), u = c.f();
  glPolygonOffset(f, u);
}
void glLineWidth_(GuestCall& c) { glLineWidth(c.f()); }
void glLineStipple_(GuestCall& c) {
  const GLint factor = (GLint)c.i();
  glLineStipple(factor, (GLushort)c.i());
}
void glHint_(GuestCall& c) {
  const GLenum t = c.i();
  glHint(t, c.i());
}
void glPushAttrib_(GuestCall& c) { glPushAttrib(c.i()); }
void glPopAttrib_(GuestCall& c) { glPopAttrib(); }
void glFinish_(GuestCall& c) { glFinish(); }
void glFlush_(GuestCall& c) { glFlush(); }
void glGetError_(GuestCall& c) { c.ret(glGetError()); }

// The window's front and back buffers are one offscreen framebuffer here.
GLenum buffer(GLenum b) {
  if (b == GL_AUX0) return GL_COLOR_ATTACHMENT1_EXT;
  return (b == GL_BACK || b == GL_FRONT || b == GL_FRONT_AND_BACK || b == GL_BACK_LEFT || b == GL_FRONT_LEFT)
             ? GL_COLOR_ATTACHMENT0_EXT
             : b;
}
void glDrawBuffer_(GuestCall& c) { glDrawBuffer(buffer(c.i())); }
void glReadBuffer_(GuestCall& c) { glReadBuffer(buffer(c.i())); }

void glPixelStorei_(GuestCall& c) {
  const GLenum p = c.i();
  const GLint v = (GLint)c.i();
  if (p == GL_UNPACK_SWAP_BYTES || p == GL_PACK_SWAP_BYTES) {
    if (t_current) (p == GL_UNPACK_SWAP_BYTES ? t_current->unpack_swap : t_current->pack_swap) = v != 0;
    glPixelStorei(p, g_big != (v != 0));
    return;
  }
  glPixelStorei(p, v);
}

// glGet: the host's answer, back to the guest in its byte order. However
// many values the name has, the host writes them; the ones it didn't touch
// still hold the marker.
template <typename T>
int written(const T* v, T marker) {
  int n = 16;
  while (n > 0 && memcmp(&v[n - 1], &marker, sizeof(T)) == 0) n--;
  return n > 0 ? n : 1;
}
GLenum guest_buffer(GLenum b) {
  if (b == GL_COLOR_ATTACHMENT0_EXT) return GL_BACK;
  if (b == GL_COLOR_ATTACHMENT1_EXT) return GL_AUX0;
  return b;
}
void glGetIntegerv_(GuestCall& c) {
  const GLenum p = c.i();
  const u32 address = c.i();
  u8* out = gp(c, address);
  if (p == GL_COMPRESSED_TEXTURE_FORMATS) {
    GLint count = 0;
    glGetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &count);
    if (count <= 0) return;
    if (!address || !c.mem.range_ok(address, (u64)count * 4, GuestMemory::kWrite)) {
      log_warn("OpenGL: invalid compressed texture format result");
      return;
    }
    std::vector<GLint> formats((size_t)count);
    glGetIntegerv(p, formats.data());
    for (GLint k = 0; k < count; k++) wr32(out + 4 * k, (u32)formats[(size_t)k]);
    return;
  }
  GLint v[16];
  const GLint marker = 0x7A5A7A5A;
  for (GLint& x : v) x = marker;
  glGetIntegerv(p, v);
  if (p == GL_DRAW_BUFFER || p == GL_READ_BUFFER) v[0] = (GLint)guest_buffer((GLenum)v[0]);
  if (p == GL_AUX_BUFFERS && t_current) v[0] = 1;
  if (p == GL_UNPACK_SWAP_BYTES) v[0] = t_current && t_current->unpack_swap;
  if (p == GL_PACK_SWAP_BYTES) v[0] = t_current && t_current->pack_swap;
  const int n = written(v, marker);
  for (int k = 0; out && k < n; k++) wr32(out + 4 * k, (u32)v[k]);
}
void glGetBooleanv_(GuestCall& c) {
  const GLenum p = c.i();
  const u32 address = c.i();
  GLint count = 16;
  GLenum count_name = 0;
  if (p == GL_COMPRESSED_TEXTURE_FORMATS) count_name = GL_NUM_COMPRESSED_TEXTURE_FORMATS;
  else if (p == 0x8df8) count_name = 0x8df9;  // shader binary formats
  else if (p == 0x87ff) count_name = 0x87fe;  // program binary formats
  if (count_name) {
    glGetIntegerv(count_name, &count);
    if (count <= 0) return;
  }
  std::vector<GLboolean> values((size_t)count, 0x7f);
  glGetBooleanv(p, values.data());
  if (p == GL_AUX_BUFFERS && t_current) values[0] = GL_TRUE;
  if (p == GL_UNPACK_SWAP_BYTES) values[0] = t_current && t_current->unpack_swap;
  if (p == GL_PACK_SWAP_BYTES) values[0] = t_current && t_current->pack_swap;
  size_t n = values.size();
  while (n && values[n - 1] == 0x7f) n--;
  if (!n) return;
  if (!address || !c.mem.range_ok(address, n, GuestMemory::kWrite)) {
    log_warn("OpenGL: invalid boolean state result");
    return;
  }
  memcpy(gp(c, address), values.data(), n);
}
void glGetFloatv_(GuestCall& c) {
  const GLenum p = c.i();
  u8* out = gp(c, c.i());
  GLfloat v[16];
  u32 bits = 0x7FA5A5A5;
  GLfloat marker;
  memcpy(&marker, &bits, 4);
  for (GLfloat& x : v) x = marker;
  glGetFloatv(p, v);
  if (p == GL_DRAW_BUFFER || p == GL_READ_BUFFER) v[0] = (GLfloat)guest_buffer((GLenum)v[0]);
  if (p == GL_AUX_BUFFERS && t_current) v[0] = 1;
  const int n = written(v, marker);
  for (int k = 0; out && k < n; k++) {
    u32 w;
    memcpy(&w, &v[k], 4);
    wr32(out + 4 * k, w);
  }
}
void glGetDoublev_(GuestCall& c) {
  const GLenum p = c.i();
  u8* out = gp(c, c.i());
  GLdouble v[16];
  u64 bits = 0x7FF5A5A5A5A5A5A5ull;
  GLdouble marker;
  memcpy(&marker, &bits, 8);
  for (GLdouble& x : v) x = marker;
  glGetDoublev(p, v);
  if (p == GL_DRAW_BUFFER || p == GL_READ_BUFFER) v[0] = (GLdouble)guest_buffer((GLenum)v[0]);
  if (p == GL_AUX_BUFFERS && t_current) v[0] = 1;
  const int n = written(v, marker);
  for (int k = 0; out && k < n; k++) {
    u64 w;
    memcpy(&w, &v[k], 8);
    wr64(out + 8 * k, w);
  }
}

// The extensions offered by both the host and cocaine. ARB programs are
// advertised for Halo even though not all of their entry points are done yet.
const char* const kExtensions[] = {
    "GL_ARB_multitexture", "GL_EXT_compiled_vertex_array", "GL_ARB_texture_env_combine",
    "GL_EXT_texture_env_combine", "GL_ARB_texture_env_add", "GL_EXT_texture_env_add", "GL_ARB_texture_env_dot3",
    "GL_EXT_texture_filter_anisotropic", "GL_SGIS_generate_mipmap", "GL_EXT_abgr", "GL_EXT_bgra",
    "GL_ARB_texture_border_clamp", "GL_EXT_texture_edge_clamp", "GL_SGIS_texture_edge_clamp", "GL_EXT_stencil_wrap",
    "GL_ARB_texture_cube_map", "GL_EXT_separate_specular_color", "GL_EXT_packed_pixels", "GL_APPLE_packed_pixels",
    "GL_EXT_texture_lod_bias", "GL_EXT_rescale_normal", "GL_ARB_texture_non_power_of_two",
    "GL_ARB_texture_compression", "GL_EXT_texture_compression_s3tc",
    "GL_ARB_texture_rectangle", "GL_EXT_texture_rectangle",
    "GL_ARB_vertex_program", "GL_ARB_fragment_program",
};
void glGetString_(GuestCall& c) {
  const GLenum name = c.i();
  static std::mutex mu;
  static std::unordered_map<GLenum, u32> made;
  std::lock_guard<std::mutex> lk(mu);
  auto it = made.find(name);
  if (it != made.end()) {
    c.ret(it->second);
    return;
  }
  const char* host = (const char*)glGetString(name);
  if (!host) {  // no context yet: nothing to say, and nothing to remember
    c.ret(0);
    return;
  }
  std::string s = host;
  if (name == GL_EXTENSIONS) {
    std::string kept;
    for (const char* e : kExtensions) {
      const std::string padded = " " + s + " ";
      if (padded.find(std::string(" ") + e + " ") != std::string::npos) kept += std::string(e) + " ";
    }
    s = kept;
  } else if (name == GL_RENDERER) {
    s += " (through cocaine)";
  } else if (name == GL_VERSION) {
    s = "1.5 " + s;  // what the game can use; the rest it can't reach anyway
  }
  const u32 at = c.scratch((u32)s.size() + 1);
  if (at) memcpy(c.mem.host(at), s.c_str(), s.size() + 1);
  made[name] = at;
  c.ret(at);
}

// ---- GL: matrices -------------------------------------------------------------

void glMatrixMode_(GuestCall& c) { glMatrixMode(c.i()); }
void glLoadIdentity_(GuestCall& c) { glLoadIdentity(); }
void glPushMatrix_(GuestCall& c) { glPushMatrix(); }
void glPopMatrix_(GuestCall& c) { glPopMatrix(); }
void glLoadMatrixf_(GuestCall& c) { glLoadMatrixf(Floats<16>(c, c.i()).v); }
void glMultMatrixf_(GuestCall& c) { glMultMatrixf(Floats<16>(c, c.i()).v); }
void glTranslatef_(GuestCall& c) {
  const float x = c.f(), y = c.f(), z = c.f();
  glTranslatef(x, y, z);
}
void glScalef_(GuestCall& c) {
  const float x = c.f(), y = c.f(), z = c.f();
  glScalef(x, y, z);
}
void glRotatef_(GuestCall& c) {
  const float a = c.f(), x = c.f(), y = c.f(), z = c.f();
  glRotatef(a, x, y, z);
}
void glOrtho_(GuestCall& c) {
  const double l = c.d(), r = c.d(), b = c.d(), t = c.d(), n = c.d(), f = c.d();
  glOrtho(l, r, b, t, n, f);
}
void glFrustum_(GuestCall& c) {
  const double l = c.d(), r = c.d(), b = c.d(), t = c.d(), n = c.d(), f = c.d();
  glFrustum(l, r, b, t, n, f);
}
void glClipPlane_(GuestCall& c) {
  const GLenum p = c.i();
  glClipPlane(p, Doubles<4>(c, c.i()).v);
}

// ---- GL: immediate mode -------------------------------------------------------

void glBegin_(GuestCall& c) {
  const GLenum mode = c.i();
  glBegin(mode);
}
void glEnd_(GuestCall& c) { glEnd(); }
void glNormal3f_(GuestCall& c) {
  const float x = c.f(), y = c.f(), z = c.f();
  glNormal3f(x, y, z);
}
void glRasterPos2f_(GuestCall& c) {
  const float x = c.f(), y = c.f();
  glRasterPos2f(x, y);
}
void glBitmap_(GuestCall& c) {
  const GLsizei width = (GLsizei)c.i(), height = (GLsizei)c.i();
  const float x = c.f(), y = c.f(), dx = c.f(), dy = c.f();
  const u32 address = c.i();
  if (width > 0 && height > 0) {
    GLint row_length = 0, skip_rows = 0, skip_pixels = 0, alignment = 0;
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_rows);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_pixels);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
    const int limit = 1 << 20;
    if (width > limit || height > limit || row_length < 0 || row_length > limit ||
        skip_rows < 0 || skip_rows > limit || skip_pixels < 0 || skip_pixels > limit || alignment <= 0) {
      log_warn("OpenGL: invalid bitmap unpack state");
      return;
    }
    const u64 row = ((u64)(row_length ? row_length : width) + 7) / 8;
    const u64 stride = (row + alignment - 1) / alignment * alignment;
    const u64 bytes = (u64)(skip_rows + height - 1) * stride + ((u64)skip_pixels + width + 7) / 8;
    if (!address || bytes > UINT32_MAX || !c.mem.range_ok(address, bytes, GuestMemory::kRead)) {
      log_warn("OpenGL: invalid bitmap data");
      return;
    }
  }
  glBitmap(width, height, x, y, dx, dy, gp(c, address));
}
void glGenLists_(GuestCall& c) { c.ret(glGenLists((GLsizei)c.i())); }
void glNewList_(GuestCall& c) {
  const GLuint list = c.i();
  glNewList(list, c.i());
}
void glEndList_(GuestCall& c) { glEndList(); }
void glCallList_(GuestCall& c) { glCallList(c.i()); }
void glListBase_(GuestCall& c) { glListBase(c.i()); }
void glDeleteLists_(GuestCall& c) {
  const GLuint list = c.i();
  glDeleteLists(list, (GLsizei)c.i());
}
void glCallLists_(GuestCall& c) {
  const s32 count = (s32)c.i();
  const GLenum type = c.i();
  const u32 address = c.i();
  int size = 0;
  switch (type) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: size = 1; break;
    case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_2_BYTES: size = 2; break;
    case GL_3_BYTES: size = 3; break;
    case GL_INT: case GL_UNSIGNED_INT: case GL_FLOAT: case GL_4_BYTES: size = 4; break;
    default: log_warn("OpenGL: invalid display list element type 0x%x", type); return;
  }
  if (count < 0 || (count && (!address || !c.mem.range_ok(address, (u64)count * size, GuestMemory::kRead)))) {
    log_warn("OpenGL: invalid display list elements");
    return;
  }
  const u8* src = gp(c, address);
  if (!c.powerpc || size == 1 || type == GL_2_BYTES || type == GL_3_BYTES || type == GL_4_BYTES) {
    glCallLists(count, type, src);
    return;
  }
  const size_t bytes = (size_t)count * size;
  auto convert = [&](u8* dst) {
    for (int i = 0; i < count; i++)
      for (int j = 0; j < size; j++) dst[(size_t)i * size + j] = src[(size_t)i * size + size - 1 - j];
  };
  u8 small[16];
  if (bytes <= sizeof small) {
    convert(small);
    glCallLists(count, type, small);
    return;
  }
  std::vector<u8> converted(bytes);
  convert(converted.data());
  glCallLists(count, type, converted.data());
}
void glPushClientAttrib_(GuestCall& c) {
  const GLbitfield mask = c.i();
  if (t_current) {
    ClientArrays s;
    s.mask = mask;
    s.vertex = t_current->vertex;
    s.normal = t_current->normal;
    s.color = t_current->color_array;
    for (int i = 0; i < 8; i++) s.tex[i] = t_current->tex[i];
    s.attribs = t_current->attribs;
    s.unit = t_current->client_unit;
    s.unpack_swap = t_current->unpack_swap;
    s.pack_swap = t_current->pack_swap;
    t_current->client_stack.push_back(std::move(s));
  }
  glPushClientAttrib(mask);
}
void glPopClientAttrib_(GuestCall& c) {
  glPopClientAttrib();
  if (!t_current || t_current->client_stack.empty()) return;
  ClientArrays s = std::move(t_current->client_stack.back());
  t_current->client_stack.pop_back();
  if (s.mask & GL_CLIENT_VERTEX_ARRAY_BIT) {
    t_current->vertex = std::move(s.vertex);
    t_current->normal = std::move(s.normal);
    t_current->color_array = std::move(s.color);
    for (int i = 0; i < 8; i++) t_current->tex[i] = std::move(s.tex[i]);
    t_current->attribs = std::move(s.attribs);
    t_current->client_unit = s.unit;
  }
  if (s.mask & GL_CLIENT_PIXEL_STORE_BIT) {
    t_current->unpack_swap = s.unpack_swap;
    t_current->pack_swap = s.pack_swap;
  }
}
void glVertex2f_(GuestCall& c) {
  const float x = c.f(), y = c.f();
  glVertex2f(x, y);
}
void glVertex2i_(GuestCall& c) {
  const s32 x = (s32)c.i(), y = (s32)c.i();
  glVertex2i(x, y);
}
void glVertex3f_(GuestCall& c) {
  const float x = c.f(), y = c.f(), z = c.f();
  glVertex3f(x, y, z);
}
void glVertex3d_(GuestCall& c) {
  const double x = c.d(), y = c.d(), z = c.d();
  glVertex3d(x, y, z);
}
void glVertex2fv_(GuestCall& c) { glVertex2fv(Floats<2>(c, c.i()).v); }
void glVertex3fv_(GuestCall& c) { glVertex3fv(Floats<3>(c, c.i()).v); }
void glVertex3dv_(GuestCall& c) { glVertex3dv(Doubles<3>(c, c.i()).v); }
void glTexCoord2f_(GuestCall& c) {
  const float s = c.f(), t = c.f();
  glTexCoord2f(s, t);
}
void glTexCoord2d_(GuestCall& c) {
  const double s = c.d(), t = c.d();
  glTexCoord2d(s, t);
}
void glTexCoord2i_(GuestCall& c) {
  const s32 s = (s32)c.i(), t = (s32)c.i();
  glTexCoord2i(s, t);
}
void glTexCoord2s_(GuestCall& c) {
  const GLshort s = (GLshort)c.i(), t = (GLshort)c.i();
  glTexCoord2s(s, t);
}
void glTexCoord4f_(GuestCall& c) {
  const float s = c.f(), t = c.f(), r = c.f(), q = c.f();
  glTexCoord4f(s, t, r, q);
}
void glTexCoord2fv_(GuestCall& c) { glTexCoord2fv(Floats<2>(c, c.i()).v); }
void glColor3f_(GuestCall& c) {
  const float r = c.f(), g = c.f(), b = c.f();
  glColor3f(r, g, b);
}
void glColor4f_(GuestCall& c) {
  const float r = c.f(), g = c.f(), b = c.f(), a = c.f();
  glColor4f(r, g, b, a);
}
void glColor4fv_(GuestCall& c) { glColor4fv(Floats<4>(c, c.i()).v); }
void glColor3fv_(GuestCall& c) { glColor3fv(Floats<3>(c, c.i()).v); }
void glColor3i_(GuestCall& c) {
  const s32 r = (s32)c.i(), g = (s32)c.i(), b = (s32)c.i();
  glColor3i(r, g, b);
}
void glColor3ub_(GuestCall& c) {
  const GLubyte r = (GLubyte)c.i(), g = (GLubyte)c.i(), b = (GLubyte)c.i();
  glColor3ub(r, g, b);
}
void glColor4ub_(GuestCall& c) {
  const GLubyte r = (GLubyte)c.i(), g = (GLubyte)c.i(), b = (GLubyte)c.i(), a = (GLubyte)c.i();
  glColor4ub(r, g, b, a);
}
void glColor3ubv_(GuestCall& c) {
  const u8* p = gp(c, c.i());
  if (p) glColor3ubv(p);
}
void glColor4ubv_(GuestCall& c) {
  const u8* p = gp(c, c.i());
  if (p) glColor4ubv(p);
}
void glRecti_(GuestCall& c) {
  const s32 a = (s32)c.i(), b = (s32)c.i(), d = (s32)c.i(), e = (s32)c.i();
  glRecti(a, b, d, e);
}

// ---- GL: lighting, fog, texture environment -----------------------------------

int params_of(GLenum pname) {
  switch (pname) {
    case GL_AMBIENT: case GL_DIFFUSE: case GL_SPECULAR: case GL_POSITION: case GL_EMISSION:
    case GL_AMBIENT_AND_DIFFUSE: case GL_FOG_COLOR: case GL_TEXTURE_ENV_COLOR: case GL_OBJECT_PLANE:
    case GL_EYE_PLANE: case GL_LIGHT_MODEL_AMBIENT: case GL_TEXTURE_BORDER_COLOR:
      return 4;
    case GL_SPOT_DIRECTION: case GL_COLOR_INDEXES:
      return 3;
    default:
      return 1;
  }
}
void glFogf_(GuestCall& c) {
  const GLenum p = c.i();
  glFogf(p, c.f());
}
void glFogi_(GuestCall& c) {
  const GLenum p = c.i();
  glFogi(p, (GLint)c.i());
}
void glFogfv_(GuestCall& c) {
  const GLenum p = c.i();
  glFogfv(p, Floats<4>(c, c.i(), params_of(p)).v);
}
void glLightf_(GuestCall& c) {
  const GLenum l = c.i(), p = c.i();
  glLightf(l, p, c.f());
}
void glLightfv_(GuestCall& c) {
  const GLenum l = c.i(), p = c.i();
  glLightfv(l, p, Floats<4>(c, c.i(), params_of(p)).v);
}
void glGetLightfv_(GuestCall& c) {
  const GLenum l = c.i(), p = c.i();
  const u32 address = c.i();
  if (!address || !c.mem.range_ok(address, 4 * params_of(p), GuestMemory::kWrite)) {
    log_warn("OpenGL: invalid light query result");
    return;
  }
  const u32 bits = 0x7FA5A5A5;
  GLfloat marker;
  memcpy(&marker, &bits, 4);
  GLfloat v[4];
  for (GLfloat& value : v) value = marker;
  glGetLightfv(l, p, v);
  u8* out = gp(c, address);
  int n = 4;
  while (n && memcmp(&v[n - 1], &marker, sizeof(marker)) == 0) n--;
  for (int k = 0; k < n; k++) {
    u32 w;
    memcpy(&w, &v[k], 4);
    wr32(out + 4 * k, w);
  }
}
void glLightModelfv_(GuestCall& c) {
  const GLenum p = c.i();
  glLightModelfv(p, Floats<4>(c, c.i(), params_of(p)).v);
}
void glMaterialfv_(GuestCall& c) {
  const GLenum f = c.i(), p = c.i();
  glMaterialfv(f, p, Floats<4>(c, c.i(), params_of(p)).v);
}
void glTexEnvf_(GuestCall& c) {
  const GLenum t = c.i(), p = c.i();
  glTexEnvf(t, p, c.f());
}
void glTexEnvi_(GuestCall& c) {
  const GLenum t = c.i(), p = c.i();
  glTexEnvi(t, p, (GLint)c.i());
}
void glTexEnvfv_(GuestCall& c) {
  const GLenum t = c.i(), p = c.i();
  glTexEnvfv(t, p, Floats<4>(c, c.i(), params_of(p)).v);
}
void glGetTexEnviv_(GuestCall& c) {
  const GLenum t = c.i(), p = c.i();
  u8* out = gp(c, c.i());
  GLint v[4] = {};
  glGetTexEnviv(t, p, v);
  for (int k = 0; out && k < params_of(p); k++) wr32(out + 4 * k, (u32)v[k]);
}
void glTexGeni_(GuestCall& c) {
  const GLenum t = c.i(), p = c.i();
  glTexGeni(t, p, (GLint)c.i());
}
void glTexGenfv_(GuestCall& c) {
  const GLenum t = c.i(), p = c.i();
  glTexGenfv(t, p, Floats<4>(c, c.i(), params_of(p)).v);
}

// ---- GL: textures ---------------------------------------------------------------

bool legacy_texture_filter() {
  static const bool enabled = [] {
    const char* value = getenv("COCAINE_GL_LEGACY_TEXTURE_FILTER");
    return value && strcmp(value, "1") == 0;
  }();
  return enabled;
}

void update_texture_mip_range(GLenum target, bool maximum_set = false) {
  if (!legacy_texture_filter() || !t_current || target != GL_TEXTURE_2D) return;
  GLint name = 0, base = 0, maximum = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &name);
  glGetTexParameteriv(target, GL_TEXTURE_BASE_LEVEL, &base);
  glGetTexParameteriv(target, GL_TEXTURE_MAX_LEVEL, &maximum);
  if (!name) return;
  auto& textures = *t_current->textures;
  std::lock_guard<std::mutex> lock(textures.mutex);
  auto it = textures.mip_ranges.find((GLuint)name);
  if (it != textures.mip_ranges.end() && (maximum_set || maximum != it->second.applied))
    it->second.requested = maximum;
  GLint width = 0, height = 0, format = 0;
  GLint size_limit = 0, level_limit = 0;
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &size_limit);
  while (size_limit > 1) {
    size_limit /= 2;
    level_limit++;
  }
  if (base <= level_limit) {
    glGetTexLevelParameteriv(target, base, GL_TEXTURE_WIDTH, &width);
    glGetTexLevelParameteriv(target, base, GL_TEXTURE_HEIGHT, &height);
    glGetTexLevelParameteriv(target, base, GL_TEXTURE_INTERNAL_FORMAT, &format);
  }
  const bool dxt = format == GL_COMPRESSED_RGB_S3TC_DXT1_EXT ||
                   format == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT ||
                   format == GL_COMPRESSED_RGBA_S3TC_DXT3_EXT ||
                   format == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
  if (!dxt || width <= 0 || height <= 0) {
    if (it != textures.mip_ranges.end()) {
      glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, it->second.requested);
      textures.mip_ranges.erase(it);
    }
    return;
  }
  if (it == textures.mip_ranges.end())
    it = textures.mip_ranges.emplace((GLuint)name, TextureMipRange{maximum, maximum}).first;
  auto& range = it->second;
  GLint last = base;
  while (last < range.requested && (width > 1 || height > 1)) {
    width = std::max(1, width / 2);
    height = std::max(1, height / 2);
    GLint next_width = 0, next_height = 0, next_format = 0;
    glGetTexLevelParameteriv(target, last + 1, GL_TEXTURE_WIDTH, &next_width);
    glGetTexLevelParameteriv(target, last + 1, GL_TEXTURE_HEIGHT, &next_height);
    glGetTexLevelParameteriv(target, last + 1, GL_TEXTURE_INTERNAL_FORMAT, &next_format);
    if (next_width != width || next_height != height || next_format != format) break;
    last++;
  }
  // Keep the guest's filter and range limit, but never sample missing DXT mip levels.
  const GLint effective = std::min(range.requested, last);
  if (effective != maximum) glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, effective);
  range.applied = effective;
}

void glGenTextures_(GuestCall& c) {
  const s32 n = (s32)c.i();
  u8* out = gp(c, c.i());
  if (n <= 0 || !out) return;
  std::vector<GLuint> v((size_t)n);
  glGenTextures(n, v.data());
  for (s32 k = 0; k < n; k++) wr32(out + 4 * k, v[(size_t)k]);
}
void glDeleteTextures_(GuestCall& c) {
  const s32 n = (s32)c.i();
  const u8* in = gp(c, c.i());
  if (n <= 0 || !in) return;
  std::vector<GLuint> v((size_t)n);
  for (s32 k = 0; k < n; k++) v[(size_t)k] = rd32(in + 4 * k);
  glDeleteTextures(n, v.data());
  if (legacy_texture_filter() && t_current) {
    auto& textures = *t_current->textures;
    std::lock_guard<std::mutex> lock(textures.mutex);
    for (GLuint name : v) textures.mip_ranges.erase(name);
  }
}
void glBindTexture_(GuestCall& c) {
  const GLenum t = c.i();
  const GLuint name = c.i();
  const bool initialize = legacy_texture_filter() && t == GL_TEXTURE_2D && name && !glIsTexture(name);
  glBindTexture(t, name);
  if (initialize && glIsTexture(name)) glTexParameteri(t, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
}
void glTexParameteri_(GuestCall& c) {
  const GLenum t = c.i(), p = c.i();
  const GLint value = (GLint)c.i();
  glTexParameteri(t, p, value);
  if (p == GL_TEXTURE_BASE_LEVEL || p == GL_TEXTURE_MAX_LEVEL)
    update_texture_mip_range(t, p == GL_TEXTURE_MAX_LEVEL && value >= 0);
}
void glTexParameterf_(GuestCall& c) {
  const GLenum t = c.i(), p = c.i();
  const GLfloat value = c.f();
  glTexParameterf(t, p, value);
  if (p == GL_TEXTURE_BASE_LEVEL || p == GL_TEXTURE_MAX_LEVEL)
    update_texture_mip_range(t, p == GL_TEXTURE_MAX_LEVEL && std::isfinite(value) && value >= 0);
}
void glGetTexParameteriv_(GuestCall& c) {
  const GLenum target = c.i(), parameter = c.i();
  u8* out = gp(c, c.i());
  GLint values[4] = {};
  glGetTexParameteriv(target, parameter, values);
  for (int k = 0; out && k < (parameter == GL_TEXTURE_BORDER_COLOR ? 4 : 1); k++)
    wr32(out + 4 * k, (u32)values[k]);
}
void glTexImage2D_(GuestCall& c) {
  const GLenum target = c.i();
  const GLint level = (GLint)c.i(), internal = (GLint)c.i();
  const GLsizei w = (GLsizei)c.i(), h = (GLsizei)c.i();
  const GLint border = (GLint)c.i();
  const GLenum format = c.i(), type = c.i();
  const u32 pixels = c.i();
  glTexImage2D(target, level, internal, w, h, border, format, type, gp(c, pixels));
  update_texture_mip_range(target);
}
void glTexImage3D_(GuestCall& c) {
  const GLenum target = c.i();
  const GLint level = (GLint)c.i(), internal = (GLint)c.i();
  const GLsizei width = (GLsizei)c.i(), height = (GLsizei)c.i(), depth = (GLsizei)c.i();
  const GLint border = (GLint)c.i();
  const GLenum format = c.i(), type = c.i();
  const u32 pixels = c.i();
  if (pixels && width > 0 && height > 0 && depth > 0) {
    int bytes_per_pixel = pixel_size(format, type);
    if (type == GL_HALF_FLOAT) bytes_per_pixel = pixel_size(format, GL_SHORT);
    if (format == GL_DEPTH_STENCIL && type == GL_UNSIGNED_INT_24_8) bytes_per_pixel = 4;
    if (!bytes_per_pixel) {
      log_warn("OpenGL: unsupported 3D texture format 0x%x type 0x%x", format, type);
      return;
    }
    GLint row_length = 0, image_height = 0, skip_pixels = 0, skip_rows = 0, skip_images = 0, alignment = 0;
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
    glGetIntegerv(GL_UNPACK_IMAGE_HEIGHT, &image_height);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_pixels);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_rows);
    glGetIntegerv(GL_UNPACK_SKIP_IMAGES, &skip_images);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
    if (row_length < 0 || image_height < 0 || skip_pixels < 0 || skip_rows < 0 || skip_images < 0 ||
        alignment <= 0) {
      log_warn("OpenGL: invalid 3D texture unpack state");
      return;
    }
    using Wide = __uint128_t;
    const Wide row_bytes = (Wide)(row_length ? row_length : width) * bytes_per_pixel;
    const Wide stride = (row_bytes + alignment - 1) / alignment * alignment;
    const Wide image_stride = stride * (image_height ? image_height : height);
    const Wide bytes = ((Wide)skip_images + depth - 1) * image_stride +
                       ((Wide)skip_rows + height - 1) * stride + ((Wide)skip_pixels + width) * bytes_per_pixel;
    if (bytes > UINT32_MAX || !c.mem.range_ok(pixels, (u32)bytes, GuestMemory::kRead)) {
      log_warn("OpenGL: invalid 3D texture data");
      return;
    }
  }
  glTexImage3D(target, level, internal, width, height, depth, border, format, type, gp(c, pixels));
}
void glTexSubImage2D_(GuestCall& c) {
  const GLenum target = c.i();
  const GLint level = (GLint)c.i(), x = (GLint)c.i(), y = (GLint)c.i();
  const GLsizei w = (GLsizei)c.i(), h = (GLsizei)c.i();
  const GLenum format = c.i(), type = c.i();
  const u32 pixels = c.i();
  glTexSubImage2D(target, level, x, y, w, h, format, type, gp(c, pixels));
}
void glCopyTexSubImage2D_(GuestCall& c) {
  const GLenum target = c.i();
  const GLint level = (GLint)c.i(), xoffset = (GLint)c.i(), yoffset = (GLint)c.i();
  const GLint x = (GLint)c.i(), y = (GLint)c.i();
  const GLsizei width = (GLsizei)c.i(), height = (GLsizei)c.i();
  glCopyTexSubImage2D(target, level, xoffset, yoffset, x, y, width, height);
}
bool compressed_data_valid(GuestCall& c, u32 pixels, GLsizei size, bool allow_null = true) {
  if (size > 0 && ((!pixels && !allow_null) ||
                   (pixels && !c.mem.range_ok(pixels, (u32)size, GuestMemory::kRead)))) {
    log_warn("OpenGL: invalid compressed texture data");
    return false;
  }
  return true;
}
// Compressed blocks have a fixed byte layout, independent of the guest CPU.
void glCompressedTexImage1D_(GuestCall& c) {
  const GLenum target = c.i();
  const GLint level = (GLint)c.i();
  const GLenum format = c.i();
  const GLsizei width = (GLsizei)c.i();
  const GLint border = (GLint)c.i();
  const GLsizei size = (GLsizei)c.i();
  const u32 pixels = c.i();
  if (compressed_data_valid(c, pixels, size))
    glCompressedTexImage1D(target, level, format, width, border, size, gp(c, pixels));
}
void glCompressedTexImage2D_(GuestCall& c) {
  const GLenum target = c.i();
  const GLint level = (GLint)c.i();
  const GLenum format = c.i();
  const GLsizei width = (GLsizei)c.i(), height = (GLsizei)c.i();
  const GLint border = (GLint)c.i();
  const GLsizei size = (GLsizei)c.i();
  const u32 pixels = c.i();
  if (compressed_data_valid(c, pixels, size)) {
    glCompressedTexImage2D(target, level, format, width, height, border, size, gp(c, pixels));
    update_texture_mip_range(target);
  }
}
void glCompressedTexImage3D_(GuestCall& c) {
  const GLenum target = c.i();
  const GLint level = (GLint)c.i();
  const GLenum format = c.i();
  const GLsizei width = (GLsizei)c.i(), height = (GLsizei)c.i(), depth = (GLsizei)c.i();
  const GLint border = (GLint)c.i();
  const GLsizei size = (GLsizei)c.i();
  const u32 pixels = c.i();
  if (compressed_data_valid(c, pixels, size))
    glCompressedTexImage3D(target, level, format, width, height, depth, border, size, gp(c, pixels));
}
void glCompressedTexSubImage1D_(GuestCall& c) {
  const GLenum target = c.i();
  const GLint level = (GLint)c.i(), x = (GLint)c.i();
  const GLsizei width = (GLsizei)c.i();
  const GLenum format = c.i();
  const GLsizei size = (GLsizei)c.i();
  const u32 pixels = c.i();
  if (compressed_data_valid(c, pixels, size, false))
    glCompressedTexSubImage1D(target, level, x, width, format, size, gp(c, pixels));
}
void glCompressedTexSubImage2D_(GuestCall& c) {
  const GLenum target = c.i();
  const GLint level = (GLint)c.i(), x = (GLint)c.i(), y = (GLint)c.i();
  const GLsizei width = (GLsizei)c.i(), height = (GLsizei)c.i();
  const GLenum format = c.i();
  const GLsizei size = (GLsizei)c.i();
  const u32 pixels = c.i();
  if (compressed_data_valid(c, pixels, size, false))
    glCompressedTexSubImage2D(target, level, x, y, width, height, format, size, gp(c, pixels));
}
void glCompressedTexSubImage3D_(GuestCall& c) {
  const GLenum target = c.i();
  const GLint level = (GLint)c.i(), x = (GLint)c.i(), y = (GLint)c.i(), z = (GLint)c.i();
  const GLsizei width = (GLsizei)c.i(), height = (GLsizei)c.i(), depth = (GLsizei)c.i();
  const GLenum format = c.i();
  const GLsizei size = (GLsizei)c.i();
  const u32 pixels = c.i();
  if (compressed_data_valid(c, pixels, size, false))
    glCompressedTexSubImage3D(target, level, x, y, z, width, height, depth, format, size, gp(c, pixels));
}
void glGetTexLevelParameteriv_(GuestCall& c) {
  const GLenum target = c.i();
  const GLint level = (GLint)c.i();
  const GLenum parameter = c.i();
  const u32 out = c.i();
  if (!out || !c.mem.range_ok(out, 4, GuestMemory::kWrite)) {
    log_warn("OpenGL: invalid texture level result");
    return;
  }
  GLint value = 0;
  glGetTexLevelParameteriv(target, level, parameter, &value);
  wr32(gp(c, out), (u32)value);
}
void glGetCompressedTexImage_(GuestCall& c) {
  const GLenum target = c.i();
  const GLint level = (GLint)c.i();
  const u32 pixels = c.i();
  GLint size = 0;
  glGetTexLevelParameteriv(target, level, GL_TEXTURE_COMPRESSED_IMAGE_SIZE, &size);
  if (size > 0 && (!pixels || !c.mem.range_ok(pixels, (u32)size, GuestMemory::kWrite))) {
    log_warn("OpenGL: invalid compressed texture result");
    return;
  }
  glGetCompressedTexImage(target, level, gp(c, pixels));
}
void glGetProgramivARB_(GuestCall& c) {
  const GLenum target = c.i(), pname = c.i();
  const u32 address = c.i();
  if (!address || !c.mem.range_ok(address, 4, GuestMemory::kWrite)) {
    log_warn("OpenGL: invalid program query result");
    return;
  }
  GLint value = 0x7a5a7a5a;
  glGetProgramivARB(target, pname, &value);
  if (value != 0x7a5a7a5a) wr32(gp(c, address), (u32)value);
}
void glGenProgramsARB_(GuestCall& c) {
  const GLsizei count = (GLsizei)c.i();
  const u32 address = c.i();
  if (count <= 0) return glGenProgramsARB(count, nullptr);
  if (!address || !c.mem.range_ok(address, (u64)count * 4, GuestMemory::kWrite)) {
    log_warn("OpenGL: invalid program name result");
    return;
  }
  std::vector<GLuint> names((size_t)count);
  glGenProgramsARB(count, names.data());
  u8* out = gp(c, address);
  for (GLsizei i = 0; i < count; i++) wr32(out + 4 * (size_t)i, names[(size_t)i]);
}
void glDeleteProgramsARB_(GuestCall& c) {
  const GLsizei count = (GLsizei)c.i();
  const u32 address = c.i();
  if (count <= 0) return glDeleteProgramsARB(count, nullptr);
  if (!address || !c.mem.range_ok(address, (u64)count * 4, GuestMemory::kRead)) {
    log_warn("OpenGL: invalid program names");
    return;
  }
  const u8* in = gp(c, address);
  std::vector<GLuint> names((size_t)count);
  for (GLsizei i = 0; i < count; i++) names[(size_t)i] = rd32(in + 4 * (size_t)i);
  glDeleteProgramsARB(count, names.data());
}
void glBindProgramARB_(GuestCall& c) {
  const GLenum target = c.i();
  glBindProgramARB(target, c.i());
}
void glProgramStringARB_(GuestCall& c) {
  const GLenum target = c.i(), format = c.i();
  const GLsizei length = (GLsizei)c.i();
  const u32 address = c.i();
  if (length > 0 && (!address || !c.mem.range_ok(address, (u32)length, GuestMemory::kRead))) {
    log_warn("OpenGL: invalid program string");
    return;
  }
  glProgramStringARB(target, format, length, gp(c, address));
}
void glProgramEnvParameter4fvARB_(GuestCall& c) {
  const GLenum target = c.i();
  const GLuint index = c.i();
  const u32 address = c.i();
  if (!address || !c.mem.range_ok(address, 16, GuestMemory::kRead)) {
    log_warn("OpenGL: invalid program environment parameter");
    return;
  }
  glProgramEnvParameter4fvARB(target, index, Floats<4>(c, address).v);
}
void glReadPixels_(GuestCall& c) {
  const GLint x = (GLint)c.i(), y = (GLint)c.i();
  const GLsizei w = (GLsizei)c.i(), h = (GLsizei)c.i();
  const GLenum format = c.i(), type = c.i();
  glReadPixels(x, y, w, h, format, type, gp(c, c.i()));
}
void glGetTexImage_(GuestCall& c) {
  const GLenum target = c.i();
  const GLint level = (GLint)c.i();
  const GLenum format = c.i(), type = c.i();
  glGetTexImage(target, level, format, type, gp(c, c.i()));
}
void glActiveTextureARB_(GuestCall& c) { glActiveTextureARB(c.i()); }

// ---- GL: vertex arrays ------------------------------------------------------------

int type_size(GLenum t) {
  switch (t) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
    case GL_DOUBLE: return 8;
    default: return 4;
  }
}
void set_pointer(GuestCall& c, Array& a, bool has_size, GLint fixed_size) {
  a.size = has_size ? (GLint)c.i() : fixed_size;
  a.type = c.i();
  a.stride = (GLsizei)c.i();
  a.ptr = c.i();
}
void glVertexPointer_(GuestCall& c) {
  if (t_current) set_pointer(c, t_current->vertex, true, 0);
}
void glNormalPointer_(GuestCall& c) {
  if (t_current) set_pointer(c, t_current->normal, false, 3);
}
void glColorPointer_(GuestCall& c) {
  if (t_current) set_pointer(c, t_current->color_array, true, 0);
}
void glTexCoordPointer_(GuestCall& c) {
  if (t_current) set_pointer(c, t_current->tex[t_current->client_unit & 7], true, 0);
}
Array* array_for(GLenum cap) {
  if (!t_current) return nullptr;
  switch (cap) {
    case GL_VERTEX_ARRAY: return &t_current->vertex;
    case GL_NORMAL_ARRAY: return &t_current->normal;
    case GL_COLOR_ARRAY: return &t_current->color_array;
    case GL_TEXTURE_COORD_ARRAY: return &t_current->tex[t_current->client_unit & 7];
    default: return nullptr;
  }
}
void glEnableClientState_(GuestCall& c) {
  const GLenum cap = c.i();
  if (Array* a = array_for(cap)) a->on = true;
  glEnableClientState(cap);
}
void glDisableClientState_(GuestCall& c) {
  const GLenum cap = c.i();
  if (Array* a = array_for(cap)) a->on = false;
  glDisableClientState(cap);
}
void glClientActiveTextureARB_(GuestCall& c) {
  const GLenum unit = c.i();
  if (t_current) t_current->client_unit = (int)(unit - GL_TEXTURE0);
  glClientActiveTextureARB(unit);
}
void glVertexAttribPointerARB_(GuestCall& c) {
  const GLuint index = c.i();
  const GLint size = (GLint)c.i();
  const GLenum type = c.i();
  const GLboolean normalized = (GLboolean)c.i();
  const GLsizei stride = (GLsizei)c.i();
  const u32 ptr = c.i();
  const bool valid_type = type == GL_BYTE || type == GL_UNSIGNED_BYTE || type == GL_SHORT ||
                          type == GL_UNSIGNED_SHORT || type == GL_INT || type == GL_UNSIGNED_INT ||
                          type == GL_FLOAT || type == GL_DOUBLE;
  if (!t_current || index >= t_current->attribs.size() || size < 1 || size > 4 || stride < 0 || !valid_type) {
    glVertexAttribPointerARB(index, size, type, normalized, stride, nullptr);
    return;
  }
  Array& a = t_current->attribs[index];
  a.size = size;
  a.type = type;
  a.normalized = normalized;
  a.stride = stride;
  a.ptr = ptr;
}
void glEnableVertexAttribArrayARB_(GuestCall& c) {
  const GLuint index = c.i();
  if (t_current && index < t_current->attribs.size()) t_current->attribs[index].on = true;
  glEnableVertexAttribArrayARB(index);
}
void glDisableVertexAttribArrayARB_(GuestCall& c) {
  const GLuint index = c.i();
  if (t_current && index < t_current->attribs.size()) t_current->attribs[index].on = false;
  glDisableVertexAttribArrayARB(index);
}

// Elements lo..hi of an array, converted for the host, at the same indices.
const void* convert(GuestCall& c, Array& a, u32 lo, u32 hi) {
  const int ts = type_size(a.type), es = a.size * ts;
  const u32 stride = a.stride ? (u32)a.stride : (u32)es;
  a.host.resize((size_t)(hi + 1) * (size_t)es);
  if (es > 0 && hi >= lo && (u64)a.ptr + (u64)hi * stride + (u64)es <= GuestMemory::kSpan) {
    const u8* src = gp(c, a.ptr + lo * stride);
    u8* dst = a.host.data() + (size_t)lo * (size_t)es;
    copy_vertex_array(src, dst, (size_t)hi - lo + 1, (size_t)a.size, (size_t)ts, stride, c.powerpc);
    return a.host.data();
  }
  for (u32 i = lo; i <= hi; i++) {
    const u8* src = gp(c, a.ptr + i * stride);
    u8* dst = a.host.data() + (size_t)i * (size_t)es;
    if (!g_big) {  // the host's order already
      memcpy(dst, src, (size_t)es);
      continue;
    }
    for (int k = 0; k < a.size; k++, src += ts, dst += ts) {
      switch (ts) {
        case 1: dst[0] = src[0]; break;
        case 2: dst[0] = src[1]; dst[1] = src[0]; break;
        case 4: dst[0] = src[3]; dst[1] = src[2]; dst[2] = src[1]; dst[3] = src[0]; break;
        default: for (int b = 0; b < 8; b++) dst[b] = src[7 - b]; break;
      }
    }
  }
  return a.host.data();
}
bool program_uses_generic_attribute(size_t index) {
  GLint length = 0;
  glGetProgramivARB(GL_VERTEX_PROGRAM_ARB, GL_PROGRAM_LENGTH_ARB, &length);
  if (length <= 0) return true;
  std::string source((size_t)length, '\0');
  glGetProgramStringARB(GL_VERTEX_PROGRAM_ARB, GL_PROGRAM_STRING_ARB, source.data());
  std::string tokens;
  bool comment = false;
  for (char ch : source) {
    if (ch == '\n' || ch == '\r') comment = false;
    else if (ch == '#') comment = true;
    else if (!comment && !std::isspace((unsigned char)ch)) tokens += ch;
  }
  const std::string binding = "vertex.attrib";
  size_t at = 0;
  while ((at = tokens.find(binding, at)) != std::string::npos) {
    at += binding.size();
    if (at >= tokens.size() || tokens[at++] != '[') return true;
    if (at < tokens.size() && tokens[at] == '+') at++;
    if (at >= tokens.size() || !std::isdigit((unsigned char)tokens[at])) return true;
    size_t number = 0;
    while (at < tokens.size() && std::isdigit((unsigned char)tokens[at])) {
      const unsigned digit = (unsigned)(tokens[at++] - '0');
      if (number > (SIZE_MAX - digit) / 10) return true;
      number = number * 10 + digit;
    }
    if (at >= tokens.size() || tokens[at++] != ']') return true;
    if (number == index) return true;
  }
  return false;
}
bool prepare_arrays(GuestCall& c, u32 lo, u32 hi) {
  Context* x = t_current;
  if (!x) return false;
  const bool vertex_program = glIsEnabled(GL_VERTEX_PROGRAM_ARB);
  auto array_pointer = [&](Array& a, GLsizei& stride, bool checked) -> const void* {
    stride = 0;
    if ((!c.powerpc || type_size(a.type) == 1) && a.ptr && a.stride >= 0) {
      const u64 element = (u64)a.size * type_size(a.type);
      const u64 source_stride = a.stride ? (u32)a.stride : element;
      const u64 end = (u64)a.ptr + (u64)hi * source_stride + element;
      if (element && end <= GuestMemory::kSpan &&
          ((checked && lo == 0) || c.mem.range_ok(a.ptr, end - a.ptr, GuestMemory::kRead))) {
        stride = a.stride;
        return gp(c, a.ptr);
      }
    }
    return convert(c, a, lo, hi);
  };
  for (size_t i = 0; i < x->attribs.size(); i++) {
    Array& a = x->attribs[i];
    if (!a.on) continue;
    // Fixed-function draws use conventional arrays, with generic position as a fallback.
    if (!vertex_program && (i != 0 || x->vertex.on)) continue;
    const u64 element = (u64)a.size * type_size(a.type);
    const u64 step = a.stride ? (u32)a.stride : element;
    const u64 start = (u64)a.ptr + (u64)lo * step;
    const u64 end = (u64)a.ptr + (u64)hi * step + element;
    if (!a.ptr || end > GuestMemory::kSpan || start >= end ||
        !c.mem.range_ok((u32)start, end - start, GuestMemory::kRead)) {
      if (vertex_program && !program_uses_generic_attribute(i)) continue;
      log_warn("OpenGL: invalid vertex attribute array %zu (pointer %#x, vertices %u..%u, vertex program %s)",
               i, a.ptr, lo, hi, vertex_program ? "enabled" : "disabled");
      return false;
    }
    GLsizei stride;
    const void* ptr = array_pointer(a, stride, true);
    glVertexAttribPointerARB((GLuint)i, a.size, a.type, a.normalized, stride, ptr);
  }
  if (x->vertex.on && x->vertex.ptr) {
    GLsizei stride;
    const void* ptr = array_pointer(x->vertex, stride, false);
    glVertexPointer(x->vertex.size, x->vertex.type, stride, ptr);
  }
  if (x->normal.on && x->normal.ptr) {
    GLsizei stride;
    const void* ptr = array_pointer(x->normal, stride, false);
    glNormalPointer(x->normal.type, stride, ptr);
  }
  if (x->color_array.on && x->color_array.ptr) {
    GLsizei stride;
    const void* ptr = array_pointer(x->color_array, stride, false);
    glColorPointer(x->color_array.size, x->color_array.type, stride, ptr);
  }
  bool switched = false;
  for (int u = 0; u < 8; u++) {
    Array& a = x->tex[u];
    if (!a.on || !a.ptr) continue;
    glClientActiveTextureARB(GL_TEXTURE0 + (GLenum)u);
    switched = true;
    GLsizei stride;
    const void* ptr = array_pointer(a, stride, false);
    glTexCoordPointer(a.size, a.type, stride, ptr);
  }
  if (switched) glClientActiveTextureARB(GL_TEXTURE0 + (GLenum)x->client_unit);
  return true;
}
void glDrawArrays_(GuestCall& c) {
  const GLenum mode = c.i();
  const GLint first = (GLint)c.i();
  const GLsizei count = (GLsizei)c.i();
  if (count <= 0 || first < 0) return;
  if ((u64)first + (u64)count - 1 > UINT32_MAX ||
      !prepare_arrays(c, (u32)first, (u32)((u64)first + count - 1))) return;
  glDrawArrays(mode, first, count);
}
void draw_elements(GuestCall& c, GLenum mode, GLsizei count, GLenum type, u32 indices) {
  const u8* in = gp(c, indices);
  if (count <= 0 || !in) return;
  const bool direct = type == GL_UNSIGNED_BYTE ||
                      (!c.powerpc && (type == GL_UNSIGNED_SHORT || type == GL_UNSIGNED_INT));
  static thread_local std::vector<GLuint> idx;
  if (!direct) idx.resize((size_t)count);
  u32 lo = 0xFFFFFFFFu, hi = 0;
  for (GLsizei k = 0; k < count; k++) {
    const u32 v = type == GL_UNSIGNED_BYTE ? in[k] : type == GL_UNSIGNED_SHORT ? rd16(in + 2 * k) : rd32(in + 4 * k);
    if (!direct) idx[(size_t)k] = v;
    lo = std::min(lo, v);
    hi = std::max(hi, v);
  }
  if (!prepare_arrays(c, lo, hi)) return;
  const void* elements = direct ? static_cast<const void*>(in) : static_cast<const void*>(idx.data());
  glDrawElements(mode, count, direct ? type : GL_UNSIGNED_INT, elements);
}
void glDrawElements_(GuestCall& c) {
  const GLenum mode = c.i();
  const GLsizei count = (GLsizei)c.i();
  const GLenum type = c.i();
  const u32 indices = c.i();
  draw_elements(c, mode, count, type, indices);
}
void glDrawRangeElements_(GuestCall& c) {
  const GLenum mode = c.i();
  c.i();  // The range is only a hint; draw_elements finds the actual bounds.
  c.i();
  const GLsizei count = (GLsizei)c.i();
  const GLenum type = c.i();
  const u32 indices = c.i();
  draw_elements(c, mode, count, type, indices);
}
// One element of each enabled array, as immediate mode would give it.
void glArrayElement_(GuestCall& c) {
  Context* x = t_current;
  const u32 i = c.i();
  if (!x) return;
  // Integers are scaled to [0, 1] or [-1, 1] only where GL scales them:
  // colours of every integer type, and normals (whose types are all
  // signed). Vertices and texture coordinates keep their integer values.
  auto get = [&](const Array& a, bool scaled, GLfloat out[4]) {
    const int ts = type_size(a.type);
    const u32 stride = a.stride ? (u32)a.stride : (u32)(a.size * ts);
    const u8* p = gp(c, a.ptr + i * stride);
    for (int k = 0; k < 4; k++) out[k] = k == 3 ? 1.0f : 0.0f;
    for (int k = 0; k < a.size && k < 4; k++, p += ts) {
      switch (a.type) {
        case GL_UNSIGNED_BYTE: out[k] = scaled ? p[0] / 255.0f : p[0]; break;
        case GL_BYTE: out[k] = scaled ? (s8)p[0] / 127.0f : (s8)p[0]; break;
        case GL_UNSIGNED_SHORT: out[k] = scaled ? rd16(p) / 65535.0f : rd16(p); break;
        case GL_SHORT: out[k] = scaled ? (s16)rd16(p) / 32767.0f : (s16)rd16(p); break;
        case GL_UNSIGNED_INT: out[k] = (float)(scaled ? rd32(p) / 4294967295.0 : (double)rd32(p)); break;
        case GL_INT: out[k] = (float)(scaled ? (s32)rd32(p) / 2147483647.0 : (double)(s32)rd32(p)); break;
        case GL_DOUBLE: out[k] = (float)rdd(p); break;
        default: out[k] = rdf(p); break;
      }
    }
  };
  GLfloat v[4];
  for (int u = 0; u < 8; u++) {
    if (!x->tex[u].on || !x->tex[u].ptr) continue;
    get(x->tex[u], false, v);
    glMultiTexCoord4fv(GL_TEXTURE0 + (GLenum)u, v);
  }
  if (x->color_array.on && x->color_array.ptr) {
    get(x->color_array, true, v);
    glColor4fv(v);
  }
  if (x->normal.on && x->normal.ptr) {
    get(x->normal, true, v);
    glNormal3fv(v);
  }
  if (x->vertex.on && x->vertex.ptr) {
    get(x->vertex, false, v);
    if (x->vertex.size == 2) v[2] = 0;
    glVertex4fv(v);
  }
}
void glLockArraysEXT_(GuestCall& c) {}  // the arrays are copied at each draw anyway
void glUnlockArraysEXT_(GuestCall& c) {}

// ---- the table --------------------------------------------------------------------

#define H(name) {#name, name##_}
const HookSpec kGL[] = {
    {"CGLSetCurrentContext", cglSetCurrentContext},
    {"CGLGetCurrentContext", cglGetCurrentContext},
    H(glEnable), H(glDisable), H(glDepthRange), H(glClearDepth), H(glLightModeli), H(glMaterialf),
    H(glPointSize), H(glShadeModel), H(glIsEnabled), H(glClear), H(glClearColor), H(glViewport), H(glBlendFunc),
    H(glBlendEquation),
    H(glScissor), H(glColorMaterial),
    H(glAlphaFunc), H(glCullFace), H(glFrontFace), H(glDepthFunc), H(glDepthMask), H(glColorMask),
    H(glStencilFunc), H(glStencilMask), H(glStencilOp), H(glPolygonMode), H(glPolygonOffset), H(glLineWidth),
    H(glLineStipple),
    H(glHint), H(glPushAttrib), H(glPopAttrib), H(glPushClientAttrib), H(glPopClientAttrib),
    H(glFinish), H(glFlush), H(glGetError), H(glDrawBuffer), H(glReadBuffer),
    H(glPixelStorei), H(glGetIntegerv), H(glGetBooleanv), H(glGetFloatv), H(glGetDoublev), H(glGetString), H(glMatrixMode),
    H(glLoadIdentity), H(glPushMatrix), H(glPopMatrix), H(glLoadMatrixf), H(glMultMatrixf), H(glTranslatef),
    H(glScalef), H(glRotatef), H(glOrtho), H(glFrustum), H(glClipPlane), H(glBegin), H(glEnd), H(glVertex2f),
    H(glNormal3f), H(glRasterPos2f), H(glBitmap), H(glGenLists), H(glNewList), H(glEndList),
    H(glCallList), H(glCallLists), H(glListBase), H(glDeleteLists),
    H(glVertex2i), H(glVertex3f), H(glVertex3d), H(glVertex2fv), H(glVertex3fv), H(glVertex3dv),
    H(glTexCoord2f), H(glTexCoord2d), H(glTexCoord2i), H(glTexCoord2s), H(glTexCoord4f), H(glTexCoord2fv),
    H(glColor3f), H(glColor4f),
    H(glColor4fv), H(glColor3fv), H(glColor3i), H(glColor3ub), H(glColor4ub), H(glColor3ubv), H(glColor4ubv), H(glRecti),
    H(glFogf), H(glFogi), H(glFogfv), H(glLightf), H(glLightfv), H(glGetLightfv), H(glLightModelfv), H(glMaterialfv),
    H(glTexEnvf), H(glTexEnvi), H(glTexEnvfv), H(glGetTexEnviv), H(glTexGeni), H(glTexGenfv), H(glGenTextures),
    H(glDeleteTextures), H(glBindTexture), H(glTexParameteri), H(glTexParameterf),
    H(glGetTexParameteriv), H(glTexImage2D), H(glTexImage3D),
    H(glTexSubImage2D), H(glCopyTexSubImage2D), H(glReadPixels), H(glGetTexImage), H(glActiveTextureARB), H(glVertexPointer), H(glNormalPointer),
    H(glCompressedTexImage1D), H(glCompressedTexImage2D), H(glCompressedTexImage3D),
    H(glCompressedTexSubImage1D), H(glCompressedTexSubImage2D), H(glCompressedTexSubImage3D),
    H(glGetCompressedTexImage), H(glGetTexLevelParameteriv),
    H(glGetProgramivARB), H(glGenProgramsARB), H(glDeleteProgramsARB), H(glBindProgramARB),
    H(glProgramStringARB), H(glProgramEnvParameter4fvARB),
    {"glCompressedTexImage1DARB", glCompressedTexImage1D_},
    {"glCompressedTexImage2DARB", glCompressedTexImage2D_},
    {"glCompressedTexImage3DARB", glCompressedTexImage3D_},
    {"glCompressedTexSubImage1DARB", glCompressedTexSubImage1D_},
    {"glCompressedTexSubImage2DARB", glCompressedTexSubImage2D_},
    {"glCompressedTexSubImage3DARB", glCompressedTexSubImage3D_},
    {"glGetCompressedTexImageARB", glGetCompressedTexImage_},
    H(glColorPointer), H(glTexCoordPointer), H(glEnableClientState), H(glDisableClientState),
    H(glVertexAttribPointerARB), H(glEnableVertexAttribArrayARB), H(glDisableVertexAttribArrayARB),
    H(glClientActiveTextureARB), H(glDrawArrays), H(glDrawElements), H(glDrawRangeElements),
    {"glDrawRangeElementsEXT", glDrawRangeElements_}, H(glArrayElement), H(glLockArraysEXT),
    H(glUnlockArraysEXT),
};
#undef H

const HookSpec kAGL[] = {
    {"aglGetVersion", aglGetVersion, true},
    {"aglChoosePixelFormat", aglChoosePixelFormat},
    {"aglChoosePixelFormatCFM", aglChoosePixelFormat},
    {"aglQueryRendererInfo", aglQueryRendererInfo},
    {"aglQueryRendererInfoCFM", aglQueryRendererInfo},
    {"aglNextRendererInfo", aglNextRendererInfo},
    {"aglDescribeRenderer", aglDescribeRenderer},
    {"aglDestroyRendererInfo", aglDestroyRendererInfo},
    {"aglCreateContextCFM", aglCreateContext},
    {"aglDescribePixelFormat", aglDescribePixelFormat},
    {"aglDisable", aglDisable},
    {"aglDestroyPixelFormat", aglDestroyPixelFormat},
    {"aglCreateContext", aglCreateContext},
    {"aglDestroyContext", aglDestroyContext},
    {"aglSetCurrentContext", aglSetCurrentContext},
    {"aglGetCurrentContext", aglGetCurrentContext},
    {"aglSetDrawable", aglSetDrawable},
    {"aglGetDrawable", aglGetDrawable},
    {"aglUpdateContext", aglUpdateContext},
    {"aglSwapBuffers", aglSwapBuffers},
    {"aglSetFullScreen", aglSetFullScreen},
    {"aglSetInteger", agl_true},
    {"aglConfigure", agl_true},
    {"aglGetError", aglGetError},
    {"aglErrorString", aglErrorString},
};
const HookSpec kGLU[] = {
    {"gluOrtho2D", gluOrtho2D}, {"gluPerspective", gluPerspective_}, {"gluLookAt", gluLookAt_},
    {"gluNewQuadric", gluNewQuadric_}, {"gluDeleteQuadric", gluDeleteQuadric_},
    {"gluQuadricNormals", gluQuadricNormals_}, {"gluQuadricTexture", gluQuadricTexture_},
    {"gluQuadricOrientation", gluQuadricOrientation_},
    {"gluSphere", gluSphere_}, {"gluCylinder", gluCylinder_}, {"gluDisk", gluDisk_},
    {"gluPartialDisk", gluPartialDisk_}, {"gluBuild2DMipmaps", gluBuild2DMipmaps_},
};
const HookSpec kCGL[] = {
    {"CGLChoosePixelFormat", cglChoosePixelFormat},
    {"CGLDescribePixelFormat", cglDescribePixelFormat},
    {"CGLDestroyPixelFormat", cglDestroyPixelFormat},
    {"CGLRetainPixelFormat", cglRetainPixelFormat},
    {"CGLReleasePixelFormat", cglReleasePixelFormat},
    {"CGLCreateContext", cglCreateContext},
    {"CGLDestroyContext", cglDestroyContext},
    {"CGLRetainContext", cglRetainContext},
    {"CGLReleaseContext", cglReleaseContext},
    {"CGLClearDrawable", cglClearDrawable},
    {"CGLSetFullScreen", cglSetFullScreen},
    {"CGLSetFullScreenOnDisplay", cglSetFullScreenOnDisplay},
    {"CGLSetSurface", cglSetSurface},
    {"CGLUpdateContext", cglUpdateContext},
    {"CGLSetParameter", cglSetParameter},
    {"CGLGetParameter", cglGetParameter},
    {"CGLFlushDrawable", cglFlushDrawable},
};

const HookLibrary kLibraries[] = {
    {"/OpenGL.framework/Versions/A/Libraries/libGL.dylib", "gl", kGL, sizeof kGL / sizeof kGL[0], true},
    {"/AGL.framework/Versions/A/AGL", "agl", kAGL, sizeof kAGL / sizeof kAGL[0], true},
    {"/OpenGL.framework/Versions/A/Libraries/libGLU.dylib", "", kGLU, sizeof kGLU / sizeof kGLU[0], true},
    {"/OpenGL.framework/Versions/A/OpenGL", "", kCGL, sizeof kCGL / sizeof kCGL[0], true},
};

}  // namespace

const HookLibrary* gl_hook_libraries(size_t& n) {
  n = sizeof kLibraries / sizeof kLibraries[0];
  return kLibraries;
}
