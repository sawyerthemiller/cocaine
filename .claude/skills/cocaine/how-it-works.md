# How cocaine works

Moved from the README on 2026-11-18. The design of each piece, with the reasons; keep it current when a piece changes.

- **Guest memory.** A native Apple Silicon process can't use addresses below
  4 GB, which is exactly where a 32-bit program lives. So cocaine reserves a
  4 GB window and guest address A is host address `base + A`.
- **CPU cores.** One interpreter per architecture (`src/cpu/ppc`,
  `src/cpu/x86`). They run guest code until it traps into the kernel. They are
  the simple, exact reference.
- **The translator.** PowerPC code doesn't normally go through the
  interpreter: it is translated to ARM64, a block at a time, the first time
  it runs (`src/cpu/ppc/ppc_jit.cpp`). A block is a straight run of guest
  instructions, each turned into a few host ones. The seventeen busiest
  guest registers (r0 to r12, r29 to r31 and the condition register) live
  in host registers while translated code runs, and are put back in the
  CPU state on every way out, so the rest of cocaine never sees the
  difference. Blocks jump straight to each other once both exist; a
  return or a call through a pointer finds its block in a table without
  leaving translated code. AltiVec's common instructions (the float
  arithmetic, permutes, merges, splats, loads and stores, conversions,
  compares and the even and odd multiplies that Leopard's drawing lives on) become NEON: a vector register
  is kept byte-reversed, so loaded whole its elements are in reverse order,
  each in host byte order, and element-wise work is the same instruction;
  only what cares where an element sits is renumbered (`vperm` is a `TBL`
  over {b, a} with index `~c & 31`, `lvx` a load and a 16-byte reverse).
  Vector float ops check the FPSCR rounds to nearest, since AltiVec always
  does, and leave the rare other case to the interpreter. Anything else
  rare (the remaining AltiVec, the odder floating point, traps) is done by
  calling the interpreter for that one instruction, which is what keeps
  the translator small and lets it be checked: the same tests run both ways and must agree bit for bit. When
  guest memory holding code is unmapped, mapped over, or flushed by the
  guest's own "I have written code here" call, the blocks from it are
  dropped. A forked child keeps its parent's translations.
- **The Intel translator** (`src/cpu/x86/x86_jit.cpp`) is built the same
  way, with two differences that x86 forces. Its flags: nearly every x86
  instruction sets six of them and nearly nobody reads them. The
  translator looks ahead, and where the flags are about to be overwritten
  unread it emits the bare operation. Where a compare (or any arithmetic)
  has a conditional jump, set or move close behind, the pair becomes ARM's
  own: a flag-setting ARM instruction and a branch on ARM's flags, with
  the x86 condition turned into the ARM one that means the same after
  that kind of instruction (ARM's carry is upside down after a
  subtraction; a byte-sized compare is done at the top of the register so
  that ARM's flags come out as an 8-bit operation's). And where the
  guest's flags might still be wanted later, the instruction leaves a
  note in the guest's state (which operation, on what) in place of
  working them out; the flags are worked out from the note only if
  something does read them, and always before anything outside translated
  code looks. The few instructions that take the carry in still call the
  interpreter's routine. And its code changes without notice (the stubs for
  calling other libraries are rewritten in place on first use), so code in
  memory the guest can write is translated an instruction at a time, and
  each such block first checks that the bytes it was made from are still
  there.

  Floating point is translated too. The SSE registers stay in the guest's
  state; each instruction loads what it needs, does the work with the
  Mac's own floating point or vector instruction, and stores the result.
  That covers arithmetic on one number or all of them at once, compares,
  conversions, moves, shuffles, the bitwise instructions and the plainer
  integer vector ones. Where x86 and ARM give different answers, the
  translated code notices and lets the interpreter do that one instruction:
  a result that is not a number (the two disagree about which one), and a
  conversion to an integer that doesn't fit. The Mac's rounding mode is set
  to the guest's while translated code runs. x87 keeps its exact 80-bit
  registers, and the interpreter's routine still does the arithmetic, but
  translated code calls it and carries on, where at first each x87
  instruction ended the block. What still ends a block and goes to the
  interpreter one instruction at a time: MMX, string moves, 16-bit
  operands, and the rarer SSE instructions.
