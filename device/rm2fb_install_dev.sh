#!/bin/sh
# rm2fb_install_dev.sh — runs ON the reMarkable 2 (as root, via ssh).
#
# Places the timower/rM2-stuff "rm2display" files (from a verified staging
# dir), writes a manifest, and runs a smoke test that takes the display
# over from xochitl and always gives it back.
#
# Safety properties:
#   - only runs on a device whose /proc/device-tree/model says "reMarkable 2"
#   - every staged file is hash-verified before use
#   - NEVER overwrites a file that differs from the expected content
#     (aborts instead); identical files are skipped (idempotent re-runs)
#   - everything it writes is recorded in /opt/rm2fb/manifest
#   - if interrupted mid smoke-test, xochitl is restored on exit
#
# usage: sh rm2fb_install_dev.sh [STAGE_DIR] [NO_TEST]
#   STAGE_DIR  default /tmp/rm2fb-stage (created by the PC-side installer)
#   NO_TEST    1 = skip the smoke test (files only)
#
# Test hook: RM2FB_ROOT prefixes all device paths (sandbox testing only).

set -u

STAGE="${1:-/tmp/rm2fb-stage}"
NO_TEST="${2:-0}"
R="${RM2FB_ROOT:-}"
PKG="timower/rM2-stuff rm2display v0.1.4"

SMOKE=0
on_exit() {
    [ "$SMOKE" = 1 ] || return 0
    SMOKE=0
    echo "(interrupted during display test; restoring system UI...)" >&2
    systemctl stop rm2fb.service rm2fb.socket 2>/dev/null
    systemctl start xochitl 2>/dev/null
}
trap on_exit EXIT
trap 'on_exit; exit 130' INT
trap 'on_exit; exit 143' TERM
trap 'on_exit; exit 129' HUP

die() {
    echo "ERROR: $*" >&2
    exit 1
}

file_hash() {
    sha256sum "$1" 2>/dev/null | cut -d' ' -f1
}

if [ -z "$R" ]; then
    [ "$(id -u)" = "0" ] || die "must run as root"
    [ "$(uname -m)" = "armv7l" ] || die "expected armv7l, got $(uname -m)"
fi
command -v sha256sum >/dev/null 2>&1 || die "sha256sum not found; integrity checks require it"

MODEL_FILE="$R/proc/device-tree/model"
if [ -f "$MODEL_FILE" ]; then
    MODEL="$(cat "$MODEL_FILE" 2>/dev/null)"
    case "$MODEL" in
        *"reMarkable 2"*) ;;
        *) die "device model is '$MODEL'; this installer is for reMarkable 2 only" ;;
    esac
else
    MODEL="unknown"
fi

OSVER=""
if [ -f "$R/usr/share/remarkable/update.conf" ]; then
    OSVER="$(grep -h '^REMARKABLE_RELEASE_VERSION=' "$R/usr/share/remarkable/update.conf" 2>/dev/null | cut -d= -f2 | tr -d '"')"
fi
[ -n "$OSVER" ] || OSVER="$(head -n 1 "$R/etc/version" 2>/dev/null || true)"

echo "== rm2fb installer =="
echo "model:  $MODEL"
echo "os:     ${OSVER:-unknown}"
echo "stage:  $STAGE"
echo

[ -d "$STAGE" ] || die "staging dir not found: $STAGE (PC-side installer creates it)"
[ -f "$STAGE/rm2fb_files.list" ] || die "missing $STAGE/rm2fb_files.list"

# ---- 1) verify staged tree against the pinned table ----
echo "-- verifying staged files"
CONFLICTS=""
while read -r kind final stage check; do
    [ -n "${stage:-}" ] || continue
    if [ "$kind" = "F" ]; then
        [ -f "$STAGE/$stage" ] || die "staging missing file: $stage"
        [ "$(file_hash "$STAGE/$stage")" = "$check" ] || die "staging hash mismatch: $stage"
    else
        [ -L "$STAGE/$stage" ] || die "staging missing symlink: $stage"
        [ "$(readlink "$STAGE/$stage")" = "$check" ] || die "staging symlink mismatch: $stage"
    fi
done < "$STAGE/rm2fb_files.list"
echo "ok"

