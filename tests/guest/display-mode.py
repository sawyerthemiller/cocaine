import ctypes
import sys
import time

cg = ctypes.CDLL("/System/Library/Frameworks/ApplicationServices.framework/Frameworks/CoreGraphics.framework/CoreGraphics")
cf = ctypes.CDLL("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")
uint = ctypes.c_uint
ptr = ctypes.c_void_p
cg.CGMainDisplayID.restype = uint
cg.CGDisplayCurrentMode.argtypes = [uint]
cg.CGDisplayCurrentMode.restype = ptr
cg.CGDisplayBestModeForParameters.argtypes = [uint, uint, uint, uint, ctypes.POINTER(uint)]
cg.CGDisplayBestModeForParameters.restype = ptr
cg.CGDisplaySwitchToMode.argtypes = [uint, ptr]
cg.CGDisplayPixelsWide.argtypes = [uint]
cg.CGDisplayPixelsHigh.argtypes = [uint]
cf.CFRetain.argtypes = [ptr]
cf.CFRetain.restype = ptr
cf.CFRelease.argtypes = [ptr]

display = cg.CGMainDisplayID()
width, height, bits, original_width, original_height = map(int, sys.argv[1:])
dsp = ctypes.CDLL("/System/Library/Frameworks/DrawSprocket.framework/DrawSprocket")
dsp.DSpFindBestContext.argtypes = [ptr, ctypes.POINTER(ptr)]
dsp.DSpContext_GetAttributes.argtypes = [ptr, ptr]
dsp.DSpContext_Reserve.argtypes = [ptr, ptr]
dsp.DSpContext_Release.argtypes = [ptr]
assert dsp.DSpStartup() == 0
try:
    for mode_width, mode_height in ((640, 480), (800, 600), (1024, 768)):
        for depth in (16, 32):
            # DSpContextAttributes: dimensions, color needs, depth masks,
            # preferred depths and page count (the rest remains zero).
            attrs = (uint * 18)()
            attrs[1], attrs[2], attrs[5] = mode_width, mode_height, 2
            attrs[8] = attrs[9] = 0xffffffff
            attrs[10] = attrs[11] = depth
            attrs[12] = 1
            context = ptr()
            assert dsp.DSpFindBestContext(attrs, ctypes.byref(context)) == 0
            found = (uint * 18)()
            assert dsp.DSpContext_GetAttributes(context, found) == 0
            assert (found[1], found[2], found[11]) == (mode_width, mode_height, depth), (
                mode_width, mode_height, depth, tuple(found))
            assert dsp.DSpContext_Reserve(context, attrs) == 0
            assert dsp.DSpContext_Release(context) == 0
finally:
    dsp.DSpShutdown()
original = cg.CGDisplayCurrentMode(display)
assert original
cf.CFRetain(original)
exact = uint()
for depth in (16, 32):
    small = cg.CGDisplayBestModeForParameters(display, depth, 640, 480, ctypes.byref(exact))
    assert small and exact.value, "missing exact 640x480 %d-bit mode" % depth
mode = cg.CGDisplayBestModeForParameters(display, bits, width, height, ctypes.byref(exact))
assert mode and exact.value
assert cg.CGDisplaySwitchToMode(display, mode) == 0
assert cg.CGDisplayPixelsWide(display) == width and cg.CGDisplayPixelsHigh(display) == height
time.sleep(30)
assert cg.CGDisplaySwitchToMode(display, original) == 0
assert cg.CGDisplayPixelsWide(display) == original_width and cg.CGDisplayPixelsHigh(display) == original_height
time.sleep(25)
cf.CFRelease(original)
print "display mode switch ok"
