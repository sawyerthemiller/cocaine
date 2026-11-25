#!/bin/sh
# Runs every guest test program under cocaine and compares its output and exit
# status with a reference build of the same source.
#
# Most tests are plain C, so the reference is a native build for this Mac.
# The Intel-only tests (x87, SSE) have nothing native to compare with; their
# reference is a 64-bit Intel build run under Rosetta, which has both.
#
# usage: run.sh <path to cocaine> [work directory]
set -u

cocaine=${1:?usage: run.sh <path to cocaine> [work directory]}
here=$(cd "$(dirname "$0")" && pwd)
work=${2:-$(mktemp -d)}
mkdir -p "$work"

pass=0
fail=0
skip=0

# No window of cocaine's own while testing: the guest's display isn't shown.
export COCAINE_NO_VIEWER=1
# And never this Mac's file picker: a guest's open or save panel is answered
# "cancel" unless a test names an answer (COCAINE_PICK_ANSWER or these two).
export COCAINE_PICK_OPEN=
export COCAINE_PICK_SAVE=
unset COCAINE_PICK_REAL
# The clicks below are for the 1280 x 1024 desktop Leopard has on its own.
export COCAINE_DISPLAY=1280x1024

have_rosetta=no
arch -x86_64 /usr/bin/true 2>/dev/null && have_rosetta=yes

# reference <test> <kind>: builds the reference program for one test.
# Some Macs run system tweaks that print a line or two from every program
# started, the reference builds and cocaine alike, stamped with the time and
# process number. They say nothing about the test, so they're left out.
drop_noise() {
  grep -a -v -E '\[(AppleSharpener|BlurTweak)\] ' "$1" > "$1.kept"
  mv "$1.kept" "$1"
}

reference() {
  arch_flags=
  [ "$2" = rosetta ] && arch_flags="-arch x86_64 -msse3"
  clang $arch_flags -O1 -ffp-contract=off -fsigned-char -DRT_HOST -I"$here/guest" \
    "$here/guest/$1.c" "$here/guest/rt.c" "$here/guest/host_shim.c" -o "$work/$1.host"
}