# ---- 2) conflict scan: refuse to clobber anything we don't recognize ----
CONFLICTS=""
while read -r kind final stage check; do
    [ -n "${final:-}" ] || continue
    t="$R/$final"
    case "$kind" in
        F)
            if [ -L "$t" ]; then
                CONFLICTS="$CONFLICTS
  $final: is a symlink, expected regular file"
            elif [ -e "$t" ] && [ "$(file_hash "$t")" != "$check" ]; then
                CONFLICTS="$CONFLICTS
  $final: exists with different content"
            fi
            ;;
        L)
            if [ -e "$t" ] && [ ! -L "$t" ]; then
                CONFLICTS="$CONFLICTS
  $final: is a regular file, expected symlink"
            elif [ -L "$t" ] && [ "$(readlink "$t")" != "$check" ]; then
                CONFLICTS="$CONFLICTS
  $final: symlink points elsewhere"
            fi
            ;;
    esac
done < "$STAGE/rm2fb_files.list"
[ -z "$CONFLICTS" ] || die "refusing to overwrite existing files:$CONFLICTS
Run ./uninstall_rm2fb.sh first, then install again."

# ---- 3) place files ----
echo "-- installing"
while read -r kind final stage check; do
    [ -n "${final:-}" ] || continue
    t="$R/$final"
    case "$kind" in
        F)
            if [ -f "$t" ] && [ "$(file_hash "$t")" = "$check" ]; then
                echo "  exists   $final"
            else
                mkdir -p "$(dirname "$t")" || die "mkdir failed: $final"
                cp -f "$STAGE/$stage" "$t" || die "copy failed: $final"
                [ "$(file_hash "$t")" = "$check" ] || die "post-copy verify failed: $final"
                echo "  installed $final"
            fi
            ;;
        L)
            if [ -L "$t" ] && [ "$(readlink "$t")" = "$check" ]; then
                echo "  exists   $final"
            else
                mkdir -p "$(dirname "$t")" || die "mkdir failed: $final"
                ln -s "$check" "$t" || die "symlink failed: $final"
                echo "  installed $final"
            fi
            ;;
    esac
done < "$STAGE/rm2fb_files.list"
chmod +x "$R/opt/bin/rm2fb_server" 2>/dev/null

# launcher for the remarkx reader (only if xreader is present)
LAUNCHER_LINE=""
if [ -f "$R/opt/rmx/xreader.sh" ]; then
    cat > "$R/opt/rmx/xreader-rm2.sh" <<'LAUNCHER'
#!/bin/sh
# xreader-rm2 — run the remarkx reader under the rm2fb display stack (RM2).
# Takes the e-ink panel over from xochitl; always restores xochitl on exit.
# Generated by rm2fb_install_dev.sh; local edits are lost on reinstall.

restore() {
    systemctl stop rm2fb.service rm2fb.socket 2>/dev/null
    systemctl reset-failed xochitl 2>/dev/null
    systemctl start xochitl 2>/dev/null
}
trap restore EXIT
trap 'restore; exit 130' INT
trap 'restore; exit 143' TERM
trap 'restore; exit 129' HUP

[ -f /opt/rmx/xreader.sh ] || { echo "xreader.sh missing (run install_device.sh first)"; exit 1; }
[ -e /opt/lib/librm2fb_client.so.1 ] || { echo "rm2fb client lib missing (run install_rm2fb.sh first)"; exit 1; }

systemctl stop xochitl 2>/dev/null
i=0
while [ $i -lt 50 ]; do
    case "$(systemctl is-active xochitl 2>/dev/null)" in
        inactive|failed|unknown) break ;;
    esac
    i=$((i + 1))
    sleep 0.2
done
sleep 1
systemctl reset-failed rm2fb.service 2>/dev/null
systemctl start rm2fb.socket rm2fb.service 2>/dev/null
sleep 1
if [ "$(systemctl is-active rm2fb 2>/dev/null)" != "active" ]; then
    echo "rm2fb failed to start (diagnose: journalctl -u rm2fb -n 30)"
    restore
    exit 1
