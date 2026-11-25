import ctypes
import os
import signal
import sys
import threading
import time
import unicodedata

import objc
import AppKit
from Foundation import (NSAutoreleasePool, NSObject, NSThread, NSRunLoop, NSDate, NSTimer,
                        NSDefaultRunLoopMode)
from AppKit import (NSApplication, NSWindow, NSEvent, NSOpenPanel, NSSavePanel, NSButton, NSView,
                    NSBackingStoreBuffered, NSTitledWindowMask, NSApplicationDefined,
                    NSModalPanelRunLoopMode, NSEventTrackingRunLoopMode)

# Open and save panels. Leopard's own can't be built here; cocaine leaves the
# panel object hollow and has this Mac's file picker answer when it is run.
# In a test the picker's helper answers from a file instead (the stand-in,
# COCAINE_PICK_ANSWER), which this script rewrites before each panel, and
# notes every request it gets in another (COCAINE_PICK_LOG). After every
# panel the notes must hold one more request, saying what the panel was set
# to: a panel that never reached the picker fails here.
#
# Arguments: a work folder (seen under the same path by guest and host), the
# system root's path on this Mac, and the real home folder.
# Prints "panels ok N", N being how many times the picker was asked.
work, root, home = sys.argv[1:4]
ANSWER, LOG = work + "/answer", work + "/requests"
asked = [0]
step = ["start"]

def fail(text):
    sys.stdout.write("FAIL (%s) %s\n" % (step[0], text))
    sys.stdout.flush()
    os._exit(1)

def check(good, text):
    if not good:
        fail(text)

def u(s):
    if isinstance(s, unicode):
        return s
    return unicode(s, "utf-8")

def answer(paths, delay=0):
    f = open(ANSWER, "wb")
    if delay:
        f.write("delay %g\n" % delay)
    for p in paths:
        f.write(u(p).encode("utf-8") + "\n")
    f.close()

def requests():
    try:
        lines = open(LOG, "rb").read().split("\n")
    except IOError:
        return []
    out = []
    for line in lines:
        if not line.startswith("request\t"):
            continue
        fields = {}
        for field in line.split("\t")[1:]:
            key, _, value = field.partition("=")
            fields.setdefault(key, []).append(value)
        out.append(fields)
    return out

def one_more(**want):
    asked[0] += 1
    seen = requests()
    check(len(seen) == asked[0], "the picker has had %d requests; %d panels were run" % (len(seen), asked[0]))
    for key, value in want.items():
        if not isinstance(value, list):
            value = [value]
        got = seen[-1].get(key, [])
        check(got == value, "the picker was told %s=%r; the panel was set to %r" % (key, got, value))

def picked(panel, paths, opens=True):
    paths = [u(p) for p in paths]
    check(panel.filename() == paths[0], "filename is %r, not %r" % (panel.filename(), paths[0]))
    check(panel.URL().path() == paths[0], "URL is %r" % (panel.URL().path(),))
    check(panel.directory() == os.path.dirname(paths[0]), "directory is %r" % (panel.directory(),))
    if opens:
        check(list(panel.filenames()) == paths, "filenames are %r, not %r" % (list(panel.filenames()), paths))
        check([x.path() for x in panel.URLs()] == paths, "URLs are %r" % ([x.path() for x in panel.URLs()],))

def cancelled(panel):
    check(panel.filename() is None, "filename after a cancel is %r" % (panel.filename(),))
    check(panel.URL() is None, "URL after a cancel is %r" % (panel.URL(),))
    names = panel.filenames()
    check(names is not None and len(names) == 0, "filenames after a cancel are %r" % (names,))

libobjc = ctypes.CDLL("/usr/lib/libobjc.A.dylib")
libobjc.sel_registerName.restype = ctypes.c_void_p
libobjc.sel_registerName.argtypes = [ctypes.c_char_p]
libobjc.objc_msgSend.restype = ctypes.c_void_p
libobjc.objc_msgSend.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
def pointer(obj, selector):
    return libobjc.objc_msgSend(objc.pyobjc_id(obj), libobjc.sel_registerName(selector))

