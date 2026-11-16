#!/bin/bash
# perf-run.sh — AI-OS v1.0 performance pass (host measurements).
#
# Reproduces every number in docs/PERFORMANCE.md except the boot
# timeline (that one is measured by the boot harness; see the doc).
#
#   ./tools/perf-run.sh            full run, writes /tmp/aios-perf.txt
#
# What it measures:
#   1. clean-build wall time (make clean && make)
#   2. per-suite test wall time (10 suites)
#   3. shell cold-start + line latency (version / calc / NL route),
#      7 runs each, min + median
#   4. micro-LM classify + generate throughput
#   5. memory recall latency on a 100-entry store
set -euo pipefail
cd "$(dirname "$0")/.."   # repo root
OUT=/tmp/aios-perf.txt
: > "$OUT"

say() { echo "$@" | tee -a "$OUT"; }

median() {  # median of stdin numbers
    tr ' ' '\n' | sort -n | awk '{a[NR]=$1} END { if (NR%2) print a[(NR+1)/2]; else printf "%.1f\n", (a[NR/2]+a[NR/2+1])/2 }'
}

say "== AI-OS v1.0 performance run: $(date -u '+%Y-%m-%d %H:%M:%S UTC') =="
say "host: $(uname -sr), $(nproc) logical cores"
say ""

# ---- 1. clean build -------------------------------------------------
say "== clean build =="
T0=$(date +%s.%N)
make clean >/dev/null 2>&1
make -j"$(nproc)" >/dev/null 2>&1
T1=$(date +%s.%N)
BUILD=$(echo "$T1 $T0" | awk '{printf "%.1f", $1-$2}')
say "clean build (make -j$(nproc)): ${BUILD}s"
say ""

# ---- 2. per-suite test time ----------------------------------------
say "== per-suite wall time =="
make test >/dev/null 2>&1   # build the suites (and prove green) before timing
TOTAL=0
for t in build/tests/test_*; do
    [ -x "$t" ] || continue
    T0=$(date +%s.%N)
    ./"$t" >/dev/null 2>&1
    T1=$(date +%s.%N)
    D=$(echo "$T1 $T0" | awk '{printf "%.3f", $1-$2}')
    TOTAL=$(echo "$TOTAL $D" | awk '{printf "%.3f", $1+$2}')
    printf '  %-36s %ss\n' "$(basename "$t")" "$D" | tee -a "$OUT"
done
say "  (sum of suites, single-threaded)   ${TOTAL}s"
say ""

# ---- 3. shell latency ----------------------------------------------
say "== shell latency (7 runs: min / median), cold start included =="
line_latency() {  # $1 = aiosh line
    local ts=()
    for _ in 1 2 3 4 5 6 7; do
        T0=$(date +%s.%N)
        ./aiosh --scripted --store /tmp/aios-perf-store -c "$1" >/dev/null 2>&1
        T1=$(date +%s.%N)
        ts+=("$(echo "$T1 $T0" | awk '{printf "%.1f", ($1-$2)*1000}')")
    done
    local med min
    med=$(printf '%s\n' "${ts[@]}" | median)
    min=$(printf '%s\n' "${ts[@]}" | sort -n | head -1)
    printf '  %-34s %sms / %sms\n' "$1" "$min" "$med" | tee -a "$OUT"
}
line_latency "version"
line_latency "6*7"
line_latency "list files"
line_latency "summarize the workspace"
line_latency "remember perf test v1.0 ; recall perf"
say ""

# ---- 4. micro-LM throughput ----------------------------------------
say "== micro-LM (deterministic, offline) =="
T0=$(date +%s.%N)
for _ in $(seq 1 200); do ./aiosh --micro --store /tmp/aios-perf-store -c "lm classify run the nightly report in the background" >/dev/null 2>&1; done
T1=$(date +%s.%N)
say "  200x classify lines: $(echo "$T1 $T0" | awk '{printf "%.2f", $1-$2}')s"
T0=$(date +%s.%N)
for _ in $(seq 1 200); do ./aiosh --micro --store /tmp/aios-perf-store -c "lm gen about the scheduler" >/dev/null 2>&1; done
T1=$(date +%s.%N)
say "  200x generate lines: $(echo "$T1 $T0" | awk '{printf "%.2f", $1-$2}')s"
say "  (each run = full shell process: create, classify, print, teardown)"
say ""

# ---- 5. memory recall on a populated store --------------------------
say "== memory recall latency (100-entry store) =="
rm -rf /tmp/aios-perf-mem
python3 - <<'EOF'
import os, json
store = "/tmp/aios-perf-mem"
os.makedirs(store, exist_ok=True)
lines = []
for i in range(100):
    lines.append(json.dumps({"tier":"episodic","kind":"note",
        "text":f"perf entry {i}: the scheduler lends cpu budget between classes",
        "ts_ms": 1700000000000 + i*1000, "use": 0}))
with open(os.path.join(store, "memory.journal"), "w") as f:
    f.write("\n".join(lines) + "\n")
print("store seeded: 100 episodic entries")
EOF
TS=""
for _ in 1 2 3 4 5; do
    T0=$(date +%s.%N)
    ./aios-memctl /tmp/aios-perf-mem recall "cpu budget lending" >/dev/null 2>&1
    T1=$(date +%s.%N)
    TS="$TS $(echo "$T1 $T0" | awk '{printf "%.1f", ($1-$2)*1000}')"
done
MIN=$(echo $TS | tr ' ' '\n' | sort -n | head -1)
MED=$(echo $TS | tr ' ' '\n' | median)
say "  recall latency, min / median: ${MIN}ms / ${MED}ms  (runs:$TS)"
say ""

say "== perf run complete — raw log: $OUT =="
