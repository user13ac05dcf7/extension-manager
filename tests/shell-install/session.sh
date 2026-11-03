#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs inside the container from run.sh: builds Extension Manager and the driver.
set -uo pipefail
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
# Glycin's nested bubblewrap cannot run under Podman's syscall restrictions
export GLYCIN_DISABLE_SANDBOX=i-know-the-risks
gnome-shell --version
cp -a /src /tmp/src && cd /tmp/src && rm -rf _build
if ! { meson setup /tmp/b -Dbacktrace=false >/tmp/meson.log 2>&1 && ninja -C /tmp/b >/tmp/ninja.log 2>&1; }; then
    tail -30 /tmp/meson.log /tmp/ninja.log
    exit 1
fi
objects=$(ls /tmp/b/src/extension-manager.p/*.o | grep -v '/main.c.o$')
# shellcheck disable=SC2046,SC2086
gcc -o /tmp/driver /test/driver.c $objects -I src -I /tmp/b/src -I /tmp/b \
    $(pkg-config --cflags --libs gtk4 libadwaita-1 json-glib-1.0 libsoup-3.0 libxml-2.0 \
      glycin-2 glycin-gtk4-2 gnutls libarchive gio-unix-2.0) -lm || exit 1
mkdir -p ~/.local/share/glib-2.0/schemas
cp /tmp/b/data/*.gschema.xml ~/.local/share/glib-2.0/schemas/
glib-compile-schemas ~/.local/share/glib-2.0/schemas
# The Shell needs a system bus; a private one keeps the host's out of reach
export DBUS_SYSTEM_BUS_ADDRESS=unix:path="$XDG_RUNTIME_DIR/system-bus"
dbus-daemon --session --address="$DBUS_SYSTEM_BUS_ADDRESS" --fork --nopidfile
exec dbus-run-session -- bash /test/inner.sh
