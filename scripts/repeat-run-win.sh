#!/usr/bin/env bash
# Repeat the launch and report only the failure signature, to tell a deterministic crash
# from a race or an uninitialised read.
cd "$(dirname "${BASH_SOURCE[0]}")/.." || exit 1
runs=${1:-4}
for i in $(seq 1 "$runs"); do
    echo "===== RUN $i ====="
    bash scripts/run-hokuto-win.sh > "/tmp/bbport-run-$i.log" 2>&1
    echo "exit=$?"
    grep -E 'fault|STOP|bytes at RIP|guest frames|== exit' "/tmp/bbport-run-$i.log" | head -6
    echo "  (full log: /tmp/bbport-run-$i.log)"
done