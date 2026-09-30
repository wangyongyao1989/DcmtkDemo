#!/bin/bash
# run_all.sh -- regenerate the whole host-side deliverable tree from /tmp/dental_cbct.
#
#   bash /tmp/ai_build/run_all.sh            # rebuild everything in place (~6 min)
#   CLEAN=1 bash /tmp/ai_build/run_all.sh    # archive derived outputs first, then rebuild
#   SKIP_VARIANTS=1 bash /tmp/ai_build/run_all.sh   # skip the feature-design study (~4 min)
#
# Requires: python3 (3.11+), numpy, scipy, nibabel, pydicom, onnx, onnxruntime.
# Nothing in this script downloads anything; the only inputs are
# /tmp/dental_cbct/{images,labels,dentalsegmentator} (see prep.py load_*).
# Every step is deterministic (prep.TRAIN_SEED) and re-running this script must
# reproduce the byte sizes / sha256 recorded in metrics.json.
set -euo pipefail

cd "$(dirname "$0")"          # /tmp/ai_build
HERE=$(pwd)
SRC=/tmp/dental_cbct
PY=${PYTHON:-python3}
export OMP_NUM_THREADS=${OMP_NUM_THREADS:-4}
LOGDIR="$HERE/work"
mkdir -p "$LOGDIR"

[ -d "$SRC" ] || { echo "FATAL: input dir $SRC missing"; exit 2; }
for f in build_app_volumes.py prep.py train_model.py make_parity.py make_report.py verify_all.py; do
  [ -f "$HERE/$f" ] || { echo "FATAL: $HERE/$f missing"; exit 2; }
done
$PY -c "import numpy, scipy, nibabel, pydicom, onnx, onnxruntime" || {
  echo "FATAL: missing python deps; install with"
  echo "  $PY -m pip install --disable-pip-version-check --trusted-host pypi.org" \
       "--trusted-host files.pythonhosted.org numpy scipy nibabel pydicom onnx onnxruntime"
  exit 2; }

if [ "${CLEAN:-0}" = "1" ]; then
  # Non-destructive by policy: another host process may own these paths, so archive
  # (mv) instead of deleting, then rebuild from scratch.
  TS=$(date +%Y%m%dT%H%M%S)
  BK="$LOGDIR/clean_backup_$TS"
  mkdir -p "$BK"
  for p in app_volume gt model parity_ref metrics.json toothseg_report.md work/cache; do
    if [ -e "$HERE/$p" ]; then mkdir -p "$BK/$(dirname "$p")"; mv "$HERE/$p" "$BK/$p"; fi
  done
  mkdir -p "$HERE/app_volume" "$HERE/gt" "$HERE/model" "$HERE/parity_ref" "$LOGDIR/cache"
  echo "cleaned: previous outputs archived to $BK"
fi

run () {  # run <step> <script>
  echo "=== [$1] $2  ($(date -u +%FT%TZ))"
  /usr/bin/time -l $PY "$2" >"$LOGDIR/$1_log.txt" 2>&1 || {
      echo "FATAL: step $1 failed, see $LOGDIR/$1_log.txt"; tail -20 "$LOGDIR/$1_log.txt"; exit 1; }
  tail -3 "$LOGDIR/$1_log.txt" || true
  echo "--- [$1] done"
}

# 1. canonical 192x192x128 @0.60mm app volumes + GT masks + DICOM series (+ round-trip assert)
run 01_app_volumes build_app_volumes.py

# 2. feature-design study (radii / band-pass), train-split cases only.
#    Must run BEFORE train_model.py: metrics.json embeds its result.
if [ "${SKIP_VARIANTS:-0}" = "1" ]; then
  echo "=== [02_variants] skipped (SKIP_VARIANTS=1)"
else
  run 02_variants variants.py
fi

# 3. numpy-only training of the 1x1x1 conv head + ONNX export + metrics.json
run 03_train train_model.py

# 4. parity fixtures for the held-out case, built from the DICOM series re-read
run 04_parity make_parity.py

# 5. definition-of-done checks (writes work/verify_summary.json, which the report
#    quotes; exits non-zero on any failure -- the failure is re-raised at the end so
#    the report still gets written for inspection)
echo "=== [05_verify] verify_all.py"
VRC=0
$PY verify_all.py >"$LOGDIR/05_verify_log.txt" 2>&1 || VRC=$?
grep -E "^(FAIL|[0-9]+ checks run)" "$LOGDIR/05_verify_log.txt" || true
tail -14 "$LOGDIR/05_verify_log.txt" || true
if [ "$VRC" != "0" ]; then echo "!!! verify_all.py exited $VRC (see $LOGDIR/05_verify_log.txt)"; fi

# 6. report (generated only from measured artefacts, including the verify summary)
run 06_report make_report.py

# 7. reproducibility: rebuild volumes/GT/DICOM into work/repro_check/ and prove the
#    shipped artefacts are byte-identical (non-destructive, no rm of shipped paths)
echo "=== [07_repro] repro_check.py"
RC=0
$PY repro_check.py >"$LOGDIR/07_repro_log.txt" 2>&1 || RC=$?
grep -E "^FAIL" "$LOGDIR/07_repro_log.txt" || true
tail -2 "$LOGDIR/07_repro_log.txt" || true
if [ "$RC" != "0" ]; then echo "!!! repro_check.py exited $RC"; fi

echo "=== OK  $(date -u +%FT%TZ)  artifacts under $HERE:"
ls -1 "$HERE/metrics.json" "$HERE/SPEC.md" "$HERE/toothseg_report.md" \
      "$HERE/model/teeth_cnn.onnx" "$HERE/parity_ref/summary.txt"
du -sh "$HERE/app_volume" "$HERE/gt" "$HERE/parity_ref" 2>/dev/null || true
[ "$VRC" = "0" ] && [ "$RC" = "0" ] || exit 1
exit 0
