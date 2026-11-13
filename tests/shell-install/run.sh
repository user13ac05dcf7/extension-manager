#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Installs one signed package from a catalogue on a stock GNOME Shell 51 (Fedora 45,
# headless, in rootless Podman), then restarts the Shell as a new login would.
# Usage: tests/shell-install/run.sh [log file]
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
src=$(cd -- "$here/../.." && pwd)
work=$(mktemp -d)
log=${1:-$work/run.log}
image=localhost/exm-shell-install:51
python3 "$here/make-site.py" "$work/site"
podman build -q --tag "$image" "$here" >/dev/null
podman run --rm --userns=keep-id:uid=1000,gid=1000 --user 1000:1000 \
    --cap-drop=all --security-opt=no-new-privileges --memory=4g --cpus=4 --timeout=900 \
    --volume "$src:/src:ro,z" --volume "$here:/test:ro,z" --volume "$work/site:/site:ro,z" \
    "$image" bash /test/session.sh >"$log" 2>&1 || true
grep -E '^(==|DRIVER:|RESULT)|State:|PROTO-EXT|doesn.t exist|enabled-extensions' "$log"
grep -q '^RESULT: pass' "$log"