def windows():
    return AppKit.NSCountWindows(None)

pool = NSAutoreleasePool.alloc().init()
app = NSApplication.sharedApplication()

one, two, folder = work + "/one.txt", work + "/two.txt", work + "/a folder"
for name in (one, two):
    f = open(name, "wb")
    f.write("picked\n")
    f.close()
os.mkdir(folder)

# ---- fifty of each, made and let go: nothing on the screen, nothing dies ----
step[0] = "making panels"
before = windows()
inner = NSAutoreleasePool.alloc().init()
for i in range(50):
    o, s = NSOpenPanel.openPanel(), NSSavePanel.savePanel()
    check(o.windowNumber() <= 0 and s.windowNumber() <= 0, "a new panel has a window on the screen")
    o.setMessage_(u"panel %d" % i)
    s.setDirectory_(u(work))
    del o, s
del inner
NSRunLoop.currentRunLoop().runUntilDate_(NSDate.dateWithTimeIntervalSinceNow_(0.3))
check(windows() == before, "making panels left %d windows where there were %d" % (windows(), before))
check(NSOpenPanel.alloc().init().windowNumber() <= 0, "a panel from alloc and init has a window")

# ---- settings: what Leopard starts with, and what is set reads back ----
step[0] = "settings"
o, s = NSOpenPanel.openPanel(), NSSavePanel.savePanel()
check(o.canChooseFiles() and not o.canChooseDirectories() and o.resolvesAliases()
      and not o.allowsMultipleSelection() and not o.treatsFilePackagesAsDirectories(), "an open panel's starting flags")
check(not s.canChooseFiles() and s.canCreateDirectories() and not o.canCreateDirectories()
      and not s.isExtensionHidden(), "a save panel's starting flags")
check(o.prompt() == u"Open" and s.prompt() == u"Save", "prompts start as %r and %r" % (o.prompt(), s.prompt()))
check(s.nameFieldLabel() == u"Save As:", "the name label starts as %r" % (s.nameFieldLabel(),))
check(o.message() == u"" and o.directory() is None and o.accessoryView() is None,
      "message, directory and accessory view start as %r, %r, %r" % (o.message(), o.directory(), o.accessoryView()))
for setter, getter in (("setAllowsMultipleSelection_", "allowsMultipleSelection"),
                       ("setCanChooseFiles_", "canChooseFiles"),
                       ("setCanChooseDirectories_", "canChooseDirectories"),
                       ("setResolvesAliases_", "resolvesAliases"),
                       ("setTreatsFilePackagesAsDirectories_", "treatsFilePackagesAsDirectories"),
                       ("setExtensionHidden_", "isExtensionHidden")):
    for value in (True, False, True):
        getattr(o, setter)(value)
        check(bool(getattr(o, getter)()) == value, "%s after %s(%r)" % (getter, setter, value))
        check(getattr(o, getter)() in (0, 1), "%s is not a whole 0 or 1" % getter)
for value, back in ((u"Choose It", u"Choose It"), (None, u"Open"), (u"", u"Open"), (u"Pick", u"Pick")):
    o.setPrompt_(value)
    check(o.prompt() == back, "prompt after %r is %r" % (value, o.prompt()))
check(pointer(o, "prompt") == pointer(o, "prompt"), "prompt is a new object each time")
for value, back in ((u"Name it:", u"Name it:"), (None, u"Save As:"), (u"", u"Save As:")):
    s.setNameFieldLabel_(value)
    check(s.nameFieldLabel() == back, "name label after %r is %r" % (value, s.nameFieldLabel()))
for value, back in ((u"a message", u"a message"), (None, u""), (u"again", u"again")):
    o.setMessage_(value)
    check(o.message() == back, "message after %r is %r" % (value, o.message()))
