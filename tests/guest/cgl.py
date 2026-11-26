import ctypes
import os
import sys

cgl = ctypes.CDLL("/System/Library/Frameworks/OpenGL.framework/Versions/A/OpenGL")
gl = ctypes.CDLL("/System/Library/Frameworks/OpenGL.framework/Versions/A/Libraries/libGL.dylib")
glu = ctypes.CDLL("/System/Library/Frameworks/OpenGL.framework/Versions/A/Libraries/libGLU.dylib")
cg = ctypes.CDLL("/System/Library/Frameworks/ApplicationServices.framework/Versions/A/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics")
uint = ctypes.c_uint
intp = ctypes.POINTER(ctypes.c_int)
uintp = ctypes.POINTER(uint)

cgl.CGLChoosePixelFormat.argtypes = [intp, uintp, intp]
cgl.CGLDescribePixelFormat.argtypes = [uint, ctypes.c_int, uint, intp]
cgl.CGLRetainPixelFormat.argtypes = [uint]
cgl.CGLRetainPixelFormat.restype = uint
cgl.CGLReleasePixelFormat.argtypes = [uint]
cgl.CGLDestroyPixelFormat.argtypes = [uint]
cgl.CGLCreateContext.argtypes = [uint, uint, uintp]
cgl.CGLRetainContext.argtypes = [uint]
cgl.CGLRetainContext.restype = uint
cgl.CGLReleaseContext.argtypes = [uint]
cgl.CGLDestroyContext.argtypes = [uint]
cgl.CGLSetParameter.argtypes = [uint, uint, intp]
cgl.CGLGetParameter.argtypes = [uint, uint, intp]
cgl.CGLSetFullScreen.argtypes = [uint]
# Leopard 10.5.0's OpenGL doesn't have this one yet; later updates do.
has_on_display = hasattr(cgl, "CGLSetFullScreenOnDisplay")
if has_on_display:
    cgl.CGLSetFullScreenOnDisplay.argtypes = [uint, uint]
cgl.CGLClearDrawable.argtypes = [uint]
cgl.CGLFlushDrawable.argtypes = [uint]
gl.CGLSetCurrentContext.argtypes = [uint]
gl.CGLGetCurrentContext.restype = uint
gl.glMatrixMode.argtypes = [uint]
gl.glGetDoublev.argtypes = [uint, ctypes.POINTER(ctypes.c_double)]
gl.glGetIntegerv.argtypes = [uint, intp]
gl.glGetBooleanv.argtypes = [uint, ctypes.POINTER(ctypes.c_ubyte)]
gl.glPixelStorei.argtypes = [uint, ctypes.c_int]
gl.glPushClientAttrib.argtypes = [uint]
gl.glGenLists.argtypes = [ctypes.c_int]
gl.glGenLists.restype = uint
gl.glNewList.argtypes = [uint, uint]
gl.glClearColor.argtypes = [ctypes.c_float] * 4
gl.glClear.argtypes = [uint]
gl.glCallLists.argtypes = [ctypes.c_int, uint, ctypes.c_void_p]
gl.glListBase.argtypes = [uint]
gl.glReadPixels.argtypes = [ctypes.c_int] * 4 + [uint, uint, ctypes.c_void_p]
gl.glViewport.argtypes = [ctypes.c_int] * 4
gl.glScissor.argtypes = [ctypes.c_int] * 4
gl.glColor4f.argtypes = [ctypes.c_float] * 4
gl.glColor3fv.argtypes = [ctypes.POINTER(ctypes.c_float)]
gl.glVertex3d.argtypes = [ctypes.c_double] * 3
gl.glVertex3dv.argtypes = [ctypes.POINTER(ctypes.c_double)]
gl.glTexCoord2d.argtypes = [ctypes.c_double] * 2
gl.glTexCoord2i.argtypes = [ctypes.c_int] * 2
gl.glTexCoord2s.argtypes = [ctypes.c_short] * 2
gl.glLineStipple.argtypes = [ctypes.c_int, ctypes.c_ushort]
gl.glLightfv.argtypes = [uint, uint, ctypes.POINTER(ctypes.c_float)]
gl.glGetLightfv.argtypes = [uint, uint, ctypes.POINTER(ctypes.c_float)]
gl.glGetTexImage.argtypes = [uint, ctypes.c_int, uint, uint, ctypes.c_void_p]
gl.glBlendEquation.argtypes = [uint]
gl.glVertexPointer.argtypes = [ctypes.c_int, uint, ctypes.c_int, ctypes.c_void_p]
gl.glColorPointer.argtypes = [ctypes.c_int, uint, ctypes.c_int, ctypes.c_void_p]
gl.glDrawRangeElements.argtypes = [uint, uint, uint, ctypes.c_int, uint, ctypes.c_void_p]
glu.gluPerspective.argtypes = [ctypes.c_double] * 4
glu.gluLookAt.argtypes = [ctypes.c_double] * 9
glu.gluNewQuadric.restype = uint
glu.gluDeleteQuadric.argtypes = [uint]
glu.gluQuadricNormals.argtypes = [uint, uint]
glu.gluQuadricTexture.argtypes = [uint, ctypes.c_ubyte]
glu.gluQuadricOrientation.argtypes = [uint, uint]
glu.gluSphere.argtypes = [uint, ctypes.c_double, ctypes.c_int, ctypes.c_int]
glu.gluCylinder.argtypes = [uint, ctypes.c_double, ctypes.c_double, ctypes.c_double, ctypes.c_int, ctypes.c_int]
glu.gluDisk.argtypes = [uint, ctypes.c_double, ctypes.c_double, ctypes.c_int, ctypes.c_int]
glu.gluPartialDisk.argtypes = [uint, ctypes.c_double, ctypes.c_double, ctypes.c_int, ctypes.c_int, ctypes.c_double, ctypes.c_double]
glu.gluBuild2DMipmaps.argtypes = [uint, ctypes.c_int, ctypes.c_int, ctypes.c_int, uint, uint, ctypes.c_void_p]

