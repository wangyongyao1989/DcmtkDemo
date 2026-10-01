#!/bin/bash
# run_all.sh -- regenerate the host-side deliverable tree from the NIfTI inputs.
#
#   bash scripts/run_all.sh                     # rebuild in place (~6 min)
#   CLEAN=1 bash scripts/run_all.sh             # archive derived outputs first
#   SKIP_VARIANTS=1 bash scripts/run_all.sh     # skip the feature-design study (~4 min)
#   SKIP_TRAIN=1 bash scripts/run_all.sh        # keep model/teeth_cnn.onnx as-is (fixtures/report only)
#   AI_BUILD=/tmp/out bash scripts/run_all.sh   # write elsewhere (keeps the repo clean)
#
# Inputs come from ../raw (override: DENTAL_SRC=/path), outputs go to the
# cbctmeasure/src/host/ai directory this script sits under (override: AI_BUILD).
# Requires: python3 (3.11+), numpy, scipy, nibabel, pydicom, onnx, onnxruntime.
# Nothing in this script downloads anything.
# Every step is deterministic (prep.TRAIN_SEED) and re-running this script must
# reproduce model/teeth_cnn.onnx byte-for-byte -- its sha256 is recorded twice:
# parity_ref/device_parity.json (model.sha256) and, as the shipped asset, in
# cbctmeasure/src/main/assets/models/teeth_cnn.onnx.  Step 05 asserts both.
set -euo pipefail

SCRIPTS=$(cd "$(dirname "$0")" && pwd)     # .../host/ai/scripts
HERE=${AI_BUILD:-$(dirname "$SCRIPTS")}    # == prep.OUT_DIR
SRC=${DENTAL_SRC:-$(dirname "$SCRIPTS")/raw}  # == prep.SRC_DIR（输入永远跟着仓库走）
mkdir -p "$HERE"
# AI_BUILD must agree with what prep.py computes, or the steps would read and
# write two different trees; prep.py honours the same env var, so just export it.
export AI_BUILD="$HERE" DENTAL_SRC="$SRC"
PY=${PYTHON:-python3}
export OMP_NUM_THREADS=${OMP_NUM_THREADS:-4}
LOGDIR="$HERE/work"
mkdir -p "$LOGDIR"

[ -d "$SRC" ] || { echo "FATAL: input dir $SRC missing"; exit 2; }
# train_model.py 是早期的 1x1x1 逐体素 MLP 实验（现在只作为 model/teeth_cnn_pointwise_old.onnx
# 的历史备份存在），真正产出随包模型的是 train2.py 的 3x3x3 主干。
# 早先这里把 train_model.py 列为必需步骤，clean run 会把 14477B 的随包模型覆盖成 3851B 的旧结构，
# 然后 step 05 以 "every Conv has kernel_shape [1,1,1]" 失败收尾 —— 看起来像模型坏了，其实是流水线接错。
for f in dump_template.py build_app_volumes.py prep.py train2.py \
         make_parity.py make_parity_device.py make_report.py verify_all.py; do
  [ -f "$SCRIPTS/$f" ] || { echo "FATAL: $SCRIPTS/$f missing"; exit 2; }
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

# 输出骨架无条件建好：AI_BUILD 指向一个全新目录（clean clone 做复现校验）时，
# 后续步骤只写文件不建目录，缺目录会以 FileNotFoundError 假报成"输入不全"。
mkdir -p "$HERE/app_volume" "$HERE/gt" "$HERE/model" "$HERE/parity_ref" "$HERE/work/cache"

run () {  # run <step> <script>
  echo "=== [$1] $2  ($(date -u +%FT%TZ))"
  /usr/bin/time -l $PY "$SCRIPTS/$2" >"$LOGDIR/$1_log.txt" 2>&1 || {
      echo "FATAL: step $1 failed, see $LOGDIR/$1_log.txt"; tail -20 "$LOGDIR/$1_log.txt"; exit 1; }
  tail -3 "$LOGDIR/$1_log.txt" || true
  echo "--- [$1] done"
}

