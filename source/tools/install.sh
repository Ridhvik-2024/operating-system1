#!/bin/bash
# install.sh — AI-OS v1.0 installer (host runtime).
#
#   ./tools/install.sh                install to /usr/local (asks sudo only if needed)
#   PREFIX=$HOME/.local ./tools/install.sh    user-local install, no root
#   DESTDIR=/tmp/stage PREFIX=/usr ./tools/install.sh   staged packaging
#
# Steps: toolchain check -> build -> 10-suite test -> install -> verify.
set -euo pipefail
cd "$(dirname "$0")/.."   # repo root

PREFIX=${PREFIX:-/usr/local}
DESTDIR=${DESTDIR:-}

echo "== AI-OS v1.0 installer =="
echo "   target: ${DESTDIR}${PREFIX}"

command -v cc >/dev/null || { echo "FATAL: no C compiler (cc)"; exit 1; }
command -v make >/dev/null || { echo "FATAL: no make"; exit 1; }
command -v python3 >/dev/null || echo "note: python3 missing — only needed for microlm/mkfs tools"

echo "== building =="
make -j"$(nproc 2>/dev/null || echo 2)"

echo "== testing (10 suites) =="
make test

echo "== installing =="
INSTALL_CMD=(make install "PREFIX=$PREFIX")
[ -n "$DESTDIR" ] && INSTALL_CMD+=("DESTDIR=$DESTDIR")
if [ -z "$DESTDIR" ] && [ "$PREFIX" = "/usr/local" ] && [ "$(id -u)" != 0 ]; then
    echo "(no write access without root — using sudo; use PREFIX=\$HOME/.local to avoid it)"
    sudo "${INSTALL_CMD[@]}"
else
    "${INSTALL_CMD[@]}"
fi

echo "== verifying =="
BIN="${DESTDIR}${PREFIX}/bin/aiosh"
"$BIN" -c version && echo "install verified."

cat <<'EOF'

Upgrade note: binaries are stateless — your OS-owned memory lives in
the store directory (./.aios-store by default, /data/aios in the
booted OS) and survives binary replacement. See UPGRADE.md.
EOF
