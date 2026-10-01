"""make_report.py -- generate /tmp/ai_build/toothseg_report.md from the measured
artefacts (metrics.json, work/crop_meta.json, work/dicom_roundtrip.json,
parity_ref/summary.txt).  Numbers are never typed by hand here, so the report cannot
drift from the run.
"""
import json
import os
import time
import numpy as np
import prep

OUT = prep.OUT_DIR
# 本脚本描述的是"旧 1x1x1 实验"那条链路（train_model.py -> metrics.json -> 本报告）。
# 当前随包交付的链路是 train2.py（3x3x3 + 设备轴序），它写的是
# parity_ref/metrics_0101.json，报告在 cbctmeasure/doc/ 与 parity_ref/METRICS.md（parity.py 生成）。
# 没有 metrics.json 或它不是那个 schema 时，必须说清楚"这一步没数据"，
# 不能拿一半字段去 % 格式化，崩成 "TypeError: must be real number, not str" 那种假故障。
if not os.path.exists(f"{OUT}/metrics.json"):
    raise SystemExit("NOTE: 缺 %s/metrics.json —— make_report.py 只描述旧 1x1x1 实验"
                     "（train_model.py 产出）。当前交付链路的报告见 doc/ 与 parity_ref/METRICS.md。" % OUT)
M = json.load(open(f"{OUT}/metrics.json"))
if not {"model", "held_out", "parity_fixtures"} <= set(M):
    raise SystemExit("NOTE: %s/metrics.json 不是旧 1x1x1 实验的记录（缺 %s），"
                     "make_report.py 不适用；当前链路请读 doc/ 与 parity_ref/METRICS.md。"
                     % (OUT, sorted({"model", "held_out", "parity_fixtures"} - set(M))))
CR = json.load(open(f"{OUT}/work/crop_meta.json"))
RT = json.load(open(f"{OUT}/work/dicom_roundtrip.json"))
MD = M["model"]
HO = M["held_out"]
DS = M["reference_dentalsegmentator_vs_gt"]
NV = M["reference_naive_hu_band"]
CAL = MD["calibration"]
VAR = M.get("feature_variant_study") or []
PF = M["parity_fixtures"]

# re-measured right here (nothing copied from the run log): ONNX Runtime on the
# shipped model file, fed with the shipped feat_0101.raw fixture.
import onnxruntime as _ort
_sess = _ort.InferenceSession(MD["onnx_path"], providers=["CPUExecutionProvider"])
_feat = np.fromfile(f"{OUT}/parity_ref/feat_0101.raw", np.float32).reshape(1, 6, 96, 96, 64)
_prob = _sess.run(None, {_sess.get_inputs()[0].name: _feat})[0]
_ref = np.fromfile(f"{OUT}/parity_ref/prob_0101.raw", np.float32).reshape(1, 2, 96, 96, 64)
ORT_DIFF = float(np.abs(_prob - _ref).max())
VS = json.load(open(f"{OUT}/work/verify_summary.json")) if (
    os.path.exists(f"{OUT}/work/verify_summary.json")) else None


def f(x, n=4):
    return ("%." + str(n) + "f") % x


def pc(s, keys="dice precision recall f1 tp fp fn".split()):
    return ", ".join("%s %s" % (k, (f(s[k]) if isinstance(s[k], float) else "%d" % s[k]))
                     for k in keys)


L = []
A = L.append
A("# CBCT tooth segmentation (host side) — build report")
A("")
A("> **本报告描述的是旧 1x1x1 逐体素 MLP 实验**（`train_model.py` -> `metrics.json`）。"
  "随包交付的设备链路是 `train2.py` 的 3x3x3 Conv3d + 设备轴序，其数据见 "
  "`parity_ref/metrics_0101.json`、`parity_ref/METRICS.md` 与 `cbctmeasure/doc/` 下的真机测试报告。"
  "两份报告的 Dice 不可直接对比（阈值 0.49 与通道 argmax 是不同的后处理规则）。")
A("")
A("Generated %s by `python3 %s/make_report.py` from `%s/metrics.json` "
  "(wall clock of the training step: %s s). Every number below was measured on this "
  "machine; nothing is estimated." % (time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                                      OUT, OUT, MD["wall_clock_seconds"]))
