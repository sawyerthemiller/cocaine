import ctypes

agl = ctypes.CDLL("/System/Library/Frameworks/AGL.framework/Versions/A/AGL")
gl = ctypes.CDLL("/System/Library/Frameworks/OpenGL.framework/Versions/A/Libraries/libGL.dylib")
glu = ctypes.CDLL("/System/Library/Frameworks/OpenGL.framework/Versions/A/Libraries/libGLU.dylib")
uint = ctypes.c_uint
intp = ctypes.POINTER(ctypes.c_int)

agl.aglGetVersion.argtypes = [intp, intp]
agl.aglGetVersion.restype = None
agl.aglQueryRendererInfoCFM.argtypes = [ctypes.c_void_p, ctypes.c_int]
agl.aglQueryRendererInfoCFM.restype = uint
agl.aglNextRendererInfo.argtypes = [uint]
agl.aglNextRendererInfo.restype = uint
agl.aglDescribeRenderer.argtypes = [uint, ctypes.c_int, intp]
agl.aglDestroyRendererInfo.argtypes = [uint]
agl.aglGetError.restype = uint
agl.aglChoosePixelFormatCFM.argtypes = [ctypes.c_void_p, ctypes.c_int, intp]
agl.aglChoosePixelFormatCFM.restype = uint
agl.aglDescribePixelFormat.argtypes = [uint, ctypes.c_int, intp]
agl.aglCreateContextCFM.argtypes = [uint, uint]
agl.aglCreateContextCFM.restype = uint
agl.aglDestroyPixelFormat.argtypes = [uint]
agl.aglDisable.argtypes = [uint, uint]
agl.aglSetCurrentContext.argtypes = [uint]
agl.aglGetDrawable.argtypes = [uint]
agl.aglGetDrawable.restype = uint
agl.aglSetDrawable.argtypes = [uint, uint]
agl.aglDestroyContext.argtypes = [uint]
gl.glMatrixMode.argtypes = [uint]
gl.glGetDoublev.argtypes = [uint, ctypes.POINTER(ctypes.c_double)]
glu.gluOrtho2D.argtypes = [ctypes.c_double] * 4

major, minor = ctypes.c_int(-1), ctypes.c_int(-1)
agl.aglGetVersion(ctypes.byref(major), ctypes.byref(minor))
assert (major.value, minor.value) == (2, 0)
major.value, minor.value = -1, -1
agl.aglGetVersion(ctypes.byref(major), None)
agl.aglGetVersion(None, ctypes.byref(minor))
assert (major.value, minor.value) == (2, 0)

assert not agl.aglQueryRendererInfoCFM(1, 1)
assert agl.aglGetError() == 10006
renderer = agl.aglQueryRendererInfoCFM(None, 0)
assert renderer and agl.aglGetError() == 0
value = ctypes.c_int()
renderers = set()
current = renderer
while current:
    assert current not in renderers
    renderers.add(current)
    assert agl.aglDescribeRenderer(current, 70, ctypes.byref(value))
    assert value.value != 0
    current = agl.aglNextRendererInfo(current)
assert agl.aglGetError() == 0
agl.aglDestroyRendererInfo(renderer)
assert not agl.aglDescribeRenderer(renderer, 70, ctypes.byref(value))
assert agl.aglGetError() == 10003
assert agl.aglGetError() == 0

attrs = (ctypes.c_int * 7)(4, 5, 12, 16, 73, 72, 0)
fmt = agl.aglChoosePixelFormatCFM(None, 0, attrs)
assert fmt
assert agl.aglDescribePixelFormat(fmt, 70, ctypes.byref(value))
assert value.value != 0
assert agl.aglDescribePixelFormat(fmt, 7, ctypes.byref(value))
assert value.value == 1
ctx = agl.aglCreateContextCFM(fmt, 0)
assert ctx
agl.aglDestroyPixelFormat(fmt)
assert not agl.aglDescribePixelFormat(fmt, 70, ctypes.byref(value))
assert agl.aglGetError() == 10002
assert agl.aglSetCurrentContext(ctx)
assert agl.aglSetDrawable(ctx, 0)
assert agl.aglGetDrawable(ctx) == 0 and agl.aglGetError() == 0
assert agl.aglGetDrawable(0) == 0 and agl.aglGetError() == 10004
assert agl.aglDisable(ctx, 202)
assert not agl.aglDisable(ctx, 0xffffffff)
assert agl.aglGetError() == 10010
gl.glMatrixMode(0x1701)
gl.glLoadIdentity()
glu.gluOrtho2D(0.0, 4.0, 0.0, 8.0)
matrix = (ctypes.c_double * 16)()
gl.glGetDoublev(0x0ba7, matrix)
assert abs(matrix[0] - 0.5) < 0.00001
assert abs(matrix[5] - 0.25) < 0.00001
assert abs(matrix[10] + 1.0) < 0.00001
assert abs(matrix[12] + 1.0) < 0.00001
assert abs(matrix[13] + 1.0) < 0.00001
assert agl.aglSetCurrentContext(0)
assert agl.aglDestroyContext(ctx)
print "AGL compatibility ok"
