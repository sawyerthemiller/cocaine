---
name: cocaine
description: Working on cocaine, the layer that runs Mac OS X 10.5 PowerPC and 32-bit Intel programs on Apple Silicon. Use for any change, debugging session, test run or question about this repository (its CPU translators, fake kernel, session server and daemons, window server and viewer, OpenGL/OpenAL/Core Audio passthrough, menu logo art).
---

# cocaine

cocaine loads a Leopard program into one host process per guest process,
translates its PowerPC or i386 code to ARM64, runs Apple's own libraries and
daemons from a Leopard system root as guest code, and answers their kernel
calls itself. The README is for users and kept short; the detail lives here:

- [how-it-works.md](how-it-works.md): the design of each piece, and why.
- [roadmap.md](roadmap.md): each step taken and what was learned on the way.
- [known-gaps.md](known-gaps.md): everything that doesn't work yet, and why.
- [debugging.md](debugging.md): every `COCAINE_*` variable, the tests, the source layout.

When a piece's design changes, update its section here; when a user-visible
feature or gap changes, update the README's short lists too.

## Rules

- **The SMC keys** (OSK0/OSK1) live only in `sysroots/10.5/.cocaine/osk` or
  `COCAINE_OSK`. Never write their characters anywhere: not in source, docs,
  tests, commit messages or chat. Refer to the file or the variable.
- **Apple's files in the system root stay untouched.** cocaine's own files go
  in `sysroots/10.5/.cocaine/` (the key, `art/`), and guests are shown them by
  `Process::host_path` (src/kernel/paths.cpp), which already redirects
  `/etc/localtime`, `/etc/passwd`, `/.vol/` paths and the art file.
- **Guests never touch the real `~/Library` or home dot-files**: those are
  redirected into the system root (`private_home_path`). Keep it that way.
- `sysroots/`, `test-games/`, `test-canidates/`, `*.dmg`, `*.mp3` and `build/`
  are git-ignored. Delete with the Trash, not `rm -rf`, for anything of the user's.
- The repo is public on GitHub (`sawyerthemiller/cocaine`), and others send
  pull requests: fetch before working, since `main` may have moved. Everything
  pushed, release notes included, can be read by anyone, so the key rule above
  matters all the more. Commit or push only when asked; commits end with the
  Co-Authored-By line.

## Building and testing

```bash
cmake -S . -B build -G Ninja && cmake --build build
ctest --test-dir build --output-on-failure      # about 4 minutes; 149 guest checks
```

Filter the user's Mac tweak noise (`AppleSharpener`, `BlurTweak`) out of
every command's output. The suite runs each guest test translated and
interpreted; `t_x87`/`t_sse` compare against Rosetta, and if Rosetta's
`oahd` hangs they get stuck: count stuck processes with
`ps -ax -o stat,command | grep '\.host' | grep -c '^U'`. `tests/run.sh` sets
`COCAINE_DISPLAY=1280x1024`. Look at `build/Testing/Temporary/LastTest.log`
for the per-test lines (`149 passed, 0 failed`).

## Running guests without looking at them

Screenshots are costly; prefer numbers.

- `COCAINE_NO_VIEWER=1` runs with no window. Run in the background, sleep,
  then `pkill -f build/cocaine`.
- `COCAINE_SCRIPT="wait 25; click 20 11; wait 2; shot /path/a.raw; key 53 0"`
  plays input (coordinates are guest display pixels; set `COCAINE_DISPLAY`
  explicitly for repeatable scripts; the Apple menu is about 20,11 at 1x) and saves the display
  raw: 4 bytes a pixel, unused byte first, then R G B (PowerPC sessions).
  Compare regions of two shots in Python to prove something changed; crop
  a small area into a PNG (zlib + struct, no PIL) only when it must be seen.
- Session logs: `$(getconf DARWIN_USER_TEMP_DIR)cocaine-session-<pid>.log`
  (the five newest are kept). Daemons' output and syslog land there. `-v`
  logs daemon starts, `-vv` debug lines.
- `COCAINE_SESSION_TRACE=1` logs every lookup/check-in with its sender:
  the way to see deadlocks between daemons. `COCAINE_DAEMON_STRACE=1` passes
  `--strace` to every daemon (all interleaved in the session log).
- Closing the viewer window sends a kind-0 record down its pipe; the window
  server's process then SIGTERMs the session root (pid parsed from the
  session name `cocaine.session.<pid>.<n>`), SIGKILL after 3 s.

## Reading a crash

A guest crash prints registers and, on PowerPC, each caller as
`address  Library + 0xoffset`. Name the functions with
`/opt/homebrew/opt/llvm/bin/llvm-nm --arch=ppc7400 -n <library>` (the slice
is `ppc7400`; `ppc` fails) and the nearest symbol at or below the offset.
HIToolbox's internal symbols are stripped, so names there mislead.
Disassemble with `lipo -thin ppc7400` then `tools/ppc-dis.py file addr count`
(Xcode's tools can't do PowerPC). `COCAINE_FAULT_MEM` and `COCAINE_FAULT_CODE`
add memory and code dumps.

