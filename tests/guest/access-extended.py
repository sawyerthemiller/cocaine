import ctypes
import os
import shutil
import struct
import sys
import tempfile

# access_extended: several permission questions in one system call. The
# request is an array of 16-byte entries (name offset, flags, two unused
# words) followed by the names; an offset of zero means "the same file as the
# entry before". Each entry is answered with an error number of its own.
lib = ctypes.CDLL("/usr/lib/libSystem.B.dylib")
lib.__error.restype = ctypes.POINTER(ctypes.c_int)

X_OK, W_OK, R_OK = 1, 2, 4
READ, WRITE, DELETE = 1 << 9, 1 << 10, 1 << 12
EACCES, ENOENT, ENOMEM, ENOTDIR, EINVAL, ENAMETOOLONG = 13, 2, 12, 20, 22, 63

def pack(entries, offsets=None):
    table, names = "", ""
    for i, (name, flags) in enumerate(entries):
        offset = 0
        if name is not None:
            offset = 16 * len(entries) + len(names)
            names += name + "\0"
        if offsets:
            offset = offsets[i]
        table += struct.pack("=Iiii", offset, flags, 0, 0)
    return table + names

# Returns the call's result (0, or the error number it failed with) and the
# answers. The slot after the last answer must be left alone.
def ask(entries, offsets=None, size=None):
    request = pack(entries, offsets)
    if size is None:
        size = len(request)
    request = ctypes.create_string_buffer(request, max(size, len(request)) + 1)
    answers = (ctypes.c_int * (len(entries) + 1))(*([-7] * (len(entries) + 1)))
    lib.__error().contents.value = 0
    rc = lib.accessx_np(request, size, answers, os.getuid())
    assert rc in (0, -1), rc
    assert answers[len(entries)] == -7
    if rc:
        assert list(answers) == [-7] * (len(entries) + 1), list(answers)
        return lib.__error().contents.value, None
    return 0, list(answers)[:len(entries)]

# In the guest system's own Library unless another folder is named.
if len(sys.argv) > 1:
    directory = sys.argv[1]
    os.makedirs(directory)
else:
    directory = tempfile.mkdtemp(prefix="cocaine-access-", dir=os.path.expanduser("~/Library"))
os.mkdir(directory + "/locked")
plain = directory + "/plain"
readonly = directory + "/readonly"
inside = directory + "/locked/inside"
missing = directory + "/missing"
for path in (plain, readonly, inside):
    open(path, "wb").close()
os.chmod(readonly, 0444)
os.chmod(directory + "/locked", 0555)
try:
    # What Carbon asks before it replaces a file: may the original be written?
    assert ask([(plain, WRITE)]) == (0, [0])
    assert ask([(readonly, WRITE)]) == (0, [EACCES])
    # Several questions about one file; none of them needs the last to pass.
    assert ask([(readonly, READ), (None, WRITE), (None, DELETE), (None, 0)]) == (0, [0, EACCES, 0, 0])
    # Deleting is up to the folder a file is in, writing up to the file.
    assert ask([(inside, WRITE), (None, DELETE), (None, W_OK)]) == (0, [0, EACCES, 0])
    # A file that isn't there is an answer, not a failure of the call, and
    # the files after it are still looked at.
    assert ask([(missing, WRITE), (None, READ), (plain, READ)]) == (0, [ENOENT, ENOENT, 0])
    assert ask([(plain, WRITE), (missing, WRITE)]) == (0, [0, ENOENT])
    assert ask([(plain + "/below", WRITE), (plain, R_OK | W_OK)]) == (0, [ENOTDIR, 0])
    assert ask([(directory, W_OK), (None, X_OK)]) == (0, [0, 0])
    # Names are the guest's: this one exists only in Leopard's system root.
    assert ask([("/usr/bin/python2.5", READ), (None, X_OK), (None, R_OK)]) == (0, [0, 0, 0])
    assert ask([("/usr/bin/no-such-program", X_OK)]) == (0, [ENOENT])

    # Requests the kernel refuses outright, writing no answers.
    assert ask([(plain, WRITE)], offsets=[0]) == (EINVAL, None)
    assert ask([(plain, WRITE)], size=16) == (EINVAL, None)
    assert ask([(plain, WRITE)], size=17) == (EINVAL, None)
    assert ask([(plain, WRITE)], offsets=[4000]) == (EINVAL, None)
    assert ask([(plain, WRITE), (plain, WRITE)], offsets=[32, 16]) == (EINVAL, None)
    assert ask([(plain, WRITE)], size=16 * 1024 + 1) == (ENOMEM, None)
    assert ask([(plain, WRITE)] + [(None, WRITE)] * 100) == (ENOMEM, None)
    assert ask([(plain, WRITE)] + [(None, WRITE)] * 99) == (0, [0] * 100)
    # A name that can't be looked up at all fails the whole call.
    assert ask([(plain, WRITE), (directory + "/" + "n" * 300, WRITE), (plain, WRITE)]) == (ENAMETOOLONG, None)
    answers = (ctypes.c_int * 1)()
    assert lib.accessx_np(1, 64, answers, os.getuid()) == -1
finally:
    os.chmod(directory + "/locked", 0755)
    shutil.rmtree(directory)
print "access_extended ok"
