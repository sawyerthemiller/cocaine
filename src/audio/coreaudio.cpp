// Core Audio's hardware layer for guests, done with one stand-in output device
// that plays through this Mac's own speakers.
//
// Everything a Mac program does with sound ends up at Core Audio's hardware
// layer (CoreAudio.framework's AudioHardware..., AudioDevice..., AudioStream...
// and AudioObject... functions): the output units, the Sound Manager,
// QuickTime, OpenAL's Core Audio back end. Leopard's own hardware layer needs
// its drivers and its daemon, which cocaine doesn't have, so it finds no
// devices. cocaine takes these functions over (see kernel/hooks.h) and offers
// one device of its own, "cocaine output": stereo, 32-bit float, 44.1 kHz.
// Everything Apple built on top of the hardware layer then runs, as guest
// code, unchanged.
//
// When the device is started, a guest thread (made with the guest's own
// pthread_create, so it is a proper thread of the guest's) runs the guest's
// IO procedures one buffer at a time and puts what they write into a ring,
// from which a native output unit plays. The ring is kept a few buffers
// ahead, which is what paces the guest.
//
// Values go in and out of guest memory in the guest's byte order, so the same
// code serves PowerPC and Intel guests.

#include <AudioUnit/AudioUnit.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "kernel/hooks.h"
#include "kernel/process.h"
#include "audio/midi.h"

namespace {

constexpr u32 fourcc(const char (&s)[5]) {
  return (u32)(u8)s[0] << 24 | (u32)(u8)s[1] << 16 | (u32)(u8)s[2] << 8 | (u8)s[3];
}

// The objects there are.
constexpr u32 kSystem = 1, kDevice = 0x100, kStream = 0x200;
constexpr double kRate = 44100.0;
constexpr u32 kChannels = 2;
constexpr u32 kRingFrames = 16384;
static_assert((kRingFrames & (kRingFrames - 1)) == 0);

// Errors, as Core Audio spells them.
constexpr u32 kNoErr = 0;
const u32 kUnknownProperty = fourcc("who?");
const u32 kBadObject = fourcc("!obj");
const u32 kIllegal = fourcc("nope");

// ---- the device --------------------------------------------------------------

struct Proc {
  u32 id = 0, fn = 0, client = 0;
  bool started = false;
};

// A guest's linear PCM format, as an AudioStreamBasicDescription says it.
struct Format {
  double rate = kRate;
  u32 flags = 1 | 8;  // float, packed (and big-endian on PowerPC, set when made)
  u32 bytes_per_frame = 4 * kChannels;
  u32 channels = kChannels;
  u32 bits = 32;
  bool big() const { return flags & 2; }
  bool interleaved() const { return !(flags & 0x20); }
};

// An output unit (AUHAL, default or system output), done here rather than
// by the guest's Component Manager, which finds no components under cocaine.
// It plays what the guest's render callback gives, through the device.
struct Unit {
  u32 handle = 0;
  u32 render = 0, refcon = 0;
  u32 source = 0, source_bus = 0, render_api = 0;
  bool started = false;
  Format format;
  double phase = 0;  // for resampling: how far past the last input frame the next output frame is
  float last[2] = {};
};

struct Device {
  std::mutex mu;
  std::condition_variable cv;
  std::vector<Proc> procs;
  std::vector<Unit> units;
  u32 next_id = 0x00A0D000;
  u32 frames = 512;      // buffer frame size
  u32 hog = 0xFFFFFFFFu;  // nobody
  float volume = 1.0f;
  bool worker = false, running = false, stopping = false;

  // The ring between the guest's IO procedures and the native output.
  std::vector<float> ring = std::vector<float>((size_t)kRingFrames * kChannels);
  size_t head = 0, tail = 0;  // in frames, ever-increasing
  std::atomic<u64> played{0};
  u64 start_host = 0;
  u64 rendered = 0;
  // Where the device's sample count meets the host clock: sample
  // `anchor_sample` is heard at host time `anchor_host`. Moved every turn,
  // since a late turn leaves a gap that the sample count doesn't see.
  double anchor_sample = 0;
  u64 anchor_host = 0;

