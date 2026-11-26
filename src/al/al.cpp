// OpenAL for PowerPC guests, done by the Mac's own OpenAL.
//
// Games of the time shipped OpenAL inside their bundles, built on Core Audio
// drivers cocaine doesn't have. So, as with OpenGL (src/gl/gl.cpp), cocaine
// takes over the guest's OpenAL and makes each call on the host's. Sources
// and buffers are numbers and pass straight through; devices and contexts
// are pointers, so the guest gets stand-in handles. What needs care is byte
// order: 16-bit samples, attribute lists and the arrays some calls take or
// fill are big-endian in guest memory.

#define OPENAL_DEPRECATED
#include <OpenAL/al.h>
#include <OpenAL/alc.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "kernel/hooks.h"
#include "kernel/process.h"

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace {

const bool g_trace = getenv("COCAINE_AL_TRACE") != nullptr;

// Whether the guest is big-endian (PowerPC); Intel guests need no swapping.
bool g_big = true;
inline u8* gp(GuestCall& c, u32 addr) {
  g_big = c.powerpc;
  return addr ? c.mem.host(addr) : nullptr;
}
inline u32 rd32(const u8* p) {
  return g_big ? (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]
               : (u32)p[3] << 24 | (u32)p[2] << 16 | (u32)p[1] << 8 | p[0];
}
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
inline float rdf(const u8* p) {
  const u32 v = rd32(p);
  float f;
  memcpy(&f, &v, 4);
  return f;
}
inline void wrf(u8* p, float f) {
  u32 v;
  memcpy(&v, &f, 4);
  wr32(p, v);
}

// Stand-in handles for the host's devices and contexts.
std::mutex g_mu;
std::unordered_map<u32, void*> g_handles;
u32 g_next = 0x00A1D000;
u32 handle_for(void* p) {
  if (!p) return 0;
  std::lock_guard<std::mutex> lk(g_mu);
  for (const auto& [h, q] : g_handles) {
    if (q == p) return h;
  }
  g_next += 0x10;
  g_handles[g_next] = p;
  return g_next;
}
void* host_of(u32 h) {
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_handles.find(h);
  return it == g_handles.end() ? nullptr : it->second;
}
void forget(u32 h) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_handles.erase(h);
}

// What has gone to the speakers, for -v.
std::atomic<u32> g_plays{0}, g_queued{0};
std::atomic<u64> g_bytes{0};

// How many floats a vector parameter has.
int count_of(ALenum p) {
  switch (p) {
    case AL_ORIENTATION: return 6;
    case AL_POSITION: case AL_VELOCITY: case AL_DIRECTION: return 3;
    default: return 1;
  }
}

// ---- devices and contexts -------------------------------------------------------

void alcOpenDevice_(GuestCall& c) {
  const u32 name = c.i();
  (void)name;  // the guest names its own era's devices; the Mac's default will do
  ALCdevice* d = alcOpenDevice(nullptr);
  if (!d) log_warn("OpenAL: the Mac has no sound device to open");
  c.ret(handle_for(d));
}
void alcCloseDevice_(GuestCall& c) {
  const u32 h = c.i();
  ALCdevice* d = (ALCdevice*)host_of(h);
  if (d) alcCloseDevice(d);
  forget(h);
  c.ret(1);
}
void alcCreateContext_(GuestCall& c) {
  ALCdevice* d = (ALCdevice*)host_of(c.i());
  const u8* in = gp(c, c.i());
  std::vector<ALCint> attrs;
  for (int k = 0; in && k < 64; k += 2) {
    const ALCint key = (ALCint)rd32(in + 4 * k);
    attrs.push_back(key);
    if (!key) break;
    attrs.push_back((ALCint)rd32(in + 4 * k + 4));
  }
  if (!attrs.empty() && attrs.back() != 0) attrs.push_back(0);
  ALCcontext* x = d ? alcCreateContext(d, attrs.empty() ? nullptr : attrs.data()) : nullptr;
  c.ret(handle_for(x));
}
void alcMakeContextCurrent_(GuestCall& c) {
  const u32 h = c.i();
  c.ret(alcMakeContextCurrent(h ? (ALCcontext*)host_of(h) : nullptr));
}
void alcGetCurrentContext_(GuestCall& c) { c.ret(handle_for(alcGetCurrentContext())); }
void alcDestroyContext_(GuestCall& c) {
  const u32 h = c.i();
  if (ALCcontext* x = (ALCcontext*)host_of(h)) alcDestroyContext(x);
  forget(h);
}

