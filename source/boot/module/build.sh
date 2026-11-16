#!/bin/bash
# build.sh — build the AI-OS kernel modules against the shipped kernel's
# header tree (boot/dl/kern-dev, from Alpine's linux-virt-dev package).
#
# The header package ships musl-linked host tools; we rebuild fixdep and
# modpost with the host gcc, and skip gcc-plugins/objtool (not available
# in this tree, not needed for our modules). No root required.
set -euo pipefail
KDIR="$(cd "$(dirname "$0")/../dl/kern-dev/usr/src/linux-headers-6.6.142-0-virt" && pwd)"
MODDIR="$(cd "$(dirname "$0")" && pwd)"

if [ ! -f "$KDIR/Makefile" ]; then
    echo "kernel headers missing — run boot/fetch.sh first" >&2
    exit 1
fi

# host tools: rebuild if missing or musl-linked
if [ ! -x "$KDIR/scripts/basic/fixdep" ] || ldd "$KDIR/scripts/basic/fixdep" 2>/dev/null | grep -q musl; then
    gcc -O2 -o "$KDIR/scripts/basic/fixdep" "$KDIR/scripts/basic/fixdep.c"
fi
if [ ! -x "$KDIR/scripts/mod/modpost" ] || ldd "$KDIR/scripts/mod/modpost" 2>/dev/null | grep -q musl; then
    gcc -O2 -o "$KDIR/scripts/mod/modpost" \
        "$KDIR/scripts/mod/modpost.c" "$KDIR/scripts/mod/file2alias.c" \
        "$KDIR/scripts/mod/sumversion.c" "$KDIR/scripts/mod/symsearch.c"
fi

# never let kbuild try to re-run kconfig (needs bison/flex we don't ship)
touch "$KDIR/include/config/auto.conf"

make -C "$KDIR" M="$MODDIR" \
     CONFIG_GCC_PLUGINS= CONFIG_STACK_VALIDATION= CONFIG_OBJTOOL= \
     modules "$@"
