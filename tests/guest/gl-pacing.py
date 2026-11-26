import ctypes
import sys
import time

agl = ctypes.CDLL("/System/Library/Frameworks/AGL.framework/Versions/A/AGL")
uint = ctypes.c_uint
agl.aglChoosePixelFormatCFM.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
agl.aglChoosePixelFormatCFM.restype = uint
agl.aglCreateContextCFM.argtypes = [uint, uint]
agl.aglCreateContextCFM.restype = uint
agl.aglSetCurrentContext.argtypes = [uint]
agl.aglSwapBuffers.argtypes = [uint]
agl.aglDestroyPixelFormat.argtypes = [uint]
agl.aglDestroyContext.argtypes = [uint]

attrs = (ctypes.c_int * 3)(4, 5, 0)
fmt = agl.aglChoosePixelFormatCFM(None, 0, attrs)
ctx = agl.aglCreateContextCFM(fmt, 0)
assert fmt and ctx and agl.aglSetCurrentContext(ctx)
start = time.time()
first = None
for i in range(3 if sys.argv[1] == "low-1" else 100):
    agl.aglSwapBuffers(ctx)
    if i == 0:
        first = time.time() - start
elapsed = time.time() - start
assert agl.aglSetCurrentContext(0)
assert agl.aglDestroyContext(ctx)
agl.aglDestroyPixelFormat(fmt)

if sys.argv[1] == "default":
    assert 0.65 <= elapsed < 1.25, elapsed
elif sys.argv[1] in ("60", "low-60"):
    assert 1.4 <= elapsed < 2.2, elapsed
    if sys.argv[1] == "low-60":
        assert first >= 0.01, first
elif sys.argv[1] == "low-1":
    assert 2.8 <= elapsed < 6.0, elapsed
    assert first >= 0.8, first
else:
    assert sys.argv[1] in ("0", "low-0") and elapsed < 0.45, elapsed
print "GL pacing ok"
