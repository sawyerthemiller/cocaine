# Known gaps

Moved from the README on 2026-11-18. What doesn't work yet, or works only partly, and why. The README keeps a short list of the ones a user would notice.

- PowerPC PEF/CFM launch is experimental. EV Nova 1.0.0 initializes its
  registration tool on 10.5.6 with `F_PREALLOCATE` implemented. The tested
  installation's unregistered trial reports a scenario checksum mismatch;
  whether the files or the environment cause it is not established.
  Do not bypass the game's registration or integrity checks.
- Ghost Recon Demo's extensionless app bundle is recognized and loads via
  LaunchCFMApp. With volume metadata fixed it gets past its installation
  check and reaches its menu using the AGL and GLU hooks. The checked
  10.5.6 PowerPC virtual display now offers real 640x480 modes at 16 and
  32 bits, so its menu no longer draws into a corner of an 800x600 surface.
  The launcher retains its 800x600 starting display as a compatibility
  default. The user confirmed that gameplay displays after the larger
  mode switch was fixed.
- AppleDouble resource forks are exposed read-only from temporary files when
  native metadata is missing. Their file descriptors do not retain the
  original fork's path, and writable sidecar forks are not supported.
- Virtual display mode switching is tracked only for the checked 10.5.6
  PowerPC CoreGraphics implementation. 8-bit modes and modes exceeding the
  window server's shared framebuffer are not supported.
- The viewer follows the guest display, not an individual game's window.
  Ghost Recon Demo's AGL drawable has no associated Carbon window, so it
  must fall back to the display mode rather than a window's bounds.

- Some Mach port rights are never released: rights that arrive with
  requests from other processes, IOKit's registry and iterator ports, and
  the ports of threads that have ended. That only adds up over hours of
  heavy system traffic.