- **System libraries.** Old apps need Cocoa, Carbon, QuickTime and the rest in
  PowerPC and 32-bit form, and modern macOS ships none of that. cocaine runs
  Apple's own old libraries, from a Mac OS X install you supply, as guest
  code. Guest paths under `/System`, `/usr`, `/Library` and friends are
  redirected into that system root; everything else is this Mac's own files.
  PowerPC PEF executables are passed to Leopard's `LaunchCFMApp`. Imported
  AppleDouble sidecars supply missing Finder metadata and read-only resource
  forks without changing the imported files. Parent IDs for immediate system
  root children use the guest volume root ID so Carbon can build paths.
  `setattrlist` translates 32-bit timestamps and byte order when Carbon sets
  folder dates and Finder metadata together.
  A Cocoa Save onto an existing file ends in Carbon's `FSPathReplaceObject`,
  which asks `access_extended` whether the old file may be written (several
  questions in one call; each file's go to this Mac's own `accessx_np` with
  the name translated) and then hands the new file the old one's dates,
  permissions and access control list with `setattrlist`. The list travels
  as a file security record, the same shape on both sides, with its numbers
  turned round for PowerPC in both `getattrlist` and `setattrlist`.
  Volume enumeration uses the imported root's backing volume, including its
  actual read-only status, and adds browsable host mounts under `/Volumes`.
  Carbon's `/.vol/<device>/@` and root folder ID 2 resolve those mounts.
  IOKit identifies mounted disk images and optical media by BSD device name
  and presents a minimal CD media ancestor with the `Whole` property, for
  installers that check for a disc. This does not emulate disc commands.
- **Fake kernel.** `src/kernel` stands in for what sits underneath those
  libraries:
  - BSD system calls. The host is Darwin too, so most map onto the same host
    call after translating the calling convention, structure layouts and (for
    PowerPC) byte order. `F_PREALLOCATE` translates the guest's 32-bit
    `fstore_t` to the host structure and returns the bytes allocated.
    EV Nova's registration tool needs it to initialize its preference file.
    Where today's kernel answers differently, cocaine
    answers as Leopard's did: `kevent` reports a failed change with flags
    `EV_ERROR` alone, where this Mac keeps the change's own flags too, and
    libevent (in Transmission) took the difference for a broken kqueue and
    fell back to `poll`, which cocaine didn't have yet; it hung before
    showing a window.
  - Mach traps and messages. Most kernel services on Mac OS X are messages to
    kernel ports; cocaine keeps its own port table and answers the messages
    programs really send.
  - The comm page: routines such as `memcpy` and the clock that the kernel
    maps at fixed addresses. cocaine supplies its own, most of them one
    "host call" that runs the routine as native code.

- **Mach ports and sessions.** Every port a guest names is a real port of
  the host process running it, under the same name. This Mac's own kernel
  therefore does the hard parts of Mach for the guests: queueing, moving
  port rights between processes, port sets, semaphores, timers, and noticing
  when a port dies. Guest messages are laid out for a 32-bit machine (and in
  PowerPC byte order half the time), so each one travels inside a host
  message that carries its port rights and out-of-line memory as host
  descriptors (`src/kernel/tunnel.h`). The kernel's own objects (task,
  thread, host, clock) are ports cocaine holds the receiving end of; what a
  guest sends them is answered in place. IOKit requests answered in place
  consume moved port rights but leave copied rights with the sender; losing
  the task right prevents the window server from replacing resized backing
  stores. The session server records each
  process's guest task port at handoff or join, using the host-stamped
  sender PID for joins. `task_for_pid` can look up those ports within the
  session, never host task ports; dead ports are discarded. Fork waits for
  the child's port to be registered before returning to the parent.