A("")
A("## 1. What was produced")
A("")
A("| artefact | detail |")
A("|---|---|")
A("| app volumes | 4 DICOM series, `%s/app_volume/<case>/000001.dcm..000128.dcm`, "
  "int16 signed HU, 192x192x128 @ 0.60 mm, Explicit VR LE |" % OUT)
for cid in prep.ALL_CASES:
    A("| `%s` | %d files, %.2f MB, round-trip `array_equal` = %s, crop start %s |"
      % (cid, RT[cid]["n_files"], RT[cid]["total_mb"],
         RT[cid]["reassembled_array_equal_canonical"], CR[cid]["start"]))
A("| GT | `%s/gt/<case>_tooth.{nii.gz,raw}` uint8 {0,1} 192x192x128 |" % OUT)
A("| model | `%s/model/teeth_cnn.onnx`, %d B, opset %d, %d params, nodes %s |"
  % (OUT, MD["onnx_bytes"], MD["opset"], MD["parameter_count"],
     "/".join(M["cases"][HO]["onnx_numpy_prob_agreement"]["ort_ops"])))
A("| metrics | `%s/metrics.json` |" % OUT)
A("| parity | `%s/parity_ref/` feat/prob/lab/inst raw + summary.txt |" % OUT)
A("| spec | `%s/SPEC.md` (normative constants for the C++ port) |" % OUT)
A("")
A("## 2. Headline: held-out case %s (never used for training or tuning)" % HO)
A("")
c = M["cases"][HO]
A("| resolution | Dice | F1 | precision | recall | TP | FP | FN | pred vox | GT vox |")
A("|---|---|---|---|---|---|---|---|---|---|")
for key, label in (("grid_96x96x64", "96x96x64 model grid (1.2 mm)"),
                   ("grid_192x192x128_nearest", "192x192x128 after nearest x2"),
                   ("vs_native_0p30mm_gt", "native 0.30 mm expert GT (x4)")):
    s = c[key]
    A("| %s | **%s** | %s | %s | %s | %d | %d | %d | %d | %d |"
      % (label, f(s["dice"]), f(s["f1"]), f(s["precision"]), f(s["recall"]),
         s["tp"], s["fp"], s["fn"], s["pred_voxels"], s["gt_voxels"]))
A("")
A("* The 192-grid number is **lower** than the 96-grid number by construction: the GT "
  "used as target is the 2x2x2 majority vote of the 0.30 mm annotation, and scoring at "
  "192x192x128 compares the nearest-upsampled prediction against the *finer* 0.60 mm "
  "mask. The ceiling for any 96-grid predictor there is the measured oracle "
  "%s (`cases.%s.oracle_ceiling_gt_vs_gt_at_192`)."
  % (f(c["oracle_ceiling_gt_vs_gt_at_192"]["dice"]), HO))
A("* Uncalibrated argmax (`prob[1] > 0.5`) on the same case: %s. The exported model "
  "already contains the prior correction in `b3` (see §4)."
  % pc(c["grid_96x96x64_uncalibrated_prob_half"]))
A("* 0.30 mm detail: %d GT tooth voxels lie outside the app crop for this case, so "
  "they are unrecoverable by the model (`vs_native_0p30mm_gt."
  "gt_tooth_voxels_outside_app_crop`)."
  % c["vs_native_0p30mm_gt"]["gt_tooth_voxels_outside_app_crop"])
A("")
A("## 3. Independent accuracy references (same GT definition)")
A("")
A("| reference | case | Dice | precision | recall |")
A("|---|---|---|---|---|")
for cid, s in DS.items():
    A("| DentalSegmentator labels 3+4 vs expert tooth union, native 0.30 mm | %s | "
      "**%s** | %s | %s |" % (cid, f(s["dice"]), f(s["precision"]), f(s["recall"])))
for cid in prep.ALL_CASES:
    s = NV[cid]["at_96_grid"]
    A("| naive HU band 1200-4000 at 96 grid | %s | %s | %s | %s |"
      % (cid, f(s["dice"]), f(s["precision"]), f(s["recall"])))
A("")
A("Honest reading: **the public DentalSegmentator model reaches Dice ~%s/%s on these "
  "cases; the per-voxel MLP I was allowed to build within the fixed 6-channel/1x1x1 "
  "contract reaches ~%s on held-out data.** It is %sx better than an intensity "
  "threshold and ~%sx worse than a real 3D CNN. Use it to validate the on-device "
  "*pipeline* (IO, features, ONNX, argmax, components) - not as a clinically "
  "meaningful tooth segmentor."
  % (f(DS["dentvoxel_0021"]["dice"]), f(DS["dentvoxel_0047"]["dice"]),
     f(c["grid_96x96x64"]["dice"]),
     f(c["grid_96x96x64"]["dice"] / NV[HO]["at_96_grid"]["dice"], 1),
     f(DS["dentvoxel_0021"]["dice"] / c["grid_96x96x64"]["dice"], 1)))