attrs = (ctypes.c_int * 3)(5, 73, 0)
fmt = uint()
count = ctypes.c_int()
assert cgl.CGLChoosePixelFormat(attrs, ctypes.byref(fmt), ctypes.byref(count)) == 0
assert fmt.value and count.value == 1
value = ctypes.c_int()
assert cgl.CGLDescribePixelFormat(fmt, 0, 70, ctypes.byref(value)) == 0 and value.value
assert cgl.CGLDescribePixelFormat(fmt, 0, 7, ctypes.byref(value)) == 0 and value.value == 1
assert cgl.CGLRetainPixelFormat(fmt) == fmt.value
cgl.CGLReleasePixelFormat(fmt)
ctx = uint()
assert cgl.CGLCreateContext(fmt, 0, ctypes.byref(ctx)) == 0 and ctx.value
assert cgl.CGLDestroyPixelFormat(fmt) == 0
assert cgl.CGLDestroyPixelFormat(fmt) == 10002
assert cgl.CGLRetainContext(ctx) == ctx.value
cgl.CGLReleaseContext(ctx)
assert cgl.CGLSetFullScreen(ctx) == 0
assert cgl.CGLClearDrawable(ctx) == 0
assert gl.CGLSetCurrentContext(ctx) == 0
assert gl.CGLGetCurrentContext() == ctx.value
assert cgl.CGLSetParameter(ctx, 9999, ctypes.byref(value)) == 10010
value.value = 12345678
assert cgl.CGLSetParameter(ctx, 1236, ctypes.byref(value)) == 0
value.value = 0
assert cgl.CGLGetParameter(ctx, 1236, ctypes.byref(value)) == 0 and value.value == 12345678
assert cgl.CGLGetParameter(ctx, 9999, ctypes.byref(value)) == 10010
assert cgl.CGLGetParameter(ctx, 1236, None) == 10014
assert cgl.CGLGetParameter(ctx, 1236, ctypes.cast(1, intp)) == 10014
assert cgl.CGLGetParameter(0, 1236, ctypes.byref(value)) == 10004

gl.glPixelStorei(0x0cf0, 0)
gl.glPushClientAttrib(1)
gl.glPixelStorei(0x0cf0, 1)
gl.glPopClientAttrib()
gl.glGetIntegerv(0x0cf0, ctypes.byref(value))
assert value.value == 0
boolean = (ctypes.c_ubyte * 4)(9, 9, 9, 9)
gl.glGetBooleanv(0x0cf0, boolean)
assert tuple(boolean) == (0, 9, 9, 9)
gl.glPixelStorei(0x0cf0, 1)
gl.glGetBooleanv(0x0cf0, boolean)
assert boolean[0] == 1
gl.glPixelStorei(0x0cf0, 0)
gl.glColorMask(1, 0, 1, 0)
gl.glGetBooleanv(0x0c23, boolean)
assert tuple(boolean) == (1, 0, 1, 0)
gl.glColorMask(1, 1, 1, 1)

gl.glGenProgramsARB.argtypes = [ctypes.c_int, uintp]
gl.glDeleteProgramsARB.argtypes = [ctypes.c_int, uintp]
gl.glBindProgramARB.argtypes = [uint, uint]
gl.glProgramStringARB.argtypes = [uint, uint, ctypes.c_int, ctypes.c_void_p]
gl.glGetProgramivARB.argtypes = [uint, uint, intp]
gl.glProgramEnvParameter4fvARB.argtypes = [uint, uint, ctypes.POINTER(ctypes.c_float)]
gl.glVertexAttribPointerARB.argtypes = [uint, ctypes.c_int, uint, ctypes.c_ubyte,
                                        ctypes.c_int, ctypes.c_void_p]