- **The session server.** `launchd` is the directory of system services on a
  real Mac. cocaine's stand-in (`src/session/`) is a small native program,
  one per run, started the first time a guest needs it and ended when the
  run's first program ends. It reads the job descriptions in the system root
  to learn which daemon provides which service, hands out service ports, and
  starts Apple's real daemon behind a port, as a guest, when the first
  message arrives for it. Only daemons known to work are on call (`notifyd`,
  `DirectoryService`, `dirhelper`, `securityd`, `coreservicesd`, `configd`
  (without its plug-ins, `-b`, and in the foreground, `-d`), and
  `WindowServer`, `ATSServer`, `loginwindow` and `hidd`, the last four for Intel
  programs only where the keys to them have been supplied). Each starts in
  the guest's `/`, as launchd starts daemons;
  `COCAINE_DAEMONS=all`, or a
  comma-separated list of job labels, tries others. Daemons are told they are
  root, and their messages are stamped as root's, because both they and
  their clients check. A daemon can collect its ports either of the two ways
  `launchd` offers: by name over Mach, or by asking on the socket every job
  is started with. With `-v` cocaine says where the session's log is; it
  holds what the server did, what the daemons printed, and everything any
  guest sent to `syslog`, which is often the only place a daemon explains
  itself.
- **The desktop and the menu bar.** The Dock, which draws Leopard's desktop
  picture, can't be started (apps can't start other apps yet), so cocaine
  draws one itself from inside loginwindow once the session is ready: a
  desktop-level window that ignores the mouse, with `COCAINE_WALLPAPER` (or
  `images/linen.jpeg`) drawn to fill it (`src/kernel/wallpaper.cpp`). The
  bundled image is exposed at `/.cocaine/linen.jpeg` so it remains accessible
  even when the checkout is under the host's private Library folder. The
  Apple menu's logo is swapped for a pile of powder in a copy of CoreUI's art
  file that guests are shown instead of Apple's (`src/session/art.cpp`).
  Closing the viewer window ends the whole run. SKILL.md has the details.
