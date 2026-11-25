import ctypes
import os
import shutil
import struct
import sys
import tempfile

from Foundation import NSAutoreleasePool, NSData, NSURL
from AppKit import NSDocument

# Saving a document onto a file that already exists. Cocoa writes the new
# contents to a temporary file and has Carbon's FSPathReplaceObject swap it
# in, which first asks whether the old file may be written (access_extended)
# and then gives the new file the old one's dates, permissions and access
# control list (setattrlist). Save As and Save To take another road (remove,
# then move), so only a plain Save shows whether this one works.
pool = NSAutoreleasePool.alloc().init()
lib = ctypes.CDLL("/usr/lib/libSystem.B.dylib")
carbon = ctypes.CDLL("/System/Library/Frameworks/CoreServices.framework/CoreServices")
SAVE, SAVE_AS = 0, 1

class Document(NSDocument):
    def dataOfType_error_(self, kind, error):
        return NSData.dataWithBytes_length_(self.text, len(self.text)), None

class BackedUpDocument(Document):
    def keepBackupFile(self):
        return True

class AttrList(ctypes.Structure):
    _fields_ = [("count", ctypes.c_ushort), ("reserved", ctypes.c_ushort),
                ("common", ctypes.c_uint), ("volume", ctypes.c_uint),
                ("directory", ctypes.c_uint), ("file", ctypes.c_uint),
                ("fork", ctypes.c_uint)]

def created(path):
    attrs = AttrList(5, 0, 0x200, 0, 0, 0, 0)
    result = ctypes.create_string_buffer(32)
    assert lib.getattrlist(path, ctypes.byref(attrs), result, len(result), 0) == 0
    return struct.unpack("=ii", result.raw[4:12])

def set_created(path, seconds):
    attrs = AttrList(5, 0, 0x200, 0, 0, 0, 0)
    value = ctypes.create_string_buffer(struct.pack("=ii", seconds, 0), 8)
    assert lib.setattrlist(path, ctypes.byref(attrs), value, 8, 0) == 0

# An access control list as the kernel takes and gives it: a magic number,
# two unused identifiers, the number of entries (-1 for "no list"), flags,
# then for each entry who it is about and two words, the kind and the rights.
EVERYONE = "\xab\xcd\xef" * 4 + "\0\0\0\x0c"
ALLOW, DENY = 1, 2
READ_DATA, WRITE_ATTRIBUTES = 1 << 1, 1 << 8

def access_list(entries):
    if entries is None:
        return struct.pack("=I32sII", 0x012cc16d, "", 0xffffffff, 0)
    record = struct.pack("=I32sII", 0x012cc16d, "", len(entries), 0)
    for kind, rights in entries:
        record += EVERYONE + struct.pack("=II", kind, rights)
    return record

def set_list(path, record, length=None):
    if length is None:
        length = len(record)
    attrs = AttrList(5, 0, 0x400000, 0, 0, 0, 0)
    value = struct.pack("=iI", 8, length) + record
    value = ctypes.create_string_buffer(value, len(value))
    return lib.setattrlist(path, ctypes.byref(attrs), value, len(value), 0)

def get_list(path):
    attrs = AttrList(5, 0, 0x400000, 0, 0, 0, 0)
    result = ctypes.create_string_buffer(1024)
    assert lib.getattrlist(path, ctypes.byref(attrs), result, len(result), 0) == 0
    offset, length = struct.unpack("=iI", result.raw[4:12])
    return result.raw[4 + offset:4 + offset + length]

# Asks the kernel one access_extended question about a file.
def allowed(path, right):
    request = struct.pack("=Iiii", 16, right << 8, 0, 0) + path + "\0"
    answer = ctypes.c_int(-1)
    assert lib.accessx_np(request, len(request), ctypes.byref(answer), os.getuid()) == 0
    return answer.value == 0

def write(path, text):
    f = open(path, "wb")
    f.write(text)
    f.close()

def read(path):
    f = open(path, "rb")
    try:
        return f.read()
    finally:
        f.close()

def save(document, path, text, operation=SAVE):
    document.text = text
    ok, error = document.saveToURL_ofType_forSaveOperation_error_(
        NSURL.fileURLWithPath_(path), "text", operation, None)
    return ok

def opened(path, kind=Document):
    document = kind.alloc().init()
    document.setFileURL_(NSURL.fileURLWithPath_(path))
    document.setFileType_("text")
    return document