// ---- state ----------------------------------------------------------------------

// Reading OpenAL's error clears it. An error taken for the -v tally (see
// tally) is kept here for the guest's own next alGetError, since OpenAL only
// keeps the first error until it is read.
std::atomic<ALenum> g_pending_error{AL_NO_ERROR};
ALenum take_error() {
  const ALenum pending = g_pending_error.exchange(AL_NO_ERROR);
  const ALenum now = alGetError();
  return pending != AL_NO_ERROR ? pending : now;
}

void alGetError_(GuestCall& c) { c.ret((u32)take_error()); }
void alEnable_(GuestCall& c) {
  const ALenum p = (ALenum)c.i();
  if (g_trace) log_info("OpenAL: alEnable(0x%x)", (unsigned)p);
  alEnable(p);
}
void alDistanceModel_(GuestCall& c) {
  const ALenum p = (ALenum)c.i();
  if (g_trace) log_info("OpenAL: alDistanceModel(0x%x)", (unsigned)p);
  alDistanceModel(p);
}
void alGetString_(GuestCall& c) {
  const ALenum p = (ALenum)c.i();
  static std::mutex mu;
  static std::unordered_map<ALenum, u32> made;
  std::lock_guard<std::mutex> lk(mu);
  if (auto it = made.find(p); it != made.end()) {
    c.ret(it->second);
    return;
  }
  const char* s = (const char*)alGetString(p);
  if (!s) {
    c.ret(0);
    return;
  }
  const u32 at = c.scratch((u32)strlen(s) + 1);
  if (at) memcpy(c.mem.host(at), s, strlen(s) + 1);
  made[p] = at;
  c.ret(at);
}

// ---- buffers and sources --------------------------------------------------------

void names_out(GuestCall& c, void (*gen)(ALsizei, ALuint*)) {
  const ALsizei n = (ALsizei)c.i();
  u8* out = gp(c, c.i());
  if (n <= 0 || !out) return;
  static thread_local std::vector<ALuint> v;
  v.resize((size_t)n);
  gen(n, v.data());
  for (ALsizei k = 0; k < n; k++) wr32(out + 4 * k, v[(size_t)k]);
  if (g_trace) for (ALuint name : v) log_info("OpenAL: made %u", name);
}
void names_in(GuestCall& c, void (*del)(ALsizei, const ALuint*)) {
  const ALsizei n = (ALsizei)c.i();
  const u8* in = gp(c, c.i());
  if (n <= 0 || !in) return;
  static thread_local std::vector<ALuint> v;
  v.resize((size_t)n);
  for (ALsizei k = 0; k < n; k++) v[(size_t)k] = rd32(in + 4 * k);
  if (g_trace) for (ALuint name : v) log_info("OpenAL: deleting %u", name);
  del(n, v.data());
}
void alGenBuffers_(GuestCall& c) { names_out(c, alGenBuffers); }
void alDeleteBuffers_(GuestCall& c) { names_in(c, alDeleteBuffers); }
void alGenSources_(GuestCall& c) { names_out(c, alGenSources); }
void forget_sources(ALsizei n, const ALuint* names);
void alDeleteSources_(GuestCall& c) { names_in(c, forget_sources); }
void alIsBuffer_(GuestCall& c) {
  const ALuint b = c.i();
  const ALboolean yes = alIsBuffer(b);
  if (g_trace) log_info("OpenAL: alIsBuffer(%u) = %d", b, yes);
  c.ret(yes);
}