  AudioComponentInstance unit = nullptr;
};
Device g_dev;

// The guest's host clock, which ticks in nanoseconds (see the comm page's
// kAbsoluteTime): what its AudioTimeStamps' host times are measured in.
u64 guest_now() { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
double ticks_per_frame() { return 1e9 / kRate; }

// The native side: plays what is in the ring, and silence when it runs dry.
OSStatus render(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 frames,
                AudioBufferList* out) {
  float* dst = (float*)out->mBuffers[0].mData;
  {
    std::lock_guard<std::mutex> lk(g_dev.mu);
    for (UInt32 f = 0; f < frames; f++) {
      if (g_dev.tail < g_dev.head) {
        const size_t at = (g_dev.tail & (kRingFrames - 1)) * kChannels;
        dst[2 * f] = g_dev.ring[at] * g_dev.volume;
        dst[2 * f + 1] = g_dev.ring[at + 1] * g_dev.volume;
        g_dev.tail++;
      } else {
        dst[2 * f] = dst[2 * f + 1] = 0;
      }
    }
  }
  g_dev.played += frames;
  g_dev.cv.notify_all();
  return 0;
}

bool start_native() {
  if (g_dev.unit) return true;
  AudioComponentDescription desc = {kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput,
                                    kAudioUnitManufacturer_Apple, 0, 0};
  AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
  if (!comp || AudioComponentInstanceNew(comp, &g_dev.unit) != 0) {
    log_warn("Core Audio: this Mac has no default output to play through");
    g_dev.unit = nullptr;
    return false;
  }
  AudioStreamBasicDescription f = {};
  f.mSampleRate = kRate;
  f.mFormatID = kAudioFormatLinearPCM;
  f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
  f.mBytesPerPacket = f.mBytesPerFrame = 4 * kChannels;
  f.mFramesPerPacket = 1;
  f.mChannelsPerFrame = kChannels;
  f.mBitsPerChannel = 32;
  AudioUnitSetProperty(g_dev.unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
  AURenderCallbackStruct cb = {render, nullptr};
  AudioUnitSetProperty(g_dev.unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
  if (AudioUnitInitialize(g_dev.unit) != 0 || AudioOutputUnitStart(g_dev.unit) != 0) {
    log_warn("Core Audio: this Mac's output would not start");
    AudioComponentInstanceDispose(g_dev.unit);
    g_dev.unit = nullptr;
    return false;
  }
  return true;
}

void stop_native() {
  if (!g_dev.unit) return;
  AudioOutputUnitStop(g_dev.unit);
  AudioUnitUninitialize(g_dev.unit);
  AudioComponentInstanceDispose(g_dev.unit);
  g_dev.unit = nullptr;
}

// ---- values in guest memory --------------------------------------------------

// Bytes in the guest's order, built up for a property's value.
struct Bytes {
  bool be;
  std::vector<u8> b;
  explicit Bytes(bool big) : be(big) {}
  void u32_(u32 v) {
    for (int k = 0; k < 4; k++) b.push_back((u8)(be ? v >> (24 - 8 * k) : v >> (8 * k)));
  }
  void u64_(u64 v) {
    if (be) {
      u32_((u32)(v >> 32));
      u32_((u32)v);
    } else {
      u32_((u32)v);
      u32_((u32)(v >> 32));
    }
  }
  void f64(double d) {
    u64 v;
    memcpy(&v, &d, 8);
    u64_(v);
  }
  void f32(float f) {
    u32 v;
    memcpy(&v, &f, 4);
    u32_(v);
  }
  void cstr(const char* s) { b.insert(b.end(), s, s + strlen(s) + 1); }
};

void format(Bytes& o) {  // AudioStreamBasicDescription of the device's one format
  o.f64(kRate);
  o.u32_(fourcc("lpcm"));
  o.u32_(1 | 8 | (o.be ? 2 : 0));  // float, packed, and big-endian on PowerPC
  o.u32_(4 * kChannels);
  o.u32_(1);
  o.u32_(4 * kChannels);
  o.u32_(kChannels);
  o.u32_(32);
  o.u32_(0);
}

void write_time(GuestMemory& m, u32 at, double sample, u64 host) {  // AudioTimeStamp
  u64 bits;
  memcpy(&bits, &sample, 8);
  m.w64(at, bits);
  m.w64(at + 8, host);
  const double one = 1.0;
  memcpy(&bits, &one, 8);
  m.w64(at + 16, bits);
  m.w64(at + 24, 0);
  for (u32 k = 32; k < 56; k += 4) m.w32(at + k, 0);
  m.w32(at + 56, 1 | 2 | 4);  // sample time, host time and rate valid
  m.w32(at + 60, 0);
}

// A CFString in the guest, made by the guest's CoreFoundation, for the caller
// to release. The text it is made from is put in guest memory once per
// string: scratch memory is never given back.
u32 guest_cfstring(GuestCall& c, const char* s) {
  static std::mutex mu;
  static std::vector<std::pair<std::string, u32>> texts;
  static u32 create = 0;
  u32 text = 0;
  {
    std::lock_guard<std::mutex> lk(mu);
    if (!create) create = c.symbol("CFStringCreateWithCString");
    for (const auto& [str, at] : texts) {
      if (str == s) text = at;
    }
    if (!text && (text = c.scratch((u32)strlen(s) + 1))) {
      memcpy(c.mem.host(text), s, strlen(s) + 1);
      texts.push_back({s, text});
    }
  }
  if (!create || !text) return 0;
  return c.call(create, {0, text, 0x08000100});  // UTF-8
}

std::mutex g_said_mu;
std::set<std::pair<u32, u32>> g_said;
void unknown(u32 obj, u32 sel, u32 scope) {
  std::lock_guard<std::mutex> lk(g_said_mu);
  if (!g_said.insert({obj, sel}).second) return;
  log_info("Core Audio: no property '%c%c%c%c' (scope '%c%c%c%c') on object 0x%x", (char)(sel >> 24),
           (char)(sel >> 16), (char)(sel >> 8), (char)sel, (char)(scope >> 24), (char)(scope >> 16),
           (char)(scope >> 8), (char)scope, obj);
}

// A property's value. False if there is no such property. With `sizing`, only
// the value's size matters: no CFString is made, since nobody would release it.
bool property(GuestCall& c, u32 obj, u32 sel, u32 scope, Bytes& o, bool sizing = false) {
  const bool input = scope == fourcc("inpt");
  const auto cfstring = [&](const char* s) { return sizing ? 0 : guest_cfstring(c, s); };
  if (obj == kSystem) {
    switch (sel) {
      case fourcc("dev#"): o.u32_(kDevice); return true;
      case fourcc("dOut"): case fourcc("sOut"): o.u32_(kDevice); return true;
      case fourcc("dIn "): o.u32_(0); return true;
      case fourcc("rnlp"): o.u32_(0); return true;
      case fourcc("inot"): o.u32_(0); return true;
      case fourcc("mast"): o.u32_(1); return true;
      case fourcc("slep"): o.u32_(1); return true;
      case fourcc("unot"): o.u32_(0); return true;
      case fourcc("plg#"): return true;
      case fourcc("duid"): return false;  // done by the caller (it fills in a translation)
      case fourcc("clas"): o.u32_(fourcc("asys")); return true;
      case fourcc("ownd"): o.u32_(kDevice); return true;
      default: return false;
    }
  }
  if (obj == kDevice) {
    switch (sel) {
      case fourcc("name"): o.cstr("cocaine output"); return true;
      case fourcc("makr"): o.cstr("cocaine"); return true;
      case fourcc("lnam"): o.u32_(cfstring("cocaine output")); return true;
      case fourcc("lmak"): o.u32_(cfstring("cocaine")); return true;
      case fourcc("uid "): o.u32_(cfstring("cocaine-output")); return true;
      case fourcc("muid"): o.u32_(cfstring("cocaine-output-model")); return true;
      case fourcc("clas"): o.u32_(fourcc("adev")); return true;
      case fourcc("bcls"): o.u32_(fourcc("aobj")); return true;
      case fourcc("stdv"): o.u32_(kSystem); return true;
      case fourcc("livn"): o.u32_(1); return true;
      case fourcc("goin"): case fourcc("gone"): o.u32_(g_dev.running ? 1 : 0); return true;
      case fourcc("hidn"): o.u32_(0); return true;
      case fourcc("dflt"): case fourcc("sflt"): o.u32_(input ? 0 : 1); return true;
      case fourcc("stm#"): case fourcc("ownd"):
        if (!input) o.u32_(kStream);
        return true;
      case fourcc("nsrt"): o.f64(kRate); return true;
      case fourcc("nsr#"): o.f64(kRate); o.f64(kRate); return true;
      case fourcc("fsiz"): o.u32_(g_dev.frames); return true;
      case fourcc("fsz#"): o.f64(14); o.f64(4096); return true;
      case fourcc("ltnc"): case fourcc("saft"): o.u32_(0); return true;
      case fourcc("slay"):  // AudioBufferList: one interleaved stereo buffer out, nothing in
        if (input) {
          o.u32_(0);
        } else {
          o.u32_(1);
          o.u32_(kChannels);
          o.u32_(g_dev.frames * 4 * kChannels);
          o.u32_(0);
        }
        return true;
      case fourcc("sfmt"): case fourcc("sfm#"):
        if (input) return false;
        format(o);
        return true;
      case fourcc("oink"): o.u32_(g_dev.hog); return true;
      case fourcc("mix?"): o.u32_(1); return true;
      case fourcc("dch2"): o.u32_(1); o.u32_(2); return true;
      case fourcc("srnd"):  // AudioChannelLayout: left and right, by description
        o.u32_(0);
        o.u32_(0);
        o.u32_(2);
        for (u32 label : {1u, 2u}) {
          o.u32_(label);
          o.u32_(0);
          o.f32(0);
          o.f32(0);
          o.f32(0);
        }
        return true;
      case fourcc("tran"): o.u32_(fourcc("bltn")); return true;
      case fourcc("clkd"): o.u32_(0); return true;
      case fourcc("akin"): o.u32_(kDevice); return true;
      case fourcc("volm"): o.f32(g_dev.volume); return true;
      case fourcc("mute"): o.u32_(0); return true;
      default: return false;
    }
  }
  if (obj == kStream) {
    switch (sel) {
      case fourcc("clas"): o.u32_(fourcc("astr")); return true;
      case fourcc("stdv"): o.u32_(kDevice); return true;
      case fourcc("sdir"): o.u32_(0); return true;
      case fourcc("term"): o.u32_(fourcc("spkr")); return true;
      case fourcc("schn"): o.u32_(1); return true;
      case fourcc("ltnc"): o.u32_(0); return true;
      case fourcc("sfmt"): case fourcc("pft "): case fourcc("sfm#"): case fourcc("pft#"): format(o); return true;
      case fourcc("sfma"): case fourcc("pfta"):  // AudioStreamRangedDescription
        format(o);
        o.f64(kRate);
        o.f64(kRate);
        return true;
      default: return false;
    }
  }
  return false;
}

u32 settable(u32 obj, u32 sel) {
  if (obj == kSystem) return sel == fourcc("rnlp") || sel == fourcc("unot") || sel == fourcc("slep");
  if (obj == kDevice)
    return sel == fourcc("fsiz") || sel == fourcc("oink") || sel == fourcc("nsrt") || sel == fourcc("sfmt") ||
           sel == fourcc("volm") || sel == fourcc("suse") || sel == fourcc("mute");
  if (obj == kStream) return sel == fourcc("sfmt") || sel == fourcc("pft ");
  return 0;
}

u32 set_property(GuestCall& c, u32 obj, u32 sel, u32 size, u32 data) {
  if (!settable(obj, sel)) {
    unknown(obj, sel, 0);
    return kUnknownProperty;
  }
  std::lock_guard<std::mutex> lk(g_dev.mu);
  if (obj == kDevice && sel == fourcc("fsiz") && size >= 4) {
    const u32 v = c.mem.r32(data);
    g_dev.frames = v < 64 ? 64 : v > 4096 ? 4096 : v;
  } else if (obj == kDevice && sel == fourcc("oink") && size >= 4) {
    g_dev.hog = c.mem.r32(data);
  } else if (obj == kDevice && sel == fourcc("volm") && size >= 4) {
    const u32 bits = c.mem.r32(data);
    memcpy(&g_dev.volume, &bits, 4);
  }
  // The rest (sample rate, formats) stay as they are: the guest finds that out when it asks again.
  return kNoErr;
}

// What an old-style call's scope means.
u32 scope_of(bool input) { return input ? fourcc("inpt") : fourcc("outp"); }

// Copies a value out in the old calls' way: as much as fits, and the size.
u32 give(GuestCall& c, const Bytes& o, u32 io_size, u32 data) {
  const u32 room = io_size ? c.mem.r32(io_size) : 0;
  const u32 n = (u32)o.b.size() < room ? (u32)o.b.size() : room;
  if (data && n) memcpy(c.mem.host(data), o.b.data(), n);
  if (io_size) c.mem.w32(io_size, n);
  return kNoErr;
}

u32 get(GuestCall& c, u32 obj, u32 sel, u32 scope, u32 io_size, u32 data) {
  if (obj != kSystem && obj != kDevice && obj != kStream) return kBadObject;
  if (obj == kSystem && sel == fourcc("duid") && data) {  // AudioValueTranslation: any name means this device
    const u32 out = c.mem.r32(data + 8);
    if (out) c.mem.w32(out, kDevice);
    if (io_size) c.mem.w32(io_size, 16);
    return kNoErr;
  }
  if (obj == kDevice && sel == fourcc("suse") && data) {  // which streams an IO procedure uses: all
    c.mem.w32(data + 4, 1);
    c.mem.w32(data + 8, 1);
    if (io_size) c.mem.w32(io_size, 12);
    return kNoErr;
  }
  Bytes o(c.powerpc);
  if (!property(c, obj, sel, scope, o)) {
    unknown(obj, sel, scope);
    return kUnknownProperty;
  }
  return give(c, o, io_size, data);
}

u32 info(GuestCall& c, u32 obj, u32 sel, u32 scope, u32 out_size, u32 out_writable) {
  if (obj != kSystem && obj != kDevice && obj != kStream) return kBadObject;
  Bytes o(c.powerpc);
  u32 size = 0;
  if (obj == kSystem && sel == fourcc("duid")) {
    size = 16;
  } else if (obj == kDevice && sel == fourcc("suse")) {
    size = 12;
  } else if (!property(c, obj, sel, scope, o, true)) {
    unknown(obj, sel, scope);
    return kUnknownProperty;
  } else {
    size = (u32)o.b.size();
  }
  if (out_size) c.mem.w32(out_size, size);
  if (out_writable) c.mem.w8(out_writable, settable(obj, sel) ? 1 : 0);
  return kNoErr;
}

// ---- running the guest's IO procedures ----------------------------------------

void worker_main(GuestCall& c);

// Starts the worker unless one is still going. One that was told to stop
// and hasn't yet noticed carries on instead (update_running took the stop
// back): `worker` is cleared, under the lock, at the moment a worker decides
// to finish.
void start_worker(GuestCall& c) {
  static u32 stub = 0, create = 0, tid = 0;
  if (!stub) {
    stub = native_function(c.process, "cocaine_audio_worker", worker_main);
    create = c.symbol("pthread_create");
    tid = c.scratch(16);
  }
  if (!stub || !create || !tid) {
    log_warn("Core Audio: can't start the thread that runs the guest's sound");
    return;
  }
  {
    std::lock_guard<std::mutex> lk(g_dev.mu);
    if (g_dev.worker) return;
    g_dev.worker = true;
  }
  if (c.call(create, {tid, 0, stub, kDevice}) != 0) {
    log_warn("Core Audio: the guest would not start the thread that runs its sound");
    std::lock_guard<std::mutex> lk(g_dev.mu);
    g_dev.worker = false;
  }
}

// One sample of a guest format, as a float.
float sample(GuestMemory& m, u32 at, const Format& f) {
  const u8* p = m.host(at);
  if (f.bits == 8) return (f.flags & 4) ? (s8)p[0] / 128.0f : (p[0] - 128) / 128.0f;
  if (f.bits == 16) {
    const s16 v = (s16)(f.big() ? (p[0] << 8 | p[1]) : (p[1] << 8 | p[0]));
    return v / 32768.0f;
  }
  const u32 w = f.big() ? (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]
                        : (u32)p[3] << 24 | (u32)p[2] << 16 | (u32)p[1] << 8 | p[0];
  if (f.flags & 1) {
    float v;
    memcpy(&v, &w, 4);
    return v;
  }
  return (s32)w / 2147483648.0f;
}

// The worker's guest buffers: the buffer list has room for this many
// buffers, and the data for this many bytes.
constexpr u32 kMaxBuffers = 8, kDataBytes = 4096 * 4 * 8;

// Asks an output unit's render callback for enough frames, in its own
// format and rate, to make `frames` at the device's, and adds them to `mix`.
void pull_unit(GuestCall& c, Unit& u, u32 frames, u32 ts, u32 list, u32 data, u32 flags, std::vector<float>& mix) {
  GuestMemory& m = c.mem;
  const Format& f = u.format;
  const double step = f.rate / kRate;  // input frames per output frame
  // Output frame o falls at input position phase + step * o, between input
  // frames floor(pos) - 1 and floor(pos) (frame -1 being the last one of the
  // previous call): so this many are needed. With equal rates, exactly `frames`.
  u32 need = (u32)(u.phase + step * (frames - 1)) + 1;
  const u32 bytes_per_sample = f.bits / 8;
  // However many channels the guest's format has, only as many buffers and
  // frames as the worker's guest memory holds are asked for.
  const u32 nbuf = f.interleaved() ? 1 : std::min(f.channels, kMaxBuffers);
  const u32 frame_bytes = f.interleaved() ? f.bytes_per_frame : bytes_per_sample * nbuf;
  const u32 fit = frame_bytes ? (kDataBytes - 16) / frame_bytes : 0;
  need = std::min({need, 4096u, fit});
  if (!need) return;
  const u32 per_buffer = need * (f.interleaved() ? f.bytes_per_frame : bytes_per_sample);
  m.w32(list, nbuf);
  for (u32 k = 0; k < nbuf; k++) {
    m.w32(list + 4 + 12 * k, f.interleaved() ? f.channels : 1);
    m.w32(list + 8 + 12 * k, per_buffer);
    m.w32(list + 12 + 12 * k, data + k * per_buffer);
  }
  memset(m.host(data), 0, (size_t)per_buffer * nbuf);
  m.w32(flags, 0);
  if (u.render) {
    c.call(u.render, {u.refcon, flags, ts, 0, need, list});
  } else {
    const u32 status = c.call(u.render_api, {u.source, flags, ts, u.source_bus, need, list});
    if (status) {
      static std::mutex error_mu;
      static std::set<u32> errors;
      std::lock_guard<std::mutex> lk(error_mu);
      if (errors.insert(status).second) log_warn("Core Audio: connected unit render failed (%d)", (s32)status);
      return;
    }
  }
  // Input frame k, channel ch, as a float. Past the frames asked for (a
  // rate far enough above the device's that `need` was held down), the
  // last one again.
  auto in = [&](u32 k, u32 ch) -> float {
    if (k >= need) k = need - 1;
    if (ch >= f.channels) ch = f.channels - 1;
    if (!f.interleaved() && ch >= nbuf) ch = nbuf - 1;
    if (f.interleaved()) return sample(m, data + k * f.bytes_per_frame + ch * bytes_per_sample, f);
    return sample(m, data + ch * per_buffer + k * bytes_per_sample, f);
  };
  double pos = u.phase;  // in input frames, from the frame before the first (u.last)
  for (u32 o = 0; o < frames; o++) {
    const u32 k = (u32)pos;
    const double t = pos - k;
    for (u32 ch = 0; ch < kChannels; ch++) {
      const float a = k == 0 ? u.last[ch] : in(k - 1, ch);
      const float b = k < need ? in(k, ch) : a;
      mix[2 * o + ch] += (float)(a + (b - a) * t);
    }
    pos += step;
  }
  // What was left over carries into the next call.
  const u32 used = (u32)pos;
  u.phase = pos - used;
  for (u32 ch = 0; ch < kChannels; ch++) u.last[ch] = used ? in(used - 1 < need ? used - 1 : need - 1, ch) : u.last[ch];
}

// The guest thread's body: until the device stops, keep the ring a few
// buffers ahead of the speakers.
void worker_main(GuestCall& c) {
  GuestMemory& m = c.mem;
  // Made once and kept for every worker after: scratch memory is never given
  // back, and only one worker runs at a time.
  static u32 now_ts, in_list, in_ts, out_list, out_ts, data, flags;
  if (!data) {
    now_ts = c.scratch(64), in_list = c.scratch(16), in_ts = c.scratch(64), out_list = c.scratch(4 + 12 * kMaxBuffers),
    out_ts = c.scratch(64), flags = c.scratch(16), data = c.scratch(kDataBytes);
  }
  std::vector<float> mix;
  std::vector<Proc> procs;
  std::vector<Unit> units;
  for (;;) {
    procs.clear();
    units.clear();
    u32 frames;
    size_t queued = 0;
    {
      std::unique_lock<std::mutex> lk(g_dev.mu);
      const auto ahead = [&] { return g_dev.head - g_dev.tail; };
      // A few buffers queued rides out a late turn and the host's own larger
      // slices; the times the guest is told account for them.
      g_dev.cv.wait_for(lk, std::chrono::milliseconds(50), [&] {
        return g_dev.stopping || ahead() + g_dev.frames <= 3 * g_dev.frames;
      });
      if (g_dev.stopping || !data) {
        g_dev.worker = false;  // from here a new start makes a new worker (see start_worker)
        break;
      }
      if (ahead() + g_dev.frames > 3 * g_dev.frames) continue;
      queued = ahead();
      for (const Proc& p : g_dev.procs) {
        if (p.started) procs.push_back(p);
      }
      for (const Unit& u : g_dev.units) {
        if (u.started && (u.render || u.source)) units.push_back(u);
      }
      frames = g_dev.frames;
    }
    // With nothing to run (an output unit started before it was given its
    // render callback), the device plays silence, which keeps the ring
    // pacing this loop rather than it spinning.
    // When this buffer will be heard: after what is queued ahead of it.
    const u64 host = guest_now() + (u64)(queued * ticks_per_frame());
    {
      std::lock_guard<std::mutex> lk(g_dev.mu);
      g_dev.anchor_sample = (double)g_dev.rendered;
      g_dev.anchor_host = host;
    }
    write_time(m, now_ts, (double)g_dev.played, guest_now());
    write_time(m, in_ts, (double)g_dev.rendered, host);
    write_time(m, out_ts, (double)g_dev.rendered, host);
    m.w32(in_list, 0);
    mix.assign((size_t)frames * kChannels, 0.0f);
    for (const Proc& p : procs) {
      m.w32(out_list, 1);
      m.w32(out_list + 4, kChannels);
      m.w32(out_list + 8, frames * 4 * kChannels);
      m.w32(out_list + 12, data);
      memset(m.host(data), 0, (size_t)frames * 4 * kChannels);
      c.call(p.fn, {kDevice, now_ts, in_list, in_ts, out_list, out_ts, p.client});
      for (size_t k = 0; k < (size_t)frames * kChannels; k++) {
        const u32 bits = m.r32(data + 4 * (u32)k);
        float v;
        memcpy(&v, &bits, 4);
        mix[k] += v;
      }
    }
    for (Unit& u : units) {
      if (u.source) write_time(m, out_ts, (double)g_dev.rendered * u.format.rate / kRate, host);
      else write_time(m, out_ts, (double)g_dev.rendered, host);
      pull_unit(c, u, frames, out_ts, out_list, data, flags, mix);
      std::lock_guard<std::mutex> lk(g_dev.mu);
      for (Unit& kept : g_dev.units) {
        if (kept.handle == u.handle) {
          kept.phase = u.phase;
          kept.last[0] = u.last[0];
          kept.last[1] = u.last[1];
        }
      }
    }
    {
      std::lock_guard<std::mutex> lk(g_dev.mu);
      for (u32 f = 0; f < frames; f++) {
        const size_t at = (g_dev.head & (kRingFrames - 1)) * kChannels;
        g_dev.ring[at] = mix[2 * f];
        g_dev.ring[at + 1] = mix[2 * f + 1];
        g_dev.head++;
      }
      g_dev.rendered += frames;
    }
    static float peak = 0;
    for (float v : mix) peak = std::max(peak, std::fabs(v));
    // COCAINE_AUDIO_DUMP=file, for working on cocaine: what the guest plays, as raw stereo floats.
    static FILE* dump = getenv("COCAINE_AUDIO_DUMP") ? fopen(getenv("COCAINE_AUDIO_DUMP"), "wb") : nullptr;
    if (dump) {
      fwrite(mix.data(), sizeof(float), (size_t)frames * kChannels, dump);
      fflush(dump);
    }
    static u64 last_note = 0;
    const u64 now_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (now_ns - last_note > 2000000000ull) {  // for -v: is the guest keeping up?
      last_note = now_ns;
      log_info("Core Audio: %llu frames made, %llu played, %u a turn, loudest %.3f", (unsigned long long)g_dev.rendered,
               (unsigned long long)g_dev.played.load(), frames, peak);
      peak = 0;
    }
  }
  c.ret(0);
}

void update_running(GuestCall& c) {
  bool any = false;
  {
    std::lock_guard<std::mutex> lk(g_dev.mu);
    for (const Proc& p : g_dev.procs) any = any || p.started;
    for (const Unit& u : g_dev.units) any = any || u.started;
    if (any == g_dev.running) return;
    g_dev.running = any;
    if (any) {
      // A stop just before, which the worker may not have seen yet, is taken
      // back: that worker carries on, and start_worker leaves it be.
      g_dev.stopping = false;
      g_dev.head = g_dev.tail = 0;
      g_dev.rendered = 0;
      g_dev.played = 0;
      g_dev.start_host = g_dev.anchor_host = guest_now();
      g_dev.anchor_sample = 0;
    } else {
      g_dev.stopping = true;
    }
  }
  g_dev.cv.notify_all();
  if (any) {
    start_native();
    start_worker(c);
    log_info("Core Audio: the guest started playing");
  } else {
    stop_native();
  }
}

// ---- the functions taken over --------------------------------------------------

void AudioHardwareGetPropertyInfo_(GuestCall& c) {
  const u32 sel = c.i(), size = c.i(), writable = c.i();
  c.ret(info(c, kSystem, sel, fourcc("glob"), size, writable));
}
void AudioHardwareGetProperty_(GuestCall& c) {
  const u32 sel = c.i(), size = c.i(), data = c.i();
  c.ret(get(c, kSystem, sel, fourcc("glob"), size, data));
}
void AudioHardwareSetProperty_(GuestCall& c) {
  const u32 sel = c.i(), size = c.i(), data = c.i();
  c.ret(set_property(c, kSystem, sel, size, data));
}
void ok(GuestCall& c) { c.ret(kNoErr); }

void AudioDeviceGetPropertyInfo_(GuestCall& c) {
  const u32 dev = c.i();
  c.i();  // channel
  const bool input = (c.i() & 0xFF) != 0;
  const u32 sel = c.i(), size = c.i(), writable = c.i();
  c.ret(dev == kDevice ? info(c, kDevice, sel, scope_of(input), size, writable) : fourcc("!dev"));
}
void AudioDeviceGetProperty_(GuestCall& c) {
  const u32 dev = c.i();
  c.i();
  const bool input = (c.i() & 0xFF) != 0;
  const u32 sel = c.i(), size = c.i(), data = c.i();
  c.ret(dev == kDevice ? get(c, kDevice, sel, scope_of(input), size, data) : fourcc("!dev"));
}
void AudioDeviceSetProperty_(GuestCall& c) {
  const u32 dev = c.i();
  c.i();  // when
  c.i();  // channel
  c.i();  // input
  const u32 sel = c.i(), size = c.i(), data = c.i();
  c.ret(dev == kDevice ? set_property(c, kDevice, sel, size, data) : fourcc("!dev"));
}
void AudioStreamGetPropertyInfo_(GuestCall& c) {
  const u32 st = c.i();
  c.i();
  const u32 sel = c.i(), size = c.i(), writable = c.i();
  c.ret(info(c, st, sel, fourcc("glob"), size, writable));
}
void AudioStreamGetProperty_(GuestCall& c) {
  const u32 st = c.i();
  c.i();
  const u32 sel = c.i(), size = c.i(), data = c.i();
  c.ret(get(c, st, sel, fourcc("glob"), size, data));
}
void AudioStreamSetProperty_(GuestCall& c) {
  const u32 st = c.i();
  c.i();  // when
  c.i();  // channel
  const u32 sel = c.i(), size = c.i(), data = c.i();
  c.ret(set_property(c, st, sel, size, data));
}

// The AudioObject calls take the address as a structure: selector, scope, element.
void AudioObjectHasProperty_(GuestCall& c) {
  const u32 obj = c.i(), addr = c.i();
  Bytes o(c.powerpc);
  const u32 sel = c.mem.r32(addr), scope = c.mem.r32(addr + 4);
  const bool has = (obj == kSystem && sel == fourcc("duid")) || (obj == kDevice && sel == fourcc("suse")) ||
                   property(c, obj, sel, scope, o, true);
  if (!has) unknown(obj, sel, scope);
  c.ret(has ? 1 : 0);
}
void AudioObjectIsPropertySettable_(GuestCall& c) {
  const u32 obj = c.i(), addr = c.i(), out = c.i();
  if (out) c.mem.w8(out, settable(obj, c.mem.r32(addr)) ? 1 : 0);
  c.ret(kNoErr);
}
void AudioObjectGetPropertyDataSize_(GuestCall& c) {
  const u32 obj = c.i(), addr = c.i();
  c.i();  // qualifier size
  c.i();  // qualifier
  const u32 out = c.i();
  c.ret(info(c, obj, c.mem.r32(addr), c.mem.r32(addr + 4), out, 0));
}
void AudioObjectGetPropertyData_(GuestCall& c) {
  const u32 obj = c.i(), addr = c.i();
  const u32 qsize = c.i(), qual = c.i(), size = c.i(), data = c.i();
  const u32 sel = c.mem.r32(addr);
  if (obj == kSystem && sel == fourcc("duid") && data) {  // newer form: the qualifier is the UID
    (void)qsize;
    (void)qual;
  }
  c.ret(get(c, obj, sel, c.mem.r32(addr + 4), size, data));
}
void AudioObjectSetPropertyData_(GuestCall& c) {
  const u32 obj = c.i(), addr = c.i();
  c.i();
  c.i();
  const u32 size = c.i(), data = c.i();
  c.ret(set_property(c, obj, c.mem.r32(addr), size, data));
}

void AudioDeviceAddIOProc_(GuestCall& c) {
  const u32 dev = c.i(), fn = c.i(), client = c.i();
  std::lock_guard<std::mutex> lk(g_dev.mu);
  if (dev != kDevice) return c.ret(fourcc("!dev"));
  g_dev.procs.push_back({fn, fn, client, false});
  c.ret(kNoErr);
}
void AudioDeviceCreateIOProcID_(GuestCall& c) {
  const u32 dev = c.i(), fn = c.i(), client = c.i(), out = c.i();
  std::lock_guard<std::mutex> lk(g_dev.mu);
  if (dev != kDevice) return c.ret(fourcc("!dev"));
  const u32 id = g_dev.next_id += 0x10;
  g_dev.procs.push_back({id, fn, client, false});
  if (out) c.mem.w32(out, id);
  c.ret(kNoErr);
}
void remove_proc(GuestCall& c) {
  c.i();
  const u32 id = c.i();
  {
    std::lock_guard<std::mutex> lk(g_dev.mu);
    for (size_t k = 0; k < g_dev.procs.size(); k++) {
      if (g_dev.procs[k].id == id) {
        g_dev.procs.erase(g_dev.procs.begin() + (long)k);
        break;
      }
    }
  }
  update_running(c);
  c.ret(kNoErr);
}
void start_stop(GuestCall& c, bool start) {
  const u32 dev = c.i(), id = c.i();
  if (dev != kDevice) return c.ret(fourcc("!dev"));
  {
    std::lock_guard<std::mutex> lk(g_dev.mu);
    for (Proc& p : g_dev.procs) {
      if (!id || p.id == id) p.started = start;
    }
  }
  update_running(c);
  c.ret(kNoErr);
}
void AudioDeviceStart_(GuestCall& c) { start_stop(c, true); }
void AudioDeviceStartAtTime_(GuestCall& c) { start_stop(c, true); }
void AudioDeviceStop_(GuestCall& c) { start_stop(c, false); }

void AudioDeviceGetCurrentTime_(GuestCall& c) {
  const u32 dev = c.i(), out = c.i();
  if (dev != kDevice) return c.ret(fourcc("!dev"));
  double anchor_sample;
  u64 anchor_host;
  {
    // The anchor's two halves are moved together by the worker: read them together.
    std::lock_guard<std::mutex> lk(g_dev.mu);
    if (!g_dev.running) return c.ret(fourcc("stop"));
    anchor_sample = g_dev.anchor_sample;
    anchor_host = g_dev.anchor_host;
  }
  const u64 now = guest_now();
  write_time(c.mem, out, anchor_sample + ((double)now - (double)anchor_host) / ticks_per_frame(), now);
  c.ret(kNoErr);
}
// Between the device's sample time and the host's clock: they move together
// from the moment the device started.
void AudioDeviceTranslateTime_(GuestCall& c) {
  const u32 dev = c.i(), in = c.i(), out = c.i();
  if (dev != kDevice) return c.ret(fourcc("!dev"));
  double anchor_sample;
  u64 anchor_host;
  {
    std::lock_guard<std::mutex> lk(g_dev.mu);
    anchor_sample = g_dev.anchor_sample;
    anchor_host = g_dev.anchor_host;
  }
  const u32 flags = c.mem.r32(in + 56);
  double sample;
  u64 host;
  if (flags & 1) {
    const u64 bits = c.mem.r64(in);
    memcpy(&sample, &bits, 8);
    host = (u64)((double)anchor_host + (sample - anchor_sample) * ticks_per_frame());
  } else {
    host = c.mem.r64(in + 8);
    sample = anchor_sample + ((double)host - (double)anchor_host) / ticks_per_frame();
  }
  write_time(c.mem, out, sample, host);
  c.ret(kNoErr);
}
void AudioDeviceGetNearestStartTime_(GuestCall& c) { c.ret(kNoErr); }
void AudioDeviceRead_(GuestCall& c) { c.ret(kIllegal); }

// ---- output units ----------------------------------------------------------------

const u32 kComponentBase = 0x00A0C000;  // stand-in Components, one per kind of output unit
const s32 kInvalidProperty = -10879, kBadInstance = -2147450879;

Unit* unit_of(u32 handle) {
  for (Unit& u : g_dev.units) {
    if (u.handle == handle) return &u;
  }
  return nullptr;
}

bool is_output_unit(GuestCall& c, u32 desc) {
  if (!desc) return false;
  const u32 type = c.mem.r32(desc), sub = c.mem.r32(desc + 4);
  return type == fourcc("auou") && (sub == fourcc("ahal") || sub == fourcc("def ") || sub == fourcc("sys ") || sub == 0);
}

void FindNextComponent_(GuestCall& c) {
  const u32 after = c.i(), desc = c.i();
  if (after >= kComponentBase && after < kComponentBase + 0x100) return c.ret(0);  // ours is the only one
  if (!after && is_output_unit(c, desc)) return c.ret(kComponentBase + 1);
  c.ret(c.original() ? c.call(c.original(), {after, desc}) : 0);
}
void CountComponents_(GuestCall& c) {
  const u32 desc = c.i();
  if (is_output_unit(c, desc)) return c.ret(1);
  c.ret(c.original() ? c.call(c.original(), {desc}) : 0);
}
u32 new_unit(GuestCall& c) {
  std::lock_guard<std::mutex> lk(g_dev.mu);
  Unit u;
  u.handle = g_dev.next_id += 0x10;
  if (c.powerpc) u.format.flags |= 2;
  g_dev.units.push_back(u);
  return u.handle;
}
void OpenAComponent_(GuestCall& c) {
  const u32 comp = c.i(), out = c.i();
  if (comp >= kComponentBase && comp < kComponentBase + 0x100) {
    if (out) c.mem.w32(out, new_unit(c));
    return c.ret(0);
  }
  c.ret(c.original() ? c.call(c.original(), {comp, out}) : (u32)-3000);
}
void OpenComponent_(GuestCall& c) {
  const u32 comp = c.i();
  if (comp >= kComponentBase && comp < kComponentBase + 0x100) return c.ret(new_unit(c));
  c.ret(c.original() ? c.call(c.original(), {comp}) : 0);
}
void CloseComponent_(GuestCall& c) {
  const u32 inst = c.i();
  bool ours = false;
  {
    std::lock_guard<std::mutex> lk(g_dev.mu);
    for (size_t k = 0; k < g_dev.units.size(); k++) {
      if (g_dev.units[k].handle == inst) {
        g_dev.units.erase(g_dev.units.begin() + (long)k);
        ours = true;
        break;
      }
    }
  }
  if (ours) {
    update_running(c);
    return c.ret(0);
  }
  c.ret(c.original() ? c.call(c.original(), {inst}) : (u32)-3000);
}
void GetComponentInfo_(GuestCall& c) {
  const u32 comp = c.i(), desc = c.i(), name = c.i(), info = c.i(), icon = c.i();
  if (comp >= kComponentBase && comp < kComponentBase + 0x100) {
    if (desc) {
      c.mem.w32(desc, fourcc("auou"));
      c.mem.w32(desc + 4, fourcc("ahal"));
      c.mem.w32(desc + 8, fourcc("appl"));
      c.mem.w32(desc + 12, 0);
      c.mem.w32(desc + 16, 0);
    }
    return c.ret(0);
  }
  c.ret(c.original() ? c.call(c.original(), {comp, desc, name, info, icon}) : (u32)-3000);
}

// An AudioUnit call on a unit that isn't ours goes to the guest's own code.
#define NOT_OURS(...)                                                        \
  if (!u) {                                                                 \
    c.ret(c.original() ? c.call(c.original(), {__VA_ARGS__}) : (u32)kBadInstance); \
    return;                                                                 \
  }

void read_format(GuestCall& c, u32 at, Format& f) {
  const u64 bits = c.mem.r64(at);
  memcpy(&f.rate, &bits, 8);
  f.flags = c.mem.r32(at + 12);
  f.bytes_per_frame = c.mem.r32(at + 24);
  f.channels = c.mem.r32(at + 28);
  f.bits = c.mem.r32(at + 32);
  if (!f.channels) f.channels = 1;
  if (f.bits != 8 && f.bits != 16 && f.bits != 32) f.bits = 16;
  if (!f.bytes_per_frame) f.bytes_per_frame = f.interleaved() ? f.channels * f.bits / 8 : f.bits / 8;
  if (f.rate < 1000 || f.rate > 192000) f.rate = kRate;
}
void write_format(Bytes& o, const Format& f) {
  o.f64(f.rate);
  o.u32_(fourcc("lpcm"));
  o.u32_(f.flags);
  o.u32_(f.interleaved() ? f.bytes_per_frame : f.bits / 8);
  o.u32_(1);
  o.u32_(f.interleaved() ? f.bytes_per_frame : f.bits / 8);
  o.u32_(f.channels);
  o.u32_(f.bits);
  o.u32_(0);
}

// An output unit's property. False if it has no such property.
bool unit_property(GuestCall& c, Unit& u, u32 id, u32 scope, u32 element, Bytes& o) {
  switch (id) {
    case 8: {  // stream format: what the guest gives on the input side, the device's on the output side
      Format f;
      if (scope == 1 && element == 0) {
        f = u.format;
      } else if (c.powerpc) {
        f.flags |= 2;
      }
      write_format(o, f);
      return true;
    }
    case 2: o.f64(scope == 1 ? u.format.rate : kRate); return true;  // sample rate
    case 2000: o.u32_(kDevice); return true;                          // current device
    case 14: o.u32_(4096); return true;                               // maximum frames per slice
    case 12: o.f64(0); return true;                                   // latency
    case 22: o.u32_(0); return true;                                  // last render error
    case 2001: o.u32_(u.started ? 1 : 0); return true;                // is running
    case 2003: o.u32_(element == 0 ? 1 : 0); return true;             // enable IO: output yes, input no
    case 2006: o.u32_(element == 0 ? 1 : 0); return true;             // has IO
    case 11: o.u32_(scope == 0 ? 0 : 1); return true;                 // element count
    case 19: case 0x73726E64: {                                       // channel layout; the device's preferred one
      Bytes d(c.powerpc);
      property(c, kDevice, fourcc("srnd"), fourcc("outp"), d);
      o.b = d.b;
      return true;
    }
    default: return false;
  }
}

void AudioUnitGetPropertyInfo_(GuestCall& c) {
  const u32 inst = c.i(), id = c.i(), scope = c.i(), element = c.i(), out_size = c.i(), out_writable = c.i();
  std::unique_lock<std::mutex> lk(g_dev.mu);
  Unit* u = unit_of(inst);
  if (!u) lk.unlock();
  NOT_OURS(inst, id, scope, element, out_size, out_writable);
  Unit copy = *u;
  lk.unlock();
  Bytes o(c.powerpc);
  if (!unit_property(c, copy, id, scope, element, o)) return c.ret((u32)kInvalidProperty);
  if (out_size) c.mem.w32(out_size, (u32)o.b.size());
  if (out_writable) c.mem.w8(out_writable, 1);
  c.ret(0);
}
void AudioUnitGetProperty_(GuestCall& c) {
  const u32 inst = c.i(), id = c.i(), scope = c.i(), element = c.i(), data = c.i(), io_size = c.i();
  std::unique_lock<std::mutex> lk(g_dev.mu);
  Unit* u = unit_of(inst);
  if (!u) lk.unlock();
  NOT_OURS(inst, id, scope, element, data, io_size);
  Unit copy = *u;
  lk.unlock();
  Bytes o(c.powerpc);
  if (!unit_property(c, copy, id, scope, element, o)) {
    unknown(inst, id, scope);
    return c.ret((u32)kInvalidProperty);
  }
  give(c, o, io_size, data);
  c.ret(0);
}
void AudioUnitSetProperty_(GuestCall& c) {
  const u32 inst = c.i(), id = c.i(), scope = c.i(), element = c.i(), data = c.i(), size = c.i();
  std::unique_lock<std::mutex> lk(g_dev.mu);
  Unit* u = unit_of(inst);
  if (!u) lk.unlock();
  NOT_OURS(inst, id, scope, element, data, size);
  if (id == 1) {  // kAudioUnitProperty_MakeConnection
    if (scope != 1 || element != 0 || size < 12 ||
        !data || !c.mem.range_ok(data, 12, GuestMemory::kRead) || c.mem.r32(data + 8) != element)
      return c.ret((u32)kAudioUnitErr_InvalidPropertyValue);
    const u32 source = c.mem.r32(data), bus = c.mem.r32(data + 4);
    if (!source) {
      u->source = u->source_bus = u->render_api = 0;
      return c.ret(0);
    }
    lk.unlock();
    const u32 get = c.symbol("AudioUnitGetProperty"), render = c.symbol("AudioUnitRender");
    const u32 format = c.scratch(44);
    if (!get || !render || !format) {
      log_warn("Core Audio: cannot connect a guest audio unit");
      return c.ret((u32)kAudioUnitErr_FailedInitialization);
    }
    c.mem.w32(format + 40, 40);
    const u32 status = c.call(get, {source, 8, 2, bus, format, format + 40});
    if (status) return c.ret(status);
    if (c.mem.r32(format + 40) < 40 || c.mem.r32(format + 8) != fourcc("lpcm"))
      return c.ret((u32)kAudioUnitErr_FormatNotSupported);
    Format source_format;
    read_format(c, format, source_format);
    lk.lock();
    u = unit_of(inst);
    if (!u) return c.ret((u32)kBadInstance);
    u->source = source;
    u->source_bus = bus;
    u->render_api = render;
    u->render = u->refcon = 0;
    u->format = source_format;
    u->phase = 0;
    u->last[0] = u->last[1] = 0;
  } else if (id == 8 && scope == 1 && data && size >= 40) {  // the format the guest will give
    if (c.mem.r32(data + 8) != fourcc("lpcm")) return c.ret((u32)-10868);  // kAudioUnitErr_FormatNotSupported
    read_format(c, data, u->format);
  } else if (id == 23 && data && size >= 8) {  // render callback
    u->render = c.mem.r32(data);
    u->refcon = c.mem.r32(data + 4);
    u->source = u->source_bus = u->render_api = 0;
  } else if (id == 2 && scope == 1 && data) {  // sample rate
    const u64 bits = c.mem.r64(data);
    memcpy(&u->format.rate, &bits, 8);
  }
  // Everything else (current device, channel layout and map, buffer sizes) is accepted and changes nothing.
  c.ret(0);
}
void AudioUnitInitialize_(GuestCall& c) {
  const u32 inst = c.i();
  std::unique_lock<std::mutex> lk(g_dev.mu);
  Unit* u = unit_of(inst);
  lk.unlock();
  NOT_OURS(inst);
  c.ret(0);
}
void AudioUnitUninitialize_(GuestCall& c) { AudioUnitInitialize_(c); }
void AudioUnitReset_(GuestCall& c) {
  const u32 inst = c.i(), scope = c.i(), element = c.i();
  std::unique_lock<std::mutex> lk(g_dev.mu);
  Unit* u = unit_of(inst);
  lk.unlock();
  NOT_OURS(inst, scope, element);
  c.ret(0);
}
void unit_listener(GuestCall& c) {  // add or remove a property listener or render notification: accepted
  const u32 inst = c.i(), a = c.i(), b = c.i(), d = c.i();
  std::unique_lock<std::mutex> lk(g_dev.mu);
  Unit* u = unit_of(inst);
  lk.unlock();
  NOT_OURS(inst, a, b, d);
  c.ret(0);
}
void AudioUnitGetParameter_(GuestCall& c) {
  const u32 inst = c.i(), id = c.i(), scope = c.i(), element = c.i(), out = c.i();
  std::unique_lock<std::mutex> lk(g_dev.mu);
  Unit* u = unit_of(inst);
  lk.unlock();
  NOT_OURS(inst, id, scope, element, out);
  if (out) {
    const float v = id == 14 ? g_dev.volume : 0.0f;  // kHALOutputParam_Volume
    u32 bits;
    memcpy(&bits, &v, 4);
    c.mem.w32(out, bits);
  }
  c.ret(0);
}
void AudioUnitSetParameter_(GuestCall& c) {
  const u32 inst = c.i(), id = c.i();
  c.i();  // scope
  c.i();  // element
  const float v = c.f();
  std::unique_lock<std::mutex> lk(g_dev.mu);
  Unit* u = unit_of(inst);
  if (u && id == 14) g_dev.volume = v;
  lk.unlock();
  if (!u) {  // the float went in a register or on the stack; the guest's own code wants it back the same way
    c.ret(c.original() ? (u32)kBadInstance : (u32)kBadInstance);
    return;
  }
  c.ret(0);
}
void AudioUnitRender_(GuestCall& c) {
  const u32 inst = c.i(), flags = c.i(), ts = c.i(), bus = c.i(), frames = c.i(), list = c.i();
  std::unique_lock<std::mutex> lk(g_dev.mu);
  Unit* u = unit_of(inst);
  lk.unlock();
  NOT_OURS(inst, flags, ts, bus, frames, list);
  c.ret((u32)-10863);  // kAudioUnitErr_CannotDoInCurrentContext: output units don't render on request here
}
void output_start_stop(GuestCall& c, bool start) {
  const u32 inst = c.i();
  {
    std::unique_lock<std::mutex> lk(g_dev.mu);
    Unit* u = unit_of(inst);
    if (u) u->started = start;
    lk.unlock();
    NOT_OURS(inst);
  }
  update_running(c);
  c.ret(0);
}
void AudioOutputUnitStart_(GuestCall& c) { output_start_stop(c, true); }
void AudioOutputUnitStop_(GuestCall& c) { output_start_stop(c, false); }

#define H(name) {#name, name##_}
#define K(name) {#name, name##_, true}
const HookSpec kComponents[] = {
    K(FindNextComponent), K(CountComponents), K(OpenAComponent), K(OpenComponent), K(CloseComponent),
    K(GetComponentInfo),
};
const HookSpec kUnits[] = {
    K(AudioUnitGetPropertyInfo), K(AudioUnitGetProperty), K(AudioUnitSetProperty), K(AudioUnitInitialize),
    K(AudioUnitUninitialize), K(AudioUnitReset), K(AudioUnitGetParameter), K(AudioUnitSetParameter),
    K(AudioUnitRender), K(AudioOutputUnitStart), K(AudioOutputUnitStop),
    {"AudioUnitAddPropertyListener", unit_listener, true},
    {"AudioUnitRemovePropertyListener", unit_listener, true},
    {"AudioUnitRemovePropertyListenerWithUserData", unit_listener, true},
    {"AudioUnitAddRenderNotify", unit_listener, true},
    {"AudioUnitRemoveRenderNotify", unit_listener, true},
};
const HookSpec kAudioFiles[] = {
    {"AudioFileOpenWithCallbacks", midi_audio_file_open, true},
    {"AudioFileClose", midi_audio_file_close, true},
};
#undef K
const HookSpec kAudio[] = {
    H(AudioHardwareGetPropertyInfo),
    H(AudioHardwareGetProperty),
    H(AudioHardwareSetProperty),
    {"AudioHardwareAddPropertyListener", ok},
    {"AudioHardwareRemovePropertyListener", ok},
    {"AudioHardwareAddRunLoopSource", ok},
    {"AudioHardwareRemoveRunLoopSource", ok},
    {"AudioHardwareUnload", ok},
    H(AudioDeviceGetPropertyInfo),
    H(AudioDeviceGetProperty),
    H(AudioDeviceSetProperty),
    {"AudioDeviceAddPropertyListener", ok},
    {"AudioDeviceRemovePropertyListener", ok},
    H(AudioDeviceAddIOProc),
    {"AudioDeviceRemoveIOProc", remove_proc},
    H(AudioDeviceCreateIOProcID),
    {"AudioDeviceDestroyIOProcID", remove_proc},
    H(AudioDeviceStart),
    H(AudioDeviceStartAtTime),
    H(AudioDeviceStop),
    H(AudioDeviceGetCurrentTime),
    H(AudioDeviceTranslateTime),
    H(AudioDeviceGetNearestStartTime),
    H(AudioDeviceRead),
    H(AudioStreamGetPropertyInfo),
    H(AudioStreamGetProperty),
    H(AudioStreamSetProperty),
    {"AudioStreamAddPropertyListener", ok},
    {"AudioStreamRemovePropertyListener", ok},
    H(AudioObjectHasProperty),
    H(AudioObjectIsPropertySettable),
    H(AudioObjectGetPropertyDataSize),
    H(AudioObjectGetPropertyData),
    H(AudioObjectSetPropertyData),
    {"AudioObjectAddPropertyListener", ok},
    {"AudioObjectRemovePropertyListener", ok},
    {"AudioObjectShow", ok},
};
#undef H

const HookLibrary kLibraries[] = {
    {"/CoreAudio.framework/Versions/A/CoreAudio", "", kAudio, sizeof kAudio / sizeof kAudio[0], true},
    {"/AudioUnit.framework/Versions/A/AudioUnit", "", kUnits, sizeof kUnits / sizeof kUnits[0], true},
    {"/CarbonCore.framework/Versions/A/CarbonCore", "", kComponents, sizeof kComponents / sizeof kComponents[0], true},
    {"/AudioToolbox.framework/Versions/A/AudioToolbox", "", kAudioFiles, sizeof kAudioFiles / sizeof kAudioFiles[0], true},
};

}  // namespace

const HookLibrary* audio_hook_libraries(size_t& n) {
  n = sizeof kLibraries / sizeof kLibraries[0];
  return kLibraries;
}
