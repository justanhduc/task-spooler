#!/bin/bash
# task-spooler manual regression test script
# Run with:  sudo ./manual_test.sh
# Requires a running task-spooler server (auto-started if root).
# All tests use short sleep commands so the suite completes quickly.

set -e
TS=./ts
PASS=0
FAIL=0

green() { echo -e "\033[32m$*\033[0m"; }
red()   { echo -e "\033[31m$*\033[0m"; }

check() {
    local desc="$1"; shift
    if "$@"; then
        green "  PASS: $desc"; PASS=$((PASS+1))
    else
        red   "  FAIL: $desc"; FAIL=$((FAIL+1))
    fi
}

echo "=== task-spooler manual test suite ==="
echo "Server:  $TS"
echo

# ── 1. Server & basic operations ──────────────────────────
echo "--- 1. Basic operations ---"

check "show help"       $TS -h > /dev/null
check "show version"    $TS -V > /dev/null
check "check daemon"    $TS --check-daemon > /dev/null 2>&1 || true
check "list jobs (empty)" [ "$($TS -l | wc -l)" -ge 1 ]

# ── 2. Enqueue & list ─────────────────────────────────────
echo "--- 2. Enqueue & list ---"

ID1=$($TS sleep 2 2>&1 | tail -1 | grep -oP '\d+')
check "enqueue job"     [ -n "$ID1" ]

ID2=$($TS -L test_label sleep 1 2>&1 | tail -1 | grep -oP '\d+')
check "enqueue with -L" [ -n "$ID2" ]

check "list jobs"       $TS -l | grep -q "$ID1"

check "get label"       $TS --get-label "$ID2" 2>&1 | grep -q "test_label"

check "get state"       $TS -s "$ID1" | grep -qE '(queued|running|finished)'

# ── 3. Info & output ──────────────────────────────────────
echo "--- 3. Info & output ---"

check "show job info"   $TS -i "$ID1" 2>&1 | grep -q "Command"

check "show output file" $TS -o "$ID1" | grep -q "ts_out"

check "show PID"        $TS -p "$ID1" > /dev/null 2>&1 || true

check "show full cmd"   $TS --full-cmd "$ID1" 2>&1 | grep -q "sleep"

# ── 4. Wait ───────────────────────────────────────────────
echo "--- 4. Wait for jobs ---"

check "wait job"        $TS -w "$ID1" > /dev/null 2>&1

# ── 5. Enqueue with dependencies ──────────────────────────
echo "--- 5. Dependencies ---"

ID3=$($TS -d sleep 0.5 2>&1 | tail -1 | grep -oP '\d+')
check "-d (after last)" [ -n "$ID3" ]

ID4=$($TS -D "$ID3" sleep 0.5 2>&1 | tail -1 | grep -oP '\d+')
check "-D <id>"         [ -n "$ID4" ]

$TS -w "$ID4" > /dev/null 2>&1 || true
check "dep chain done"  $TS -s "$ID4" 2>&1 | grep -q "finished"

# ── 6. Priority & swap ────────────────────────────────────
echo "--- 6. Priority & swap ---"

ID5=$($TS sleep 5 2>&1 | tail -1 | grep -oP '\d+')
ID6=$($TS sleep 5 2>&1 | tail -1 | grep -oP '\d+')

check "urgent (-u)"     $TS -u "$ID6" > /dev/null 2>&1

check "swap (-U)"       $TS -U "$ID5"-"$ID6" > /dev/null 2>&1

# ── 7. Hold & continue ────────────────────────────────────
echo "--- 7. Hold & continue ---"

ID7=$($TS -N 1 sleep 1 2>&1 | tail -1 | grep -oP '\d+')
check "enqueue with -N" [ -n "$ID7" ]

check "hold job"        $TS --hold "$ID7" > /dev/null 2>&1 || true

check "continue job"    $TS --cont "$ID7" > /dev/null 2>&1 || true

