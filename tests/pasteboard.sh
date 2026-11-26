#!/bin/sh
set -u

cocaine=${1:?expected cocaine executable}
here=$(cd "$(dirname "$0")" && pwd)
root=$here/../sysroots/10.5
if [ ! -f "$root/usr/bin/pbcopy" ] || [ ! -f "$root/usr/bin/pbpaste" ]; then
  echo "pasteboard test needs a Leopard system root"
  exit 77
fi

expected=$(printf 'first\ncaf\303\251')
fail=0
for arch in ppc i386; do
  actual=$(COCAINE_NO_VIEWER=1 LC_ALL=en_US.UTF-8 "$cocaine" --arch "$arch" --sysroot "$root" \
    "$root/bin/sh" -c 'printf first | /usr/bin/pbcopy || exit 1
      /usr/bin/pbpaste || exit 1
      printf "\n"
      printf "caf\303\251" | /usr/bin/pbcopy || exit 1
      /usr/bin/pbpaste || exit 1')
  status=$?
  if [ "$status" -ne 0 ] || [ "$actual" != "$expected" ]; then
    echo "FAIL  $arch guest pasteboard (exit $status)"
    fail=1
  else
    echo "ok    $arch guest pasteboard"
  fi
done
exit "$fail"
