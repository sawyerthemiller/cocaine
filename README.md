<h1 align="center">cocaine</h1>

Runs old Mac programs on Apple Silicon Macs - PowerPC apps - the kind Rosetta 1
used to run - and 32 bit Intel apps (the kind macOS dropped in Catalina)

IT IS NOT a virtual machine, and there's no old system to boot... Cocaine loads
the program into an ordinary Mac process, translates its old CPU's code to
ARM64 as it goes, runs Apple's own Mac OS X 10.5 Leopard libraries and system
daemons alongside it, and answers their requests to the old kernel itself.
The guest system's screen appears in a window on your Mac.

![Marble Blast Gold running under cocaine](docs/first-marbleblast.png)

## What works

- **Apps with windows, on both CPUs -** Calculator, VLC 0.9.10, Transmission 1.54 and others run
  on Leopard's own window server, with the mouse, keyboard and scroll wheel.
  Leopard draws at 1x for compatibility; the viewer handles Retina sizing
  without changing the app's coordinates.
- **Games -** Marble Blast Gold (both halves of the universal build) plays at
  60 frames a second with music and sound; GL Golf draws its course.
  Postal 2 draws its startup warning, animated intro and menu, using a
  1024 x 768 display with doubled viewer pixels where they fit. Compressed
  font textures keep their full resolution. Snes9x HQ! displays running game
  graphics with sound; it uses a 1280 x 800 display with doubled viewer pixels
  to keep its menus large and its game window on screen.
  Ghost Recon Demo's menu uses an exact 640 x 480 mode rather than drawing
  into a corner of a larger display (10.5.6 PowerPC).
  OpenGL and OpenAL calls go to your Mac's own graphics and sound.
- **Sound -** VLC plays MP3s through your Mac's speakers.
- **Mounted disk images -** browsable images mounted under `/Volumes` are
  visible to guests, including to installers that look for a CD.
- **Command line tools, shells and scripting languages -** from Leopard -
  `ls`, `bash`, `zsh`, Python 2.5, Perl, Ruby and the rest.

The cocaine window follows guest display resolution changes. Its View menu has
Actual Size (one guest pixel per Mac point), Integer Scale (whole screen pixels)
and Fit modes, also on Command-1, Command-2 and Command-3. Resizing the window
doesn't change the guest resolution; unused space is black. Closing it ends the run.

## The cocaine app