gl.glEnableVertexAttribArrayARB.argtypes = [uint]
gl.glDisableVertexAttribArrayARB.argtypes = [uint]
gl.glDrawArrays.argtypes = [uint, ctypes.c_int, ctypes.c_int]
gl.glDrawElements.argtypes = [uint, ctypes.c_int, uint, ctypes.c_void_p]
program = uint()
gl.glGenProgramsARB(1, ctypes.byref(program))
assert program.value
gl.glBindProgramARB(0x8620, program)
source = "!!ARBvp1.0\nMOV result.position, vertex.attrib[0];\nMOV result.color, program.env[0];\nEND\n"
gl.glProgramStringARB(0x8620, 0x8875, len(source), ctypes.c_char_p(source))
assert gl.glGetError() == 0
gl.glGetProgramivARB(0x8620, 0x8627, ctypes.byref(value))
assert value.value == len(source), value.value
value.value = 123
gl.glGetProgramivARB(0xffffffff, 0x8627, ctypes.byref(value))
assert gl.glGetError() == 0x0500 and value.value == 123
color = (ctypes.c_float * 4)(1, 0, 0, 1)
gl.glProgramEnvParameter4fvARB(0x8620, 0, color)
vertices = (ctypes.c_float * 12)(0, 0, 99, -1, -1, 99, 3, -1, 99, -1, 3, 99)
gl.glVertexAttribPointerARB(0, 2, 0x1406, 0, 12, vertices)
gl.glEnableVertexAttribArrayARB(0)
gl.glEnable(0x8620)
gl.glViewport(0, 0, 1, 1)
gl.glClearColor(0, 0, 0, 1)
gl.glClear(0x4000)
gl.glDrawArrays(4, 1, 3)
program_pixel = (ctypes.c_ubyte * 4)()
gl.glReadPixels(0, 0, 1, 1, 0x1908, 0x1401, program_pixel)
assert tuple(program_pixel) == (255, 0, 0, 255), tuple(program_pixel)
gl.glPushClientAttrib(2)
gl.glDisableVertexAttribArrayARB(0)
gl.glPopClientAttrib()
color[0], color[1] = 0, 1
gl.glProgramEnvParameter4fvARB(0x8620, 0, color)
elements = (ctypes.c_ushort * 3)(1, 2, 3)
gl.glDrawElements(4, 3, 0x1403, elements)
gl.glReadPixels(0, 0, 1, 1, 0x1908, 0x1401, program_pixel)
assert tuple(program_pixel) == (0, 255, 0, 255), tuple(program_pixel)
offscreen = (ctypes.c_float * 6)(2, 2, 3, 2, 2, 3)
gl.glVertexPointer(2, 0x1406, 0, offscreen)
gl.glEnableClientState(0x8074)
gl.glClear(0x4000)
gl.glDrawElements(4, 3, 0x1403, elements)
gl.glReadPixels(0, 0, 1, 1, 0x1908, 0x1401, program_pixel)
assert tuple(program_pixel) == (0, 255, 0, 255), tuple(program_pixel)
gl.glDisableClientState(0x8074)
source = "!!ARBvp1.0\nMOV result.position, vertex.attrib[0];\nMOV result.color, vertex.attrib[1];\nEND\n"
gl.glProgramStringARB(0x8620, 0x8875, len(source), ctypes.c_char_p(source))
colors = (ctypes.c_ubyte * 16)(*([0, 0, 0, 0] + [0, 0, 255, 255] * 3))
gl.glVertexAttribPointerARB(1, 4, 0x1401, 1, 0, colors)
gl.glEnableVertexAttribArrayARB(1)
gl.glDrawElements(4, 3, 0x1403, elements)
gl.glReadPixels(0, 0, 1, 1, 0x1908, 0x1401, program_pixel)
assert tuple(program_pixel) == (0, 0, 255, 255), tuple(program_pixel)
gl.glVertexAttribPointerARB(1, 4, 0x1401, 1, 0, ctypes.c_void_p(1))
gl.glDrawElements(4, 3, 0x1403, elements)
gl.glReadPixels(0, 0, 1, 1, 0x1908, 0x1401, program_pixel)
assert tuple(program_pixel) == (0, 0, 255, 255), tuple(program_pixel)
gl.glVertexAttribPointerARB(1, 4, 0x1401, 1, 0, colors)
fragment = uint()
gl.glGenProgramsARB(1, ctypes.byref(fragment))
gl.glBindProgramARB(0x8804, fragment)
fragment_source = "!!ARBfp1.0\nMOV result.color, program.env[0];\nEND\n"
gl.glProgramStringARB(0x8804, 0x8875, len(fragment_source), ctypes.c_char_p(fragment_source))
fragment_color = (ctypes.c_float * 4)(1, 0, 1, 1)
gl.glProgramEnvParameter4fvARB(0x8804, 0, fragment_color)
gl.glEnable(0x8804)
gl.glDrawElements(4, 3, 0x1403, elements)
gl.glReadPixels(0, 0, 1, 1, 0x1908, 0x1401, program_pixel)
assert tuple(program_pixel) == (255, 0, 255, 255), tuple(program_pixel)
gl.glDisable(0x8804)
gl.glBindProgramARB(0x8804, 0)
gl.glDeleteProgramsARB(1, ctypes.byref(fragment))
gl.glDisableVertexAttribArrayARB(1)
gl.glVertexAttribPointerARB(0, 2, 0x1406, 0, 0, ctypes.c_void_p(1))
fixed_vertices = (ctypes.c_float * 6)(-1, -1, 3, -1, -1, 3)
gl.glVertexPointer(2, 0x1406, 0, fixed_vertices)
gl.glEnableClientState(0x8074)
gl.glClear(0x4000)
gl.glDrawArrays(4, 0, 3)
gl.glReadPixels(0, 0, 1, 1, 0x1908, 0x1401, program_pixel)
assert tuple(program_pixel) == (0, 0, 0, 255), tuple(program_pixel)
source = "!!ARBvp1.0\n# vertex.attrib[0] is not used\nMOV result.position, vertex.position;\nMOV result.color, program.env[0];\nEND\n"
gl.glProgramStringARB(0x8620, 0x8875, len(source), ctypes.c_char_p(source))
gl.glDrawArrays(4, 0, 3)
gl.glReadPixels(0, 0, 1, 1, 0x1908, 0x1401, program_pixel)
assert tuple(program_pixel) == (0, 255, 0, 255), tuple(program_pixel)
gl.glDisableClientState(0x8074)
gl.glVertexAttribPointerARB(0, 2, 0x1406, 0, 12, vertices)
gl.glVertexAttribPointerARB(3, 4, 0x1401, 1, 0, ctypes.c_void_p(1))
gl.glEnableVertexAttribArrayARB(3)
fixed_colors = (ctypes.c_ubyte * 16)(*([0, 0, 0, 0] + [255, 0, 0, 255] * 3))
gl.glColorPointer(4, 0x1401, 0, fixed_colors)
gl.glEnableClientState(0x8076)
source = "!!ARBvp1.0\nMOV result.position, vertex.attrib[0];\nMOV result.color, vertex.color;\nEND\n"
gl.glProgramStringARB(0x8620, 0x8875, len(source), ctypes.c_char_p(source))
gl.glClear(0x4000)
gl.glDrawArrays(4, 1, 3)
gl.glReadPixels(0, 0, 1, 1, 0x1908, 0x1401, program_pixel)
assert tuple(program_pixel) == (255, 0, 0, 255), tuple(program_pixel)
source = "!!ARBvp1.0\nMOV result.position, vertex.attrib[0];\nMOV result.color, vertex.attrib[3];\nEND\n"
gl.glProgramStringARB(0x8620, 0x8875, len(source), ctypes.c_char_p(source))
gl.glClear(0x4000)
gl.glDrawArrays(4, 1, 3)
gl.glReadPixels(0, 0, 1, 1, 0x1908, 0x1401, program_pixel)
assert tuple(program_pixel) == (0, 0, 0, 255), tuple(program_pixel)
gl.glDisableClientState(0x8076)
gl.glDisableVertexAttribArrayARB(3)

# Unused attributes do not require an enabled conventional replacement.
gl.glVertexAttribPointerARB(2, 4, 0x1406, 0, 0, ctypes.c_void_p(1))
gl.glEnableVertexAttribArrayARB(2)
source = "!!ARBvp1.0\nMOV result.position, vertex.attrib[0];\nMOV result.color, {0,1,0,1};\nEND\n"
gl.glProgramStringARB(0x8620, 0x8875, len(source), ctypes.c_char_p(source))
gl.glClear(0x4000)
gl.glDrawArrays(4, 1, 3)
gl.glReadPixels(0, 0, 1, 1, 0x1908, 0x1401, program_pixel)
assert tuple(program_pixel) == (0, 255, 0, 255), tuple(program_pixel)
gl.glDisable(0x8620)
gl.glVertexPointer(2, 0x1406, 0, fixed_vertices)
gl.glEnableClientState(0x8074)
gl.glColor4f(0, 1, 0, 1)
gl.glClear(0x4000)
gl.glDrawArrays(4, 0, 3)
gl.glReadPixels(0, 0, 1, 1, 0x1908, 0x1401, program_pixel)
assert tuple(program_pixel) == (0, 255, 0, 255), tuple(program_pixel)
gl.glDisableClientState(0x8074)
gl.glEnable(0x8620)
source = "!!ARBvp1.0\nMOV result.position, vertex.attrib[0];\nMOV result.color, vertex.attrib[2];\nEND\n"
gl.glProgramStringARB(0x8620, 0x8875, len(source), ctypes.c_char_p(source))
gl.glClear(0x4000)
gl.glDrawArrays(4, 1, 3)
gl.glReadPixels(0, 0, 1, 1, 0x1908, 0x1401, program_pixel)
assert tuple(program_pixel) == (0, 0, 0, 255), tuple(program_pixel)
gl.glDisableVertexAttribArrayARB(2)
gl.glDisableVertexAttribArrayARB(0)
gl.glDisable(0x8620)
gl.glBindProgramARB(0x8620, 0)
gl.glDeleteProgramsARB(1, ctypes.byref(program))
assert gl.glGetError() == 0

