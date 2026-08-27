#!/bin/sh
# remarkx — one-click RM2 display-stack install (run on the PC, not the tablet).
#
# What it does:
#   1. verify passwordless ssh to the tablet
#   2. verify device/rm2display.ipk (bundled; downloaded once if missing)
#   3. extract the 9 files, hash-verify each
#   4. push staging + on-device installer to /tmp on the tablet
#   5. on-device installer places files (never clobbers unknown files),
#      writes /opt/rm2fb/manifest, runs a smoke test (steals display from
#      xochitl, always restores it) and cleans up
#
# Fully reversible: ./uninstall_rm2fb.sh [target]
#
# usage: ./install_rm2fb.sh [target] [--no-test] [--stage-only]
#   target      ssh target, default root@reMarkable (or $RMX_HOST)
#   --no-test   install files only, skip the display smoke test
#   --stage-only  verify ipk + staging locally, touch the device nothing
#                 (used for offline/sandbox validation)

set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
TARGET=""
NO_TEST=0
STAGE_ONLY=0
for a in "$@"; do
    case "$a" in
        --no-test) NO_TEST=1 ;;
        --stage-only) STAGE_ONLY=1 ;;
        *) TARGET="$a" ;;
    esac
done
[ -n "$TARGET" ] || TARGET="${RMX_HOST:-root@reMarkable}"

VERSION=v0.1.4
IPK_URL="https://github.com/timower/rM2-stuff/releases/download/${VERSION}/rm2display.ipk"
IPK_SHA256=06a274fa455ee3b3eee29d18a69968a1a9e4030b88c0f73f9cc2448452ae160d
SSH="ssh -o BatchMode=yes"

die() {
    echo "ERROR: $*" >&2
    exit 1
}

file_hash() {
    sha256sum "$1" 2>/dev/null | cut -d' ' -f1
}

command -v sha256sum >/dev/null 2>&1 || die "sha256sum not found on this PC"
command -v tar >/dev/null 2>&1 || die "tar not found on this PC"

# ---- 1) ssh ----
if [ "$STAGE_ONLY" != 1 ]; then
    echo "== 1/5 ssh check: $TARGET"
    $SSH "$TARGET" 'uname -m' >/dev/null 2>&1 \
        || die "cannot ssh to $TARGET without a password.
Fix key auth (ssh-copy-id $TARGET), or pass a working target: $0 root@<device>"
fi

# ---- 2) ipk ----
echo "== 2/5 package ipk"
IPK="$HERE/rm2display.ipk"
if [ ! -f "$IPK" ]; then
    mkdir -p "${HOME}/.cache/remarkx"
    if [ -f "${HOME}/.cache/remarkx/rm2display.ipk" ]; then
        IPK="${HOME}/.cache/remarkx/rm2display.ipk"
    else
        command -v curl >/dev/null 2>&1 || die "ipk not bundled and curl missing; put rm2display.ipk in $HERE/"
        echo "   downloading $IPK_URL"
        curl -fsSL --retry 2 -o "${HOME}/.cache/remarkx/rm2display.ipk" "$IPK_URL" \
            || die "download failed; put the ipk at $HERE/rm2display.ipk and retry"
        IPK="${HOME}/.cache/remarkx/rm2display.ipk"
    fi
fi
[ -f "$IPK" ] || die "missing $HERE/rm2display.ipk"
[ "$(file_hash "$IPK")" = "$IPK_SHA256" ] || die "ipk sha256 mismatch — wrong or corrupted file, aborting"
echo "   ok: $(basename "$IPK")"

# ---- 3) stage + verify ----
echo "== 3/5 staging"
T="$(mktemp -d "${TMPDIR:-/tmp}/rm2fb.XXXXXX")" || die "mktemp failed"
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/root"
tar xOf "$IPK" ./data.tar.gz 2>/dev/null > "$T/data.tar.gz" || die "cannot extract data.tar.gz from ipk"
tar xzf "$T/data.tar.gz" -C "$T/root" || die "cannot unpack data.tar.gz"