fi
LD_PRELOAD=/opt/lib/librm2fb_client.so.1 sh /opt/rmx/xreader.sh
LAUNCHER
    chmod +x "$R/opt/rmx/xreader-rm2.sh" || die "chmod launcher failed"
    LAUNCHER_LINE="F opt/rmx/xreader-rm2.sh $(file_hash "$R/opt/rmx/xreader-rm2.sh")"
    echo "  installed opt/rmx/xreader-rm2.sh (reader launcher)"
else
    echo "  (no /opt/rmx/xreader.sh found; launcher skipped — install the reader first)"
fi

# ---- 4) manifest ----
mkdir -p "$R/opt/rm2fb" || die "mkdir /opt/rm2fb failed"
{
    echo "# rm2fb install manifest (root-relative paths; do not edit)"
    echo "# package: $PKG"
    while read -r kind final stage check; do
        [ -n "${final:-}" ] || continue
        echo "$kind $final $check"
    done < "$STAGE/rm2fb_files.list"
    if [ -n "$LAUNCHER_LINE" ]; then
        echo "$LAUNCHER_LINE"
    fi
    echo "D opt/rm2fb"
} > "$R/opt/rm2fb/manifest.tmp" || die "write manifest failed"
mv "$R/opt/rm2fb/manifest.tmp" "$R/opt/rm2fb/manifest" || die "move manifest failed"
cp "$STAGE/rm2fb_files.list" "$R/opt/rm2fb/files.list" || die "save file list failed"
rm -rf "$STAGE"

systemctl daemon-reload 2>/dev/null

# ---- 5) smoke test ----
if [ "$NO_TEST" = "1" ]; then
    echo "-- smoke test skipped (NO_TEST=1)"
else
    if [ "$(systemctl is-active rm2fb 2>/dev/null)" = "active" ]; then
        die "rm2fb service is already running (a reader may be in progress); stop it first"
    fi
    echo "-- smoke test (display will go blank for a few seconds)"
    SMOKE=1
    rc_smoke=0
    systemctl stop xochitl 2>/dev/null
    i=0
    while [ $i -lt 50 ]; do
        case "$(systemctl is-active xochitl 2>/dev/null)" in
            inactive|failed|unknown) break ;;
        esac
        i=$((i + 1))
        sleep 0.2
    done
    sleep 1
    systemctl reset-failed rm2fb.service 2>/dev/null
    systemctl start rm2fb.socket rm2fb.service 2>/dev/null
    sleep 1
    if [ "$(systemctl is-active rm2fb 2>/dev/null)" != "active" ]; then
        echo "!! rm2fb service did not start. Recent journal:"
        journalctl -u rm2fb -n 30 --no-pager 2>/dev/null
        echo "!! Restoring system UI..."
        systemctl stop rm2fb.service rm2fb.socket 2>/dev/null
        systemctl start xochitl 2>/dev/null
        rc_smoke=1
    else
        echo "   rm2fb service: active"
        if [ -x "$R/opt/bin/simple" ]; then
            echo "   rendering test frame for 5s..."
            LD_PRELOAD="$R/opt/lib/librm2fb_client.so.1" sh -c \
                'printf "@timeout 5\nlabel 100 800 1200 80 rm2fb OK - reMarkable 2\n" | /opt/bin/simple' \
                >/dev/null 2>&1
        else
            echo "   (no /opt/bin/simple; frame render skipped)"
        fi
        systemctl stop rm2fb.service rm2fb.socket 2>/dev/null
        systemctl start xochitl 2>/dev/null
    fi
    SMOKE=0
fi

echo
echo "== installed files (manifest) =="
grep -E '^[FLD] ' "$R/opt/rm2fb/manifest" 2>/dev/null

if [ "${rc_smoke:-0}" != "0" ]; then
    echo
    echo "RESULT: files installed, but SMOKE TEST FAILED."
    echo "Your OS version may not be supported by this rm2fb build."
    echo "Uninstall with: ./uninstall_rm2fb.sh   (PC side)"
    exit 2
fi
echo
echo "RESULT: install OK."
echo "Read X (relay must be running):  ssh <device> 'sh /opt/rmx/xreader-rm2.sh'"
echo "Uninstall anytime:               ./uninstall_rm2fb.sh   (PC side)"