- **Windows.** The window server is Apple's, run as a guest like the other
  daemons. A real Mac has it up, and a login session set up by `loginwindow`,
  long before any app starts, and apps depend on that order: one that
  registers with the window server before the login window has finished is
  told it is the app in front, then quietly isn't, and never sees a key. So
  for a program with windows (one in an `.app`, or linking Cocoa or Carbon)
  the session server starts the window server at once and holds the
  program's service lookups until the login window has put itself in front,
  which that process's cocaine reports. Any other session leaves the window
  server alone until something asks for it. The window server draws into a
  "VirtualDisplay" of its own making; checked CoreGraphics patches for the
  original release and 10.5.6 set its size. Its cocaine copies whatever changes
  there into a small shared file, checking up to 250 times a second, and starts
  `cocaine --view` on it. cocaine hooks the window server's own
  `CGXUpdateDisplay` (a private CoreGraphics function, on both CPUs): when
  it returns, the screen holds a whole picture, and what changed is copied
  to the viewer then, on the window server's thread, before the next update
  can start, so a window halfway through being redrawn is never shown torn.
  On the checked 10.5.6 PowerPC CoreGraphics, a second hook tracks virtual
  display mode changes, including dimensions and row stride; RGB555 pixels
  are expanded to the viewer's 32-bit format. Its mode table also gets real
  640x480 RGB555 and 32-bit modes, reusing two unsupported indexed-color
  slots while keeping the existing true-color modes. The first three
  entries must stay 1x1 placeholders: the driver treats them specially.
  Without real 640x480 modes, DrawSprocket chooses 800x600 for a 640x480
  request and apps such as Ghost Recon draw their menus into a corner.
  These checked in-memory patches are not game-specific and leave viewport
  and scissor semantics alone. The shared file holds the
  current dimensions with its frame sequence, and has room for modes that
  fit the window server's shared framebuffer, including 16-bit modes expanded
  to 32 bits. Before guest code starts, `configure_host_display` selects the
  largest conventional resolution fitting the host screen's available logical
  points, with room for the viewer frame (1280x800 without a screen), unless
  `COCAINE_DISPLAY` or a game default already chose one. That size is exported
  in the environment so all session processes agree. Leopard's
  `AppleDisplayScaleFactor` defaults to 1 regardless of resolution;
  `COCAINE_UI_SCALE` explicitly opts into its unfinished enlarged drawing.
  The viewer defaults to one guest pixel per host logical point, fitting down
  if necessary. Its View menu (Command-1/2/3) selects Actual Size, Integer Scale
  or Fit, also selectable with `COCAINE_VIEWER_SCALE=actual|integer|fit`.
  Integer Scale uses the largest whole physical-pixel multiplier that fits;
  below one physical pixel it falls back to proportional downscaling. Fit
  uses the whole available rectangle without changing aspect ratio. A
  separate picture layer leaves black borders where needed; input uses that
  layer's rectangle, not the whole window. Border clicks are ignored, but
  button releases still reach the guest. Whole-pixel enlargement uses nearest
  filtering; fractional enlargement is linear. Layout and filters update
  when the window is resized or its backing scale changes, without switching
  the guest mode. Moving to another screen reapplies the selected sizing
  policy to that screen's available space. A guest mode change resizes the
  viewer to the selected presentation size on its current screen. Explicit legacy
  `COCAINE_VIEWER_DOUBLE=0|1` still requests one or two physical pixels per
  guest pixel unless `COCAINE_VIEWER_SCALE` is set or a View menu mode is chosen.
  The launcher starts Ghost Recon Demo with an 800x600 display, unless an
  explicit `COCAINE_DISPLAY` is inherited, retaining the older compatibility
  default while the guest can now switch to its exact 640x480 menu mode. It also
  sets `COCAINE_VIEWER_DOUBLE=1` unless inherited, to show each guest pixel
  twice as wide and high where the screen has room.
  A polling copier stands down while the hook is active and resumes if it
  stops reporting updates: some games change the display without calling
  `CGXUpdateDisplay`. Those fallback copies can show a partially drawn frame.
  Scripted screenshots read the published file without forcing a refresh,
  so they test what the viewer can actually see. The file's frame
  number is odd while a picture is being written, so the viewer never takes
  a half-written one. The viewer is a plain macOS window that hands each new picture
  to its layer, so this Mac's graphics hardware does the scaling
  (`src/viewer/`), and sends mouse and keys back down a pipe. Leopard's
  window server holds every screen update for the display's next refresh,
  or for an eighth of a second where a display can't say when that is, as
  this one can't; the session server switches that off in the system root's
  window server preferences, which is the difference between a button
  taking 145 ms to react and 15. Mouse and keys are written into the input system's
  shared event queue (`post_input_event` in `src/kernel/iokit.cpp`), where
  the window server finds them as it would on real hardware. Its wake-up
  notification is nonblocking: a full notification port already holds a
  wake-up, so it must not stall later input. The window server decides
  which app gets what. The viewer sends the Mac's own measure of each
  movement with it, scaled to the current guest display and viewer size.
  When a game captures the pointer for camera controls, its movement instead
  stays in host points, so resizing the viewer does not change camera
  sensitivity. When the window server moves the pointer itself
  (`IOHIDSetMouseLocation`, input method 4: two 16-bit numbers and a
  process id), the pointer stays there and, for a second and a half,
  moves by the mouse's movement from there instead of following the Mac's
  pointer, as on real hardware; a program doing that again and again (a
  game that recentres a hidden pointer) has the viewer hide the Mac's
  pointer and hold it still (`pointer_held` in the display file).
  Command-Option-M also toggles capture manually; the viewer tells the input
  system when capture changes, and releases it when the window loses focus.