// For working on cocaine: COCAINE_AL_DUMP=dir writes each buffer of sound
// the guest hands OpenAL to dir, as the host gets it (16-bit samples in this
// Mac's byte order), one file a buffer, named by order, buffer, format and rate.
void dump_buffer(ALuint b, ALenum format, const void* data, ALsizei size, ALsizei freq) {
  static const char* dir = getenv("COCAINE_AL_DUMP");
  if (!dir) return;
  static std::atomic<u32> n{0};
  char path[1024];
  snprintf(path, sizeof path, "%s/%05u-b%u-f%x-%d.raw", dir, (u32)n++, b, (u32)format, (int)freq);
  if (FILE* f = fopen(path, "wb")) {
    fwrite(data, 1, (size_t)size, f);
    fclose(f);
  }
}

// Which byte order an Intel guest's 16-bit samples come in. Leopard's own
// OpenAL takes them in Intel's order, but the OpenAL games bundled for their
// first universal builds kept PowerPC's: Marble Blast Gold's Intel copy still
// declares 16-bit samples big-endian, and the game hands them over that way.
// Sound changes smoothly from one sample to the next and the wrong reading of
// it is noise, so whichever reading jumps about less is right. A buffer that
// can't tell (near silence, or noise itself) goes by the last one that could.
std::atomic<int> g_intel_order{0};  // 0 not known yet, 1 Intel's own, 2 swapped
bool intel_samples_swapped(const u8* data, ALsizei size, ALenum format) {
  const size_t step = format == AL_FORMAT_STEREO16 ? 2 : 1;  // the same channel's last sample
  const size_t n = std::min<size_t>((size_t)size / 2, 16384);
  u64 own = 0, swapped = 0;
  for (size_t k = step; k < n; k++) {
    const u8* a = data + 2 * k;
    const u8* b = data + 2 * (k - step);
    own += (u64)std::abs((int)(s16)(a[0] | a[1] << 8) - (int)(s16)(b[0] | b[1] << 8));
    swapped += (u64)std::abs((int)(s16)(a[1] | a[0] << 8) - (int)(s16)(b[1] | b[0] << 8));
  }
  if (own * 2 < swapped) {
    g_intel_order = 1;
  } else if (swapped * 2 < own) {
    if (g_intel_order.exchange(2) != 2) log_info("OpenAL: this game's 16-bit sound is big-endian; swapping it");
  }
  return g_intel_order == 2;
}

void alBufferData_(GuestCall& c) {
  const ALuint b = c.i();
  const ALenum format = (ALenum)c.i();
  const u8* data = gp(c, c.i());
  const ALsizei size = (ALsizei)c.i();
  const ALsizei freq = (ALsizei)c.i();
  g_bytes += size > 0 ? (u64)size : 0;
  if (!data || size <= 0) {
    alBufferData(b, format, nullptr, 0, freq);
    return;
  }
  const bool sixteen = format == AL_FORMAT_MONO16 || format == AL_FORMAT_STEREO16;
  if (sixteen && (g_big || intel_samples_swapped(data, size, format))) {  // big-endian samples
    static thread_local std::vector<u8> swapped;
    swapped.resize((size_t)size);
    for (ALsizei k = 0; k + 1 < size; k += 2) {
      swapped[(size_t)k] = data[k + 1];
      swapped[(size_t)k + 1] = data[k];
    }
    dump_buffer(b, format, swapped.data(), size, freq);
    alBufferData(b, format, swapped.data(), size, freq);
  } else {
    dump_buffer(b, format, data, size, freq);
    alBufferData(b, format, data, size, freq);
  }
}
void alGetBufferi_(GuestCall& c) {
  const ALuint b = c.i();
  const ALenum p = (ALenum)c.i();
  u8* out = gp(c, c.i());
  ALint v = 0;
  alGetBufferi(b, p, &v);
  if (out) wr32(out, (u32)v);
}

