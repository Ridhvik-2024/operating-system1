#!/bin/bash
# nullspace-demo.sh — item #17, tested rather than described.
#
# The proposal claims a secret Stiefel rotation plus null-space projection gives
# "zero mathematical possibility of data leakage" — I(Z_cloud; PII) = 0. This
# script runs the claim against arithmetic instead of accepting it. It has three
# parts, in increasing order of how uncomfortable they are:
#
#   PART 1  THE TRUE PART. A rotation is an isometry: every inner product is
#           preserved, so attention angles survive and the cloud can reason.
#           Measured, not asserted.
#
#   PART 2  THE FALSE PART. A rotation is also a bijection, and a linear one with
#           only n(n-1)/2 parameters. Given n embeddings whose content the
#           attacker can guess — a form header, a footer, a standard clause —
#           the rotation is recovered exactly, and then a held-out document that
#           was never in the known set is read directly.
#
#   PART 3  THE USEFUL PART. What actually can be proven: an auditor that
#           measures how much of the protected attribute a linear probe recovers
#           and how much a quadratic probe recovers, and refuses to certify a
#           pipeline whose numbers are not both at chance. A projection that
#           cleans only the linear component passes every test its vendor writes
#           and fails this one.
#
# Usage:  ./nullspace-demo.sh
set -u
cd "$(dirname "$0")" || exit 1

SRC=source
BIN=build/nullspace-demo
LIB=$SRC/libaios.a

die() { echo "  ERROR $*" >&2; exit 1; }

if [ ! -f "$LIB" ]; then
    echo "[demo] building the library first"
    make -C "$SRC" -j4 libaios.a >/dev/null || die "make libaios.a failed"
fi
mkdir -p build
cc -std=c11 -O2 -D_GNU_SOURCE -I"$SRC/include" \
   "$SRC/apps/nullspace-demo.c" "$LIB" -pthread -lm -o "$BIN" \
   || die "could not build the demo driver"

"$BIN"
rc=$?

echo
if [ $rc -eq 0 ]; then
    echo "[demo] PASS - the isometry is real, the rotation leaks, and the auditor"
    echo "       refuses the pipeline that only looks private."
else
    echo "[demo] FAIL - a measurement differed from what the arithmetic says it should be"
fi
exit $rc
