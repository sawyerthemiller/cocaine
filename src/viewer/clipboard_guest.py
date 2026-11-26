import base64
import ctypes
import errno
import plistlib
import struct
import sys
import urllib
import urlparse

cf = ctypes.CDLL("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")
pb = ctypes.CDLL("/System/Library/Frameworks/ApplicationServices.framework/Frameworks/HIServices.framework/HIServices")
ls = ctypes.CDLL("/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/LaunchServices")
libc = ctypes.CDLL("/usr/lib/libSystem.B.dylib")
ptr = ctypes.c_void_p
uint = ctypes.c_uint

cf.CFStringCreateWithCString.argtypes = [ptr, ctypes.c_char_p, uint]
cf.CFStringCreateWithCString.restype = ptr
cf.CFDataCreate.argtypes = [ptr, ptr, ctypes.c_long]
cf.CFDataCreate.restype = ptr
cf.CFDataGetLength.argtypes = [ptr]
cf.CFDataGetLength.restype = ctypes.c_long
cf.CFDataGetBytePtr.argtypes = [ptr]
cf.CFDataGetBytePtr.restype = ptr
cf.CFRelease.argtypes = [ptr]
cf.CFEqual.argtypes = [ptr, ptr]
cf.CFEqual.restype = ctypes.c_ubyte
cf.CFArrayGetCount.argtypes = [ptr]
cf.CFArrayGetCount.restype = ctypes.c_long
cf.CFArrayGetValueAtIndex.argtypes = [ptr, ctypes.c_long]
cf.CFArrayGetValueAtIndex.restype = ptr
pb.PasteboardCreate.argtypes = [ptr, ctypes.POINTER(ptr)]
pb.PasteboardSynchronize.argtypes = [ptr]
pb.PasteboardGetItemCount.argtypes = [ptr, ctypes.POINTER(uint)]
pb.PasteboardGetItemIdentifier.argtypes = [ptr, uint, ctypes.POINTER(ptr)]
pb.PasteboardCopyItemFlavors.argtypes = [ptr, ptr, ctypes.POINTER(ptr)]
pb.PasteboardCopyItemFlavorData.argtypes = [ptr, ptr, ptr, ctypes.POINTER(ptr)]
pb.PasteboardClear.argtypes = [ptr]
pb.PasteboardPutItemFlavor.argtypes = [ptr, ptr, ptr, ptr, uint]
ls.UTTypeCreatePreferredIdentifierForTag.argtypes = [ptr, ptr, ptr]
ls.UTTypeCreatePreferredIdentifierForTag.restype = ptr
libc.stat64.argtypes = [ctypes.c_char_p, ptr]
libc.__error.restype = ctypes.POINTER(ctypes.c_int)


def string(value):
    result = cf.CFStringCreateWithCString(None, value, 0x08000100)
    if not result:
        raise RuntimeError("cannot create pasteboard string")
    return result


def check(status, operation):
    if status:
        raise RuntimeError("%s failed: %d" % (operation, status))


name = string("com.apple.pasteboard.clipboard")
flavors = [(string("public.utf8-plain-text"), "utf-8"),
           (string("public.utf16-external-plain-text"), "utf-16"),
           (string("public.utf16-plain-text"), "utf-16-be")]


def legacy_flavor(tag_class, tag):
    tag_class = string(tag_class)
    tag = string(tag)
    flavor = ls.UTTypeCreatePreferredIdentifierForTag(tag_class, tag, None)
    cf.CFRelease(tag_class)
    cf.CFRelease(tag)
    if not flavor:
        raise RuntimeError("cannot resolve a legacy pasteboard type")
    return flavor


for tag_class, tag, encoding in (
        ("com.apple.ostype", "ut16", "utf-16"),
        ("com.apple.ostype", "utxt", "utf-16-be"),
        ("com.apple.nspboard-type", "NSStringPboardType", "utf-8"),
        ("com.apple.ostype", "TEXT", "mac-roman")):
    flavor = legacy_flavor(tag_class, tag)
    if any(cf.CFEqual(flavor, existing) for existing, encoding_ in flavors):
        cf.CFRelease(flavor)
    else:
        flavors.append((flavor, encoding))
