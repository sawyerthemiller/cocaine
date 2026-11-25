# Roadmap and history

Moved from the README on 2026-11-18. Each step, what was done and what was learned getting there (the history explains many odd-looking choices in the code).

1. **Done.** Loader, both CPU cores complete (PowerPC with AltiVec; Intel
   with x87, MMX and SSE through SSE3), test harness.
2. **Mostly done.** Run command line tools from the system root: the real
   `dyld` and `libSystem`, the comm page, Mach traps and messages, the system
   calls they use, threads, processes and signals. Sockets only pass the
   basic calls through.
3. **Done for command line programs.** CoreFoundation and Foundation.
4. **Mostly done.** The system daemons an app talks to while starting. Calculator
   asks for directory services, the notification centres, the security
   server and Launch Services (`coreservicesd`), and aborts without the last.
   Faking such daemons by hand doesn't scale: an earlier stand-in for
   `coreservicesd` got an app through check-in and no further, because
   clients read the daemon's shared-memory caches directly, so a stand-in
   would have to reproduce its internal data structures. The plan instead is
   to run Apple's real daemons from the system root as guest processes too.
   That needs, in order:
   - guest threads (**done**: each guest thread is a host thread);
   - more than one guest process at a time (**done**: each guest process is
     a host process running cocaine, so `fork` is the host's fork and `exec`
     re-runs cocaine on the new program), and with them signals;
   - Mach messages between guest processes, and named shared memory
     (**done**), plus a stand-in for `launchd` to start the daemons
     (**done**; Apple's `notifyd` is the first to run and serve).

   The directory daemon and `dirhelper` followed. What they needed, found
   by trying them: `launchd`'s socket check-in, a way to ask whose task
   port one holds, a power-management object in IOKit to register with,
   file record locks, and being believed when they say they are root.

   Then the security daemon and Launch Services itself: `coreservicesd`
   refuses clients that have no security session ("Client must provide a
   valid sessionID", it says in the session's log), and `securityd` hands
   those out. With both running, an app registers and moves on.

   Still to come here: one session shared by every app the user has open,
   so that apps can see each other and the daemons start once instead of
   with every run; and the smaller daemons apps look for but survive
   without (distributed notifications, configuration, disk arbitration).

   Only the daemons that touch hardware (the window server, audio) then need
   native replacements. Run cocaine with `-v`: it names the session's log,
   which lists every service a program looked for and every daemon started.
5. **Under way.** First window. The plan is the one that worked for the
   other daemons: run Apple's real `WindowServer` as a guest. It draws into
   a display that IOKit provides, so cocaine's IOKit gets a pretend
   framebuffer (ordinary shared memory), and a small native program shows
   that memory in a real macOS window and feeds mouse and keyboard back
   into the pretend input system. The first result will be a whole Leopard
   desktop in one window; showing each app window as its own can follow,
   read from the same server.

   It turned out not to need a pretend framebuffer: finding no display
   hardware, the server makes a "VirtualDisplay" of its own in shared
   memory and draws there. So the picture exists already, in the server's
   memory (`COCAINE_DUMP` writes it to a file, which is how it was first
   seen: one flat colour, the server's background).

   Done so far: the server starts when an app asks for it (the session
   server knows which names it publishes), opens the input system with its
   shared event queue, creates its shared buffers and its display, and
   stays up. Its buffers start one 4 KB page into each allocation, and this
   Mac shares only whole 16 KB pages, so an entry covers the host pages
   around the range and the session server records how far in it starts.
   The server maps those buffers into each app by calling the kernel on the
   app's task port, which the app's own cocaine now answers.

   (A range of guest memory is several objects to this Mac's kernel, and an
   ordinary entry stands for only the first; the entries are made with the
   flag that covers the whole range.)

   First pixels: with `COCAINE_DAEMONS` naming the default daemons plus
   `com.apple.WindowServer` and `com.apple.ATSServer`, and `--arch ppc`,
   Calculator runs and stays running, and the display shows its menu bar.
   That needed paths of the form `/.vol/volume/folder/name` (how Carbon
   names files) to go through the system root, and the two kernel calls the
   Objective-C runtime uses to look at a program's threads.

   The display's pixels start 4096 bytes into its shared memory, 1280 to a
   row, four bytes each.

   Why the window is missing: the app is idle in its event loop, and the
   window server's log says "CGXOrderWinsAndPanels: error retrieving front
   process" (-600). When it puts windows in order it asks Launch Services
   which app is in front, and Launch Services turns the window server away
   because, as a root daemon here, it has no security session ("SessionGetInfo()
   returned error -50 for sessionID=0", in the session's log). Starting the
   window server before the app registers anywhere (which the session
   server now does, holding apps' lookups until the window server has
   published its ports) did not change that. The likeliest missing piece
   is `loginwindow`: on a real Mac it, not the window server, creates the
   security session for a login and tells the window server about it.

   That was it. With `com.apple.loginwindow` added to `COCAINE_DAEMONS`,
   the session server starts Apple's `loginwindow` once the window server
   is up, the complaint goes away, and Calculator's window is drawn on the
   display, shadow and all: the first window.

   The viewer (`src/viewer/`): the cocaine process running the window
   server copies the display into a small shared file (thirty times a
   second at first) and starts `cocaine --view` on it, a plain macOS window
   that shows the file. `COCAINE_NO_VIEWER=1` turns that off. It finds the
   display by a guess (the first big piece of memory the window server
   shares, laid out as 10.5 does), which wants replacing with a proper
   question to the server.

   The mouse: clicks and movement in the viewer go back down a pipe and
   are put into the input system's event queue (`post_input_event`), the
   way the kernel's own input system would. Posting was tested by clicking
   Calculator's "7" at a fixed spot: the readout
   said 7. Keys posted the same way went nowhere, and finding out why took
   four separate discoveries, each from reading the window server's own
   code (Apple left the names in):

   - The queue is laid out differently for the two CPUs. PowerPC programs
     keep 10.4's arrangement, with the queue's read and write marks at the
     front of the shared block; Intel ones have them at the back, and
     everything between shifts by twelve bytes. An event's details are in a
     different place too (28 bytes into the event on PowerPC, 40 on Intel).
     cocaine had the Intel layout for both, and the click had only worked
     by accident: the "buttons held" number happened to sit where the
     PowerPC window server looks for the write mark.
   - The window server gives the kernel a port to be told on when events
     are waiting. Nothing could be sent there, because the right to send is
     made by the message that hands it over, and that message never leaves
     the process here. Without it the server found events on its
     once-a-second timer: clicks took most of a second to land.
   - Keys go to the app in front. Calculator had registered a moment before
     the login window finished setting the session up, been told it was in
     front, and then been replaced without a word, so it never asked for
     the keyboard back and every key went to the login window. Apps are now
     held until the login window is done ("Windows", above).
   - Holding apps back stalled the login window itself: it runs a helper in
     a process group of its own and waits for it, and the session server
     took the helper for an app. It now follows parentage.

   With those, Calculator comes up as the app in front, draws its own menu
   bar, and takes typing (a click on 7, then 8 and 9
   from the keyboard). The window server, the font server and the login
   window are on call by default for PowerPC programs, so there is nothing
   to set. VLC 0.9.10's PowerPC build was the first other app tried, and it
   came up as it was: menu bar, player window, and a first-run question.

   Then two things found by using it. Clicks landed sixteen pixels to the
   left of where they were aimed, which mostly showed on small targets (a
   window's close button could not be hit): the display's pixels start one
   page into its memory, not the 64 bytes more first guessed, so the whole
   picture had been shown shifted. And everything felt slow though neither
   the app nor the window server was busy: the window server was sitting on
   each update for an eighth of a second (see "Windows" in how-it-works.md), on top of a
   viewer that copied and redrew thirty times a second each.

   Not yet: a pointer drawn on the guest's display (this Mac's own pointer
   over the viewer does the job); a logged-in user (the login window sets
   the session up and then waits, so the session is root's and "login not
   done", which apps so far don't mind); and finding the display by asking
   rather than by the guess described above. The server's own log is in the
   system root at `/var/log/windowserver.log`.

   The Intel path. The Intel window server ran from the start, but two of
   the programs it needs, the font server and the login window, are stored
   encrypted. The method is in plain sight in the system root: the "Dont
   Steal Mac OS X" kernel extension's page routine decrypts each 4096-byte
   page as two halves, AES with 256-bit keys in CBC mode from a vector of
   zeroes, the first half with the SMC's key `OSK0` and the second with
   `OSK1`, and the kernel applies it to a protected segment from the fourth
   page of the file on. cocaine's loader now does the same
   (`src/kernel/protect.cpp`) when it is given the keys. With them, the
   Intel Calculator came up on the Intel window server at the first try,
   mouse, keyboard and all: the Intel layout of the input queue, until then
   only read off the code, was right. The Intel build of VLC 0.9.10 followed,
   its windows up about 14 seconds after
   starting it at the time, when Intel code was still interpreted (4.8 s
   since it has been translated).

   Later in this step: bridge the window server to real macOS windows, plus
   events, fonts and the pasteboard.
6. **Done for games and music; video on hold.** Audio, OpenGL and
   QuickTime. VLC playing video and QuickTime are on hold (the user,
   2026-11-19): VLC was only a test app for windows and sound.

   OpenGL and OpenAL came first, for games: both are taken over and done
   by the Mac's own libraries ("Games: OpenGL and OpenAL" above). Marble
   Blast Gold was the first game to try; it crashed on a camera matrix
   full of leftover stack, which led back to OpenGL doing nothing at all
   as guest code. With both passed through it plays, sound and all
   (`docs/first-marbleblast.png`). The Intel half of the universal build
   plays too, the handlers reading arguments from the stack. Still to do:
   full screen, and QuickTime.

   Core Audio's hardware layer came next ("Sound" above), and VLC plays
   music through it, both builds. Getting there found three bugs that weren't
   about sound: the interpreter's `lock` instructions weren't atomic
   against plain stores ("Atomic instructions"); two threads mapping memory
   at once could be given the same range, because finding a free range and
   mapping it were two steps (`find_free` now reserves what it hands out);
   and code that Leopard's Intel graphics libraries write at run time was
   checked for changes only in its first 8 bytes, with the translator's
   look-ahead trusting the code after it.
7. **Mostly done.** Speed: JIT compilers for both CPUs. PowerPC has one
   (`src/cpu/ppc/ppc_jit.cpp`; "The translator" above says how it works).
   Its first version kept every guest register in memory and ran a Python
   loop 4 times as fast as the interpreter; keeping the busiest seventeen in
   host registers made that 12.7 times. AltiVec's common instructions are
   translated to NEON since 2026-11-20 (VLC's Open dialog at 2560 x 1600:
   4.5 s to 1 s). Still to do there: the rarer AltiVec and floating point
   instructions go to the interpreter one at a time (`COCAINE_JIT_STATS`
   lists them), and a compare followed by a branch could be one host
   instruction pair instead of a dozen.

   Intel followed (`src/cpu/x86/x86_jit.cpp`): the common integer
   instructions, with the interpreter's own routines called for arithmetic
   whose flags are wanted. That alone made the Python loop ten times
   faster.

   Floating point came next, because an integer-only translator made
   maths slower, not faster: every floating point instruction ended a
   block, and a Mandelbrot set that the interpreter drew in 6.0 s took
   11.5 s. With SSE translated and x87 called in line it takes 0.62 s.
   A small benchmark (`tests/bench/maths.c`; `sh tests/bench/build.sh maths`
   builds it and `tests/bench/time.sh` times it), each part translated and
   interpreted, on an M4:

   | Part | Interpreted | Integer-only translator | Now |
   |---|---|---|---|
   | Mandelbrot set, doubles | 6.01 s | 11.46 s | 0.62 s |
   | Arrays of singles (packed SSE) | 0.68 s | 1.42 s | 0.06 s |
   | Arrays of doubles, square roots | 1.22 s | 2.45 s | 0.09 s |
   | Conversions between integers, singles and doubles | 1.63 s | 2.40 s | 0.24 s |
   | `long double` arithmetic (x87) | 0.40 s | 0.58 s | 0.28 s |
   | Python loop of floating point arithmetic | 27.4 s | 4.1 s | 2.2 s |

   Flags were what was left. A wanted flag used to cost a call into the
   interpreter's arithmetic, and a compare and branch cost one every time.
   Now the pair is two ARM instructions, and flags nobody has asked for
   yet wait as a note ("The Intel translator" above). The Python loop of
   integer arithmetic went from 3.0 s to 0.8 s, and the floating point one
   in the table from 2.2 s to 0.75 s: about what the same loops take on
   the PowerPC side (0.55 s for the integer one).

   Still to do there: MMX, string moves, 16-bit operands and the rarer SSE
   instructions, which video will lean on; and keeping the SSE registers
   in the Mac's own between instructions, where now each instruction
   loads and stores them.
8. **Done.** Polish, 2026-11-18 and 19.
   - The Apple menu crashed, in SystemConfiguration, because `configd`
     wasn't running; it now runs as a guest, which needed a startup
     deadlock with DirectoryService broken (SKILL.md, "Daemons").
   - Closing the viewer window ends the run.
   - The menu bar's Apple logo is a pile of powder, and the desktop has a
     picture (SKILL.md, "The menu bar logo" and "The wallpaper").
   - Daemons start in the guest's `/`: Launch Services' `lssave` was leaving
     a file named `~` wherever cocaine was run from.
   - Cocaine.app, a launcher with Install, Keys and Run tabs, released as
     `gui-v1.0` on GitHub (SKILL.md, "Cocaine.app").
   On hold, by the user's choice: the Dock and Finder (apps launching other
   apps through Launch Services) and VLC playing video.
9. **Done for Cocoa apps.** Open and save panels, 2026-11-24. The first
   try (2026-11-20) let Leopard's panel be built and put this Mac's picker
   in front of it; the panel's browser crashed, and what was left of it
   upset the window server. This time the panel is never built: its init
   is left hollow, running it asks a helper process that shows this Mac's
   picker, and the kernel learned the two calls a Save onto an existing
   file needs (`access_extended`, and access control lists in
   `setattrlist`). Learned on the way: Objective-C methods can be taken
   over by their local symbols; a stand-in that waits must wait in slices,
   or a signal reads as an answer; and a modal session stopped from a
   timer doesn't notice until an event comes round. Carbon dialogs answer
   "cancelled" for now (known-gaps.md).
