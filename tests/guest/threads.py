import threading
import ctypes
import os
import signal
import time
total = [0]
lock = threading.Lock()
def work(n):
    for i in range(2000):
        lock.acquire()
        total[0] += n
        lock.release()
threads = [threading.Thread(target=work, args=(k,)) for k in range(1, 5)]
for t in threads: t.start()
for t in threads: t.join()

for blocked_in in ("read", "mach_msg"):
    child = os.fork()
    if child == 0:
        signal.signal(signal.SIGALRM, signal.SIG_DFL)
        signal.alarm(5)
        if blocked_in == "read":
            reader, writer = os.pipe()
        else:
            lib = ctypes.CDLL("/usr/lib/libSystem.B.dylib")
            port = ctypes.c_uint()
            task = ctypes.c_uint.in_dll(lib, "mach_task_self_").value
            assert lib.mach_port_allocate(task, 1, ctypes.byref(port)) == 0
            message = ctypes.create_string_buffer(64)

        def leave():
            time.sleep(0.1)
            os._exit(0x125)

        threading.Thread(target=leave).start()
        if blocked_in == "read":
            os.read(reader, 1)
        else:
            lib.mach_msg(message, 2, 0, len(message), port.value, 0, 0)
        os._exit(1)
    assert os.waitpid(child, 0)[1] == 37 << 8, blocked_in

print "python threads total", total[0]
