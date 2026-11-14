#!/bin/sh
# (inside the container) builds wvkbd from /src and the test clients into /tmp/b
set -eu
B=/tmp/b; mkdir -p $B
cp -r /src $B/wvkbd
make -s -C $B/wvkbd clean >/dev/null 2>&1 || true
make -s -C $B/wvkbd wvkbd-mobintl >/dev/null
P=/usr/share/wayland-protocols
gen() { wayland-scanner client-header "$1" $B/$2-client-protocol.h; wayland-scanner private-code "$1" $B/$2-protocol.c; }
gen $P/unstable/text-input/text-input-unstable-v3.xml text-input-unstable-v3
gen $P/stable/xdg-shell/xdg-shell.xml xdg-shell
gen /usr/share/wlr-protocols/unstable/wlr-virtual-pointer-unstable-v1.xml wlr-virtual-pointer-unstable-v1
cc -O1 -I$B -o $B/ti-client /t/ti-client.c $B/text-input-unstable-v3-protocol.c $B/xdg-shell-protocol.c -lwayland-client
cc -O1 -I$B -o $B/vptr /t/vptr.c $B/wlr-virtual-pointer-unstable-v1-protocol.c -lwayland-client