void tally() {  // with -v, now and then: what the game has sent to the speakers
  static std::atomic<u32> last{0};
  const u32 n = g_plays + g_queued;
  if (n / 50 != last / 50) {
    last = n;
    const ALenum error = take_error();
    if (error != AL_NO_ERROR) g_pending_error = error;  // still the guest's to read
    log_info("OpenAL: %u sounds started, %u buffers queued, %llu bytes of sound, error %d", (u32)g_plays,
             (u32)g_queued, (unsigned long long)g_bytes, (int)error);
  }
}
// ---- streams the game stalled on ------------------------------------------------
//
// A game streaming music keeps a source's queue topped up from its main loop.
// When that loop stalls (Marble Blast's does while its window is dragged),
// the queue runs dry, and this Mac's OpenAL stops the source. The OpenAL
// games of the time kept such a source playing, waiting for more; Marble
// Blast takes "stopped" for the end of the track, lets the source go, and
// then pours the rest of the song through it in silence (every buffer
// queued on a stopped source counts as played at once), so the music comes
// back as the wrong track, or garbled. So while the game still means a
// stream to play, it isn't told the stream stopped for want of buffers, and
// the stream starts again when more arrive. A stream the game stopped
// feeding while it went on running is let stop as usual.
std::mutex g_stream_mu;
// The game's running time: real time, except that a gap between two OpenAL
// calls counts as a tenth of a second at most (the game was stalled).
u64 g_active_ns = 0, g_last_call_ns = 0;
// Sources the game started and hasn't stopped, with the running time it last
// queued buffers on each.
std::unordered_map<ALuint, u64> g_playing;
constexpr u64 kStallGap = 100000000ull, kStillFeeding = 1000000000ull;

void tick_active() {
  const u64 now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  std::lock_guard<std::mutex> lk(g_stream_mu);
  if (g_last_call_ns) g_active_ns += std::min(now - g_last_call_ns, kStallGap);
  g_last_call_ns = now;
}

// Whether a source the host has stopped is only starved: a stream the game
// still means to play and fed lately.
bool starved(ALuint s) {
  {
    std::lock_guard<std::mutex> lk(g_stream_mu);
    auto it = g_playing.find(s);
    if (it == g_playing.end() || g_active_ns - it->second > kStillFeeding) return false;
  }
  ALint type = 0;
  alGetSourcei(s, AL_SOURCE_TYPE, &type);
  return type == AL_STREAMING;
}

// Deleted sources leave the list of those playing.
void forget_sources(ALsizei n, const ALuint* names) {
  {
    std::lock_guard<std::mutex> lk(g_stream_mu);
    for (ALsizei k = 0; k < n; k++) g_playing.erase(names[k]);
  }
  alDeleteSources(n, names);
}