o.setDirectory_(u(folder))
check(o.directory() == u(folder), "directory is %r" % (o.directory(),))
check(pointer(o, "directory") == pointer(o, "directory"), "directory is a new object each time")
o.setDirectory_(None)
check(o.directory() is None, "directory after nil is %r" % (o.directory(),))
view = NSView.alloc().initWithFrame_(((0, 0), (200, 40)))
held = view.retainCount()
s.setAccessoryView_(view)
check(s.accessoryView() is view and view.retainCount() > held and view.superview() is None,
      "the accessory view is not kept as it was given")
check(s.frame().size.height < 2000, "setting an accessory view resized the panel to %r" % (s.frame().size.height,))
s.setAccessoryView_(None)
check(s.accessoryView() is None, "the accessory view after nil")
o.setTitle_(u"a title")
s.setRequiredFileType_(u"txt")
check(o.title() == u"a title" and s.requiredFileType() == u"txt", "title and required type")

# ---- every way of running a modal panel: a pick, a cancel, what it was told ----
step[0] = "runModal"
p = NSOpenPanel.openPanel()
answer([one])
check(p.runModal() == 1, "runModal with a pick did not return 1")
one_more(kind="open", multi="0", files="1", dirs="0", title="", prompt="", message="", dir="", name="")
picked(p, [one])
check(os.path.exists(p.filename().encode("utf-8")), "the picked file is not there for the guest")
answer([])
check(p.runModal() == 0, "runModal with a cancel did not return 0")
# (run again, a panel starts where its last answer was)
one_more(kind="open", dir=work)
cancelled(p)

step[0] = "runModalForTypes:"
p = NSOpenPanel.openPanel()
p.setDirectory_(u(folder))
answer([two])
check(p.runModalForTypes_([u"txt", u"'TEXT'", u"public.plain-text"]) == 1, "runModalForTypes: with a pick")
one_more(type=["txt", "'TEXT'", "public.plain-text"], dir=folder)
picked(p, [two])
# A file the types leave greyed out can't be the answer: the stand-in says so, and it is a cancel.
p = NSOpenPanel.openPanel()
answer([one])
check(p.runModalForTypes_([u"rtf"]) == 0, "a file of another type was let through")
one_more(type="rtf")
cancelled(p)

step[0] = "runModalForDirectory:file:"
p = NSOpenPanel.openPanel()
answer([one])
check(p.runModalForDirectory_file_(u(folder), None) == 1, "runModalForDirectory:file: with a pick")
one_more(dir=folder)
picked(p, [one])

step[0] = "runModalForDirectory:file:types:"
p = NSOpenPanel.openPanel()
p.setTitle_(u"cocaine title")
p.setPrompt_(u"Choose It")
p.setMessage_(u"cocaine message")
p.setAllowsMultipleSelection_(True)
p.setResolvesAliases_(False)
p.setTreatsFilePackagesAsDirectories_(True)
answer([one, two])
check(p.runModalForDirectory_file_types_(u(work), u"suggested.txt", [u"txt"]) == 1, "a pick of two")
one_more(kind="open", title="cocaine title", prompt="Choose It", message="cocaine message", multi="1",
         aliases="0", packages="1", dir=work, name="suggested.txt", type="txt")
picked(p, [one, two])
check(p.title() == u"cocaine title" and p.prompt() == u"Choose It" and p.message() == u"cocaine message"
      and p.allowsMultipleSelection(), "the panel's settings changed while it ran")
# Two picks where only one is allowed is not an answer either.
p = NSOpenPanel.openPanel()
check(p.runModalForDirectory_file_types_(None, None, None) == 0, "two picks from a panel that gives one")
one_more(multi="0")

step[0] = "a folder"
p = NSOpenPanel.openPanel()
p.setCanChooseDirectories_(True)
p.setCanChooseFiles_(False)
p.setCanCreateDirectories_(True)
answer([folder])
check(p.runModal() == 1, "a folder was not picked")
one_more(files="0", dirs="1", mkdir="1")
picked(p, [folder])