# A stale generic attribute must not suppress a conventional fixed-function draw.
gl.glVertexAttribPointerARB(0, 2, 0x1406, 0, 0, ctypes.c_void_p(1))
gl.glEnableVertexAttribArrayARB(0)
fixed_vertices = (ctypes.c_float * 6)(-1, -1, 3, -1, -1, 3)
gl.glVertexPointer(2, 0x1406, 0, fixed_vertices)
gl.glEnableClientState(0x8074)
gl.glColor4f(0, 1, 0, 1)
gl.glDrawArrays(4, 0, 3)
gl.glReadPixels(0, 0, 1, 1, 0x1908, 0x1401, program_pixel)
assert tuple(program_pixel) == (0, 255, 0, 255), tuple(program_pixel)
gl.glDisableClientState(0x8074)
gl.glDisableVertexAttribArrayARB(0)
assert gl.glGetError() == 0

gl.glClear(0x4000)
gl.glColor3fv((ctypes.c_float * 3)(0.25, 0.5, 0.75))
gl.glBegin(4)
for vertex in ((-1, -1, 0), (3, -1, 0), (-1, 3, 0)):
    gl.glVertex3dv((ctypes.c_double * 3)(*vertex))
gl.glEnd()
gl.glReadPixels(0, 0, 1, 1, 0x1908, 0x1401, program_pixel)
assert all(abs(actual - expected) <= 1 for actual, expected in
           zip(program_pixel, (64, 128, 191, 255))), tuple(program_pixel)
coordinates = (ctypes.c_double * 4)()
for setter, args in ((gl.glTexCoord2d, (0.25, -0.5)),
                     (gl.glTexCoord2i, (3, -7)),
                     (gl.glTexCoord2s, (-11, 13))):
    setter(*args)
    gl.glGetDoublev(0x0b03, coordinates)
    assert tuple(coordinates) == args + (0.0, 1.0), tuple(coordinates)
gl.glLineStipple(3, 0xa53c)
gl.glGetIntegerv(0x0b25, ctypes.byref(value))
assert value.value == 0xa53c, value.value
gl.glGetIntegerv(0x0b26, ctypes.byref(value))
assert value.value == 3, value.value
gl.glLineStipple(1, 0xffff)
gl.glPushAttrib(0x40)
light = (ctypes.c_float * 4)(0.25, 0.5, 0.75, 1)
gl.glLightfv(0x4000, 0x1201, light)
queried_light = (ctypes.c_float * 4)()
gl.glGetLightfv(0x4000, 0x1201, queried_light)
assert tuple(queried_light) == tuple(light), tuple(queried_light)
queried_light[3] = 123
gl.glGetLightfv(0x4000, 0x1204, queried_light)
assert tuple(queried_light) == (0, 0, -1, 123), tuple(queried_light)
gl.glGetLightfv(0xffffffff, 0x1201, queried_light)
assert gl.glGetError() == 0x0500
assert tuple(queried_light) == (0, 0, -1, 123), tuple(queried_light)
gl.glPopAttrib()
assert gl.glGetError() == 0

gl.glMatrixMode(0x1701)
gl.glLoadIdentity()
glu.gluPerspective(90.0, 2.0, 1.0, 11.0)
matrix = (ctypes.c_double * 16)()
gl.glGetDoublev(0x0ba7, matrix)
assert abs(matrix[0] - 0.5) < 0.00001
assert abs(matrix[5] - 1.0) < 0.00001
gl.glMatrixMode(0x1700)
gl.glLoadIdentity()
glu.gluLookAt(1.0, 2.0, 3.0, 1.0, 2.0, 2.0, 0.0, 1.0, 0.0)
gl.glGetDoublev(0x0ba6, matrix)
assert all(abs(matrix[i] - expected) < 0.00001 for i, expected in enumerate(
    (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -1, -2, -3, 1))), tuple(matrix)
q = glu.gluNewQuadric()
assert q
glu.gluQuadricNormals(q, 100000)
glu.gluQuadricTexture(q, 1)
glu.gluQuadricOrientation(q, 100021)
glu.gluSphere(q, 1.0, 8, 8)
glu.gluCylinder(q, 1.0, 0.5, 2.0, 8, 8)
glu.gluDisk(q, 0.25, 1.0, 8, 2)
glu.gluPartialDisk(q, 0.25, 1.0, 8, 2, 30.0, 120.0)
assert gl.glGetError() == 0

gl.glMatrixMode(0x1701)
gl.glLoadIdentity()
gl.glMatrixMode(0x1700)
gl.glLoadIdentity()
gl.glViewport(0, 0, 32, 32)
for capability in (0x0b50, 0x0de1, 0x0b71, 0x0be2, 0x0c11, 0x0b44):
    gl.glDisable(capability)
gl.glClearColor(0, 0, 0, 1)
gl.glClear(0x4000)
gl.glColor4f(0, 1, 0, 1)
glu.gluQuadricOrientation(q, 100020)
glu.gluPartialDisk(q, 0.0, 0.9, 16, 1, 0.0, 90.0)
disk_pixel = (ctypes.c_ubyte * 4)()
gl.glReadPixels(22, 22, 1, 1, 0x1908, 0x1401, disk_pixel)
assert tuple(disk_pixel) == (0, 255, 0, 255), tuple(disk_pixel)
gl.glReadPixels(9, 9, 1, 1, 0x1908, 0x1401, disk_pixel)
assert tuple(disk_pixel) == (0, 0, 0, 255), tuple(disk_pixel)
gl.glClear(0x4000)
glu.gluDisk(q, 0.0, 0.9, 16, 1)
gl.glReadPixels(9, 9, 1, 1, 0x1908, 0x1401, disk_pixel)
assert tuple(disk_pixel) == (0, 255, 0, 255), tuple(disk_pixel)
gl.glEnable(0x0b44)
gl.glFrontFace(0x0901)
gl.glCullFace(0x0405)
gl.glClear(0x4000)
glu.gluDisk(q, 0.0, 0.9, 16, 1)
gl.glReadPixels(16, 16, 1, 1, 0x1908, 0x1401, disk_pixel)
assert tuple(disk_pixel) == (0, 255, 0, 255), tuple(disk_pixel)
gl.glClear(0x4000)
glu.gluQuadricOrientation(q, 100021)
glu.gluDisk(q, 0.0, 0.9, 16, 1)
gl.glReadPixels(16, 16, 1, 1, 0x1908, 0x1401, disk_pixel)
assert tuple(disk_pixel) == (0, 0, 0, 255), tuple(disk_pixel)
gl.glDisable(0x0b44)
gl.glClear(0x4000)
gl.glBegin(4)
gl.glVertex3d(-1.0, -1.0, 0.0)
gl.glVertex3d(1.0, -1.0, 0.0)
gl.glVertex3d(0.0, 1.0, 0.0)
gl.glEnd()
gl.glReadPixels(16, 16, 1, 1, 0x1908, 0x1401, disk_pixel)
assert tuple(disk_pixel) == (0, 255, 0, 255), tuple(disk_pixel)
glu.gluDeleteQuadric(q)

