# Debugging, tests and layout

Moved from the README on 2026-11-18.

## Environment variables

For looking into what the system daemons are doing there are a few
environment variables, none of which change anything unless set:

| Variable | What it does |
|---|---|
| `COCAINE_DAEMONS=all`, `none`, or labels | which of Apple's daemons the session may start, instead of the usual list |
| `COCAINE_NO_VIEWER=1` | don't open the viewer window |
| `COCAINE_DUMP=address:bytes:seconds:file` | write that guest memory (hexadecimal address) to `file.<pid>` after that long |
| `COCAINE_SCRIPT="wait 20; click 300 400; key 28 56; shot /tmp/a.raw"` | play mouse and key actions into the guest's input system: `wait`, `move`, `down`, `up`, `dragto`, `click`, `drag`, `key`; `settle` and `change X Y W H` time how fast the window server and the display respond; `shot FILE` saves the published display at its current mode's dimensions, without forcing a refresh (see `run_input_script` in `src/kernel/iokit.cpp`) |
| `COCAINE_POKE=x,y,seconds` | short for a script that, after that long, clicks at x,y and types 8 and 9 |
| `COCAINE_NOTE_INPUT=1` | for three seconds after each click or key, log every message the window server sends and receives, with times |
| `COCAINE_VIEWER_SNAPSHOT=file.png` | have the viewer save what its window shows, twelve seconds in |
| `COCAINE_VIEWER_SCALE=actual` | viewer sizing: `actual` is one guest pixel per Mac logical point, `integer` uses the largest whole physical-pixel scale fitting the window, `fit` scales proportionally. Also in the View menu (Command-1/2/3). Takes precedence over `COCAINE_VIEWER_DOUBLE` |
| `COCAINE_VIEWER_DOUBLE=0` | legacy override: one physical screen pixel per guest pixel, or two with `1`, fitting down if necessary. Only applies when explicitly set; game-specific defaults are retained. The View menu clears this override for that viewer |
| `COCAINE_PICK_ANSWER=file` | answer the file picker's requests from `file` instead of showing this Mac's open or save panel. The file is read afresh for each request, so a test can rewrite it between panels: an optional first line `delay N` (seconds, may be a fraction), then one absolute host path a line; no paths means cancel, a missing file is an error. The answer goes through the same checks as a real pick (`src/viewer/picker.mm`): one the real panel could not give (a file where only folders are allowed, several where one is, a type the panel would grey out) or a place the guest can't reach is an error, which the guest sees as cancel |
| `COCAINE_PICK_OPEN=path`, `COCAINE_PICK_SAVE=path` | the same for one kind of panel, without a file: paths separated by newlines, set but empty for cancel. `COCAINE_PICK_ANSWER` comes first if both are set. `tests/run.sh` sets both empty, so no test can put a real panel on the screen |
| `COCAINE_PICK_DELAY=seconds` | with `COCAINE_PICK_OPEN` or `COCAINE_PICK_SAVE`: wait that long before answering, as a person would (the helper still leaves at once if the guest goes away) |
| `COCAINE_PICK_LOG=file` | append one line to `file` for each request the file picker's helper gets: `request`, then every field as `key=value` with tabs between (tabs, line ends and backslashes inside a value written as `\t`, `\n`, `\r`, `\\`). Counting the lines proves a script really reached a panel |
| `COCAINE_PICK_REAL=1` | show the real panel even with `COCAINE_NO_VIEWER` or `COCAINE_SCRIPT` set. Without it such a run has nobody to answer a panel, so the helper answers cancel ("no viewer - file picker answered cancel") |
| `COCAINE_GUEST_PANELS=1` | leave AppKit's open and save panels alone: nothing of `src/kernel/panels.cpp` is planted and a guest builds Leopard's own panel, which dies in Disk Arbitration. For working on the panels themselves |
| `COCAINE_NOTE_MSG=number,first-last` | log every process that sends one of those Mach requests, and the answer it gets |
| `COCAINE_CALLS=address,address` | log each call a PowerPC guest makes to one of those addresses, with its arguments |
| `COCAINE_WATCH=address` | log each PowerPC store of a word to that address, and the code that made it |
| `COCAINE_SESSION_TRACE=1` | log every request the session server receives, with its sender |
| `COCAINE_DAEMON_STRACE=1` | pass `--strace` to every daemon the session starts; their system calls land, interleaved, in the session log |
| `COCAINE_TRACE_BYTES=n` | how many bytes of each Mach message `--strace` shows (48 unless set); more helps when working out a protocol |
| `COCAINE_NOTE_PATCHES=file` | append a line to `file` each time CoreGraphics is mapped, saying how many of the display-size patches took |
| `COCAINE_JIT=0` | interpret guest code instead of translating it |
| `COCAINE_JIT_BARRIERS=1` | Intel translator: put a memory barrier after every guest instruction (an experiment in memory ordering; slow) |
| `COCAINE_JIT_STATS=1` | on exit, say how much was translated and which instructions were left to the interpreter (AltiVec ones too: opcode 4 with the low 6 bits for the three-source forms, else the low 11); a number of seconds instead also reports then, for programs that don't end |
| `COCAINE_FAULT_MEM=r6:160,@2eebd0:16` | when a PowerPC guest crashes, also print memory at those registers or addresses (the crash report always names the library each caller is in) |
| `COCAINE_NAN=1` | report the first places a PowerPC guest's floating point arithmetic makes a NaN out of numbers, with the inputs (interpreted, so slow) |
| `COCAINE_GL_FRAME=n:file` | save OpenGL frame n, as cocaine read it back from the Mac's GPU, to `file` (raw, A R G B, bottom row first) |
| `COCAINE_GL_FPS=60` | the most frames a second an OpenGL game is shown at (120 unless set; 0 for no limit) |
| `COCAINE_DISPLAY=1280x1024` | the guest display's size (largest conventional mode fitting the host's available logical desktop, allowing room for the viewer's frame; 1280 x 800 with no screen). Resolved once and inherited by the session. Postal 2 keeps its 1024 x 768 default and Snes9x HQ! its 1280 x 800, with doubled viewer pixels; the launcher keeps Ghost Recon Demo at 800 x 600. Tests use 1280 x 1024 |
| `COCAINE_WALLPAPER=path` | the guest's desktop picture, a JPEG or PNG on this Mac (`images/linen.jpeg` by default, else Leopard's `DefaultDesktop.jpg`); `none` leaves the window server's plain grey (see `src/kernel/wallpaper.cpp`) |
| `COCAINE_MENU_LOGO=apple` | keep Apple's logo in the guest's menu bar instead of cocaine's (a pile of powder, drawn in each logo's colour into a copy of Leopard's art file, `.cocaine/art/` in the system root; see `src/session/art.cpp`) |
| `COCAINE_UI_SCALE=2` | opt into Leopard's unfinished resolution-independent drawing (1 by default for all display sizes). Values from 1 to 3 override the default; larger-than-1 drawing can break QuickDraw/SDL coordinates and clip windows |
| `COCAINE_AL_DUMP=dir` | write each buffer of sound a game hands OpenAL to `dir`, one raw file each, 16-bit samples in this Mac's byte order, named by order, buffer, format and rate (see `src/al/al.cpp`) |
| `COCAINE_AL_TRACE=1` | log each OpenAL call that leaves an error, and the source, listener and buffer-name calls with their arguments |
| `COCAINE_AUDIO_DUMP=file` | also write everything the guest plays to `file`, as raw 44.1 kHz stereo 32-bit floats |
| `COCAINE_FAULT_CODE=1` | when an Intel guest crashes, also print the code around the instruction and the native backtrace |
| `COCAINE_JIT_VERIFY=1` | run each translated Intel SSE instruction through the interpreter too, and report any difference |
| `COCAINE_JIT_POISON=1` | report any read of flags the Intel translator decided nobody reads |
| `COCAINE_JIT_SKIP=70,62` | leave those Intel SSE instructions (by second opcode byte) to the interpreter |
| `COCAINE_JIT_FLAGS=0` | Intel translator: work out every instruction's flags, skip none, fuse no compare with its branch (3 is the usual) |
| `COCAINE_JIT_WRITABLE=0` | Intel translator: interpret code in writable memory instead of translating it |
| `COCAINE_JIT_MATHS=0` | have the Intel translator leave floating point and vector instructions to the interpreter, as it did at first |