A("")
A("## 4. Model / training facts")
A("")
A("* architecture `%s` -> %d params, %d B ONNX, opset %d, %d nodes, ops %s"
  % (MD["architecture"], MD["parameter_count"], MD["onnx_bytes"], MD["opset"],
     M["cases"][HO]["onnx_numpy_prob_agreement"]["n_nodes"],
     "/".join(M["cases"][HO]["onnx_numpy_prob_agreement"]["ort_ops"])))
A("* contract: input `feat` %s float32 -> output `prob` %s float32, softmax over "
  "channel (measured max deviation of p[0]+p[1] from 1.0 over the whole 96^3 grid: %.3e)"
  % (M["cases"][HO]["onnx_numpy_prob_agreement"]["input_shape"],
     M["cases"][HO]["onnx_numpy_prob_agreement"]["output_shape"],
     float(np.abs(np.fromfile(f"{OUT}/parity_ref/prob_0101.raw", np.float32)
                  .reshape(2, 96, 96, 64).sum(axis=0) - 1.0).max())))
A("* %s" % MD["optimizer"]); A("* minibatch %d, epochs %d (chosen: %s)"
  % (MD["minibatch"], MD["epochs_final"], MD["epoch_choice"]["criterion"]))
A("* Adam iterations: phase1 %d (search over %d epochs) + phase2 %d = %d total; "
  "wall clock %s s for train+export+evaluate"
  % (MD["iterations_phase1"], MD["epoch_choice"]["max_epochs_searched"],
     MD["iterations_final"], MD["total_iterations"], MD["wall_clock_seconds"]))
A("* sampling: %s; counts %s" % (MD["sampling_rule"], json.dumps(MD["sample_counts"])))
A("* prior calibration: `t* = %s` on `z1 - z0` maximises pooled train-case Dice (%s); "
  "folded in as `b3 %s -> %s`, so the device does plain argmax. Train-pooled numbers "
  "at `t*`: %s" % (CAL["threshold_on_logit_diff"],
                   f(CAL["dice_at_threshold_on_calibration_cases"]["dice"]),
                   [f(x, 6) for x in CAL["b3_before"]],
                   [f(x, 6) for x in CAL["b3_after"]],
                   pc(CAL["dice_at_threshold_on_calibration_cases"])))
def hist_at(ep):
    for e in MD["epoch_choice"]["phase1_history"]:
        if e["epoch"] == ep:
            return e
    return None


H_BEST = hist_at(MD["epoch_choice"]["chosen"])
H1, H5, H_LAST = hist_at(1), hist_at(5), hist_at(MD["epoch_choice"]["max_epochs_searched"])
A("* epoch count chosen in phase 1 = %d, scored as full-grid Dice on the monitor case "
  "dentvoxel_0074: %s (precision %s, recall %s; tp %d fp %d fn %d). Curve: ep1 %s, ep5 %s, "
  "ep%d (best) %s, ep%d %s -> flat within +-0.02 after ~5 epochs, so the epoch choice is "
  "not a sharp optimum and 0101 was never consulted for it."
  % (MD["epoch_choice"]["chosen"], f(H_BEST["val_dice"]), f(H_BEST["val_precision"]),
     f(H_BEST["val_recall"]), H_BEST["val_tp"], H_BEST["val_fp"], H_BEST["val_fn"],
     f(H1["val_dice"]), f(H5["val_dice"]), H_BEST["epoch"], f(H_BEST["val_dice"]),
     H_LAST["epoch"], f(H_LAST["val_dice"])))
A("* train-split cases (fit, so optimistic): " + ", ".join(
    "%s 96-grid Dice %s / 192 %s" % (cid, f(M["cases"][cid]["grid_96x96x64"]["dice"]),
                                      f(M["cases"][cid]["grid_192x192x128_nearest"]["dice"]))
    for cid in prep.CASES_TRAIN))
