import glob
import os
import select
import struct
import subprocess
import sys
import tempfile
import time

cocaine, root, arch, api, initial_arg = sys.argv[1:]
initial = int(initial_arg)
guest = os.path.join(os.path.dirname(__file__), "guest", "gl-pacing-live.py")
with open(guest) as source:
    code = source.read()
temp = subprocess.check_output(["getconf", "DARWIN_USER_TEMP_DIR"], text=True).strip()
env = os.environ.copy()
env.update(COCAINE_NO_VIEWER="1", COCAINE_GL_FPS="1", PYTHONDONTWRITEBYTECODE="1")
env["COCAINE_GL_LOW_LATENCY"] = initial_arg
with tempfile.TemporaryFile() as errors:
    process = subprocess.Popen(
        [cocaine, "--arch", arch, "--sysroot", root, root + "/usr/bin/python2.5", "-c", code, api],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=errors, env=env, bufsize=0)
    pending = b""

    def line():
        global pending
        deadline = time.monotonic() + 30
        while b"\n" not in pending:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([process.stdout], [], [], remaining)[0]:
                raise RuntimeError("guest timed out")
            chunk = os.read(process.stdout.fileno(), 4096)
            if not chunk:
                raise RuntimeError("guest exited unexpectedly")
            pending += chunk
        result, pending = pending.split(b"\n", 1)
        return result.decode()

    try:
        for index, expected in enumerate((initial, 1 - initial, initial)):
            assert line() == "READY"
            paths = glob.glob(os.path.join(temp, "cocaine.session.%d.*" % process.pid, "gl-pacing"))
            assert len(paths) == 1, paths
            with open(paths[0], "r+b", buffering=0) as control:
                if index == 0:
                    assert struct.unpack("=I", control.read(4))[0] == initial
                    control.seek(0)
                control.write(struct.pack("=I", expected))
            process.stdin.write(b"go\n")
            result = line().split()
            assert result[0] == "RESULT", result
            duration = float(result[1])
            assert duration >= (0.8 if index == 0 else 1.4) if expected else duration < 0.6, duration
        assert process.wait(timeout=30) == 0
        deadline = time.monotonic() + 10
        while os.path.exists(paths[0]) and time.monotonic() < deadline:
            time.sleep(0.05)
        assert not os.path.exists(paths[0]), "session pacing control was not cleaned up"
    except Exception:
        if process.poll() is None:
            process.kill()
            process.wait()
        errors.seek(0)
        sys.stderr.buffer.write(errors.read())
        raise
print("live %s pacing ok (initial %s)" % (api, initial_arg))
