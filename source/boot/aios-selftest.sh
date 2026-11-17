#!/bin/sh
# aios-selftest — run the full AI-OS unit/integration suite (M1-M10) inside
# the booted AI-OS image. Called by aios-init at boot; also runnable from
# the aiosh console via:  ! /usr/bin/aios-selftest   (or fs.run wrapper)
export AIOS_TOOL_EXEC=/sbin/tool-exec
export PATH=/sbin:/bin:/usr/sbin:/usr/bin
cd /var/aios-selftest || exit 1

pass=0
fail=0
for t in /usr/lib/aios/tests/test_*; do
    name=$(basename "$t")
    out=$("$t" 2>&1)
    rc=$?
    if [ $rc -eq 0 ]; then
        echo "  SELFTEST $name: PASS ($out)"
        pass=$((pass + 1))
    else
        echo "  SELFTEST $name: FAIL (rc=$rc)"
        # The first failures, not the first lines: the suite prints its progress
        # and its failures interleaved, so `head -6` can be six successes.
        echo "$out" | grep -E '^FAIL' | head -6 | sed 's/^/      /'
        [ -z "$(echo "$out" | grep -E '^FAIL')" ] && \
            echo "$out" | tail -6 | sed 's/^/      /'
        # One line for the CI annotation, which is length-limited.
        f=$(echo "$out" | grep -E '^FAIL' | head -4 | tr '\n' ' ' | tr -s ' ')
        [ -n "$GITHUB_ACTIONS" ] && [ -n "$f" ] && \
            echo "::error title=selftest $name::$f"
        fail=$((fail + 1))
    fi
done
echo "  SELFTEST total: $pass passed, $fail failed"
if [ $fail -eq 0 ]; then
    echo "AIOS-SELFTEST: PASS"
    exit 0
else
    echo "AIOS-SELFTEST: FAIL"
    exit 1
fi