texture = (ctypes.c_ubyte * 16)(*([255, 0, 0, 255, 0, 0, 0, 0] * 2))
assert glu.gluBuild2DMipmaps(0x0de1, 3, 2, 2, 0x1907, 0x1401, texture) == 0
assert glu.gluBuild2DMipmaps(0x0de1, 3, 2, 2, 0x1907, 0x1401, ctypes.c_void_p(1)) == 100901
lists = gl.glGenLists(2)
assert lists
gl.glNewList(lists + 1, 0x1300)
gl.glClearColor(1.0, 0.0, 0.0, 1.0)
gl.glClear(0x4000)
gl.glEndList()
gl.glListBase(lists)
indices = (ctypes.c_ushort * 1)(1)
gl.glCallLists(1, 0x1403, indices)
pixel = (ctypes.c_ubyte * 4)()
gl.glReadPixels(0, 0, 1, 1, 0x1908, 0x1401, pixel)
assert tuple(pixel) == (255, 0, 0, 255), tuple(pixel)

gl.glGetString.argtypes = [uint]
gl.glGetString.restype = ctypes.c_char_p
extensions = gl.glGetString(0x1f03).split()
assert "GL_ARB_texture_compression" in extensions
assert "GL_EXT_texture_compression_s3tc" in extensions
assert "GL_ARB_texture_rectangle" in extensions
assert "GL_EXT_texture_rectangle" in extensions
assert "GL_ARB_vertex_program" in extensions
assert "GL_ARB_fragment_program" in extensions
gl.glGetIntegerv(0x86a2, ctypes.byref(value))
assert value.value > 0
formats = (ctypes.c_int * value.value)()
gl.glGetIntegerv(0x86a3, formats)
assert all(format in formats for format in (0x83f0, 0x83f2, 0x83f3)), tuple(formats)
bool_formats = (ctypes.c_ubyte * (value.value + 1))()
bool_formats[value.value] = 0x7f
gl.glGetBooleanv(0x86a3, bool_formats)
assert all(bool_formats[i] == 1 for i in range(value.value))
assert bool_formats[value.value] == 0x7f
gl.glGenTextures.argtypes = [ctypes.c_int, uintp]
gl.glBindTexture.argtypes = [uint, uint]
gl.glTexParameteri.argtypes = [uint, uint, ctypes.c_int]
gl.glGetTexLevelParameteriv.argtypes = [uint, ctypes.c_int, uint, intp]
gl.glTexCoord2f.argtypes = [ctypes.c_float] * 2
gl.glTexCoord4f.argtypes = [ctypes.c_float] * 4
gl.glTexCoord2fv.argtypes = [ctypes.POINTER(ctypes.c_float)]
gl.glVertex2f.argtypes = [ctypes.c_float] * 2
gl.glTexImage2D.argtypes = [uint, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                            ctypes.c_int, uint, uint, ctypes.c_void_p]
gl.glCopyTexSubImage2D.argtypes = [uint] + [ctypes.c_int] * 7
gl.glDrawBuffer.argtypes = [uint]
gl.glReadBuffer.argtypes = [uint]
for suffix in ("", "ARB"):
    getattr(gl, "glCompressedTexImage2D" + suffix).argtypes = [
        uint, ctypes.c_int, uint] + [ctypes.c_int] * 4 + [ctypes.c_void_p]
    getattr(gl, "glCompressedTexSubImage2D" + suffix).argtypes = [
        uint] + [ctypes.c_int] * 5 + [uint, ctypes.c_int, ctypes.c_void_p]
    getattr(gl, "glGetCompressedTexImage" + suffix).argtypes = [uint, ctypes.c_int, ctypes.c_void_p]
texture_name = uint()
gl.glGenTextures(1, ctypes.byref(texture_name))
gl.glBindTexture(0x0de1, texture_name)
gl.glTexParameteri(0x0de1, 0x2801, 0x2600)
gl.glTexParameteri(0x0de1, 0x2800, 0x2600)
gl.glMatrixMode(0x1701)
gl.glLoadIdentity()
gl.glMatrixMode(0x1700)
gl.glLoadIdentity()
gl.glViewport(0, 0, 1, 1)
gl.glEnable(0x0de1)
gl.glColor4f(1, 1, 1, 1)

def textured_pixel(coordinates=None):
    gl.glBegin(7)
    for x, y, u, v in ((-1, -1, 0, 0), (1, -1, 1, 0), (1, 1, 1, 1), (-1, 1, 0, 1)):
        if coordinates is None:
            gl.glTexCoord2f(u, v)
        else:
            gl.glTexCoord2fv(coordinates)
        gl.glVertex2f(x, y)
    gl.glEnd()
    gl.glReadPixels(0, 0, 1, 1, 0x1908, 0x1401, pixel)
    return tuple(pixel)

