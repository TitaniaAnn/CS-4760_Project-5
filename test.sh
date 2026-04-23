#!/bin/bash
# CS-4760 Project 5 — Automated Test Runner
# Run on opsys.cs.umsl.edu after 'make'
# Usage: bash test.sh
# Each test prints PASS or FAIL with a short explanation.

PASS=0
FAIL=0

pass() { echo "  PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }

ipc_clean() {
    local shm_count msq_count
    shm_count=$(ipcs -m 2>/dev/null | grep -c "0x47605" || true)
    msq_count=$(ipcs -q 2>/dev/null | grep -c "0x47606" || true)
    [ "$shm_count" -eq 0 ] && [ "$msq_count" -eq 0 ]
}

cleanup_ipc() {
    for id in $(ipcs -m 2>/dev/null | grep "0x47605" | awk '{print $2}'); do
        ipcrm -m "$id" 2>/dev/null
    done
    for id in $(ipcs -q 2>/dev/null | grep "0x47606" | awk '{print $2}'); do
        ipcrm -q "$id" 2>/dev/null
    done
}

# Make sure we start clean
cleanup_ipc

echo "======================================"
echo " CS-4760 P5 Test Suite"
echo "======================================"

# ── T01  Clean build ──────────────────────────────────────────
echo ""
echo "[T01] Clean build"
if make clean > /dev/null 2>&1 && make > build_out.txt 2>&1; then
    if grep -qiE "error:|undefined" build_out.txt; then
        fail "build errors present"
    elif grep -qiE "warning:" build_out.txt; then
        fail "build succeeded but has warnings: $(grep -iE 'warning:' build_out.txt | head -1)"
    else
        pass "no errors or warnings"
    fi
else
    fail "make returned non-zero"
fi
rm -f build_out.txt

# ── T02  Help flag ────────────────────────────────────────────
echo ""
echo "[T02] Help flag (-h)"
./oss -h > t02_out.txt 2>&1
RC=$?
if [ $RC -eq 0 ] && grep -qi "\-n" t02_out.txt && ipc_clean; then
    pass "usage printed, exit 0, no IPC created"
else
    fail "exit=$RC or usage not printed or IPC leaked"
fi
rm -f t02_out.txt

# ── T03  Minimal run / clean exit ────────────────────────────
echo ""
echo "[T03] Minimal run: -n 3 -s 2 -t 3"
timeout 30 ./oss -n 3 -s 2 -t 3 -f t03.log > /dev/null 2>&1
RC=$?
if [ $RC -eq 0 ] && [ -s t03.log ] && \
   grep -q "forked worker" t03.log && \
   grep -q "Total resource requests" t03.log && \
   ipc_clean; then
    pass "ran, produced log, statistics present, IPC clean"
else
    fail "RC=$RC / log exists=$([ -s t03.log ] && echo yes || echo no) / ipc_clean=$(ipc_clean && echo yes || echo no)"
    cleanup_ipc
fi
rm -f t03.log

# ── T04  IPC cleanup after normal exit ───────────────────────
echo ""
echo "[T04] IPC cleanup after normal exit"
cleanup_ipc
timeout 20 ./oss -n 5 -s 3 -t 2 -f /dev/null > /dev/null 2>&1
if ipc_clean; then
    pass "no shared memory or message queue segments remain"
else
    fail "IPC segments still present after exit"
    cleanup_ipc
fi

# ── T05  IPC cleanup after SIGINT ────────────────────────────
echo ""
echo "[T05] IPC cleanup after SIGINT"
cleanup_ipc
./oss -n 100 -s 10 -t 10 -f /dev/null > t05_out.txt 2>&1 &
OSS_PID=$!
sleep 2
kill -INT $OSS_PID 2>/dev/null
wait $OSS_PID 2>/dev/null
sleep 1
if ipc_clean; then
    pass "IPC removed after SIGINT"
else
    fail "IPC segments remain after SIGINT"
    cleanup_ipc
fi
rm -f t05_out.txt

# ── T06  Simultaneous-process cap ────────────────────────────
echo ""
echo "[T06] Simultaneous cap (-s 4)"
./oss -n 40 -s 4 -t 5 -f t06.log > /dev/null 2>&1 &
OSS_PID=$!
MAX_SEEN=0
for i in $(seq 1 8); do
    sleep 0.5
    COUNT=$(ps aux | grep "[w]orker" | wc -l)
    [ "$COUNT" -gt "$MAX_SEEN" ] && MAX_SEEN=$COUNT
done
wait $OSS_PID 2>/dev/null
cleanup_ipc
if [ "$MAX_SEEN" -le 4 ]; then
    pass "max simultaneous workers observed: $MAX_SEEN (limit 4)"
else
    fail "observed $MAX_SEEN simultaneous workers, limit is 4"
fi
rm -f t06.log

# ── T07  Wall-clock forking cap (5 s) ────────────────────────
echo ""
echo "[T07] Wall-clock forking cap"
START=$(date +%s)
timeout 60 ./oss -n 9999 -s 5 -t 3 -f t07.log > /dev/null 2>&1
END=$(date +%s)
ELAPSED=$((END - START))
FORKED=$(grep -c "forked worker" t07.log 2>/dev/null || echo 0)
if [ "$ELAPSED" -le 15 ] && [ "$FORKED" -lt 9999 ]; then
    pass "stopped forking after ~5s wall clock (elapsed=${ELAPSED}s, forked=$FORKED)"
else
    fail "elapsed=${ELAPSED}s, forked=$FORKED (expected to stop well before 9999)"
fi
cleanup_ipc
rm -f t07.log

# ── T08  Clock advances (not frozen) ─────────────────────────
echo ""
echo "[T08] Clock advances between events"
timeout 30 ./oss -n 5 -s 3 -t 5 -f t08.log > /dev/null 2>&1
cleanup_ipc
DISTINCT=$(grep -oP 'time \K[0-9]+:[0-9]+' t08.log 2>/dev/null | sort -u | wc -l)
if [ "$DISTINCT" -gt 5 ]; then
    pass "$DISTINCT distinct timestamps in log (clock is advancing)"
else
    fail "only $DISTINCT distinct timestamps — clock may be frozen"
fi
rm -f t08.log

# ── T09  Blocking and unblocking ─────────────────────────────
echo ""
echo "[T09] Resource blocking and unblocking"
timeout 30 ./oss -n 10 -s 8 -t 5 -f t09.log > /dev/null 2>&1
cleanup_ipc
BLOCKED=$(grep -c "not granted" t09.log 2>/dev/null || echo 0)
UNBLOCKED=$(grep -c "from freed resources" t09.log 2>/dev/null || echo 0)
if [ "$BLOCKED" -gt 0 ] && [ "$UNBLOCKED" -gt 0 ]; then
    pass "blocked=$BLOCKED events, unblocked-from-queue=$UNBLOCKED events"
elif [ "$BLOCKED" -gt 0 ]; then
    pass "blocked=$BLOCKED events (no wait-queue grants needed this run)"
else
    fail "no blocking events observed (blocked=$BLOCKED, unblocked=$UNBLOCKED)"
fi
rm -f t09.log

# ── T10  Deadlock detection runs ─────────────────────────────
echo ""
echo "[T10] Deadlock detection runs every ~1 sim-second"
timeout 30 ./oss -n 15 -s 12 -t 8 -f t10.log > /dev/null 2>&1
cleanup_ipc
DL_RUNS=$(grep -c "running deadlock detection" t10.log 2>/dev/null || echo 0)
if [ "$DL_RUNS" -gt 0 ]; then
    DL_KILLS=$(grep "terminated by deadlock" t10.log | grep -oP '[0-9]+$' | tail -1)
    pass "deadlock detection ran $DL_RUNS times; kills=${DL_KILLS:-0}"
else
    fail "deadlock detection never ran (check sim-clock interval)"
fi
rm -f t10.log

# ── T11  Statistics completeness ─────────────────────────────
echo ""
echo "[T11] End-of-run statistics completeness"
timeout 20 ./oss -n 5 -s 3 -t 3 -f t11.log > /dev/null 2>&1
cleanup_ipc
OK=1
for pattern in "Total resource requests" "Immediately granted" \
               "Deadlock detection runs" "terminated by deadlock"; do
    grep -q "$pattern" t11.log 2>/dev/null || { OK=0; fail "missing: $pattern"; }
done
[ "$OK" -eq 1 ] && pass "all four statistics lines present"
rm -f t11.log

# ── T12  Resource table format ────────────────────────────────
echo ""
echo "[T12] Resource table format"
timeout 20 ./oss -n 5 -s 3 -t 3 -f t12.log > /dev/null 2>&1
cleanup_ipc
if grep -q "OSS Resource Table" t12.log && \
   grep -q "Avail:" t12.log && \
   grep -qP "R0\s+R1" t12.log; then
    pass "resource table header, Avail row, and column headers present"
else
    fail "resource table missing or malformed"
fi
rm -f t12.log

# ── T13  No orphan processes ──────────────────────────────────
echo ""
echo "[T13] No orphan worker processes after exit"
timeout 20 ./oss -n 5 -s 3 -t 3 -f /dev/null > /dev/null 2>&1
sleep 1
ORPHANS=$(ps aux | grep "[w]orker" | wc -l)
if [ "$ORPHANS" -eq 0 ]; then
    pass "no worker processes remain"
else
    fail "$ORPHANS worker process(es) still running after oss exited"
    killall worker 2>/dev/null
fi
cleanup_ipc

# ── Summary ───────────────────────────────────────────────────
echo ""
echo "======================================"
echo " Results: $PASS passed, $FAIL failed"
echo "======================================"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
