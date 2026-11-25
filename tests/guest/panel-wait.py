from Foundation import NSAutoreleasePool
from AppKit import NSApplication, NSOpenPanel

# One open panel, run and reported. The test gives the stand-in picker a long
# delay and ends this program while it waits: the picker's helper must not be
# left behind (it would be a panel stranded on the screen).
pool = NSAutoreleasePool.alloc().init()
NSApplication.sharedApplication()
print "the panel answered %d" % NSOpenPanel.openPanel().runModal()