default_texture = uint()
gl.glGenTextures(1, ctypes.byref(default_texture))
gl.glBindTexture(0x0de1, default_texture)
green_texels = (ctypes.c_ubyte * 16)(*([0, 255, 0, 255] * 4))
gl.glTexImage2D(0x0de1, 0, 0x1908, 2, 2, 0, 0x1908, 0x1401, green_texels)
readback = (ctypes.c_float * 16)()
gl.glGetTexImage(0x0de1, 0, 0x1908, 0x1406, readback)
assert tuple(readback) == (0, 1, 0, 1) * 4, tuple(readback)
legacy_filter = os.environ.get("COCAINE_GL_LEGACY_TEXTURE_FILTER") == "1"
expected = (0, 255, 0, 255) if legacy_filter else (255, 255, 255, 255)
assert textured_pixel() == expected, tuple(pixel)
gl.glTexParameteri(0x0de1, 0x2801, 0x2702)
gl.glBindTexture(0x0de1, texture_name)
gl.glBindTexture(0x0de1, default_texture)
assert textured_pixel() == (255, 255, 255, 255), tuple(pixel)
gl.glTexParameteri(0x0de1, 0x2801, 0x2600)
assert textured_pixel() == (0, 255, 0, 255), tuple(pixel)
assert gl.glGetError() == 0
gl.glBindTexture(0x0de1, texture_name)

red_block = "\x00\xf8\x1f\x00\x00\x00\x00\x00"
for suffix in ("", "ARB"):
    for format, block, alpha in ((0x83f1, red_block, 255),
                                 (0x83f2, "\xff" * 8 + red_block, 255),
                                 (0x83f3, "\x80" + "\x00" * 7 + red_block, 128)):
        getattr(gl, "glCompressedTexImage2D" + suffix)(0x0de1, 0, format, 4, 4, 0, len(block), ctypes.c_char_p(block))
        assert gl.glGetError() == 0
        gl.glGetTexLevelParameteriv(0x0de1, 0, 0x1000, ctypes.byref(value))
        assert value.value == 4, value.value
        gl.glGetTexLevelParameteriv(0x0de1, 0, 0x86a0, ctypes.byref(value))
        assert value.value == len(block), value.value
        copied = ctypes.create_string_buffer(len(block))
        getattr(gl, "glGetCompressedTexImage" + suffix)(0x0de1, 0, copied)
        assert copied.raw == block, repr(copied.raw)
        assert textured_pixel() == (255, 0, 0, alpha), tuple(pixel)
    blue_block = "\xff" + "\x00" * 7 + "\x1f\x00\x00\xf8" + "\x00" * 4
    getattr(gl, "glCompressedTexSubImage2D" + suffix)(0x0de1, 0, 0, 0, 4, 4, 0x83f3, len(blue_block), ctypes.c_char_p(blue_block))
    assert gl.glGetError() == 0
    assert textured_pixel() == (0, 0, 255, 255), tuple(pixel)

gl.glGetTexParameteriv.argtypes = [uint, uint, intp]
gl.glTexParameterf.argtypes = [uint, uint, ctypes.c_float]
short_texture = uint()
gl.glGenTextures(1, ctypes.byref(short_texture))
gl.glBindTexture(0x0de1, short_texture)
gl.glTexParameteri(0x0de1, 0x2801, 0x2703)
smoke_block = "\x88" * 8 + red_block