## Tests

```bash
ctest --test-dir build --output-on-failure
```

Each test in `tests/guest/` is built as a guest program and as a reference,
and the emulated program must print exactly what the reference prints, bit
for bit. Each is run twice, translated and interpreted, and each way must
match. A second, quick test checks every ARM64 instruction the translators
can emit against the system's assembler
(`tests/a64-check.sh`). There are three kinds of reference:

- Plain C tests (`t_int`, `t_fp`, `t_fma`, `t_signal`) are built natively for
  this Mac. `t_signal` interrupts a long calculation with a timer's signals
  and checks that the result comes out the same.
- The Intel-only tests (`t_x87`, `t_sse`) are built for 64-bit Intel and run
  under Rosetta, which has a real x87 and SSE to compare against. They are
  skipped if Rosetta isn't installed.
- No current Mac has AltiVec, so `t_altivec` writes every operation twice:
  the real instruction for PowerPC, and a plain C model of the documented
  behaviour for the reference.

If a system root has been imported, the tests also run real Mac OS X programs
from it on both CPUs and check their output. That includes a shell script
(`tests/guest/procs.sh`) that exercises processes, pipes and signals, Python
programs that use threads and fork from them, and `t_ipc`: two processes
exchanging Mach messages with port rights and out-of-line memory in them.
`task-for-pid.py` checks self and child task lookup, immediate post-fork
lookup, exec, process exit, and rejection of processes outside the session.
`access-extended.py` asks the kernel's several-questions-at-once permission
call what Carbon asks, and `save.py` saves Cocoa documents (PyObjC, no
window) onto files that already exist, with a backup, and over a file with
an access control list. Both work in the guest system's own Library; give
either a folder name as its argument to run it somewhere else.
Three of Apple's daemons are tested through their clients: the guest system
must know the person running the tests by name, and one program must hear a
notification that another posts (`tests/guest/notify.sh`). And Calculator is
run on both CPUs. It must come up in front with Apple's window server
behind it and be handed a key typed some seconds in (which takes the window
server, the font server, the login window and the whole input path). If the
keys for the Intel system programs aren't there, the Intel one must instead
get as far as it can: through Launch Services to asking for the window
server.

