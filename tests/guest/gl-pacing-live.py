import ctypes
import os
import sys
import time

# Force a session even when this test runs without a window server.
child = os.fork()
if child == 0:
    os._exit(0)
os.waitpid(child, 0)

uint = ctypes.c_uint
if sys.argv[1] == "agl":
    agl = ctypes.CDLL("/System/Library/Frameworks/AGL.framework/Versions/A/AGL")
    agl.aglChoosePixelFormatCFM.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
    agl.aglChoosePixelFormatCFM.restype = uint
    agl.aglCreateContextCFM.argtypes = [uint, uint]
    agl.aglCreateContextCFM.restype = uint
    agl.aglSetCurrentContext.argtypes = [uint]
    agl.aglSwapBuffers.argtypes = [uint]
    attrs = (ctypes.c_int * 3)(4, 5, 0)
    fmt = agl.aglChoosePixelFormatCFM(None, 0, attrs)
    ctx = agl.aglCreateContextCFM(fmt, 0)
    assert fmt and ctx and agl.aglSetCurrentContext(ctx)
    swap = lambda: agl.aglSwapBuffers(ctx)
else:
    cgl = ctypes.CDLL("/System/Library/Frameworks/OpenGL.framework/Versions/A/OpenGL")
    gl = ctypes.CDLL("/System/Library/Frameworks/OpenGL.framework/Versions/A/Libraries/libGL.dylib")
    cg = ctypes.CDLL("/System/Library/Frameworks/ApplicationServices.framework/Versions/A/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics")
    cgl.CGLChoosePixelFormat.argtypes = [ctypes.POINTER(ctypes.c_int), ctypes.POINTER(uint),
                                         ctypes.POINTER(ctypes.c_int)]
    cgl.CGLCreateContext.argtypes = [uint, uint, ctypes.POINTER(uint)]
    gl.CGLSetCurrentContext.argtypes = [uint]
    cgl.CGLSetFullScreen.argtypes = [uint]
    cgl.CGLFlushDrawable.argtypes = [uint]
    cg.CGMainDisplayID.restype = uint
    cg.CGDisplayCapture.argtypes = [uint]
    cg.CGDisplayRelease.argtypes = [uint]
    fmt, ctx, count = uint(), uint(), ctypes.c_int()
    attrs = (ctypes.c_int * 3)(5, 73, 0)
    assert cgl.CGLChoosePixelFormat(attrs, ctypes.byref(fmt), ctypes.byref(count)) == 0
    assert cgl.CGLCreateContext(fmt, 0, ctypes.byref(ctx)) == 0
    assert gl.CGLSetCurrentContext(ctx) == 0
    display = cg.CGMainDisplayID()
    assert cg.CGDisplayCapture(display) == 0
    assert cgl.CGLSetFullScreen(ctx) == 0
    swap = lambda: cgl.CGLFlushDrawable(ctx)

for i in range(3):
    print "READY"
    sys.stdout.flush()
    assert sys.stdin.readline()
    start = time.time()
    assert swap() == 0
    print "RESULT %.6f" % (time.time() - start)
    sys.stdout.flush()

if sys.argv[1] == "cgl":
    assert cg.CGDisplayRelease(display) == 0
