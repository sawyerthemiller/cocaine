#!/bin/sh
set -u

cocaine=${1:?expected cocaine executable}
here=$(cd "$(dirname "$0")" && pwd)
root=$here/../sysroots/10.5
if [ ! -f "$root/usr/bin/python2.5" ] || [ ! -r /var/run/resolv.conf ]; then
  echo "network test needs Leopard Python and a host resolver configuration"
  exit 77
fi

realroot=$(cd "$root" && pwd -P)
fail=0
for arch in ppc i386; do
  fixture=$(mktemp -d "/tmp/cocaine-network-$arch.XXXXXX") || exit 1
  mkdir -p "$fixture/private/etc" "$fixture/private/var/run"
  ln -s "$realroot/usr" "$fixture/usr"
  ln -s "$realroot/System" "$fixture/System"
  ln -s private/etc "$fixture/etc"
  ln -s private/var "$fixture/var"
  ln -s /var/run/resolv.conf "$fixture/private/etc/resolv.conf"
  ln -s /var/run/resolv.conf "$fixture/host-resolv"
  COCAINE_NO_VIEWER=1 PYTHONDONTWRITEBYTECODE=1 "$cocaine" --arch "$arch" --sysroot "$fixture" \
    "$root/usr/bin/python2.5" -c "$(cat "$here/guest/network.py")" "$fixture/host-resolv" || fail=1
  rm -f "$fixture/host-resolv" "$fixture/etc" "$fixture/var" "$fixture/usr" "$fixture/System" \
    "$fixture/private/etc/resolv.conf" "$fixture/private/var/run/resolv.conf" \
    "$fixture/.cocaine/art/SArtFile.bin"
  find "$fixture" -depth -type d -exec rmdir {} \; 2>/dev/null
  if [ -d "$fixture" ]; then
    echo "could not remove network test fixture $fixture"
    fail=1
  fi
  for address in 127.0.0.1 ::1; do
    actual=$(COCAINE_NO_VIEWER=1 "$cocaine" --arch "$arch" --sysroot "$root" \
      "$root/usr/sbin/scutil" -r "$address" 2>&1 | grep -v -E '\[(AppleSharpener|BlurTweak)\] ')
    if [ "$actual" != "Reachable,Local Address" ]; then
      echo "FAIL $arch reachability $address: $actual"
      fail=1
    fi
  done
done
exit "$fail"