- **Games: OpenGL and OpenAL.** Leopard's OpenGL drives graphics cards no
  Apple Silicon Mac has, and run as guest code it finds no renderer at all:
  every call quietly does nothing (Marble Blast crashed several calls
  later, on a camera matrix OpenGL never filled in). So when a guest maps
  the code of libGL, libGLU, AGL, the CGL framework or an OpenAL framework (its own bundled copy
  counts), cocaine reads the library's exported symbols and writes a host
  call over the first instruction of each function it supplies
  (`src/kernel/hooks.cpp`). Whoever calls it, the game or another library,
  lands in native code that reads the arguments the PowerPC way (integers
  in r3-r10, floats in f1-f13, the rest on the stack) and makes the call on
  the Mac's own library (`src/gl/gl.cpp`, `src/al/al.cpp`). Functions of
  those libraries that aren't done yet say so once and return 0. Byte
  order is the work: values in registers need nothing, but matrices,
  vertex and index arrays, glGet answers and 16-bit sound samples are
  big-endian in guest memory and get swapped on the way through. Intel
  games' 16-bit samples usually aren't, but the OpenAL that early universal
  games bundled kept PowerPC's order on Intel too (Marble Blast Gold's
  declares its input big-endian), so for Intel guests each buffer is read
  both ways and whichever is smooth rather than noise wins. A streamed
  source that runs dry while the game still means it to play (Marble
  Blast's music, when its main loop stalls during a window drag) is
  reported as playing and started again when buffers arrive, as the
  OpenAL of the time behaved; this Mac's stops it, and the game took that
  for the end of the track. Pixel
  data needs no copy, because the host's swap-bytes pixel setting is kept
  the opposite of the guest's. Vertex arrays are noted when the guest
  points at them and converted at each draw. Packed PowerPC arrays are
  byte-swapped in vector-sized blocks; strided arrays are copied and
  swapped component by component, while Intel arrays need no swap.
  Each guest OpenGL context
  draws into an offscreen framebuffer on the Mac's GPU. AGL renderer
  queries describe the host's renderers for the virtual main display;
  pixel formats hold host CGL formats, though requested attributes are not
  yet enforced. At
  `aglSwapBuffers` cocaine reads the frame back and copies it into the
  game window's own pixels, converting to RGB555 for a 16-bit QuickDraw
  pixmap, then marks the window changed and has the
  guest's QuickDraw flush it, the way a game that drew in software did. The
  window server, the viewer and window moves then need nothing new. Cocoa's
  NSOpenGLView uses CGL instead: cocaine takes over CGL context creation,
  connects the offscreen framebuffer to the window server's surface bounds,
  and at `CGLFlushDrawable` draws the read-back pixels into the guest window
  through its CoreGraphics window context. GL Golf uses this path and GLU's
  quadrics, mipmaps, and perspective functions. CGL fullscreen contexts use
  the captured guest display's dimensions and borrowed
  `CGDisplayGetDrawingContext` instead, drawing the same read-back image at
  the display's origin. Clearing or replacing the drawable leaves fullscreen;
  no host display is captured or switched. Postal 2 uses this path, together
  with scissor clipping, color material and range-indexed drawing (which
  shares the ordinary indexed draw's guest-array conversion).
  ARB texture compression and S3TC are offered when the host supports them.
  Compressed uploads, updates and readback use the core and ARB entry points;
  their blocks have a fixed byte layout and are not swapped for PowerPC.
  Without S3TC, Postal 2's renderer skips the highest mip of compressed
  textures even for fonts: its 256x256 DXT5 font atlases became 128x128.
  Passing compression through preserves their full source resolution.
  AGL and CGL presentation is capped at 120 frames a second by default to
  reduce latency for app-drawn cursors; `COCAINE_GL_FPS` overrides it,
  including `60` for the former limit and `0` for no limit.
  Before starting guest code, Snes9x HQ! gets a default `COCAINE_UI_SCALE=1`:
  its game window is partly off-screen at 2x. It uses a 1280x800 guest
  display and doubled viewer pixels to keep menus large. Programs linked to
  SDL 1.2 also get a default `COCAINE_UI_SCALE=1`. SDL's old QuickDraw driver
  locates its software pixels by subtracting a view's point frame from its
  window's pixel frame.
  At 2x that puts Postal 2's warning bitmap 385 rows into the backing store,
  while its flush covers the first 384 rows: a blank part of the window.
  Keeping these programs at 1x makes the coordinates agree, without changing
  other programs' usual scaling. Postal 2 defaults to a 1024x768 guest display
  and doubled viewer pixels instead of Leopard's unfinished 2x drawing:
  its menu is larger without breaking the software warning screen. This
  applies to both bundle and executable launches, including the launcher.
  These game defaults are retained while applications are tested with the
  general 1x policy. Explicit `COCAINE_UI_SCALE`, `COCAINE_DISPLAY` and
  `COCAINE_VIEWER_DOUBLE` still take precedence over game defaults;
  `COCAINE_VIEWER_SCALE` overrides the viewer's legacy doubling setting.
  CGL pixel format requests,
  like AGL's, are not yet enforced. Those
  QuickDraw calls are guest functions, run from native code
  (`Process::call_guest`): a fresh stack frame, a return address that
  holds a "come back" host call, and the CPU loop run until it hits it.
- **Sound.** Everything a Mac program does with sound ends up at Core
  Audio's hardware layer (`AudioHardware...`, `AudioDevice...`,
  `AudioStream...`, `AudioObject...`), and Leopard's version needs drivers
  and a daemon cocaine doesn't have, so it finds no devices. cocaine takes
  that layer over, on both CPUs, and offers one device of its own,
  "cocaine output": stereo, 32-bit float, 44.1 kHz (`src/audio/`). When the
  device starts, a guest thread (made with the guest's own `pthread_create`,
  running a native function cocaine plants for it) calls the guest's IO
  procedures a buffer at a time and puts what they write into a ring, which
  a native output unit plays; the ring is kept a few buffers ahead, which
  paces the guest. The guest's Component Manager finds no components under
  cocaine either, so the output units (AUHAL, default and system output)
  are done the same way, on the same thread: their render callbacks are
  called for the format the guest set (16- or 32-bit, either byte order,
  interleaved or not, resampled if the rate differs), and requests for any
  other kind of component go on to the guest's own Component Manager. The
  times the guest is told are what matters most: a sample's host time is
  when it will really be heard, measured on the guest's clock (which ticks
  in nanoseconds), from an anchor moved every turn. Get that wrong and VLC,
  thinking every buffer late, plays silence. Leopard's AudioFile reader
  rejects MIDI files used by NSSound (for example GL Golf's music). When it
  rejects a callback-backed MIDI file, `src/audio/midi.cpp` reads the
  sequence with the Mac's MusicSequence, renders it on demand through the
  Mac's DLS synthesizer, and gives the guest a seekable PCM WAVE through
  AudioFile callbacks. NSSound still controls playback and looping; other
  file types stay with Leopard's AudioFile reader.
- **Calling on, and passing on.** Taking over works on Intel guests too:
  the host call is `int 0xFE` and its number, arguments come off the stack,
  and the function returns by popping its return address. A function taken
  over can keep the guest's own version callable: its first instructions
  are copied out, followed by a jump to the rest, so a handler can pass on
  the calls it doesn't handle itself.
- **Atomic instructions.** An Intel `lock` instruction (and `xchg`) has to be
  atomic against every access to its operand, including plain stores from
  other threads, and not only against other locked instructions. The
  interpreter reads the operand once, holds the write back, and puts it in
  with a real compare-and-swap; if another thread changed the operand
  meanwhile, the instruction runs again. (At first they only took one
  process-wide lock, so a plain store landing in the middle, such as a
  spinlock's release, was lost; faster translated code made that common
  enough to crash Intel VLC about one run in three.)
- **The person running cocaine.** The system root has its own list of
  users, which doesn't include you. The first time a session starts, the
  server adds a record for the current user (name, numbers, home folder) to
  the directory daemon's files in the system root, and programs that read
  `/etc/passwd` directly are shown a copy with a line for you.
- **Your home folder, minus its Library.** Old programs see your real home
  folder, so they can open your documents. They do not see its `Library`
  or its dot-files: those are where programs keep settings, caches and
  keychains, under names that haven't changed in twenty years, and an old
  program let loose there reads settings it can't parse and writes over
  the ones a current program of the same name uses. The guest system has
  its own, in the system root at the same path
  (`sysroots/10.5/Users/<you>/Library`).