[Cocaine.app](https://github.com/sawyerthemiller/cocaine/releases) does the
setting up and running for you, in three tabs

- **Install -** downloads cocaine from GitHub and builds it, checks for
  updates and rebuilds, and imports Leopard from your install DVD image.
- **Keys -** takes the two SMC keys for Intel apps and keeps them in the
  system root, readable only by you.
- **Run -** pick an old app or drop one on it, choose Auto, PowerPC or
  Intel, and run it. Closing its window, or pressing stop, ends it. Kill All
  ends every cocaine process on your Mac. Ghost Recon Demo uses an 800 x 600
  display and doubled viewer pixels where they fit, unless `COCAINE_DISPLAY`
  or `COCAINE_VIEWER_DOUBLE` was set explicitly. OpenGL apps run at up to
  120 frames a second by default to reduce input latency, at the cost of more
  power; `COCAINE_GL_FPS=60` restores the previous limit. Press Command-Option-M in
  the guest window to capture or release the mouse; capture also releases
  when the window loses focus.

When it opens it checks GitHub for a newer version of itself and asks before updating

It isn't signed by a registered developer, so the first time, control-click it and choose open (or run `xattr -dr com.apple.quarantine Cocaine.app`) 

It still needs the build tools below. You can build it yourself with `gui/build.sh`.

## What you need

- An Apple Silicon Mac with Xcode's command line tools, CMake and Ninja.
- A **Mac OS X 10.5 Leopard** retail install DVD image. It is universal, so it
  covers PowerPC and Intel at once.
- For **Intel apps with windows**, the two keys from an Intel Mac's SMC
  chip. Apple shipped a few of the Intel system's own programs encrypted,
  and the old kernel decrypted them with keys read from the hardware. Put the
  64 characters (`OSK0` then `OSK1`) in `sysroots/10.5/.cocaine/osk`, or in
  the `COCAINE_OSK` variable. They aren't in this repository and must stay
  out of it. Both keys must be correct: incorrect keys can make Leopard's
  login window and font server crash, leaving a blank screen. PowerPC apps
  need none of this, and a universal app runs its
  PowerPC half unless you ask for `--arch i386`.
- The programs to run. None come with this repository.
- Classic PowerPC PEF/CFM support is experimental. EV Nova 1.0.0 initializes
  its registration tool, but the tested scenario files report a checksum mismatch.
  Ghost Recon Demo reaches gameplay, with mouse capture available. The menu
  can be cropped unless the display starts at 800 x 600.

## Building and running

<details>
<summary>Build and import Leopard</summary>

```bash
cmake -S . -B build -G Ninja
cmake --build build
sh tools/import-sysroot.sh /path/to/leopard-install-dvd.iso
```

</details>

The importer unpacks Leopard into `sysroots/10.5`

Then -

<details>
<summary>Run a program</summary>

```bash
export COCAINE_SYSROOT=$PWD/sysroots/10.5
./build/cocaine sysroots/10.5/Applications/Calculator.app
./build/cocaine "/path/to/Marble Blast Gold.app"
./build/cocaine --arch i386 /path/to/VLC.app
./build/cocaine sysroots/10.5/bin/ls -la /
```

</details>

A window titled "cocaine: Mac OS X 10.5" opens after a few seconds, once
Leopard's window server and login window have started. For an installer that
asks for a CD, mount its disk image on your Mac before running the installer.
Mount the image containing the Mac installer and its files; a different disc
or a Windows-only disc may not contain what the installer needs.

<details>
<summary>Environment variables</summary>

A few settings, as environment variables:

| Variable | What it does |
|---|---|
| `COCAINE_DISPLAY=1280x1024` | the guest's screen size (a conventional resolution fitting your Mac's available logical desktop by default; 1280 x 800 without a screen) |
| `COCAINE_UI_SCALE=2` | opt into Leopard's unfinished enlarged drawing (1 by default; higher values can break old apps) |
| `COCAINE_VIEWER_SCALE=fit` | viewer sizing - `actual` (default), `integer` or `fit`; doesn't change guest coordinates |
| `COCAINE_GL_FPS=0` | lift the 120 frames a second cap on games |
| `COCAINE_WALLPAPER=/path/to/picture.jpg` | the desktop picture, a JPEG or PNG (`images/linen.jpeg` by default - `none` for Leopard's plain grey) |
| `COCAINE_MENU_LOGO=apple` | keep Apple's logo in the menu bar instead of cocaine's |
| `COCAINE_NO_VIEWER=1` | run without the window |

</details>

`cocaine --help` lists the command line options.

## Tests

<details>
<summary>Run the tests</summary>

```bash
ctest --test-dir build --output-on-failure
```

</details>

Each test program runs translated and interpreted, and must match a
reference bit for bit, with a system root imported, real Leopard programs
and apps are tested on both CPUs too.

## Not yet

- QuickTime, and video in VLC.
- File dialogs in Carbon apps - they answer cancel. iCloud Drive can't be picked from.
- The Dock and Finder - apps can't yet start other apps.
- Copy and paste between apps.
- The tools `ps` and `top`.
- Only one window for the whole guest screen.

<details>
<summary>Under the hood</summary>

The design, the history, every debugging switch and the full list of gaps are in [.claude/skills/cocaine/](.claude/skills/cocaine/SKILL.md)

```
src/cpu/          the PowerPC and Intel translators and interpreters
src/kernel/       the stand-in kernel: system calls, Mach messages, IOKit, signals, paths
src/session/      the stand-in for launchd, which starts Leopard's daemons
src/viewer/       the window showing the guest's screen
src/gl/ src/al/   src/audio/   OpenGL, OpenAL and Core Audio, done by your Mac's own
tools/            the system root importer, and tools for reading Apple's code
gui/              cocaine.app, the launcher (SwiftUI - gui/build.sh)
tests/            test programs and the suite
```

</details>