for entry in hello:native t_int:native t_fp:native t_fma:native t_altivec:native \
  t_signal:native t_x87:rosetta t_sse:rosetta; do
  test=${entry%%:*}
  kind=${entry##*:}
  [ -f "$here/guest/$test.c" ] || continue
  if [ "$kind" = rosetta ] && [ "$have_rosetta" = no ]; then
    echo "skip  $test: needs Rosetta for its reference build"
    skip=$((skip + 1))
    continue
  fi
  if ! reference "$test" "$kind"; then
    echo "FAIL  $test: the reference build failed"
    fail=$((fail + 1))
    continue
  fi
  COCAINE_TEST=yes "$work/$test.host" alpha "two words" 3 >"$work/$test.expected" 2>&1
  expected_status=$?
  drop_noise "$work/$test.expected"

  for bin in "$here"/bin/*/"$test" "$here"/bin/*/"$test"_*; do
    [ -f "$bin" ] || continue
    cpu=$(basename "$(dirname "$bin")")
    # Guest code runs two ways: translated to ARM64, which is the usual
    # one, and interpreted. Each must match the reference by itself.
    for way in translated interpreted; do
      name="$cpu/$(basename "$bin"), $way"
      actual=$work/$(echo "$name" | tr '/, ' '---').actual
      jit=1
      [ "$way" = interpreted ] && jit=0
      COCAINE_JIT=$jit COCAINE_TEST=yes "$cocaine" "$bin" alpha "two words" 3 >"$actual" 2>&1
      status=$?
      drop_noise "$actual"
      if [ "$status" -eq "$expected_status" ] && cmp -s "$work/$test.expected" "$actual"; then
        echo "ok    $name"
        pass=$((pass + 1))
      else
        echo "FAIL  $name (exit $status, expected $expected_status)"
        diff "$work/$test.expected" "$actual" | head -20
        fail=$((fail + 1))
      fi
    done
  done
done

# Real Mac OS X programs, run from an imported system root if there is one.
root=$here/../sysroots/10.5
# real <arch> <expected output> <program inside the root> [arguments...]
real() {
  arch=$1 expected=$2 program=$3
  shift 3
  name="$arch$program $*"
  actual=$("$cocaine" --arch "$arch" --sysroot "$root" "$root$program" "$@" 2>/dev/null)
  # printf, not echo: the names contain backslashes that echo would interpret.
  if [ "$actual" = "$expected" ]; then
    printf 'ok    %s\n' "$name"
    pass=$((pass + 1))
  else
    printf 'FAIL  %s\n  expected: %s\n  actual:   %s\n' "$name" "$expected" "$actual"
    fail=$((fail + 1))
  fi
}
if [ -f "$root/usr/lib/dyld" ]; then
  version=$(/usr/libexec/PlistBuddy -c 'Print :ProductVersion' \
    "$root/System/Library/CoreServices/SystemVersion.plist")
  for arch in ppc i386; do
    real $arch "hello world" /bin/echo hello world
    real $arch "42" /bin/expr 6 '*' 7
    real $arch "pi is 3.14159, to 5 places" /usr/bin/printf 'pi is %.5f, to %d places\n' 3.14159265 5
    real $arch "1.4142 AWK" /usr/bin/awk 'BEGIN { printf "%.4f %s\n", sqrt(2), toupper("awk") }'
    real $arch "$(wc -l <"$here/guest/rt.c" | tr -d ' ')" /usr/bin/awk 'END { print NR }' "$here/guest/rt.c"
    real $arch "Darwin 9.0.0" /usr/bin/uname -sr
    # CoreFoundation, then Foundation with the Objective-C runtime.
    real $arch "$version" /usr/bin/sw_vers -productVersion
    real $arch "Mac OS X" /usr/bin/defaults read /System/Library/CoreServices/SystemVersion ProductName
    if [ -f "$root/usr/bin/python2.5" ]; then
      if [ "$arch" = i386 ]; then
        for jit in 1 0; do
          log="$work/pattern-$jit.log"
          actual=$(COCAINE_JIT=$jit PYTHONDONTWRITEBYTECODE=1 "$cocaine" --arch "$arch" --sysroot "$root" \
            "$root/usr/bin/python2.5" -c "$(cat "$here/guest/pattern.py")" 2>"$log")
          status=$?
          if [ "$status" -eq 0 ] && [ "$actual" = "pattern fill ok" ]; then
            echo "ok    i386 pattern fill (JIT $jit)"
            pass=$((pass + 1))
          else
            echo "FAIL  i386 pattern fill (JIT $jit): $actual"
            drop_noise "$log"
            cat "$log"
            fail=$((fail + 1))
          fi
        done
      fi
      log="$work/file-metadata-$arch.log"
      actual=$(PYTHONDONTWRITEBYTECODE=1 "$cocaine" --arch "$arch" --sysroot "$root" \
        "$root/usr/bin/python2.5" -c "$(cat "$here/guest/appledouble.py")" 2>"$log")
      status=$?
      if [ "$status" -eq 0 ] && [ "$actual" = "file metadata ok" ]; then
        echo "ok    $arch file metadata"
        pass=$((pass + 1))
      else
        echo "FAIL  $arch file metadata: $actual"
        drop_noise "$log"
        cat "$log"
        fail=$((fail + 1))
      fi
      log="$work/volume-metadata-$arch.log"
      actual=$(PYTHONDONTWRITEBYTECODE=1 "$cocaine" --arch "$arch" --sysroot "$root" \
        "$root/usr/bin/python2.5" -c "$(cat "$here/guest/volume.py")" 2>"$log")
      status=$?
      if [ "$status" -eq 0 ] && [ "$actual" = "volume metadata ok" ]; then
        echo "ok    $arch volume metadata"
        pass=$((pass + 1))
      else
        echo "FAIL  $arch volume metadata: $actual"
        drop_noise "$log"
        cat "$log"
        fail=$((fail + 1))
      fi
      log="$work/agl-$arch.log"
      actual=$(PYTHONDONTWRITEBYTECODE=1 "$cocaine" --arch "$arch" --sysroot "$root" \
        "$root/usr/bin/python2.5" -c "$(cat "$here/guest/agl.py")" 2>"$log")
      status=$?
      if [ "$status" -eq 0 ] && [ "$actual" = "AGL compatibility ok" ]; then
        echo "ok    $arch AGL compatibility"
        pass=$((pass + 1))
      else
        echo "FAIL  $arch AGL compatibility: $actual"
        drop_noise "$log"
        cat "$log"
        fail=$((fail + 1))
      fi
      for fps in default 60 0; do
        log="$work/gl-pacing-$arch-$fps.log"
        if [ "$fps" = default ]; then
          actual=$(env -u COCAINE_GL_FPS PYTHONDONTWRITEBYTECODE=1 "$cocaine" --arch "$arch" --sysroot "$root" \
            "$root/usr/bin/python2.5" -c "$(cat "$here/guest/gl-pacing.py")" "$fps" 2>"$log")
        else
          actual=$(COCAINE_GL_FPS="$fps" PYTHONDONTWRITEBYTECODE=1 "$cocaine" --arch "$arch" --sysroot "$root" \
            "$root/usr/bin/python2.5" -c "$(cat "$here/guest/gl-pacing.py")" "$fps" 2>"$log")
        fi
        status=$?
        if [ "$status" -eq 0 ] && [ "$actual" = "GL pacing ok" ]; then
          echo "ok    $arch GL pacing ($fps)"
          pass=$((pass + 1))
        else
          echo "FAIL  $arch GL pacing ($fps): $actual"
          drop_noise "$log"
          cat "$log"
          fail=$((fail + 1))
        fi
      done
      log="$work/cgl-$arch.log"
      actual=$(PYTHONDONTWRITEBYTECODE=1 "$cocaine" --arch "$arch" --sysroot "$root" \
        "$root/usr/bin/python2.5" -c "$(cat "$here/guest/cgl.py")" 2>"$log")
      status=$?
      if [ "$status" -eq 0 ] && [ "$actual" = "CGL compatibility ok" ]; then
        echo "ok    $arch CGL compatibility"
        pass=$((pass + 1))
      else
        echo "FAIL  $arch CGL compatibility: $actual"
        drop_noise "$log"
        cat "$log"
        fail=$((fail + 1))
      fi
      log="$work/midi-$arch.log"
      actual=$(PYTHONDONTWRITEBYTECODE=1 "$cocaine" --arch "$arch" --sysroot "$root" \
        "$root/usr/bin/python2.5" -c "$(cat "$here/guest/midi.py")" 2>"$log")
      status=$?
      if [ "$status" -eq 0 ] && [ "$actual" = "MIDI audio compatibility ok" ]; then
        echo "ok    $arch MIDI audio compatibility"
        pass=$((pass + 1))
      else
        echo "FAIL  $arch MIDI audio compatibility: $actual"
        drop_noise "$log"
        cat "$log"
        fail=$((fail + 1))
      fi
      log="$work/sysctl-$arch.log"
      actual=$(PYTHONDONTWRITEBYTECODE=1 "$cocaine" --arch "$arch" --sysroot "$root" \
        "$root/usr/bin/python2.5" -c "$(cat "$here/guest/sysctl.py")" 2>"$log")
      status=$?
      if [ "$status" -eq 0 ] && [ "$actual" = "process name sysctl ok" ]; then
        echo "ok    $arch process name sysctl"
        pass=$((pass + 1))
      else
        echo "FAIL  $arch process name sysctl: $actual"
        drop_noise "$log"
        cat "$log"
        fail=$((fail + 1))
      fi
      log="$work/task-for-pid-$arch.log"
      actual=$(PYTHONDONTWRITEBYTECODE=1 "$cocaine" --arch "$arch" --sysroot "$root" \
        "$root/usr/bin/python2.5" -c "$(cat "$here/guest/task-for-pid.py")" 2>"$log")
      status=$?
      if [ "$status" -eq 0 ] && [ "$actual" = "task_for_pid ok" ]; then
        echo "ok    $arch task_for_pid"
        pass=$((pass + 1))
      else
        echo "FAIL  $arch task_for_pid: $actual"
        drop_noise "$log"
        cat "$log"
        fail=$((fail + 1))
      fi
      # Several permission questions in one system call, as Carbon asks them.
      log="$work/access-extended-$arch.log"
      actual=$(PYTHONDONTWRITEBYTECODE=1 "$cocaine" --arch "$arch" --sysroot "$root" \
        "$root/usr/bin/python2.5" -c "$(cat "$here/guest/access-extended.py")" 2>"$log")
      status=$?
      if [ "$status" -eq 0 ] && [ "$actual" = "access_extended ok" ]; then
        echo "ok    $arch access_extended"
        pass=$((pass + 1))
      else
        echo "FAIL  $arch access_extended: $actual"
        drop_noise "$log"
        cat "$log"
        fail=$((fail + 1))
      fi
      # Cocoa documents saved onto files that are already there, which goes
      # through Carbon's FSPathReplaceObject. No window is opened.
      log="$work/save-$arch.log"
      actual=$(PYTHONDONTWRITEBYTECODE=1 "$cocaine" --arch "$arch" --sysroot "$root" \
        "$root/usr/bin/python2.5" -c "$(cat "$here/guest/save.py")" 2>"$log")
      status=$?
      if [ "$status" -eq 0 ] && [ "$actual" = "document save ok" ]; then
        echo "ok    $arch document save onto an existing file"
        pass=$((pass + 1))
      else
        echo "FAIL  $arch document save onto an existing file: $actual"
        drop_noise "$log"
        cat "$log"
        fail=$((fail + 1))
      fi
    fi
    # Processes: the shell forks, runs programs and scripts, pipes them
    # together and traps signals; arch starts programs for the other CPU.
    mkdir -p "$work/procs-$arch"
    real $arch "$(cat "$here/guest/procs.expected")" /bin/sh "$here/guest/procs.sh" "$work/procs-$arch"
    # A real system daemon: Apple's notifyd, started on demand by the session
    # server, answering a client's requests. (The value reads back as 0
    # because Leopard's notifyd forgets a name once nobody is registered for
    # it; what matters is that every request gets an answer, not "Failed".)
    real $arch "$(printf 'cocaine.test 0\nposted: 0')" /bin/sh -c \
      '/usr/bin/notifyutil -s cocaine.test 42 -g cocaine.test -p cocaine.test; echo "posted: $?"'
    # Three of Apple's daemons at once. The directory daemon knows the person
    # running the tests, by name; then one program waits for a notification
    # that another posts, which takes the directory daemon (to find the
    # user), dirhelper (for the user's temporary folder, which it collects
    # its port for over launchd's socket) and notifyd.
    real $arch "$(id -un) staff" /bin/sh -c 'echo $(/usr/bin/id -un) $(/usr/bin/id -gn)'
    real $arch "$(printf 'cocaine.ping\nwaiter finished: 0')" /bin/sh "$here/guest/notify.sh"
    # Windows need Apple's window server, font server and login window. The
    # Intel halves of the last two are stored encrypted, so the Intel test
    # depends on whether the keys to them have been supplied (see the README).
    windows=yes
    if [ $arch = i386 ] && [ -z "${COCAINE_OSK:-}" ] && [ ! -s "$root/.cocaine/osk" ]; then
      windows=no
    fi
    if [ $windows = yes ]; then
      # A real application with a window, and all three of those started
      # for it. Some seconds in, cocaine's input system clicks the mouse and
      # types 8 and 9; the test is that Calculator, as the application in
      # front, is handed the press of the 8 key (key code 28, 0x1c) by the
      # window server. The Intel half is emulated more slowly, so it is
      # given longer.
      after=20
      [ $arch = i386 ] && after=30
      log="$work/calculator-$arch.log"
      COCAINE_POKE=360,784,$after COCAINE_NOTE_MSG=29330 "$cocaine" --arch $arch --sysroot "$root" \
        "$root/Applications/Calculator.app" > "$log" 2>&1 &
      app=$!
      typed=no
      for second in $(seq 1 $((after + 30))); do
        sleep 1
        # (The window server writes events most significant byte first on
        # both CPUs, so an Intel guest's log shows them the other way round.)
        if grep -a -q -e "00014037 0000000a.*00014009 0000001c" \
          -e "37400100 0a000000.*09400100 1c000000" "$log"; then
          typed=yes
          break
        fi
        kill -0 $app 2>/dev/null || break
      done
      kill $app 2>/dev/null
      wait $app 2>/dev/null
      if [ $typed = yes ]; then
        echo "ok    $arch Calculator.app opens a window and takes a typed key"
        pass=$((pass + 1))
      else
        echo "FAIL  $arch Calculator.app was not handed a typed key"
        fail=$((fail + 1))
      fi
      if [ "$arch" = ppc ]; then
        if [ "$version" = 10.5.6 ] && [ -f "$root/usr/bin/python2.5" ]; then
          mode_work=$(mktemp -d /tmp/cocaine-mode-test.XXXXXX)
          for modes in '1280 1024 800 600 16' '800 600 1024 768 32' \
                       '1280 1024 640 480 16' '800 600 640 480 32'; do
            set -- $modes
            original_w=$1 original_h=$2 mode_w=$3 mode_h=$4 mode_bits=$5
            first=$mode_work/switched.raw
            second=$mode_work/restored.raw
            log=$mode_work/app.log
            actual=$(COCAINE_DISPLAY="${original_w}x${original_h}" \
              COCAINE_SCRIPT="wait 20; shot $first; wait 20; shot $second" \
              PYTHONDONTWRITEBYTECODE=1 "$cocaine" --arch ppc --sysroot "$root" \
              "$root/usr/bin/python2.5" -c "$(cat "$here/guest/display-mode.py")" \
              "$mode_w" "$mode_h" "$mode_bits" "$original_w" "$original_h" 2>"$log")
            status=$?
            if [ "$status" -eq 0 ] && [ "$actual" = "display mode switch ok" ] &&
              [ -s "$first" ] && [ -s "$second" ] &&
              python3 -c 'import sys; paths = sys.argv[1:3]; sizes = [int(n) for n in sys.argv[3:]]; data = [open(p, "rb").read() for p in paths]; sys.exit(0 if all(len(d) == n and len({d[i:i+4] for i in range(0, len(d), 64)}) > 3 for d, n in zip(data, sizes)) else 1)' \
                "$first" "$second" "$((mode_w * mode_h * 4))" "$((original_w * original_h * 4))"; then
              echo "ok    ppc display mode ${original_w}x${original_h} to ${mode_w}x${mode_h} and restore"
              pass=$((pass + 1))
            else
              echo "FAIL  ppc display mode ${original_w}x${original_h} to ${mode_w}x${mode_h} and restore: $actual"
              drop_noise "$log"
              cat "$log"
              fail=$((fail + 1))
            fi
            rm -f "$first" "$second" "$log"
          done
          rmdir "$mode_work"
        fi

        # Exercise an explicit large display, not just Leopard's native size:
        # a partial or missing CoreGraphics patch used to produce a black screen.
        display_work=$(mktemp -d /tmp/cocaine-display-test.XXXXXX)
        frame=$display_work/frame.raw
        COCAINE_DISPLAY=2560x1600 \
          COCAINE_SCRIPT="wait 15; shot $frame; wait 5; shot $frame; wait 5; shot $frame" \
          "$cocaine" -vv --arch ppc --sysroot "$root" \
          "$root/Applications/Calculator.app" > "$display_work/app.log" 2>&1 &
        display_app=$!
        session_log="$(getconf DARWIN_USER_TEMP_DIR)cocaine-session-$display_app.log"
        display_ok=no
        for second in $(seq 1 45); do
          sleep 1
          if [ -s "$frame" ] && grep -q 'the login window has the session ready' "$session_log" 2>/dev/null &&
            python3 -c 'import sys; p = open(sys.argv[1], "rb").read(); sys.exit(0 if len(p) == 2560*1600*4 and len({p[i:i+4] for i in range(0, len(p), 64)}) > 16 else 1)' "$frame"; then
            display_ok=yes
            break
          fi
          kill -0 "$display_app" 2>/dev/null || break
        done
        kill "$display_app" 2>/dev/null
        wait "$display_app" 2>/dev/null
        if [ "$display_ok" = yes ] && ! grep -q "display copier stopped" "$session_log"; then
          echo "ok    ppc Calculator.app draws a full-size desktop"
          pass=$((pass + 1))
        else
          echo "FAIL  ppc Calculator.app did not draw a full-size desktop"
          fail=$((fail + 1))
        fi
        rm -f "$frame" "$display_work/app.log"
        rmdir "$display_work"
      fi
    else
      # Without them the Intel half gets as far as one can: all of AppKit
      # loaded and, with Apple's security and Launch Services daemons behind
      # it, registered as a running app. What stops it is the next thing it
      # asks for, a connection to the window server.
      if "$cocaine" --arch $arch --sysroot "$root" --max-insns 8000000000 \
        "$root/Applications/Calculator.app" 2>&1 | grep -q "connection to the WindowServer"; then
        echo "ok    $arch Calculator.app reaches the window server (no keys for windows)"
        pass=$((pass + 1))
      else
        echo "FAIL  $arch Calculator.app does not reach the window server"
        fail=$((fail + 1))
      fi
    fi
    # zsh waits for its children with a SIGCHLD handler and sigsuspend.
    real $arch "zsh 42 sub" /bin/zsh -f -c 'print zsh $(( 6 * 7 )) $(/bin/echo sub)'
    # Threads: Leopard's Python, four threads sharing a lock.
    real $arch "python threads total 20000" \
      /System/Library/Frameworks/Python.framework/Versions/2.5/Resources/Python.app/Contents/MacOS/Python \
      "$here/guest/threads.py"
    # ... and forking from one of several threads.
    real $arch "fork from threads: [7, 0, 9]" \
      /System/Library/Frameworks/Python.framework/Versions/2.5/Resources/Python.app/Contents/MacOS/Python \
      "$here/guest/forkthr.py"
  done
  # Threads again, through pthreads directly: atomic instructions and mutexes.
  if [ -f "$here/bin-dynamic/i386/t_threads" ]; then
    actual=$("$cocaine" --sysroot "$root" "$here/bin-dynamic/i386/t_threads" 2>/dev/null)
    if [ "$actual" = "atomic 120000 locked 120000 joined 10" ]; then
      echo "ok    i386/t_threads"
      pass=$((pass + 1))
    else
      echo "FAIL  i386/t_threads: $actual"
      fail=$((fail + 1))
    fi
  fi
  # Mach messages between two processes: port rights, out-of-line memory, the
  # session's stand-in for launchd, and a notice from the kernel.
  if [ -f "$here/bin-dynamic/i386/t_ipc" ]; then
    actual=$("$cocaine" --sysroot "$root" "$here/bin-dynamic/i386/t_ipc" 2>/dev/null)
    if [ "$actual" = "$(cat "$here/guest/t_ipc.expected")" ]; then
      echo "ok    i386/t_ipc"
      pass=$((pass + 1))
    else
      printf 'FAIL  i386/t_ipc:\n%s\n' "$actual"
      fail=$((fail + 1))
    fi
  fi
  real ppc "Power Macintosh" /usr/bin/uname -m
  real i386 "i386" /usr/bin/uname -m
else
  echo "skip  real Mac OS X programs: no system root (see tools/import-sysroot.sh)"
  skip=$((skip + 1))
fi

echo "$pass passed, $fail failed, $skip skipped"
[ "$fail" -eq 0 ] && [ "$pass" -gt 0 ]