def upload_smoke_level(level, width, height, block=smoke_block):
    data = block * (((width + 3) // 4) * ((height + 3) // 4))
    gl.glCompressedTexImage2DARB(0x0de1, level, 0x83f2, width, height,
                                 0, len(data), ctypes.c_char_p(data))
    assert gl.glGetError() == 0

def texture_parameter(parameter):
    gl.glGetTexParameteriv(0x0de1, parameter, ctypes.byref(value))
    return value.value

# Halo supplies only part of the particle's mip chain, sometimes out of order.
upload_smoke_level(2, 2, 1)
upload_smoke_level(0, 8, 4)
upload_smoke_level(1, 4, 2)
expected = (255, 0, 0, 136) if legacy_filter else (255, 255, 255, 255)
assert textured_pixel() == expected, tuple(pixel)
assert texture_parameter(0x2801) == 0x2703
assert texture_parameter(0x813d) == (2 if legacy_filter else 1000)
gl.glTexParameteri(0x0de1, 0x813c, 1000)
assert gl.glGetError() == 0
gl.glTexParameteri(0x0de1, 0x813c, 0)
assert textured_pixel() == expected, tuple(pixel)
if legacy_filter:
    gl.glEnable(0x0be2)
    gl.glBlendFunc(0x0302, 0x0303)
    gl.glClearColor(0, 1, 0, 1)
    gl.glClear(0x4000)
    assert textured_pixel()[:3] == (136, 119, 0), tuple(pixel)
    gl.glDisable(0x0be2)

gl.glTexParameteri(0x0de1, 0x813d, 0)
upload_smoke_level(1, 4, 2)
assert texture_parameter(0x813d) == 0
gl.glTexParameterf(0x0de1, 0x813d, 1)
upload_smoke_level(2, 2, 1)
assert texture_parameter(0x813d) == 1
gl.glTexParameteri(0x0de1, 0x813d, 1000)
upload_smoke_level(3, 1, 1, "\xff" * 8 + "\x1f\x00\x00\xf8" + "\x00" * 4)
assert textured_pixel() == (0, 0, 255, 255), tuple(pixel)
gl.glTexImage2D(0x0de1, 0, 0x1908, 2, 2, 0, 0x1908, 0x1401, green_texels)
assert texture_parameter(0x813d) == 1000
gl.glTexParameteri(0x0de1, 0x2801, 0x2600)
assert textured_pixel() == (0, 255, 0, 255), tuple(pixel)
gl.glDeleteTextures(1, ctypes.byref(short_texture))
gl.glBindTexture(0x0de1, short_texture)
upload_smoke_level(0, 8, 4)
gl.glTexParameteri(0x0de1, 0x2801, 0x2703)
upload_smoke_level(1, 4, 2)
assert texture_parameter(0x813d) == (1 if legacy_filter else 1000)
shared_fmt = uint()
shared_ctx = uint()
assert cgl.CGLChoosePixelFormat(attrs, ctypes.byref(shared_fmt), ctypes.byref(count)) == 0
assert cgl.CGLCreateContext(shared_fmt, ctx, ctypes.byref(shared_ctx)) == 0
value.value = -1
assert cgl.CGLGetParameter(shared_ctx, 1236, ctypes.byref(value)) == 0 and value.value == 0
value.value = -123
assert cgl.CGLSetParameter(shared_ctx, 1236, ctypes.byref(value)) == 0
assert cgl.CGLGetParameter(ctx, 1236, ctypes.byref(value)) == 0 and value.value == 12345678
assert gl.CGLSetCurrentContext(shared_ctx) == 0
gl.glBindTexture(0x0de1, short_texture)
upload_smoke_level(2, 2, 1)
assert texture_parameter(0x813d) == (2 if legacy_filter else 1000)
assert gl.CGLSetCurrentContext(ctx) == 0
assert textured_pixel() == expected, tuple(pixel)
assert cgl.CGLDestroyContext(shared_ctx) == 0
assert cgl.CGLDestroyPixelFormat(shared_fmt) == 0
gl.glDeleteTextures(1, ctypes.byref(short_texture))
gl.glBindTexture(0x0de1, texture_name)

texels = (ctypes.c_ubyte * 16)(255, 0, 0, 255, 0, 255, 0, 255,
                               0, 0, 255, 255, 255, 255, 255, 255)
gl.glTexImage2D(0x0de1, 0, 0x1908, 2, 2, 0, 0x1908, 0x1401, texels)
assert textured_pixel((ctypes.c_float * 2)(0.75, 0.25)) == (0, 255, 0, 255), tuple(pixel)
assert textured_pixel((ctypes.c_float * 2)(0.25, 0.75)) == (0, 0, 255, 255), tuple(pixel)
gl.glBegin(7)
for x, y in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
    gl.glTexCoord4f(0.5, 1.5, 0, 2)
    gl.glVertex2f(x, y)
gl.glEnd()
gl.glReadPixels(0, 0, 1, 1, 0x1908, 0x1401, pixel)
assert tuple(pixel) == (0, 0, 255, 255), tuple(pixel)
gl.glClearColor(1, 0, 1, 1)
gl.glClear(0x4000)
gl.glCopyTexSubImage2D(0x0de1, 0, 1, 0, 0, 0, 1, 1)
assert textured_pixel((ctypes.c_float * 2)(0.75, 0.25)) == (255, 0, 255, 255), tuple(pixel)
gl.glGetIntegerv(0x0c00, ctypes.byref(value))
assert value.value == 1, value.value
gl.glDrawBuffer(0x0409)
gl.glReadBuffer(0x0409)
gl.glGetIntegerv(0x0c01, ctypes.byref(value))
assert value.value == 0x0409, value.value
gl.glGetIntegerv(0x0c02, ctypes.byref(value))
assert value.value == 0x0409, value.value
gl.glClearColor(0, 1, 0, 1)
gl.glClear(0x4000)
gl.glCopyTexSubImage2D(0x0de1, 0, 0, 0, 0, 0, 1, 1)
gl.glReadBuffer(0x0405)
gl.glDrawBuffer(0x0405)
gl.glReadPixels(0, 0, 1, 1, 0x1908, 0x1401, pixel)
assert tuple(pixel) == (255, 0, 255, 255), tuple(pixel)
assert textured_pixel((ctypes.c_float * 2)(0.25, 0.25)) == (0, 255, 0, 255), tuple(pixel)
assert gl.glGetError() == 0
gl.glDisable(0x0de1)
gl.glBindTexture(0x0de1, 0)

rectangle = uint()
gl.glGenTextures(1, ctypes.byref(rectangle))
gl.glBindTexture(0x84f5, rectangle)
gl.glTexParameteri(0x84f5, 0x2801, 0x2600)
gl.glTexParameteri(0x84f5, 0x2800, 0x2600)
gl.glTexImage2D(0x84f5, 0, 0x1908, 2, 2, 0, 0x1908, 0x1401, texels)
gl.glEnable(0x84f5)
assert textured_pixel((ctypes.c_float * 2)(1.5, 0.5)) == (0, 255, 0, 255), tuple(pixel)
gl.glDisable(0x84f5)
gl.glBindTexture(0x84f5, 0)
assert gl.glGetError() == 0

gl.glClearColor(0.75, 0, 0, 1)
gl.glClear(0x4000)
gl.glEnable(0x0be2)
gl.glBlendFunc(1, 1)
gl.glBlendEquation(0x800b)
gl.glColor4f(0.25, 0, 0, 1)
gl.glBegin(7)
for x, y in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
    gl.glVertex2f(x, y)
gl.glEnd()
gl.glReadPixels(0, 0, 1, 1, 0x1908, 0x1401, pixel)
assert abs(pixel[0] - 128) <= 1 and pixel[1] == 0 and pixel[2] == 0, tuple(pixel)
gl.glBlendEquation(0x8006)
gl.glDisable(0x0be2)
gl.glColor4f(1, 1, 1, 1)

gl.glTexImage3D.argtypes = [uint, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                            ctypes.c_int, ctypes.c_int, uint, uint, ctypes.c_void_p]
texture3d = uint()
gl.glGenTextures(1, ctypes.byref(texture3d))
gl.glBindTexture(0x806f, texture3d)
gl.glTexParameteri(0x806f, 0x2801, 0x2600)
gl.glTexParameteri(0x806f, 0x2800, 0x2600)
gl.glPixelStorei(0x806d, 1)
texels3d = (ctypes.c_ushort * 8)(65535, 0, 0, 65535, 0, 0, 32768, 65535)
gl.glTexImage3D(0x806f, 0, 0x1908, 1, 1, 1, 0, 0x1908, 0x1403, texels3d)
assert gl.glGetError() == 0
gl.glPixelStorei(0x806d, 0)
gl.glGetTexLevelParameteriv(0x806f, 0, 0x8071, ctypes.byref(value))
assert value.value == 1
gl.glTexImage3D(0x806f, 0, 0x1908, 1, 1, 1, 0, 0x1908, 0x1403, ctypes.c_void_p(1))
gl.glEnable(0x806f)
assert textured_pixel() == (0, 0, 128, 255), tuple(pixel)
gl.glDisable(0x806f)
gl.glBindTexture(0x806f, 0)

cg.CGMainDisplayID.restype = uint
cg.CGDisplayIDToOpenGLDisplayMask.argtypes = [uint]
cg.CGDisplayIDToOpenGLDisplayMask.restype = uint
cg.CGDisplayCapture.argtypes = [uint]
cg.CGDisplayRelease.argtypes = [uint]
cg.CGDisplayBaseAddress.argtypes = [uint]
cg.CGDisplayBaseAddress.restype = ctypes.c_void_p
cg.CGDisplayBytesPerRow.argtypes = [uint]
cg.CGDisplayPixelsWide.argtypes = [uint]
cg.CGDisplayPixelsHigh.argtypes = [uint]
display = cg.CGMainDisplayID()
assert cg.CGDisplayCapture(display) == 0
try:
    assert cgl.CGLSetFullScreen(0) == 10004
    if has_on_display:
        assert cgl.CGLSetFullScreenOnDisplay(ctx, 0) == 10006
    assert cgl.CGLSetFullScreen(ctx) == 0
    mask = cg.CGDisplayIDToOpenGLDisplayMask(display)
    if has_on_display:
        assert cgl.CGLSetFullScreenOnDisplay(ctx, mask) == 0
    gl.glViewport(0, 0, 16, 16)
    gl.glClearColor(1.0, 0.0, 0.0, 1.0)
    gl.glClear(0x4000)
    gl.glEnable(0x0c11)
    gl.glScissor(0, 0, 1, 1)
    gl.glClearColor(0.0, 1.0, 0.0, 1.0)
    gl.glClear(0x4000)
    gl.glReadPixels(0, 0, 1, 1, 0x1908, 0x1401, pixel)
    assert tuple(pixel) == (0, 255, 0, 255), tuple(pixel)
    gl.glReadPixels(2, 2, 1, 1, 0x1908, 0x1401, pixel)
    assert tuple(pixel) == (255, 0, 0, 255), tuple(pixel)
    gl.glDisable(0x0c11)
    gl.glMatrixMode(0x1701)
    gl.glLoadIdentity()
    gl.glMatrixMode(0x1700)
    gl.glLoadIdentity()
    vertices = (ctypes.c_float * 6)(-1, -1, 3, -1, -1, 3)
    elements = (ctypes.c_ushort * 3)(0, 1, 2)
    gl.glVertexPointer(2, 0x1406, 0, vertices)
    gl.glEnableClientState(0x8074)
    gl.glColor4f(0.0, 0.0, 1.0, 1.0)
    gl.glDrawRangeElements(4, 0, 2, 3, 0x1403, elements)
    gl.glReadPixels(8, 8, 1, 1, 0x1908, 0x1401, pixel)
    assert tuple(pixel) == (0, 0, 255, 255), tuple(pixel)
    padded = (ctypes.c_float * 24)()
    for index, x, y in ((2, -1, -1), (4, 3, -1), (7, -1, 3)):
        padded[3 * index] = x
        padded[3 * index + 1] = y
    sparse = (ctypes.c_ushort * 3)(2, 4, 7)
    gl.glVertexPointer(2, 0x1406, 12, padded)
    gl.glColor4f(0.0, 1.0, 0.0, 1.0)
    gl.glDrawRangeElements(4, 2, 7, 3, 0x1403, sparse)
    gl.glReadPixels(8, 8, 1, 1, 0x1908, 0x1401, pixel)
    assert tuple(pixel) == (0, 255, 0, 255), tuple(pixel)
    assert gl.glGetError() == 0
    assert cgl.CGLFlushDrawable(ctx) == 0
    width = cg.CGDisplayPixelsWide(display)
    height = cg.CGDisplayPixelsHigh(display)
    stride = cg.CGDisplayBytesPerRow(display)
    base = cg.CGDisplayBaseAddress(display)
    shown = ctypes.string_at(base + (height // 2) * stride + (width // 2) * 4, 4)
    rgb = shown[1:4] if sys.byteorder == "big" else shown[2::-1]
    assert rgb == "\xff\x00\x00", repr(shown)
    # Keep the pacing-order case's single-frame setup.
    readback_colors = () if len(sys.argv) > 1 and sys.argv[1] == "pacing-order" else (
        (0.2, 0.4, 0.8), (0.8, 0.2, 0.4))
    for red, green, blue in readback_colors:
        gl.glClearColor(red, green, blue, 0.6)
        gl.glClear(0x4000)
        gl.glEnable(0x0c11)
        gl.glScissor(0, 0, 1, 1)
        gl.glClearColor(0, 1, 0, 1)
        gl.glClear(0x4000)
        gl.glDisable(0x0c11)
        pack_state = ((0x0d00, 1), (0x0d02, width + 7), (0x0d03, 2),
                      (0x0d04, 3), (0x0d05, 8))
        for name, setting in pack_state:
            gl.glPixelStorei(name, setting)
        assert cgl.CGLFlushDrawable(ctx) == 0
        for name, setting in pack_state:
            gl.glGetIntegerv(name, ctypes.byref(value))
            assert value.value == setting, (name, value.value, setting)
        shown = ctypes.string_at(base + (height // 2) * stride + (width // 2) * 4, 4)
        rgb = shown[1:4] if sys.byteorder == "big" else shown[2::-1]
        assert tuple(ord(ch) for ch in rgb) == tuple(int(c * 255 + 0.5) for c in (red, green, blue)), repr(shown)
        shown = ctypes.string_at(base + (height - 1) * stride, 4)
        rgb = shown[1:4] if sys.byteorder == "big" else shown[2::-1]
        assert rgb == "\x00\xff\x00", repr(shown)
        for name, setting in ((0x0d00, 0), (0x0d02, 0), (0x0d03, 0), (0x0d04, 0), (0x0d05, 4)):
            gl.glPixelStorei(name, setting)
        assert gl.glGetError() == 0
    if len(sys.argv) > 1 and sys.argv[1] == "pacing-order":
        import threading
        import time
        assert gl.CGLSetCurrentContext(0) == 0
        started = threading.Event()
        finished = threading.Event()
        errors = []

        def repaint():
            if gl.CGLSetCurrentContext(ctx) != 0:
                errors.append("current context")
                started.set()
                return
            gl.glClearColor(0.0, 1.0, 0.0, 1.0)
            gl.glClear(0x4000)
            started.set()
            if cgl.CGLFlushDrawable(ctx) != 0:
                errors.append("flush")
            finished.set()
            gl.CGLSetCurrentContext(0)

        worker = threading.Thread(target=repaint)
        worker.start()
        started.wait(10)
        assert started.isSet()
        visible_while_blocked = False
        deadline = time.time() + 2
        while time.time() < deadline and not finished.isSet():
            pixel = ctypes.string_at(base + (height // 2) * stride + (width // 2) * 4, 4)
            rgb = pixel[1:4] if sys.byteorder == "big" else pixel[2::-1]
            if rgb == "\x00\xff\x00":
                visible_while_blocked = True
                break
            time.sleep(0.005)
        worker.join()
        assert not errors, errors
        assert visible_while_blocked
        assert gl.CGLSetCurrentContext(ctx) == 0
    assert cgl.CGLClearDrawable(ctx) == 0
    assert cgl.CGLFlushDrawable(ctx) == 10005
finally:
    assert cg.CGDisplayRelease(display) == 0
assert gl.CGLSetCurrentContext(0) == 0
assert cgl.CGLDestroyContext(ctx) == 0
assert cgl.CGLDestroyContext(ctx) == 10004
print "CGL compatibility ok"