file_flavor = legacy_flavor("com.apple.nspboard-type", "NSFilenamesPboardType")
file_url_flavors = [string("public.file-url"), legacy_flavor("com.apple.ostype", "furl")]
rich_text_flavors = [
    ("rtf", [string("public.rtf"), legacy_flavor("com.apple.nspboard-type", "NSRTFPboardType"),
             legacy_flavor("com.apple.ostype", "RTF ")]),
    ("html", [string("public.html"), legacy_flavor("com.apple.nspboard-type", "NSHTMLPboardType"),
              legacy_flavor("com.apple.ostype", "HTML")])]
image_flavors = [
    ("tiff", [string("public.tiff"), legacy_flavor("com.apple.nspboard-type", "NSTIFFPboardType")]),
    ("png", [string("public.png"), legacy_flavor("com.apple.ostype", "PNGf"),
             legacy_flavor("com.apple.nspboard-type", "Apple PNG pasteboard type")]),
    ("jpeg", [string("public.jpeg"), legacy_flavor("com.apple.ostype", "JPEG")])]
board = ptr()
check(pb.PasteboardCreate(name, ctypes.byref(board)), "PasteboardCreate")
cached_content = {}
have_content = False


def flavor_data(item, flavor, available):
    if not any(cf.CFEqual(flavor, existing) for existing in available):
        return None
    data = ptr()
    status = pb.PasteboardCopyItemFlavorData(board, item, flavor, ctypes.byref(data))
    if status == -25133:
        return None
    check(status, "PasteboardCopyItemFlavorData")
    try:
        return ctypes.string_at(cf.CFDataGetBytePtr(data), cf.CFDataGetLength(data))
    finally:
        cf.CFRelease(data)


def item_flavors(item):
    array = ptr()
    check(pb.PasteboardCopyItemFlavors(board, item, ctypes.byref(array)), "PasteboardCopyItemFlavors")
    try:
        # Retain the strings while the array is released.
        result = [cf.CFArrayGetValueAtIndex(array, index) for index in range(cf.CFArrayGetCount(array))]
        for flavor in result:
            cf.CFRetain(flavor)
        return result
    finally:
        cf.CFRelease(array)


cf.CFRetain.argtypes = [ptr]
cf.CFRetain.restype = ptr


def file_identity(path):
    stat = ctypes.create_string_buffer(108)
    if libc.stat64(path.encode("utf-8"), stat):
        error = libc.__error().contents.value
        if error in (errno.ENOENT, errno.EACCES, errno.ENOTDIR):
            return None
        raise OSError(error, "stat64 failed")
    device = struct.unpack("=I", stat.raw[0:4])[0]
    inode = struct.unpack("=Q", stat.raw[8:16])[0]
    return {"dev": str(device), "ino": str(inode)}