- OpenGL and OpenAL pass through for both CPUs, but only the
  functions used by the games tested so far: much of OpenGL 1.x, some AGL,
  CGL and GLU calls, and OpenAL 1.1's playback calls. Another game will want others, which
  announce themselves on the first call ("... is not passed through to the
  Mac yet"). AGL and CGL pixel-format requests are logged but not enforced: the
  existing fixed accelerated format is used instead. CGL fullscreen draws
  into the captured guest display (Postal 2's animated intro and menu render).
  AGL fullscreen (`aglSetFullScreen`) isn't done, so AGL games have to
  be set to play in a window. DrawSprocket runs as guest code; Ghost Recon
  Demo uses it to select the virtual display's 640x480 menu modes.
- Sound comes out of one stereo 44.1 kHz device, with no input. The guest's
  Component Manager finds no components at all under cocaine (why is not
  known yet); the output units are supplied instead, but other components
  (Sound Manager's, QuickTime's codecs) still can't be found.
- OpenGL games are held to 120 frames a second by default (`COCAINE_GL_FPS`). Past
  the screen's refresh, each
  frame only costs power, twice, since the guest's window server composites
  each one. If experimental 2x UI drawing is explicitly enabled, a game's
  window is enlarged by the emulated window server every frame, which was
  most of what it cost while playing
  (about a quarter of a core on PowerPC, two fifths on Intel, for Marble
  Blast's 800 x 600).
- Leopard's 2x drawing was never finished: Carbon programs that don't ask
  for it, Marble Blast among them, are drawn at 1x and enlarged, which looks
  as it did before, only bigger. All programs now default to 1x; viewer-side
  enlargement preserves coordinates but cannot add detail to bitmap artwork
  or fonts. OpenGL still renders at guest drawable resolution, not at a
  separate Retina resolution. SDL 1.2's QuickDraw driver mixes point and pixel
  coordinates at 2x,
  which can leave software windows blank. Forcing COCAINE_UI_SCALE=2 can
  bring this back; the default fixes Postal 2's startup warning. Snes9x HQ!
  uses a 1280x800 display with 1x UI scaling and doubled viewer pixels because
  its game window is partly off-screen at 2x.
  Postal 2 uses a smaller 1024x768 guest display and doubles pixels in the
  viewer instead, enlarging the menu without enabling that broken drawing mode.
- A default button's blue pulse stuttered at 2560 x 1600 with 2x UI drawing: measured
  on 2026-11-20 in VLC's Open Source dialog (PowerPC), the button gets a
  new picture about twice a second, in step with the text cursor's blink,
  where at `COCAINE_DISPLAY=1280x1024` it gets about thirty and pulses
  smoothly. The guest's display memory and the viewer's file agree, so the
  copy to the viewer isn't it; a hook on `CGXFlushWindow` changed nothing.
  The two runs differ in size and in 2x drawing (`COCAINE_UI_SCALE`), which
  weren't told apart; 2x is suspected, with the button redrawn only when the
  main thread next wakes. The default is now 1x on a logical-sized display;
  the original combination has not been rechecked.

- Guest pages are 4 KB and Apple Silicon pages are 16 KB, so guest memory
  protections are tracked but not enforced. The exception is the zero page,
  which is arranged to have a host page to itself: null-pointer accesses
  fault as they should.
- A task port sent to another process can answer some kernel calls, including
  shared-memory mapping and `pid_for_task`, but not all of them (reading
  another process's memory, for example).
- IOKit has almost no devices: most searches come back empty. There is a
  power-management root that never announces sleep or wake, and an input
  system with one mouse and one keyboard's worth of events, fed from the
  viewer. Mounted disk images and optical media have only the minimal CD
  registry entries needed for detection; raw disc commands, eject and media
  notifications are not emulated. No display device: the window server makes
  do with a virtual one.
- The guest's display is one desktop in a host window (a conventional mode
  fitting the host's logical desktop unless `COCAINE_DISPLAY` or a game
  default says otherwise),
  with no pointer of its own drawn on it. Nobody is logged in to the guest
  system: the login window sets the session up and stops there.
- The Intel translator leaves MMX, string moves, 16-bit operands and the
  rarer SSE instructions (packs, saturating arithmetic, shifts by a
  register, the SSE3 extras) to the interpreter (about 65 million
  instructions a second on an M4), so code made mostly of those is no
  faster than before. x87 arithmetic is done in software at its full 80
  bits, so `long double` code gains little from translation. The SSE
  registers are loaded and stored around every instruction that uses
  them, which is most of what a floating point loop now costs.
- Neither translator ever frees what it has translated. Each has room
  for 128 MB of ARM64 code, which is a lot of program; past that, new code
  is interpreted. The less common AltiVec and floating point instructions
  are handed to the interpreter one at a time, so code that is mostly
  those runs no faster than before.
- The pasteboard server isn't running, so copy and paste between apps
  doesn't work.
- The daemons start afresh with each run that needs them, which costs a
  second or so every time.
- `task_for_pid` finds guest processes in the same session once they have
  registered their task ports, and accepts only the caller's own task port
  as its first argument. A spawned process may not have registered yet when
  `posix_spawn` returns; an exec replaces its guest task port.
- Each run of cocaine is a session of its own, with its own daemons: two
  apps started separately can't see each other.
- A guest's own crash (a bad memory access, an illegal instruction) always
  ends it with cocaine's report. A handler the guest installed for that is
  not called.
- A signal that lands in the instant between cocaine checking for one and a
  system call starting to block waits until that call returns. `sigsuspend`
  is exempt: it is exact.
- `ps` and `top` don't work (no process list `sysctl`).
- A guest that ends by a signal it sent itself for a crash (`abort`) exits
  with status 128 + the signal number rather than dying by the signal, to
  keep this Mac from writing a crash report about cocaine each time.
- A shared file mapping is really shared when the guest lets the kernel pick
  the address and the file offset is a multiple of 16 KB. Otherwise, and for
  all private mappings, the guest gets its own copy of the file's bytes.
- Memory shared by passing a port can be mapped at an address of the
  guest's choosing only if that address sits the same distance into a host
  page as where it came from (which is how the window server uses it).
- A few Intel system programs (the font server and the login window among
  them) are encrypted on disk, and decrypting them takes keys that come
  from an Intel Mac's hardware (see "What you need" in the README). Without the
  keys those programs can't run and Intel apps get no windows. Their
  PowerPC halves are not encrypted.
- Now and then a system daemon crashes while a session is starting (seen
  once with the Intel directory daemon). It is started again, and nothing
  waits on the window server's session for more than 25 seconds, but the
  cause isn't found.
- x87 arithmetic is exact 80-bit, but its transcendental instructions (`fsin`,
  `fyl2x` and friends) are computed in double precision, so their last few
  bits differ from a real x87.
- Floating point exception flags are not tracked, and neither are the
  flush-to-zero modes of SSE and AltiVec. `rcpps`, `rsqrtps` and AltiVec's
  estimate instructions return exact results where real CPUs approximate.
- The importer skips files that are hard links to other files (manual pages
  and a few tools such as `/bin/ln`), because this folder doesn't allow
  creating hard links.
- Guests can't launch other apps through Launch Services: loginwindow's
  `LSOpenApplication` calls fail with -10827, so there is no Dock, Finder or
  SystemUIServer (on hold, by the user's choice, 2026-11-19). The desktop
  picture the Dock would draw is drawn by cocaine instead.
- Launch Services can't save the user's copy of its database: its `lssave`
  helper can't work out the user's cache folder (the lookup is in
  CarbonCore, not yet understood), so it writes to an empty path plus `~`,
  which lands in the system root as `sysroots/10.5/~`. Harmless: the
  database is built again in memory on each run.
- A few file system calls Leopard's libraries make while saving and copying
  are not there yet. Each is logged once as "unimplemented system call" and
  fails with "not supported".
  - `fsetxattr` (237): `NSString writeToFile:atomically:encoding:error:` and
    TextEdit store a text file's encoding as an extended attribute. The file
    is written without it. Extended attributes are also listed as none and
    read as absent.
  - `chmod_extended` and `fchmod_extended` (282, 283): `copyItemAtPath:`
    uses them to give the copy its source's permissions and access control
    list in one go. The copy is made and has the permissions (checked);
    whether a list on the source reaches it is not checked. The C library's
    `acl_set_file` family is built on them too, so a guest can set a list
    only through `setattrlist`, as Carbon does. The `stat_extended` family
    still reports every file as having no list; `getattrlist` reports the
    real one.
  - `exchangedata` (223): only Carbon's `FSExchangeObjects` uses it, and it
    fails. Cocoa's saves don't need it: they go through
    `FSPathReplaceObject`, which renames. On an APFS disk Carbon answers -50
    by itself without asking the kernel; on HFS+ it asks. Swapping the two
    names (`renamex_np` with `RENAME_SWAP`) would not be the same thing:
    `exchangedata` swaps the contents and leaves each name with its file ID,
    and which file a guest's FSRef names after a swap of names hasn't been
    checked.
- Leopard's open and save panels don't work: the file browser stays
  blank, and VLC's Browse button crashes in `DASessionScheduleWithRunLoop`
  (no disk arbitration daemon). Tried on 2026-11-20 and rolled back: running
  diskarbitrationd, and showing this Mac's picker in NSOpenPanel's place.
  Opening Leopard's panel at all upsets the program's window server traffic
  (oversized event messages, replies landing on the wrong call,
  `CGSAddTrackingRect` errors), leaving its dialogs unresponsive; that is
  the thing to fix first.