void alSourcePlay_(GuestCall& c) {
  const ALuint s = c.i();
  if (g_trace) log_info("OpenAL: alSourcePlay(%u)", s);
  {
    std::lock_guard<std::mutex> lk(g_stream_mu);
    g_playing[s] = g_active_ns;
  }
  alSourcePlay(s);
  g_plays++;
  tally();
}
void alSourceStop_(GuestCall& c) {
  const ALuint s = c.i();
  if (g_trace) log_info("OpenAL: alSourceStop(%u)", s);
  {
    std::lock_guard<std::mutex> lk(g_stream_mu);
    g_playing.erase(s);
  }
  alSourceStop(s);
}
void alSourcef_(GuestCall& c) {
  const ALuint s = c.i();
  const ALenum p = (ALenum)c.i();
  const float v = c.f();
  if (g_trace) log_info("OpenAL: alSourcef(%u, 0x%x, %g)", s, (unsigned)p, v);
  alSourcef(s, p, v);
}
void alSource3f_(GuestCall& c) {
  const ALuint s = c.i();
  const ALenum p = (ALenum)c.i();
  const float x = c.f(), y = c.f(), z = c.f();
  if (g_trace) log_info("OpenAL: alSource3f(%u, 0x%x, %g %g %g)", s, (unsigned)p, x, y, z);
  alSource3f(s, p, x, y, z);
}
void alSourcefv_(GuestCall& c) {
  const ALuint s = c.i();
  const ALenum p = (ALenum)c.i();
  const u8* in = gp(c, c.i());
  ALfloat v[6] = {};
  for (int k = 0; in && k < count_of(p); k++) v[k] = rdf(in + 4 * k);
  if (g_trace) log_info("OpenAL: alSourcefv(%u, 0x%x, %g %g %g)", s, (unsigned)p, v[0], v[1], v[2]);
  alSourcefv(s, p, v);
}
void alSourcei_(GuestCall& c) {
  const ALuint s = c.i();
  const ALenum p = (ALenum)c.i();
  const ALint v = (ALint)c.i();
  if (g_trace) log_info("OpenAL: alSourcei(%u, 0x%x, %d)", s, (unsigned)p, v);
  alSourcei(s, p, v);
}
void alGetSourcef_(GuestCall& c) {
  const ALuint s = c.i();
  const ALenum p = (ALenum)c.i();
  u8* out = gp(c, c.i());
  ALfloat v = 0;
  alGetSourcef(s, p, &v);
  if (out) wrf(out, v);
}
void alGetSourcefv_(GuestCall& c) {
  const ALuint s = c.i();
  const ALenum p = (ALenum)c.i();
  u8* out = gp(c, c.i());
  ALfloat v[6] = {};
  alGetSourcefv(s, p, v);
  for (int k = 0; out && k < count_of(p); k++) wrf(out + 4 * k, v[k]);
}
void alGetSourcei_(GuestCall& c) {
  const ALuint s = c.i();
  const ALenum p = (ALenum)c.i();
  u8* out = gp(c, c.i());
  ALint v = 0;
  alGetSourcei(s, p, &v);
  if (p == AL_SOURCE_STATE && v == AL_STOPPED && starved(s)) v = AL_PLAYING;
  if (g_trace) log_info("OpenAL: alGetSourcei(%u, 0x%x) = 0x%x", s, (unsigned)p, (unsigned)v);
  if (out) wr32(out, (u32)v);
}
void alSourceQueueBuffers_(GuestCall& c) {
  const ALuint s = c.i();
  const ALsizei n = (ALsizei)c.i();
  const u8* in = gp(c, c.i());
  if (n <= 0 || !in) return;
  static thread_local std::vector<ALuint> v;
  v.resize((size_t)n);
  for (ALsizei k = 0; k < n; k++) v[(size_t)k] = rd32(in + 4 * k);
  alSourceQueueBuffers(s, n, v.data());
  if (g_trace) log_info("OpenAL: alSourceQueueBuffers(%u, %d) first %u", s, (int)n, v[0]);
  bool playing;
  {
    std::lock_guard<std::mutex> lk(g_stream_mu);
    auto it = g_playing.find(s);
    playing = it != g_playing.end();
    if (playing) it->second = g_active_ns;
  }
  ALint state = 0;
  if (playing) alGetSourcei(s, AL_SOURCE_STATE, &state);
  if (state == AL_STOPPED) {  // ran dry while the game wasn't looking: carry on
    if (g_trace) log_info("OpenAL: source %u ran out of sound; starting it again", s);
    alSourcePlay(s);
  }
  g_queued += (u32)n;
  tally();
}
void alSourceUnqueueBuffers_(GuestCall& c) {
  const ALuint s = c.i();
  const ALsizei n = (ALsizei)c.i();
  u8* out = gp(c, c.i());
  if (n <= 0 || !out) return;
  static thread_local std::vector<ALuint> v;
  v.resize((size_t)n);
  alSourceUnqueueBuffers(s, n, v.data());
  if (g_trace) log_info("OpenAL: alSourceUnqueueBuffers(%u, %d) first %u", s, (int)n, v[0]);
  for (ALsizei k = 0; k < n; k++) wr32(out + 4 * k, v[(size_t)k]);
}