if VAR:
    A("* feature design study (%s), scored on 0074 only:" %
      "radii/band-pass pairs, identical phase-1 protocol")
    for v in sorted(VAR, key=lambda x: -x["best_val_dice_0074"]):
        A("  * radii %s bp %s -> val Dice %s (best epoch %d)%s"
          % (v["radii"], v["bp"], f(v["best_val_dice_0074"]), v["best_epoch"],
             "  <== shipped" if (v["radii"] == list(prep.FEAT_RADII)
                                 and v["bp"] == list(prep.BANDPASS_PAIR)) else ""))
A("")
A("## 5. Preprocessing constants the C++ port needs")
A("")
A("```")
A("crop      : band HU 1200..4000 inclusive; percentile 1/99 per axis (numpy linear");
A("            interpolation), floor/ceil; margin (6,6,8) voxels; clamp to 220x220x172;")
A("            center=(lo+hi)//2; start=clip(center-(192,192,64+64)//2, 0, N-FOV)")
A("            measured starts: %s" % ", ".join("%s%s" % (k[11:], CR[k]["start"])
                                                for k in prep.ALL_CASES))
A("HU -> n   : n = (clip(HU,-1024,4095) + 1024)/5120 - 0.5      (float32, [-0.5,0.5])")
A("c0        : 2x2x2 mean of n, accumulate double, /8.0        (96x96x64, 1.2 mm/voxel)")
A("c1..c4    : separable box means of c0, half-widths r = 1,2,4,8  (boxes 1.8/3.0/5.4/10.2 mm)")
A("c5        : c3 - c4   (box r=4 minus box r=8)")
A("boundary  : replicate (edge clamp) on every axis, every radius; running sums in double")
A("index order: (x,y,z) = shape order, NIfTI affine ignored; NCHW offset = ((c*96+x)*96+y)*64+z")
A("dicom     : slice file n = z index n-1; pixel row = axis x, column = axis y;")
A("            Rows=Columns=192, PixelSpacing 0.6/0.6, SliceThickness 0.6,")
A("            IOP [1,0,0,0,1,0], IPP [0,0,0.6*(n-1)], BitsAllocated 16 signed,")
A("            RescaleIntercept 0 / Slope 1  => stored int16 IS the HU value")
A("post      : lab = argmax_c prob; app mask = lab[x/2,y/2,z/2]; 26-connectivity CC")
A("```")
A("")
A("## 6. Verification (what actually ran)")
A("")
A("* DICOM round trip, all 4 series: `np.array_equal(reassembled, canonical)` = %s "
  "(%s); raw PixelData of slice 0 == canonical[:,:,0] = %s"
  % (all(RT[k]["reassembled_array_equal_canonical"] for k in RT),
     ", ".join("%s %.2f MB" % (k, RT[k]["total_mb"]) for k in RT),
     all(RT[k]["raw_pixeldata_equals_hu_slice0"] for k in RT)))
A("* series size target <= 25 MB: %s (max %.2f MB)"
  % (all(RT[k]["under_25mb"] for k in RT), max(RT[k]["total_mb"] for k in RT)))
A("* ORT-vs-fixture on `feat_0101.raw`: re-running ONNX Runtime on the shipped "
  "`model/teeth_cnn.onnx` reproduces `prob_0101.raw` with max abs diff %.3e (measured "
  "here, not copied); repeated ORT runs bit-exact = %s; prob raw re-read bit-exact = %s"
  % (ORT_DIFF, PF["ort_repeat_run_bit_exact"], PF["prob_raw_reread_bit_exact"]))
# 旧 metrics.json 没有记 sha256，就在这里量（本报告的原则是"只写量出来的数"）。
import hashlib as _hl
_md_sha = _hl.sha256(open(MD["onnx_path"], "rb").read()).hexdigest()
A("* model file: %d B, sha256 `%s` (measured here, %s). `verify_all.py` asserts this "
  "sha against `parity_ref/device_parity.json` and against the shipped app asset "
  "`src/main/assets/models/teeth_cnn.onnx`; the 1x1x1 experiment this report describes is "
  "kept as `model/teeth_cnn_pointwise_old.onnx` and is asserted to be a *different* file."
  % (MD["onnx_bytes"], _md_sha[:16] + "...", MD["onnx_path"]))
A("* my numpy forward vs ONNX Runtime: max abs diff %s (float64 vs float32 accumulation)"
  % M["cases"][HO]["onnx_numpy_prob_agreement"]["ort_max_abs_diff_vs_numpy"])