# ---- a slow answer, with a signal the guest handles arriving meanwhile ----
step[0] = "a signal during the wait"
alarms = []
signal.signal(signal.SIGALRM, lambda number, frame: alarms.append(number))
p = NSOpenPanel.openPanel()
answer([one], delay=3)
signal.alarm(1)
began = time.time()
check(p.runModal() == 1, "a signal during the wait turned the pick into a cancel")
took = time.time() - began
one_more()
picked(p, [one])
check(2.8 < took < 20, "the panel answered after %.1f s, not 3" % took)
time.sleep(0.05)
check(alarms == [signal.SIGALRM], "the guest's handler ran %d times" % len(alarms))

# ---- two threads ask at once: each gets an answer, one after the other ----
step[0] = "two threads"
first, second = NSOpenPanel.openPanel(), NSOpenPanel.openPanel()
codes = {}
def other():
    inner = NSAutoreleasePool.alloc().init()
    time.sleep(0.3)
    codes["second began"] = time.time()
    codes["second"] = second.runModalForDirectory_file_types_(None, None, None)
    codes["second at"] = time.time()
    del inner
answer([two], delay=1)
thread = threading.Thread(target=other)
began = time.time()
thread.start()
codes["first"] = first.runModalForDirectory_file_types_(None, None, None)
thread.join()
check(codes.get("first") == 1 and codes.get("second") == 1, "two threads got %r" % (codes,))
asked[0] += 1
one_more()
picked(first, [two])
picked(second, [two])
check(codes["second began"] - began < 0.9, "the second thread did not ask while the first was waiting")
check(codes["second at"] - began > 1.9, "the second thread's panel did not wait for the first's")

# ---- saving ----
step[0] = "save"
p = NSSavePanel.savePanel()
p.setRequiredFileType_(u"txt")
p.setNameFieldLabel_(u"Name it:")
p.setCanSelectHiddenExtension_(True)
# Leopard puts the required extension on a name that lacks it.
answer([work + "/saved by the guest"])
check(p.runModalForDirectory_file_(u(work), u"suggested.txt") == 1, "a save panel with a name")
one_more(kind="save", required="txt", label="Name it:", dir=work, name="suggested.txt", canhideext="1", mkdir="1")
picked(p, [work + "/saved by the guest.txt"], opens=False)
f = open(p.filename().encode("utf-8"), "wb")
f.write("written by the guest\n")
f.close()
# A name the app allows as it is stays as it is; ':' in a suggested name is '/' to this Mac's name field.
p = NSSavePanel.savePanel()
p.setAllowedFileTypes_([u"txt", u"text"])
p.setAllowsOtherFileTypes_(True)
answer([work + "/other.dat"])
check(p.runModalForDirectory_file_(None, u"a:b.txt") == 1, "a save panel allowing other types")
one_more(kind="save", type=["txt", "text"], othertypes="1", name="a/b.txt", dir="")
picked(p, [work + "/other.dat"], opens=False)
answer([])
check(p.runModal() == 0, "a cancelled save panel")
one_more(kind="save")
check(p.filename() is None, "filename after a cancelled save is %r" % (p.filename(),))

# ---- paths: what the guest can't reach is refused, what it can comes back as the guest names it ----
step[0] = "paths"
p = NSOpenPanel.openPanel()
p.setCanChooseDirectories_(True)
answer([home + "/Library/Preferences"])   # the guest has a Library of its own in that place
check(p.runModal() == 0, "a folder in the real Library was handed to the guest")
one_more()
cancelled(p)
answer([root + "/System/Library/CoreServices/SystemVersion.plist"])
check(p.runModal() == 1, "a file inside the system root")
one_more()
picked(p, ["/System/Library/CoreServices/SystemVersion.plist"])
check("ProductVersion" in open(p.filename().encode("utf-8")).read(), "the system root's file does not open")
# Names are handed over as the disk spells them, whichever way the picker spelled them.
names = {"nfc": "caf\xc3\xa9 one.txt", "nfd": "cafe\xcc\x81 two.txt"}
for stored in names.values():
    open(work + "/" + stored, "wb").close()
