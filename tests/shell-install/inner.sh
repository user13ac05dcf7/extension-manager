# SPDX-License-Identifier: GPL-3.0-or-later
# Runs inside the session bus from session.sh.
set -uo pipefail
KEY=$(cat /site/key.pub)
UUID=proto@exm.test
failures=0
check() { if eval "$2"; then echo "RESULT ok: $1"; else echo "RESULT FAIL: $1"; failures=$((failures + 1)); fi; }

(cd /site && exec python3 -m http.server 8000 --bind 127.0.0.1 >/tmp/http.log 2>&1) &

start_shell() {
    gnome-shell --headless --wayland --no-x11 --virtual-monitor 1024x768 >>"/tmp/shell-$1.log" 2>&1 &
    SHELL_PID=$!
    for _ in $(seq 60); do
        gdbus call --session -d org.gnome.Shell -o /org/gnome/Shell \
            -m org.freedesktop.DBus.Properties.Get org.gnome.Shell ShellVersion >/dev/null 2>&1 && return 0
        sleep 1
    done
    echo "Shell did not start"; tail -30 "/tmp/shell-$1.log"; exit 1
}
stop_shell() { kill "$SHELL_PID"; wait "$SHELL_PID" 2>/dev/null; }
install() { EXM_EXTENSION_REPOSITORY=http://localhost:8000/$1/index.json EXM_EXTENSION_REPOSITORY_KEY=$2 /tmp/driver $UUID; }
installed() { [ -e ~/.local/share/gnome-shell/extensions/$UUID ]; }

start_shell 1
echo "== index changed after signing"
check "refused" "! install badsig $KEY && ! installed"
echo "== package changed after listing"
check "refused" "! install badsha $KEY && ! installed"
echo "== index signed by another key"
check "refused" "! install good 37rwPeZLAvu/3PO70cbnr6trHwutr7SetPnOkCNm5/g= && ! installed"
echo "== good"
check "installed" "install good $KEY && installed"
echo "enabled-extensions: $(gsettings get org.gnome.shell enabled-extensions)"
gnome-extensions info $UUID 2>&1 | grep -i "exist" || true
check "Shell does not load it before a new login" "! gnome-extensions info $UUID >/dev/null 2>&1"
stop_shell

echo "== new login"
start_shell 2
sleep 3
gnome-extensions info $UUID 2>&1 | grep -E 'State:'
check "active after a new login" "gnome-extensions info $UUID 2>/dev/null | grep -q 'State: ACTIVE'"
grep -h 'PROTO-EXT' /tmp/shell-2.log
stop_shell

if [ "$failures" -eq 0 ]; then echo "RESULT: pass"; else echo "RESULT: $failures failed"; fi
