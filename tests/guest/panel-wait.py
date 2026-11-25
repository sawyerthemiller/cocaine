import os
import sys
import threading
import time

from Foundation import NSAutoreleasePool
from AppKit import NSApplication, NSOpenPanel

# One open panel, run and reported. The test gives the stand-in picker a long
# delay and ends this program while it waits: the picker's helper must not be
# left behind (it would be a panel stranded on the screen). The test ends it
# from outside; with "exit" as the argument, another thread here calls exit
# two seconds in, while the main one waits in the panel.
pool = NSAutoreleasePool.alloc().init()
NSApplication.sharedApplication()
if sys.argv[1:] == ["exit"]:
    def leave():
        time.sleep(2)
        os._exit(7)
    threading.Thread(target=leave).start()
print "the panel answered %d" % NSOpenPanel.openPanel().runModal()