Two host tests cover the file picker's helper without any guest or panel.
`picker` (`tests/picker.mm`, which includes `src/viewer/picker.mm` the way
`tests/viewer-resize.mm` includes the viewer) checks its plain functions: the
messages, the open panel's type filter against the type lists VLC,
Transmission and TextEdit really pass, how a saved name is finished, the
stand-in answer file, the log line, and the path check, in a made-up system
root and home folder under a temporary folder. `picker-process`
(`tests/picker-process.cpp`) starts `cocaine --pick` as a guest's process
would, with a socket pair as descriptor 3, and checks the replies to each
`COCAINE_PICK_*` variable, that hanging up during a delay ends the helper
within a second, and that an impossible answer is an error. The viewer test
checks what the viewer holds back from the guest while a panel is up.

The guest side of the panels is tested with real guests, on both CPUs (the
Intel ones only with the keys, like Calculator), the stand-in answering
every time. `tests/guest/panels.py` is one PyObjC program that runs some
forty panels, rewriting the stand-in's answer file before each: fifty open
and fifty save panels made and let go with no window, every setting read
back, each way of running a panel with a pick and a cancel, a folder,
several files, types, a save with its extension added, a pick that takes
three seconds while a signal arrives, two threads asking at once, sheets
and a modeless panel answered from the run loop, a sheet on a modal dialog
that stops the modal session from its did-end method, and the paths a guest
can't reach. After each panel it reads `COCAINE_PICK_LOG`: there must be one
more request, saying what the panel was set to, so a panel that never
reached the picker fails. `tests/run.sh` then checks from outside that the
count matches, the saved file is where the stand-in said, and nothing was
written to a refused place. TextEdit is run with a script: Open cancelled,
the Apple menu opened (a panel once left the window server unable to open
it), a file opened, a letter typed and saved over it, and Save As to
another name, the test handing the stand-in each answer as the requests
come in. A guest that ends during a 60-second pick, by a thread of its own
calling exit or ended from outside, must take the helper with it within
seconds (`panel-wait.py`), and a Carbon `NavGetFile` from Python must come
back cancelled (`nav.py`).

The volume test always checks the guest root. To exercise a mounted disk
image too, set `COCAINE_TEST_VOLUME` to its `/Volumes/...` mount point before
running the suite. Optionally set `COCAINE_TEST_VOLUME_MARKER` to a file
relative to that mount; the test checks it through both the mount and `/.vol`.

The built guest programs live in `tests/bin/`, so running the tests needs
nothing extra. Rebuilding them after an edit needs Homebrew LLVM for the
PowerPC half: `sh tests/build-guest.sh`.

## Source layout

```
src/main.cpp        command line tool
src/macho/          Mach-O parser
src/mem/            the 4 GB guest address space
src/cpu/ppc/        PowerPC: the interpreter, AltiVec, and the translator to ARM64
                    (ppc_jit.cpp, with its instruction encoder in a64.h)
src/cpu/x86/        32-bit Intel: the interpreter, x87 (with its 80-bit maths), MMX
                    and SSE, and the translator to ARM64 (x86_jit.cpp)
src/kernel/         the fake kernel: process loading, BSD system calls, sysctl,
                    Mach ports and messages, the comm page, path translation,
                    getattrlist, sockets, threads, processes, signals, IOKit
                    and its input system, decrypting protected programs,
                    taking over guest library functions (hooks.cpp), and
                    AppKit's open and save panels (panels.cpp)
src/session/        the session server, cocaine's stand-in for launchd
src/viewer/         the window that shows the guest system's display, and the
                    helper that shows this Mac's file picker (picker.mm)
src/gl/, src/al/    OpenGL and OpenAL for guests, done by the Mac's own
src/audio/          Core Audio's hardware layer and output units for guests, played
                    through the Mac's own output
tools/              import-sysroot.sh; find-mig-stub.py, for working out the
                    private message protocols of system daemons; and
                    ppc-dis.py, a small PowerPC disassembler for reading
                    Apple's PowerPC code
tests/guest/        test programs and their tiny runtime
tests/tools/        elf2macho.py, a small linker for PowerPC test programs, and
                    a64-check.cpp, which checks the translators' ARM64 encodings
docs/               screenshots of firsts
tests/bin/          the built guest test programs
tests/bench/        a maths benchmark for the Intel translator, and how to build and time it
tests/bin-dynamic/  ones linked against the real libSystem (need a system root to run)
sysroots/           imported Mac OS X system roots (not in git)
```