# In the guest system's own Library unless another folder is named.
if len(sys.argv) > 1:
    directory = sys.argv[1]
    os.makedirs(directory)
else:
    directory = tempfile.mkdtemp(prefix="cocaine-save-", dir=os.path.expanduser("~/Library"))
try:
    # A document that was opened from a file, changed and saved, twice. The
    # file keeps the permissions and creation date it had.
    path = directory + "/opened.txt"
    write(path, "old")
    os.chmod(path, 0640)
    set_created(path, 1200000000)
    document = opened(path)
    assert save(document, path, "first save")
    assert read(path) == "first save"
    assert save(document, path, "second save")
    assert read(path) == "second save"
    assert os.stat(path).st_mode & 0777 == 0640, oct(os.stat(path).st_mode)
    assert created(path) == (1200000000, 0), created(path)

    # A new document's first Save, onto a name that is taken.
    path = directory + "/untitled.txt"
    write(path, "old")
    document = Document.alloc().init()
    assert save(document, path, "untitled")
    assert read(path) == "untitled"

    # The same with the old file kept beside the new one.
    path = directory + "/backed up.txt"
    write(path, "old")
    assert save(opened(path, BackedUpDocument), path, "new")
    assert read(path) == "new"
    assert read(directory + "/backed up~.txt") == "old"

    # A file that may not be written is left as it was.
    path = directory + "/readonly.txt"
    write(path, "old")
    os.chmod(path, 0444)
    assert not save(opened(path), path, "new")
    assert read(path) == "old"

    # Access control lists. The kernel takes one and acts on it: with reading
    # denied to everyone, the file's owner can't read it either.
    path = directory + "/listed.txt"
    write(path, "old")
    assert get_list(path) == ""
    assert set_list(path, access_list([(DENY, READ_DATA)])) == 0
    assert get_list(path) == access_list([(DENY, READ_DATA)]), repr(get_list(path))
    assert not allowed(path, READ_DATA)
    try:
        read(path)
        raise AssertionError("a file everyone is denied was read")
    except IOError:
        pass
    assert set_list(path, access_list(None)) == 0
    assert get_list(path) == ""
    assert read(path) == "old"
    assert set_list(path, access_list([]), 40) == -1
    assert set_list(path, "\0" + access_list([])[1:]) == -1
    assert set_list(path, access_list([(ALLOW, READ_DATA)] * 129)) == -1
    # A saved document keeps the list its file had.
    kept = access_list([(DENY, WRITE_ATTRIBUTES), (ALLOW, READ_DATA)])
    assert set_list(path, kept) == 0
    assert allowed(path, READ_DATA) and not allowed(path, WRITE_ATTRIBUTES)
    assert save(opened(path), path, "listed")
    assert read(path) == "listed"
    assert get_list(path) == kept, repr(get_list(path))
    assert not allowed(path, WRITE_ATTRIBUTES)

    # The roads that never needed any of this still work.
    path = directory + "/fresh.txt"
    assert save(Document.alloc().init(), path, "fresh")
    assert read(path) == "fresh"
    path = directory + "/save as.txt"
    write(path, "old")
    assert save(Document.alloc().init(), path, "save as", SAVE_AS)
    assert read(path) == "save as"

    # Carbon's call by itself: the replacement takes the original's place
    # and is gone from where it was.
    path = directory + "/original"
    replacement = directory + "/replacement"
    write(path, "original")
    write(replacement, "replacement")
    assert carbon.FSPathReplaceObject(path, replacement, None, None, None, 0) == 0
    assert read(path) == "replacement"
    assert not os.path.exists(replacement)
    # It refuses (afpAccessDenied) when the original may not be written,
    # unless told not to ask.
    write(replacement, "again")
    os.chmod(path, 0444)
    assert carbon.FSPathReplaceObject(path, replacement, None, None, None, 0) == -5000
    assert read(path) == "replacement" and read(replacement) == "again"
    assert carbon.FSPathReplaceObject(path, replacement, None, None, None, 0x10) == 0
    assert read(path) == "again"
    assert os.stat(path).st_mode & 0777 == 0444

    # Nothing temporary is left behind.
    assert sorted(os.listdir(directory)) == [
        "backed up.txt", "backed up~.txt", "fresh.txt", "listed.txt", "opened.txt",
        "original", "readonly.txt", "save as.txt", "untitled.txt"], os.listdir(directory)
finally:
    shutil.rmtree(directory)
print "document save ok"