listing = os.listdir(work)
for kind, stored in names.items():
    check(stored in listing, "the %s name is not stored as written" % kind)
    other = unicodedata.normalize(kind == "nfc" and "NFD" or "NFC", unicode(stored, "utf-8"))
    check(other.encode("utf-8") != stored, "the two spellings of the %s name are the same" % kind)
    answer([u(work) + u"/" + other])
    check(p.runModal() == 1, "the %s name was not picked" % kind)
    one_more()
    got = p.filename().encode("utf-8")
    check(got == work + "/" + stored, "the %s name came back as %r" % (kind, got))
    check(os.path.basename(got) in listing, "the %s name is not the folder's own entry" % kind)
# Saves that would land somewhere else than the person chose.
p = NSSavePanel.savePanel()
for refused in (home + "/Library/cocaine-panel-test-refused.txt", home + "/.cocaine-panel-test-refused",
                root + "/Applications/cocaine-panel-test-refused.txt"):
    answer([refused])
    check(p.runModal() == 0, "a save to %s was allowed" % refused)
    one_more(kind="save")
    check(p.filename() is None, "a refused save left the name %r" % (p.filename(),))

# ---- sheets and the modeless panel: begun, then answered from the run loop ----
MODES = [NSDefaultRunLoopMode, NSModalPanelRunLoopMode, NSEventTrackingRunLoopMode]
DID_END = "panelDidEnd:returnCode:contextInfo:"
CONTEXT = 0x1234abc
now = {}

def later(target, selector, argument, delay):
    target.performSelector_withObject_afterDelay_inModes_(selector, argument, delay, MODES)

def nudge():
    app.postEvent_atStart_(NSEvent.otherEventWithType_location_modifierFlags_timestamp_windowNumber_context_subtype_data1_data2_(
        NSApplicationDefined, (0, 0), 0, 0.0, 0, None, 0, 0, 0), True)

def begin(panel, how, paths, delay=0, opens=True):
    now.clear()
    now.update({"panel": panel, "paths": paths, "delay": delay, "opens": opens, "began": time.time(),
                "windows": windows(), "returned": False})
    answer(paths, delay)
    if how == "sheet":
        panel.beginSheetForDirectory_file_modalForWindow_modalDelegate_didEndSelector_contextInfo_(
            u(work), None, window, steps, DID_END, CONTEXT)
    elif how == "sheet with types":
        panel.beginSheetForDirectory_file_types_modalForWindow_modalDelegate_didEndSelector_contextInfo_(
            u(work), None, [u"txt"], window, steps, DID_END, CONTEXT)
    else:
        panel.beginForDirectory_file_types_modelessDelegate_didEndSelector_contextInfo_(
            u(work), None, [u"txt"], steps, DID_END, CONTEXT)
    check("code" not in now, "the panel answered before begin... returned")
    now["returned"] = True

