import errno
import fcntl
import os
import socket
import struct
import sys

host_config = sys.argv[1]
guest_config = "/var/run/resolv.conf"

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
try:
    fcntl.ioctl(sock.fileno(), -2147195266, struct.pack("i", 1))
    assert fcntl.fcntl(sock.fileno(), fcntl.F_GETFL) & os.O_NONBLOCK
    fcntl.ioctl(sock.fileno(), -2147195266, struct.pack("i", 0))
    assert not (fcntl.fcntl(sock.fileno(), fcntl.F_GETFL) & os.O_NONBLOCK)
    ifreq = fcntl.ioctl(sock.fileno(), -1071617775, struct.pack("16s16x", "lo0"))
    assert struct.unpack_from("H", ifreq, 16)[0] & 0x9 == 0x9
finally:
    sock.close()

route = socket.socket(17, socket.SOCK_RAW, 0)
fd = os.dup(route.fileno())
route.close()
try:
    try:
        os.write(fd, "bad")
        raise AssertionError("malformed route request succeeded")
    except OSError, e:
        assert e.errno != 0
    header = struct.pack("=HBBH2x7I14I", *(
        (108, 5, 4, 0, 0, 1, 0, 456, 0, 0, 0) + (0,) * 14))
    address = struct.pack("!BBH4s8x", 16, socket.AF_INET, 0,
                          socket.inet_aton("127.0.0.1"))
    assert os.write(fd, header + address) == 108
    reply = os.read(fd, 1024)
    assert len(reply) >= 92
    assert struct.unpack_from("=HBB", reply)[1:] == (5, 4)
    assert struct.unpack_from("=I", reply, 20)[0] == 456
    assert struct.unpack_from("=I", reply, 24)[0] == 0
    assert os.write(fd, header + address) == 108
    short_reply = os.read(fd, 8)
    assert len(short_reply) == 8
    assert struct.unpack_from("=HBB", short_reply)[1:] == (5, 4)
finally:
    os.close(fd)

assert os.readlink("/etc/resolv.conf") == guest_config
fd = os.open("/etc/resolv.conf", os.O_RDONLY)
try:
    snapshot = os.fstat(fd)
    original = os.stat(host_config)
    assert (snapshot.st_dev, snapshot.st_ino) != (original.st_dev, original.st_ino)
    content = os.read(fd, 65536)
    assert content == open(host_config, "rb").read()
    try:
        os.write(fd, "not writable")
        raise AssertionError("resolver snapshot was writable")
    except OSError, e:
        assert e.errno == errno.EBADF
finally:
    os.close(fd)

try:
    os.open(guest_config, os.O_WRONLY)
    raise AssertionError("missing guest configuration opened for writing")
except OSError, e:
    assert e.errno == errno.ENOENT

fd = os.open(guest_config, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0600)
try:
    os.write(fd, "nameserver 192.0.2.1\n")
finally:
    os.close(fd)
assert open("/etc/resolv.conf", "rb").read() == "nameserver 192.0.2.1\n"
os.unlink(guest_config)

os.symlink("/var/run/missing-resolver", guest_config)
try:
    try:
        os.open("/etc/resolv.conf", os.O_RDONLY)
        raise AssertionError("dangling guest configuration used host fallback")
    except OSError, e:
        assert e.errno == errno.ENOENT
finally:
    os.unlink(guest_config)

print "network checks ok"