# 0. DICOM 写盘模板：从仓库资产 app/src/main/assets/neck_ct 导出到 work/。
#    没有它 step 01 起不来（clean clone 实测 FileNotFoundError）。
run 00_template dump_template.py

# 1. canonical 192x192x128 @0.60mm app volumes + GT masks + DICOM series (+ round-trip assert)
run 01_app_volumes build_app_volumes.py

# 2. feature-design study (radii / band-pass), train-split cases only.
#    Must run BEFORE train_model.py: metrics.json embeds its result.
if [ "${SKIP_VARIANTS:-0}" = "1" ]; then
  echo "=== [02_variants] skipped (SKIP_VARIANTS=1)"
else
  run 02_variants variants.py
fi

# 3. numpy-only training of the 3x3x3 Conv3d head (6->8->8->2, k3 p1) + ONNX export.
#    This is the trainer whose output ships as cbctmeasure/src/main/assets/models/teeth_cnn.onnx,
#    so it is the one step that can overwrite an artefact the app actually runs.
#    SKIP_TRAIN=1 keeps the existing model/teeth_cnn.onnx (use it when you only want to
#    rebuild fixtures or the report); the sha256 is re-checked against the app asset in step 05.
if [ "${SKIP_TRAIN:-0}" = "1" ]; then
  echo "=== [03_train] skipped (SKIP_TRAIN=1) —— 保留现有 model/teeth_cnn.onnx"
else
  run 03_train train2.py
fi

# 4. parity fixtures for the held-out case, built from the DICOM series re-read:
#    canonical host axis order (make_parity.py) + device axis order (make_parity_device.py).
#    The device set is the one the Android AI layer is parity-checked against, and it is the
#    one committed under parity_ref/device_*_0101.raw.
run 04_parity make_parity.py
run 04b_parity_device make_parity_device.py

# 5. definition-of-done checks (writes work/verify_summary.json, which the report
#    quotes; exits non-zero on any failure -- the failure is re-raised at the end so
#    the report still gets written for inspection)
echo "=== [05_verify] verify_all.py"
VRC=0
$PY "$SCRIPTS/verify_all.py" >"$LOGDIR/05_verify_log.txt" 2>&1 || VRC=$?
grep -E "^(FAIL|[0-9]+ checks run)" "$LOGDIR/05_verify_log.txt" || true
tail -14 "$LOGDIR/05_verify_log.txt" || true
if [ "$VRC" != "0" ]; then echo "!!! verify_all.py exited $VRC (see $LOGDIR/05_verify_log.txt)"; fi

# 6. report (generated only from measured artefacts, including the verify summary)
run 06_report make_report.py

# 7. reproducibility: rebuild volumes/GT/DICOM into work/repro_check/ and prove the
#    shipped artefacts are byte-identical (non-destructive, no rm of shipped paths)
echo "=== [07_repro] repro_check.py"
RC=0
$PY "$SCRIPTS/repro_check.py" >"$LOGDIR/07_repro_log.txt" 2>&1 || RC=$?
grep -E "^FAIL" "$LOGDIR/07_repro_log.txt" || true
tail -2 "$LOGDIR/07_repro_log.txt" || true
if [ "$RC" != "0" ]; then echo "!!! repro_check.py exited $RC"; fi

echo "=== OK  $(date -u +%FT%TZ)  artifacts under $HERE:"
# metrics.json 只有旧的 1x1x1 实验（train_model.py）会写；train2.py 写的是
# parity_ref/metrics_0101.json + work/train2_full.json，所以这里按存在与否列出。
for f in metrics.json toothseg_report.md model/teeth_cnn.onnx \
         parity_ref/parity_0101.json parity_ref/metrics_0101.json \
         parity_ref/device_parity.json "$SCRIPTS/SPEC.md"; do
  [ -e "$HERE/$f" ] || [ -e "$f" ] || continue
  ls -1 "$HERE/$f" 2>/dev/null || ls -1 "$f"
done
du -sh "$HERE/app_volume" "$HERE/gt" "$HERE/parity_ref" 2>/dev/null || true
[ "$VRC" = "0" ] && [ "$RC" = "0" ] || exit 1
exit 0
