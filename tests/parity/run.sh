#!/usr/bin/env bash
# Parity test runner: runs every tests/*.va through the Python interpreter,
# the native vself interpreter, and the native vc compiler, and reports where
# the three disagree. Run from the vanta/ dir: bash tests/run.sh
cd "$(dirname "$0")/.."
PY="python3 vanta.py"; VS="./vanta.va.bin"; VC="./vc.va.bin"
pass=0; total=0; vsgap=0; vcgap=0
printf "%-22s %-8s %-8s %-8s\n" "test" "python" "vself" "vc"
printf "%-22s %-8s %-8s %-8s\n" "----" "------" "-----" "--"
for f in tests/*.va; do
    total=$((total+1)); name=$(basename "$f")
    py=$($PY "$f" 2>&1)
    vs=$($VS "$f" 2>&1)
    vcraw=$($VC "$f" 2>&1)
    if echo "$vcraw" | grep -q '^----$'; then vc=$(echo "$vcraw" | sed '1,/^----$/d'); else vc="<compile-failed>"; fi
    ps="ref"
    if [ "$vs" = "$py" ]; then vss="ok"; else vss="DIFF"; vsgap=$((vsgap+1)); fi
    if [ "$vc" = "$py" ]; then vcs="ok"; else vcs="DIFF"; vcgap=$((vcgap+1)); fi
    if [ "$vss" = "ok" ] && [ "$vcs" = "ok" ]; then pass=$((pass+1)); fi
    printf "%-22s %-8s %-8s %-8s\n" "$name" "$ps" "$vss" "$vcs"
done
echo
echo "FULL PARITY (all 3 agree): $pass / $total"
echo "vself matches python:      $((total-vsgap)) / $total"
echo "vc matches python:         $((total-vcgap)) / $total"