// ---- the listener ---------------------------------------------------------------

void alListenerf_(GuestCall& c) {
  const ALenum p = (ALenum)c.i();
  const float v = c.f();
  if (g_trace) log_info("OpenAL: alListenerf(0x%x, %g)", (unsigned)p, v);
  alListenerf(p, v);
}
void alListener3f_(GuestCall& c) {
  const ALenum p = (ALenum)c.i();
  const float x = c.f(), y = c.f(), z = c.f();
  if (g_trace) log_info("OpenAL: alListener3f(0x%x, %g %g %g)", (unsigned)p, x, y, z);
  alListener3f(p, x, y, z);
}
void alListenerfv_(GuestCall& c) {
  const ALenum p = (ALenum)c.i();
  const u8* in = gp(c, c.i());
  ALfloat v[6] = {};
  for (int k = 0; in && k < count_of(p); k++) v[k] = rdf(in + 4 * k);
  if (g_trace) log_info("OpenAL: alListenerfv(0x%x, %g %g %g %g %g %g)", (unsigned)p, v[0], v[1], v[2], v[3], v[4], v[5]);
  alListenerfv(p, v);
}
void alGetListenerf_(GuestCall& c) {
  const ALenum p = (ALenum)c.i();
  u8* out = gp(c, c.i());
  ALfloat v = 0;
  alGetListenerf(p, &v);
  if (out) wrf(out, v);
}
void alGetListener3f_(GuestCall& c) {
  const ALenum p = (ALenum)c.i();
  u8* x = gp(c, c.i());
  u8* y = gp(c, c.i());
  u8* z = gp(c, c.i());
  ALfloat a = 0, b = 0, d = 0;
  alGetListener3f(p, &a, &b, &d);
  if (x) wrf(x, a);
  if (y) wrf(y, b);
  if (z) wrf(z, d);
}
void alGetListeneri_(GuestCall& c) {
  const ALenum p = (ALenum)c.i();
  u8* out = gp(c, c.i());
  ALint v = 0;
  alGetListeneri(p, &v);
  if (out) wr32(out, (u32)v);
}

// For working on cocaine: COCAINE_AL_TRACE=1 logs each call that leaves an
// OpenAL error, and the source calls with their arguments.
void after(const char* name) {
  tick_active();
  if (!g_trace) return;
  const ALenum e = alGetError();
  if (e == AL_NO_ERROR) return;
  log_info("OpenAL: %s left error 0x%x", name, (unsigned)e);
  ALenum none = AL_NO_ERROR;
  g_pending_error.compare_exchange_strong(none, e);  // still the guest's to read
}

#define H(name) {#name, [](GuestCall& c) { name##_(c); after(#name); }}
const HookSpec kAL[] = {
    H(alcOpenDevice), H(alcCloseDevice), H(alcCreateContext), H(alcMakeContextCurrent), H(alcGetCurrentContext),
    H(alcDestroyContext), H(alGetError), H(alEnable), H(alDistanceModel), H(alGetString), H(alGenBuffers),
    H(alDeleteBuffers), H(alGenSources), H(alDeleteSources), H(alIsBuffer), H(alBufferData), H(alGetBufferi),
    H(alSourcePlay), H(alSourceStop), H(alSourcef), H(alSource3f), H(alSourcefv), H(alSourcei), H(alGetSourcef),
    H(alGetSourcefv), H(alGetSourcei), H(alSourceQueueBuffers), H(alSourceUnqueueBuffers), H(alListenerf),
    H(alListener3f), H(alListenerfv), H(alGetListenerf), H(alGetListener3f), H(alGetListeneri),
};
#undef H

const HookLibrary kLibraries[] = {
    {"/OpenAL.framework/Versions/A/OpenAL", "al", kAL, sizeof kAL / sizeof kAL[0], true},
};

}  // namespace

const HookLibrary* al_hook_libraries(size_t& n) {
  n = sizeof kLibraries / sizeof kLibraries[0];
  return kLibraries;
}