A null pointer read at a small address often means Apple code used the result
of a request cocaine didn't answer. "unanswered Mach message N" lines name
it: MIG ids 2800s are IOKit, 3400s task, 3800s vm, 200s host, 400s bootstrap.
An unanswered message gets `MIG_BAD_ID`; give it a real answer, or a proper
error such as IOKit's `kNotFound`.

## Daemons (src/session/session.cpp)

- `kWorkingDaemons` and `kWindowDaemons` list the Apple daemons a session may
  start (on demand, at the first lookup of their service). Others are told
  "no such service". configd was added on 2026-11-18 after the Apple menu
  crashed in SCPreferences without it. Leopard 10.5.6's window server needs
  `hidd` and waits for `com.apple.iohideventsystem` until it starts.
- `pboard` supplies the guest session's pasteboard; lookups wait until it
  checks in so its shared memory exists before clients use it. The viewer's
  clipboard bridge connects text, images and files to macOS using a guest
  Python 2.5 helper.
- `kExtraArguments`: configd runs with `-b` (no plug-ins: they'd manage the
  Mac's real network, power, Bluetooth) and `-d` (stay in the foreground: it
  only does so for launchd as pid 1, and guests see the session server's pid).
- `kHiddenServices`: configd is told DirectoryService doesn't exist, because
  DirectoryService waits on configd while starting and configd looks up a
  user while starting: a deadlock that stalled the window server. Its library
  falls back to `/etc/passwd`. A daemon looking up its own not-yet-collected
  service is also refused, for the same reason.
- `kPublishedLater`: services a daemon publishes itself (the window server's).
- Adding a daemon: read its launchd plist, start it with `-v`, and watch
  `COCAINE_SESSION_TRACE` for lookups that never get answered.
- loginwindow's `LSOpenApplication` calls fail (-10827): guests can't yet
  launch other apps through Launch Services, so no Dock, Finder or
  SystemUIServer.

## Taking over guest functions (src/kernel/hooks.cpp)

`HookLibrary{suffix, prefix, specs, count, intel}` names a library by its
path's end; listed functions get a host call planted at their entry when the
library's code is mapped. Handlers read arguments with `GuestCall`
(`i()`, `f()`, `d()`), answer with `ret()`, can call guest functions with
`call(fn, {args})`, find them with `symbol("Name")`, get guest memory with
`scratch()`, and pass through with `original()` when `keep` is set.
`native_function` makes a guest-callable function (used for Core Audio's
worker thread). OpenGL/OpenAL (src/gl, src/al), Core Audio (src/audio) and
the window server's `CGXUpdateDisplay` (screen publishing) work this way.
A spec can also name an Objective-C method as written (`-[NSSavePanel
filename]`, found by its local symbol, the category passed over), and a
library with `whole` set is taken over only if every spec is found: AppKit's
open and save panels (`src/kernel/panels.cpp`). In a handler, guest code
that throws never comes back: hold nothing across a guest call. A guest
function called through its address gets that address in r12 on PowerPC
(`call_guest_args`), which an Objective-C method finds its data by.

## The menu bar logo (src/session/art.cpp)

Leopard draws the Apple menu's logo from PNGs in CoreUI's `SArtFile.bin`:
header `u16 version=1, u16 count, u32 data start`, then `count` u32 offsets to
16-byte entries `u16 kind (2 = PNG, 1 = PDF), u16, u16 width, u16 height,
u32 length, u32 offset from data start`, then the data (little-endian). Eight
entries are logos (19x22 and 22x22: black, white for "open", graphite, blue);
they're told from the same-sized search-field buttons because the logo's top
row (the leaf tip) is over a pixel right of centre. Each session writes a
copy with those entries pointing at a pile of powder drawn in the logo's
colour, appended at the end, to `.cocaine/art/SArtFile.bin`; `host_path`
shows the guest that copy. `COCAINE_MENU_LOGO=apple` turns it off.

## The wallpaper (src/kernel/wallpaper.cpp)

Leopard's Dock draws the desktop picture, and the Dock can't be launched
(see Launch Services above), so cocaine draws it from inside loginwindow.
When loginwindow's process receives the window server's
`kSetFrontProcessDone` (the moment it tells the session server
`kSessionReady`, in `guest_mach_msg`), `Process::draw_wallpaper` calls the
guest's CoreGraphics: `CGSMainConnectionID`, `CGDisplayBounds` (a struct
return: hidden pointer first), `CGSNewRegionWithRect`, `CGSNewWindow`,
`CGSSetWindowLevel` at `CGWindowLevelForKey(2)` (desktop), `CGSOrderWindow`,
`CGWindowContextCreate`, `CGDataProviderCreateWithFilename` with the JPEG or
PNG image constructor, then `CGContextDrawImage` scaled to fill and
`CGContextFlush`. `CGSNewWindow` takes floats, so it goes through
`call_guest_args`, which puts float arguments in f1-f13 on PowerPC (each
still using a GPR slot) and on the stack on Intel. A CGRect by value is four
words in GPRs or on the stack on both CPUs. `COCAINE_WALLPAPER` picks the
file (`none` turns it off); the default is the repo's `images/linen.jpeg`,
found from the cocaine binary's path, else Leopard's `DefaultDesktop.jpg`.
The window ignores the mouse (`CGSSetWindowTags` bit 9, as Cocoa's
`setIgnoresMouseEvents:`): taking clicks, it made loginwindow the active
program and the front app stopped getting keys (the suite's Calculator key
test caught it). The bundled picture is exposed at `/.cocaine/linen.jpeg`,
including when the checkout is in the host's private Library folder.

## Files appearing where cocaine was run

Daemons start in the guest's `/` (the system root), as launchd starts them.
Before 2026-11-18 they started in whatever folder cocaine was run from, and
Launch Services' `lssave` helper (started by coreservicesd when an app such
as Marble Blast registers itself) left its database as a file named `~`
there: it can't work out the per-user cache folder for the user's copy, so
its path is empty plus the `~` of its safe-save copy. It now lands in
`sysroots/10.5/~`, harmlessly. The underlying cache-folder lookup (in
CarbonCore) is still unexplained. To catch a guest writing a stray file, log
in `Process::host_path` when the path matches, with `getpid()` and
`exe_path_`, and call `dump_registers` for the callers.