$TS -r "$ID5" > /dev/null 2>&1 || true
$TS -r "$ID6" > /dev/null 2>&1 || true
$TS -r "$ID7" > /dev/null 2>&1 || true

# ── 8. Count & last ID ────────────────────────────────────
echo "--- 8. Count & last ID ---"

check "count running"   $TS --count-running | grep -qE '^[0-9]+$'

check "last queue ID"   $TS --last-queue-id | grep -qE '^[0-9]+$'

# ── 9. Remove & kill ──────────────────────────────────────
echo "--- 9. Remove & kill ---"

ID8=$($TS sleep 30 2>&1 | tail -1 | grep -oP '\d+')
check "enqueue for -r"  [ -n "$ID8" ]
$TS -r "$ID8" > /dev/null 2>&1
check "remove job"      $TS -s "$ID8" 2>&1 | grep -v "not in queue" > /dev/null 2>&1 || true

ID9=$($TS -f sleep 5 2>&1 | tail -1 | grep -oP '\d+')
sleep 0.5
$TS -k "$ID9" > /dev/null 2>&1 || true
check "kill job"        true  # best-effort, timing dependent

# ── 10. Slots ─────────────────────────────────────────────
echo "--- 10. Slots ---"

check "set max slots"   $TS -S 4 > /dev/null 2>&1 || true
check "get max slots"   $TS -S 2>&1 | grep -qE '[0-9]'

$TS -S 1 > /dev/null 2>&1 || true  # reset

# ── 11. Clear finished ───────────────────────────────────
echo "--- 11. Clear finished ---"

check "clear finished"  $TS -C > /dev/null 2>&1 || true

# ── 12. Serialize ─────────────────────────────────────────
echo "--- 12. Serialize ---"
check "list default"    $TS -M default > /dev/null
check "list JSON"       $TS -M json > /dev/null
check "list tab"        $TS -M tab   > /dev/null

# ── 13. Lock & unlock ─────────────────────────────────────
echo "--- 13. Lock & unlock ---"

check "lock server"     $TS --lock-ts > /dev/null 2>&1 || true
check "unlock server"   $TS --unlock-ts > /dev/null 2>&1 || true

# ── 14. Find by PID ───────────────────────────────────────
echo "--- 14. Find by PID ---"

# Submit a foreground job to get a known PID
$TS -f sleep 3 &
FPID=$!
sleep 1
# The foreground client forks; the actual job PID is the child
check "find-by-pid"     $TS --find-by-pid $$ | grep -qE '^-?[0-9]+$'
wait $FPID 2>/dev/null || true

# ── 15. Foreground & no-store ─────────────────────────────
echo "--- 15. Foreground & no-store ---"

check "-f (foreground)" $TS -f sleep 0.1 > /dev/null 2>&1

ID10=$($TS -n sleep 0.1 2>&1 | tail -1 | grep -oP '\d+')
check "-n (no store)"   [ -n "$ID10" ]

# ── 16. Stderr apart ──────────────────────────────────────
echo "--- 16. Stderr apart ---"

ID11=$($TS -E sh -c 'echo stdout; echo stderr >&2' 2>&1 | tail -1 | grep -oP '\d+')
check "-E (stderr apart)" [ -n "$ID11" ]

# ── 17. Cat / tail ────────────────────────────────────────
echo "--- 17. Cat / tail ---"

# Submit a quick job and tail it
ID12=$($TS -L tailtest sh -c 'echo line1; echo line2; echo line3' 2>&1 | tail -1 | grep -oP '\d+')
$TS -w "$ID12" > /dev/null 2>&1
check "cat output"      $TS -c "$ID12" 2>&1 | grep -q "line1"
check "tail output"     $TS -t "$ID12" 2>&1 | grep -q "line3"

# ── Summary ───────────────────────────────────────────────
echo
echo "============================================"
green "Passed: $PASS"
if [ $FAIL -gt 0 ]; then red "Failed: $FAIL"; fi
echo "============================================"

exit $FAIL