def read_content():
    global cached_content, have_content
    modified = pb.PasteboardSynchronize(board) & 1
    if have_content and not modified:
        return cached_content, False
    count = uint()
    check(pb.PasteboardGetItemCount(board, ctypes.byref(count)), "PasteboardGetItemCount")
    content = {}
    if not count.value:
        cached_content, have_content = content, True
        return content, True
    item = ptr()
    check(pb.PasteboardGetItemIdentifier(board, 1, ctypes.byref(item)), "PasteboardGetItemIdentifier")
    available = item_flavors(item)
    raw = flavor_data(item, file_flavor, available)
    for flavor in available:
        cf.CFRelease(flavor)
    paths = []
    if raw is not None:
        paths = plistlib.readPlistFromString(raw)
        if not isinstance(paths, list) or not all(isinstance(p, basestring) and p.startswith("/") for p in paths):
            raise RuntimeError("invalid guest file clipboard")
    else:
        for index in range(1, count.value + 1):
            item = ptr()
            check(pb.PasteboardGetItemIdentifier(board, index, ctypes.byref(item)), "PasteboardGetItemIdentifier")
            available = item_flavors(item)
            for flavor in file_url_flavors:
                raw = flavor_data(item, flavor, available)
                if raw is not None:
                    url = urlparse.urlsplit(raw)
                    if url.scheme != "file" or url.netloc not in ("", "localhost"):
                        raise RuntimeError("invalid guest file URL")
                    paths.append(urllib.unquote(url.path).decode("utf-8"))
                    break
            for flavor in available:
                cf.CFRelease(flavor)
    if paths:
        content["files"] = []
        for path in paths:
            entry = {"path": path}
            identity = file_identity(path)
            if identity is not None:
                entry["identity"] = identity
            content["files"].append(entry)
    item = ptr()
    check(pb.PasteboardGetItemIdentifier(board, 1, ctypes.byref(item)), "PasteboardGetItemIdentifier")
    available = item_flavors(item)
    for flavor, encoding in flavors:
        raw = flavor_data(item, flavor, available)
        if raw is not None:
            content["text"] = raw.decode(encoding)
            break
    for kind, choices in rich_text_flavors + image_flavors:
        for flavor in choices:
            raw = flavor_data(item, flavor, available)
            if raw is not None:
                content[kind] = plistlib.Data(raw)
                break
    for flavor in available:
        cf.CFRelease(flavor)
    cached_content, have_content = content, True
    return content, True


def put(item, flavor, raw):
    buf = ctypes.create_string_buffer(raw, len(raw) + 1)
    data = cf.CFDataCreate(None, buf, len(raw))
    if not data:
        raise RuntimeError("cannot create pasteboard data")
    try:
        check(pb.PasteboardPutItemFlavor(board, ptr(item), flavor, data, 0),
              "PasteboardPutItemFlavor")
    finally:
        cf.CFRelease(data)


def write_content(content):
    global have_content
    pb.PasteboardSynchronize(board)
    check(pb.PasteboardClear(board), "PasteboardClear")
    written = {}

    def put_once(item, flavor, raw):
        previous = written.setdefault(item, [])
        if not any(cf.CFEqual(flavor, existing) for existing in previous):
            put(item, flavor, raw)
            previous.append(flavor)

    if "text" in content:
        for flavor, encoding in flavors:
            try:
                raw = content["text"].encode(encoding)
            except UnicodeEncodeError:
                continue
            put_once(1, flavor, raw)
    for kind, choices in rich_text_flavors + image_flavors:
        if kind in content:
            for flavor in choices:
                put_once(1, flavor, content[kind].data)
    if content.get("files"):
        paths = content["files"]
        put_once(1, file_flavor, plistlib.writePlistToString(paths))
        for index, path in enumerate(paths):
            url = "file://" + urllib.quote(path.encode("utf-8"))
            for flavor in file_url_flavors:
                put_once(index + 1, flavor, url)
    have_content = False


while True:
    line = sys.stdin.readline()
    if not line:
        break
    if line == "G\n":
        content, changed = read_content()
        answer = "T" + base64.b64encode(content["text"].encode("utf-8")) if "text" in content else "E"
    elif line.startswith("S"):
        write_content({"text": base64.b64decode(line[1:].strip()).decode("utf-8")})
        answer = "O"
    elif line == "P\n":
        content, changed = read_content()
        answer = "P" + base64.b64encode(plistlib.writePlistToString(content)) if changed else "U"
    elif line.startswith("W"):
        write_content(plistlib.readPlistFromString(base64.b64decode(line[1:].strip())))
        answer = "O"
    elif line.startswith("V"):
        path = base64.b64decode(line[1:].strip()).decode("utf-8")
        identity = file_identity(path)
        answer = "N" if identity is None else "V" + base64.b64encode(plistlib.writePlistToString(identity))
    elif line == "C\n":
        write_content({})
        answer = "O"
    else:
        raise RuntimeError("invalid clipboard bridge command")
    sys.stdout.write(answer + "\n")
    sys.stdout.flush()
