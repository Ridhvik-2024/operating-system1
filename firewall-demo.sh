#!/bin/bash
# firewall-demo.sh — the M10 agent firewall, shown refusing real attack chains.
#
# What this demonstrates, and what it deliberately does not:
#
#   DEMONSTRATED  every refusal is an exact integer vector check. Run it twice and
#                 you get the same sentences. There is no model, no weights, no
#                 training distribution to fall outside of.
#
#   NOT HERE      a "neural conservation critic". A learned critic is a heuristic:
#                 it cannot be shown to hold, and it fails silently on inputs it
#                 was not trained on. Calling a heuristic a conservation law is
#                 the substitution this project exists to refuse. If one is ever
#                 added it must PROPOSE a species mapping, and the exact check
#                 here must still decide.
#
# Usage:  ./firewall-demo.sh            run all scenarios
#         ./firewall-demo.sh --model    print the model only (what a human signs off on)
set -u
cd "$(dirname "$0")" || exit 1

SRC=source
BIN=build/firewall-demo
LIB=$SRC/libaios.a

die() { echo "  ERROR $*" >&2; exit 1; }

# ---- build -----------------------------------------------------------
if [ ! -f "$LIB" ]; then
    echo "[demo] building the library first"
    make -C "$SRC" -j4 libaios.a >/dev/null || die "make libaios.a failed"
fi
mkdir -p build
cc -std=c11 -O1 -D_GNU_SOURCE -I"$SRC/include" \
   "$SRC/apps/firewall-demo.c" "$LIB" -pthread -lm -o "$BIN" \
   || die "could not build the demo driver"

if [ "${1:-}" = "--model" ]; then
    "$BIN" --model
    exit $?
fi

"$BIN"
rc=$?

echo
if [ $rc -eq 0 ]; then
    echo "[demo] PASS - every refused chain was refused for the reason shown,"
    echo "       and every allowed chain was allowed for the reason shown."
else
    echo "[demo] FAIL - a verdict differed from what the model says it should be"
fi
exit $rc
