#!/bin/sh
# remarkx — one-click RM2 display-stack uninstall (run on the PC).
#
# Removes exactly what install_rm2fb.sh placed (verified by hash via the
# manifest on the tablet), restores xochitl, cleans /tmp staging.
#
# usage: ./uninstall_rm2fb.sh [target]
#   target: ssh target, default root@reMarkable (or $RMX_HOST)

set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
TARGET=""
for a in "$@"; do
    case "$a" in
        --no-test|--stage-only) ;;
        *) TARGET="$a" ;;
    esac
done
[ -n "$TARGET" ] || TARGET="${RMX_HOST:-root@reMarkable}"

SSH="ssh -o BatchMode=yes"
die() {
    echo "ERROR: $*" >&2
    exit 1
}

echo "== ssh check: $TARGET"
$SSH "$TARGET" 'uname -m' >/dev/null 2>&1 \
    || die "cannot ssh to $TARGET without a password"

scp -o BatchMode=yes -q "$HERE/rm2fb_uninstall_dev.sh" "$TARGET:/tmp/rm2fb_uninstall_dev.sh" \
    || die "cannot copy on-device uninstaller"

rc=0
$SSH "$TARGET" 'sh /tmp/rm2fb_uninstall_dev.sh' || rc=$?

$SSH "$TARGET" 'rm -f /tmp/rm2fb_uninstall_dev.sh' 2>/dev/null
exit "$rc"