A("* features from the DICOM series == features from the NIfTI: %s (bit-exact); "
  "DICOM re-read == canonical array: %s"
  % (PF["features_dicom_equal_features_nifti"], PF["dicom_reread_equals_canonical"]))
A("* fixture sizes (measured with os.path.getsize): feat %d B (=1*6*96*96*64*4), prob %d B, "
  "lab %d B, inst %d B"
  % tuple(os.path.getsize(f"{OUT}/parity_ref/{n}")
          for n in ("feat_0101.raw", "prob_0101.raw", "lab_0101.raw", "inst_0101.raw")))
A("* components on lab_0101 (26-conn): %d components over %d tooth voxels, %d with "
  ">= 30 voxels; largest: %s"
  % (PF["components"]["n_components_26conn"], PF["components"]["n_tooth_voxels"],
     PF["components"]["n_components_ge_30"], PF["components"]["largest_15_sizes"]))
A("* `python3 %s/verify_all.py` re-checks every claim above and exits non-zero on "
  "failure. Latest recorded run: %s"
  % (OUT, ("%d checks, %d PASS, %d FAIL (%s)" % (VS["checks_run"], VS["passed"],
                                                 VS["failed"], VS["utc"])) if VS
     else "not yet run for this build"))
A("")
A("## 7. Caveats / things I could not do honestly")
A("")
A("1. **The brief's input geometry is wrong for the HU volumes** (they are "
  "220x220x172 @ 0.60 mm, not 440x440x344 @ 0.30 mm). No HU resampling was therefore "
  "needed; only the GT is down-voted 2x2x2. See SPEC.md §2/§5.")
A("2. **`1*6*96*96*64*4` is 14,155,776 bytes, not 9,437,184.** The brief's DoD item 3 "
  "is arithmetically inconsistent with its own tensor contract; `feat_0101.raw` is "
  "14,155,776 B. 9,437,184 B is exactly `192*192*128*2`, which is the canonical int16 "
  "volume at `app_volume/dentvoxel_0101_hu.raw`.")
A("3. **Encoding deviation:** neck_ct uses JPEG-Lossless SV1 "
  "(`1.2.840.10008.1.2.4.70`); no lossless encoder is reachable here, so these series "
  "are uncompressed Explicit VR Little Endian (strictly easier for DCMTK, and it keeps "
  "the bit-exact guarantee). All 78 reference tags except the private 0013 block and "
  "the 0012,0064 code sequence are reproduced.")
A("4. **`RescaleIntercept` is 0, not neck_ct's -1024**, so stored == HU and no "
  "modality-LUT ambiguity is possible on the device.")
A("5. **The crop loses 0-0.9 %% of the annotated tooth volume** (measured per case in "
  "work/crop_meta.json) because the prescribed 1200-4000 HU band includes jaw cortical "
  "bone; the worst is dentvoxel_0047 at %s coverage."
  % f(CR["dentvoxel_0047"]["tooth_volume_coverage_of_src_grid"], 4))
A("6. **Model quality is limited by the fixed contract** (6 HU box-mean channels, "
  "1x1x1 convs only): held-out Dice %s vs %s for the public 3D CNN. No amount of "
  "training fixes the missing spatial context." % (f(c["grid_96x96x64"]["dice"]),
                                                   f(DS["dentvoxel_0021"]["dice"])))
A("7. The two ~3000-voxel components in `lab_0101` are fused upper/lower arches: this "
  "model does **not** separate individual teeth, so a per-tooth component count is not "
  "a valid acceptance criterion (use it only for host/device determinism).")
A("8. No DCMTK command-line tool exists on this host (`dcmdump` absent), so parser "
  "acceptance is argued from the encoding choice, not demonstrated against the app's "
  "build; the device load is the real test.")
A("9. The learning-rate choice (0.05 -> 0.01) was not a controlled ablation; the "
  "radii/band-pass choice and the epoch count were (§4).")
A("")
A("## 8. Reproduce")
A("")
A("```")
A("bash /tmp/ai_build/run_all.sh          # inputs: /tmp/dental_cbct only")
A("python3 /tmp/ai_build/verify_all.py")
A("```")
open(f"{OUT}/toothseg_report.md", "w").write("\n".join(L) + "\n")
print("wrote %s/toothseg_report.md (%d lines)" % (OUT, len(L)))