class Steps(NSObject):
    def panelDidEnd_returnCode_contextInfo_(self, panel, code, context):
        check(now.get("returned"), "the answer came before begin... returned")
        check("code" not in now, "the panel answered twice")
        now["code"] = code
        check(NSThread.isMainThread(), "the answer came on another thread")
        check(panel is now["panel"], "the answer names another panel")
        check(context == CONTEXT, "the context came back as %r" % (context,))
        check(code == (now["paths"] and 1 or 0), "the panel's code is %r" % (code,))
        took = time.time() - now["began"]
        check(took > now["delay"] - 0.2, "the answer came after %.1f s, not %g" % (took, now["delay"]))
        check(windows() == now["windows"], "a panel put a window on the screen")
        check(panel.windowNumber() <= 0, "the panel itself has a window")
        if now.get("types") is not None:
            one_more(dir=work, type=now["types"])
        else:
            one_more(dir=work)
        if code:
            picked(panel, now["paths"], now["opens"])
        elif now["opens"]:
            cancelled(panel)
        if now.get("modal"):
            # As an app's own dialog would: end the modal session from here. Nothing else
            # is going on (no mouse, no keys), and the session must still notice.
            app.stopModalWithCode_(7)
            now["stopped"] = time.time()
        else:
            later(self, "next:", None, 0.05)
    panelDidEnd_returnCode_contextInfo_ = objc.selector(panelDidEnd_returnCode_contextInfo_, signature="v@:@i^v")

    def next_(self, _):
        todo.pop(0)()

    def inModal_(self, _):
        check(NSRunLoop.currentRunLoop().currentMode() == NSModalPanelRunLoopMode, "the dialog is not modal")
        begin(NSOpenPanel.openPanel(), "sheet", [one], delay=5)
        now["modal"] = True

    def watchdog_(self, _):
        fail("the run loop was still going after 90 s")

def sheet_pick():
    step[0] = "a sheet"
    begin(NSOpenPanel.openPanel(), "sheet", [one])
def sheet_cancel():
    step[0] = "a sheet with types, cancelled"
    begin(NSOpenPanel.openPanel(), "sheet with types", [])
    now["types"] = "txt"
def modeless():
    step[0] = "a modeless panel"
    begin(NSOpenPanel.openPanel(), "modeless", [two])
    now["types"] = "txt"
def save_sheet():
    step[0] = "a save sheet"
    panel = NSSavePanel.savePanel()
    panel.setAccessoryView_(NSView.alloc().initWithFrame_(((0, 0), (200, 40))))
    panel.setRequiredFileType_(u"txt")
    begin(panel, "sheet", [work + "/sheet.txt"], opens=False)
def modal_host():
    # A dialog of the app's own, run as a modal session, with a default button pulsing
    # in it (drawn by another thread). A panel is begun on it and takes five seconds.
    step[0] = "a sheet on a modal dialog"
    later(steps, "inModal:", None, 0.3)
    code = app.runModalForWindow_(window)
    returned = time.time()
    check(code == 7, "the modal session ended with %r" % (code,))
    check(returned - now.get("stopped", 0) < 1.0,
          "the modal session went on %.1f s after it was stopped" % (returned - now.get("stopped", 0)))
    later(steps, "next:", None, 0.05)
def finish():
    step[0] = "the end"
    app.stop_(None)
    nudge()

window = NSWindow.alloc().initWithContentRect_styleMask_backing_defer_(
    ((200, 300), (480, 320)), NSTitledWindowMask, NSBackingStoreBuffered, False)
window.setReleasedWhenClosed_(False)
button = NSButton.alloc().initWithFrame_(((300, 20), (120, 32)))
button.setBezelStyle_(1)
button.setTitle_(u"OK")
button.setKeyEquivalent_(u"\r")
window.contentView().addSubview_(button)
window.makeKeyAndOrderFront_(None)
app.activateIgnoringOtherApps_(True)
steps = Steps.alloc().init()
todo = [sheet_pick, sheet_cancel, modeless, save_sheet, modal_host, finish]
later(steps, "next:", None, 0.3)
timer = NSTimer.timerWithTimeInterval_target_selector_userInfo_repeats_(90.0, steps, "watchdog:", None, False)
for mode in MODES:
    NSRunLoop.currentRunLoop().addTimer_forMode_(timer, mode)
app.run()
check(not todo, "the run loop ended with %d steps to go" % len(todo))
check(len(requests()) == asked[0], "the picker has had %d requests; %d panels were run" % (len(requests()), asked[0]))
window.close()
print "panels ok %d" % asked[0]