cat > "$T/root/rm2fb_files.list" <<'EOF'
F opt/bin/rm2fb_server opt/bin/rm2fb_server 459c3e8a4d47cf304a8a591b9858b4847c67d7d88ab25e400adaa5e6b27fec79
F opt/lib/librm2fb_client.so.1.1.0 opt/lib/librm2fb_client.so.1.1.0 6d0c42f4556186741015bbc64fdc6cde71cd146af347449f1f1cb94cda4fdfc5
L opt/lib/librm2fb_client.so.1 opt/lib/librm2fb_client.so.1 librm2fb_client.so.1.1.0
L opt/lib/librm2fb_client.so opt/lib/librm2fb_client.so librm2fb_client.so.1
F opt/lib/librm2fb_client_no_hook.so opt/lib/librm2fb_client_no_hook.so 04a049d59e0824aa708ba9456ac5c35132dcca9fc60d12cd34b7b76e0059f33e
F opt/lib/librm2fb_server.so opt/lib/librm2fb_server.so f674e9c27219aef7df7dcb47b7d30348d027a2013b895441b4a8abe18050bffd
L usr/lib/librm2fb_client.so.1 usr/lib/librm2fb_client.so.1 /opt/lib/librm2fb_client.so.1
F etc/systemd/system/rm2fb.service lib/systemd/system/rm2fb.service 14f3e7b2f88f38665867103e381d2d6b5da6eb02a3d2141b1ff30e77234eabc4
F etc/systemd/system/rm2fb.socket lib/systemd/system/rm2fb.socket 6fa25ca35c81901e38b15dc4454308b0b6f46d5df53473fbbd68edfc912729a1
EOF

while read -r kind final stage check; do
    [ -n "${stage:-}" ] || continue
    if [ "$kind" = "F" ]; then
        [ -f "$T/root/$stage" ] || die "staging missing: $stage"
        [ "$(file_hash "$T/root/$stage")" = "$check" ] || die "staging hash mismatch: $stage"
    else
        [ -L "$T/root/$stage" ] || die "staging missing symlink: $stage"
        [ "$(readlink "$T/root/$stage")" = "$check" ] || die "staging symlink mismatch: $stage"
    fi
done < "$T/root/rm2fb_files.list"
echo "   9/9 files verified"

if [ "$STAGE_ONLY" = 1 ]; then
    echo "== stage-only: staging OK (no device touched). staging kept at $T"
    exit 0
fi

# ---- 4) push ----
echo "== 4/5 push to device"
$SSH "$TARGET" 'rm -rf /tmp/rm2fb-stage && mkdir -p /tmp/rm2fb-stage' \
    || die "cannot prepare /tmp/rm2fb-stage on device"
tar cf - -C "$T/root" . | $SSH "$TARGET" 'tar xf - -C /tmp/rm2fb-stage' \
    || die "push staging failed"
scp -o BatchMode=yes -q "$HERE/rm2fb_install_dev.sh" "$TARGET:/tmp/rm2fb_install_dev.sh" \
    || die "cannot copy on-device installer"

# ---- 5) install on device ----
echo "== 5/5 install on device"
rc=0
$SSH "$TARGET" "sh /tmp/rm2fb_install_dev.sh /tmp/rm2fb-stage $NO_TEST" || rc=$?

echo
if [ $rc -eq 0 ]; then
    echo "Done. To read X (relay must be running on this PC):"
    echo "    ssh $TARGET 'sh /opt/rmx/xreader-rm2.sh'"
    echo
    echo "Uninstall anytime:"
    echo "    $HERE/uninstall_rm2fb.sh $TARGET"
elif [ $rc -eq 2 ]; then
    echo "WARNING: files are installed, but the smoke test failed (see output above)."
    echo "Likely cause: this rm2fb build does not support the tablet's OS version."
    echo "Uninstall with: $HERE/uninstall_rm2fb.sh $TARGET"
else
    echo "Install failed (exit $rc). Staging kept at /tmp/rm2fb-stage on device for inspection."
    exit "$rc"
fi
exit 0