## Cocaine.app, the launcher (gui/)

A SwiftUI app built without Xcode: `gui/build.sh` compiles `gui/Sources/*.swift`
with `swiftc -swift-version 5 -target arm64-apple-macos14.0`, assembles the
bundle with `gui/Info.plist`, draws the icon (the powder pile, light on a dark
tile) with `gui/make-icon.swift` + `iconutil`, and signs ad hoc; `--zip`
makes `gui/build/Cocaine.zip` for a release. Releases are tagged `gui-vX.Y`
(`gh release create gui-v1.0 gui/build/Cocaine.zip`); bump
`CFBundleShortVersionString` with each.

- `Shell.swift`: the tool search path (a Finder-launched app has a bare
  PATH), quick commands, `RunningCommand` (streams output line by line,
  stopped on quit), `LogBuffer` (thread-safe, shown on the main thread).
- `Model.swift`: `AppModel` keeps the checkout path (default
  `~/Library/Application Support/Cocaine/cocaine`) and system root (default
  `<checkout>/sysroots/10.5`) in UserDefaults. "Built" means
  `build/cocaine` exists; the commit it was built from is written to
  `build/.built-commit` after each build through the app, so a hand-built
  checkout shows "unknown commit". Updates: `git fetch`, then
  `git rev-list --count HEAD..@{u}`; update is `git pull --ff-only` (fails on
  local changes, by design). Keys are validated (32 printable ASCII each),
  written as 64 bytes to `<sysroot>/.cocaine/osk` with mode 600, removed to
  the Trash, never logged. `Architectures.of` reads the Mach-O, fat or
  PowerPC PEF header (an app's main program from its bundle) for the Run
  tab's mode checks. PEF is identified but not yet allowed to run.
  `killAll` (the Run tab's Kill All, since 1.3) counts with `pgrep -x cocaine`,
  sends `pkill -x cocaine`, and `-9` to any still there three seconds on; the
  exact name leaves the launcher itself (Cocaine) alone. Log lines are plain
  since 1.2: no `$` before commands, and status lines end in three dots
  ("Updating cocaine...", "Getting source from the GH repo...", "Complete...").
- `SelfUpdate.swift` (since 1.4): on every launch `LauncherUpdate` asks
  GitHub for the releases, takes the highest `gui-vX.Y` tag with a zip, and
  if it is newer than `CFBundleShortVersionString` asks ("An update to the
  Cocaine launcher is now available." / "Do you want to do it?", Yes or No,
  no off switch). Yes downloads and unpacks the zip and hands over to a
  small script that waits for the app to quit, swaps the bundle and reopens
  it. An older release is never offered, and GitHub being unreachable says
  nothing. A release must carry a `.zip` asset or it is ignored.
- `Views.swift`: the tabs. Run is blocked only when the program has no code
  for the chosen CPU; a missing key only warns (command line Intel programs
  don't need it). The Run tab has no recent list: only the last program is
  remembered, for the next launch, under the UserDefaults key `recents` (a
  one-item array, so lists saved by 1.0 still restore their first entry).
  The Keys tab doesn't show where the keys are kept.
- Wording: the user rewrote the app's text on 2026-11-19 in their own style
  (lowercase status words such as "ready", "not built", "up to date"; dashes
  rather than colons, as in "built from abc1234 - older than the source").
  Keep new text in that style, and keep the README's own style too
  (dash-led bullets, collapsible `<details>` sections).
- A copy opened from a checkout's `gui/build/` uses that checkout unless one
  was chosen (`AppModel.enclosingCheckout`).
- To test the logic without clicking, compile `Shell.swift` and `Model.swift`
  with a small `main.swift` that drives `AppModel` (with made-up keys only).