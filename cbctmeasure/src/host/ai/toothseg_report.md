# CBCT tooth segmentation (host side) — build report

> **本报告描述的是旧 1x1x1 逐体素 MLP 实验**（`train_model.py` -> `metrics.json`）。随包交付的设备链路是 `train2.py` 的 3x3x3 Conv3d + 设备轴序，其数据见 `parity_ref/metrics_0101.json`、`parity_ref/METRICS.md` 与 `cbctmeasure/doc/` 下的真机测试报告。两份报告的 Dice 不可直接对比（阈值 0.49 与通道 argmax 是不同的后处理规则）。

Generated 2026-09-30T20:07:14Z by `python3 /Users/wangyao/androidproject/DcmtkDemo/cbctmeasure/src/host/ai/make_report.py` from `/Users/wangyao/androidproject/DcmtkDemo/cbctmeasure/src/host/ai/metrics.json` (wall clock of the training step: 102.2 s). Every number below was measured on this machine; nothing is estimated.

## 1. What was produced

| artefact | detail |
|---|---|
| app volumes | 4 DICOM series, `/Users/wangyao/androidproject/DcmtkDemo/cbctmeasure/src/host/ai/app_volume/<case>/000001.dcm..000128.dcm`, int16 signed HU, 192x192x128 @ 0.60 mm, Explicit VR LE |
| `dentvoxel_0021` | 128 files, 9.65 MB, round-trip `array_equal` = True, crop start [13, 22, 23] |
| `dentvoxel_0047` | 128 files, 9.65 MB, round-trip `array_equal` = True, crop start [9, 23, 27] |
| `dentvoxel_0074` | 128 files, 9.65 MB, round-trip `array_equal` = True, crop start [11, 16, 22] |
| `dentvoxel_0101` | 128 files, 9.65 MB, round-trip `array_equal` = True, crop start [13, 19, 24] |
| GT | `/Users/wangyao/androidproject/DcmtkDemo/cbctmeasure/src/host/ai/gt/<case>_tooth.{nii.gz,raw}` uint8 {0,1} 192x192x128 |
| model | `/Users/wangyao/androidproject/DcmtkDemo/cbctmeasure/src/host/ai/model/teeth_cnn.onnx`, 3851 B, opset 17, 818 params, nodes Conv/Relu/Softmax |
| metrics | `/Users/wangyao/androidproject/DcmtkDemo/cbctmeasure/src/host/ai/metrics.json` |
| parity | `/Users/wangyao/androidproject/DcmtkDemo/cbctmeasure/src/host/ai/parity_ref/` feat/prob/lab/inst raw + summary.txt |
| spec | `/Users/wangyao/androidproject/DcmtkDemo/cbctmeasure/src/host/ai/SPEC.md` (normative constants for the C++ port) |

## 2. Headline: held-out case dentvoxel_0101 (never used for training or tuning)

| resolution | Dice | F1 | precision | recall | TP | FP | FN | pred vox | GT vox |
|---|---|---|---|---|---|---|---|---|---|
| 96x96x64 model grid (1.2 mm) | **0.6671** | 0.6671 | 0.8228 | 0.5609 | 6781 | 1460 | 5308 | 8241 | 12089 |
| 192x192x128 after nearest x2 | **0.6549** | 0.6549 | 0.7871 | 0.5607 | 51894 | 14034 | 40657 | 65928 | 92551 |
| native 0.30 mm expert GT (x4) | **0.6510** | 0.6510 | 0.7712 | 0.5631 | 406753 | 120671 | 315541 | 527424 | 722294 |

* The 192-grid number is **lower** than the 96-grid number by construction: the GT used as target is the 2x2x2 majority vote of the 0.30 mm annotation, and scoring at 192x192x128 compares the nearest-upsampled prediction against the *finer* 0.60 mm mask. The ceiling for any 96-grid predictor there is the measured oracle 0.9132 (`cases.dentvoxel_0101.oracle_ceiling_gt_vs_gt_at_192`).
* Uncalibrated argmax (`prob[1] > 0.5`) on the same case: dice 0.6608, precision 0.5350, recall 0.8638, f1 0.6608, tp 10443, fp 9077, fn 1646. The exported model already contains the prior correction in `b3` (see §4).
* 0.30 mm detail: 0 GT tooth voxels lie outside the app crop for this case, so they are unrecoverable by the model (`vs_native_0p30mm_gt.gt_tooth_voxels_outside_app_crop`).

## 3. Independent accuracy references (same GT definition)

| reference | case | Dice | precision | recall |
|---|---|---|---|---|
| DentalSegmentator labels 3+4 vs expert tooth union, native 0.30 mm | dentvoxel_0021 | **0.9645** | 0.9437 | 0.9861 |
| DentalSegmentator labels 3+4 vs expert tooth union, native 0.30 mm | dentvoxel_0047 | **0.9709** | 0.9632 | 0.9787 |
| naive HU band 1200-4000 at 96 grid | dentvoxel_0021 | 0.3112 | 0.1857 | 0.9614 |
| naive HU band 1200-4000 at 96 grid | dentvoxel_0047 | 0.3472 | 0.2129 | 0.9398 |
| naive HU band 1200-4000 at 96 grid | dentvoxel_0074 | 0.2847 | 0.1672 | 0.9563 |
| naive HU band 1200-4000 at 96 grid | dentvoxel_0101 | 0.4069 | 0.2598 | 0.9376 |

Honest reading: **the public DentalSegmentator model reaches Dice ~0.9645/0.9709 on these cases; the per-voxel MLP I was allowed to build within the fixed 6-channel/1x1x1 contract reaches ~0.6671 on held-out data.** It is 1.6x better than an intensity threshold and ~1.4x worse than a real 3D CNN. Use it to validate the on-device *pipeline* (IO, features, ONNX, argmax, components) - not as a clinically meaningful tooth segmentor.

## 4. Model / training facts

* architecture `Conv(6->24,1x1x1)+Relu+Conv(24->24,1x1x1)+Relu+Conv(24->2,1x1x1)+Softmax` -> 818 params, 3851 B ONNX, opset 17, 6 nodes, ops Conv/Relu/Softmax
* contract: input `feat` [1, 6, 96, 96, 64] float32 -> output `prob` [1, 2, 96, 96, 64] float32, softmax over channel (measured max deviation of p[0]+p[1] from 1.0 over the whole 96^3 grid: 1.192e-07)
* Adam, lr=0.01, betas=(0.9,0.999), eps=1e-08
* minibatch 4096, epochs 110 (chosen: Dice at the Dice-optimal logit threshold on the full 96x96x64 grid of dentvoxel_0074)
* Adam iterations: phase1 3360 (search over 120 epochs) + phase2 4180 = 7540 total; wall clock 102.2 s for train+export+evaluate
* sampling: all positives + negative pool (TRAIN_STRIDE=1 -> every label-0 reduced-grid voxel) capped at NEG_PER_POS=4 x #positives via sorted(rng(TRAIN_SEED+case_index).permutation)[:k]; counts {"0021": 47920, "0047": 64000, "0074": 43065, "final_fit": 154985, "final_positives": 30997}
* prior calibration: `t* = 1.817192554473877` on `z1 - z0` maximises pooled train-case Dice (0.6946); folded in as `b3 ['0.028831', '-0.028831'] -> ['0.028831', '-1.846023']`, so the device does plain argmax. Train-pooled numbers at `t*`: dice 0.6946, precision 0.6991, recall 0.6901, f1 0.6946, tp 21391, fp 9205, fn 9606
* epoch count chosen in phase 1 = 110, scored as full-grid Dice on the monitor case dentvoxel_0074: 0.6541 (precision 0.6541, recall 0.6541; tp 5634 fp 2979 fn 2979). Curve: ep1 0.5947, ep5 0.6475, ep110 (best) 0.6541, ep120 0.6331 -> flat within +-0.02 after ~5 epochs, so the epoch choice is not a sharp optimum and 0101 was never consulted for it.
* train-split cases (fit, so optimistic): dentvoxel_0021 96-grid Dice 0.7144 / 192 0.6940, dentvoxel_0047 96-grid Dice 0.6883 / 192 0.6696, dentvoxel_0074 96-grid Dice 0.6823 / 192 0.6611
* feature design study (radii/band-pass pairs, identical phase-1 protocol), scored on 0074 only:
  * radii [1, 2, 4, 8] bp [3, 4] -> val Dice 0.6541 (best epoch 110)  <== shipped
  * radii [1, 2, 4, 16] bp [3, 4] -> val Dice 0.6395 (best epoch 20)
  * radii [1, 2, 4, 8] bp [1, 4] -> val Dice 0.6302 (best epoch 110)
  * radii [1, 2, 4, 16] bp [1, 4] -> val Dice 0.6165 (best epoch 70)
  * radii [1, 3, 6, 12] bp [1, 4] -> val Dice 0.6149 (best epoch 50)

## 5. Preprocessing constants the C++ port needs

```
crop      : band HU 1200..4000 inclusive; percentile 1/99 per axis (numpy linear
            interpolation), floor/ceil; margin (6,6,8) voxels; clamp to 220x220x172;
            center=(lo+hi)//2; start=clip(center-(192,192,64+64)//2, 0, N-FOV)
            measured starts: 021[13, 22, 23], 047[9, 23, 27], 074[11, 16, 22], 101[13, 19, 24]
HU -> n   : n = (clip(HU,-1024,4095) + 1024)/5120 - 0.5      (float32, [-0.5,0.5])
c0        : 2x2x2 mean of n, accumulate double, /8.0        (96x96x64, 1.2 mm/voxel)
c1..c4    : separable box means of c0, half-widths r = 1,2,4,8  (boxes 1.8/3.0/5.4/10.2 mm)
c5        : c3 - c4   (box r=4 minus box r=8)
boundary  : replicate (edge clamp) on every axis, every radius; running sums in double
index order: (x,y,z) = shape order, NIfTI affine ignored; NCHW offset = ((c*96+x)*96+y)*64+z
dicom     : slice file n = z index n-1; pixel row = axis x, column = axis y;
            Rows=Columns=192, PixelSpacing 0.6/0.6, SliceThickness 0.6,
            IOP [1,0,0,0,1,0], IPP [0,0,0.6*(n-1)], BitsAllocated 16 signed,
            RescaleIntercept 0 / Slope 1  => stored int16 IS the HU value
post      : lab = argmax_c prob; app mask = lab[x/2,y/2,z/2]; 26-connectivity CC
```

## 6. Verification (what actually ran)

* DICOM round trip, all 4 series: `np.array_equal(reassembled, canonical)` = True (dentvoxel_0021 9.65 MB, dentvoxel_0047 9.65 MB, dentvoxel_0074 9.65 MB, dentvoxel_0101 9.65 MB); raw PixelData of slice 0 == canonical[:,:,0] = True
* series size target <= 25 MB: True (max 9.65 MB)
* ORT-vs-fixture on `feat_0101.raw`: re-running ONNX Runtime on the shipped `model/teeth_cnn.onnx` reproduces `prob_0101.raw` with max abs diff 0.000e+00 (measured here, not copied); repeated ORT runs bit-exact = True; prob raw re-read bit-exact = True
* model file: 3851 B, sha256 `3092730fc2866074...` (measured here, /tmp/ai_build/model/teeth_cnn.onnx). `verify_all.py` asserts this sha against `parity_ref/device_parity.json` and against the shipped app asset `src/main/assets/models/teeth_cnn.onnx`; the 1x1x1 experiment this report describes is kept as `model/teeth_cnn_pointwise_old.onnx` and is asserted to be a *different* file.
* my numpy forward vs ONNX Runtime: max abs diff 1.5795230865478516e-06 (float64 vs float32 accumulation)
* features from the DICOM series == features from the NIfTI: not run (work/cache missing) (bit-exact); DICOM re-read == canonical array: True
* fixture sizes (measured with os.path.getsize): feat 14155776 B (=1*6*96*96*64*4), prob 4718592 B, lab 589824 B, inst 1179648 B
* components on lab_0101 (26-conn): 36 components over 9931 tooth voxels, 12 with >= 30 voxels; largest: [5159, 2575, 668, 417, 230, 178, 129, 129, 116, 66, 33, 32, 26, 25, 23]
* `python3 /Users/wangyao/androidproject/DcmtkDemo/cbctmeasure/src/host/ai/verify_all.py` re-checks every claim above and exits non-zero on failure. Latest recorded run: 56 checks, 56 PASS, 0 FAIL (2026-09-30T20:05:06Z)

## 7. Caveats / things I could not do honestly

1. **The brief's input geometry is wrong for the HU volumes** (they are 220x220x172 @ 0.60 mm, not 440x440x344 @ 0.30 mm). No HU resampling was therefore needed; only the GT is down-voted 2x2x2. See SPEC.md §2/§5.
2. **`1*6*96*96*64*4` is 14,155,776 bytes, not 9,437,184.** The brief's DoD item 3 is arithmetically inconsistent with its own tensor contract; `feat_0101.raw` is 14,155,776 B. 9,437,184 B is exactly `192*192*128*2`, which is the canonical int16 volume at `app_volume/dentvoxel_0101_hu.raw`.
3. **Encoding deviation:** neck_ct uses JPEG-Lossless SV1 (`1.2.840.10008.1.2.4.70`); no lossless encoder is reachable here, so these series are uncompressed Explicit VR Little Endian (strictly easier for DCMTK, and it keeps the bit-exact guarantee). All 78 reference tags except the private 0013 block and the 0012,0064 code sequence are reproduced.
4. **`RescaleIntercept` is 0, not neck_ct's -1024**, so stored == HU and no modality-LUT ambiguity is possible on the device.
5. **The crop loses 0-0.9 % of the annotated tooth volume** (measured per case in work/crop_meta.json) because the prescribed 1200-4000 HU band includes jaw cortical bone; the worst is dentvoxel_0047 at 0.9911 coverage.
6. **Model quality is limited by the fixed contract** (6 HU box-mean channels, 1x1x1 convs only): held-out Dice 0.6671 vs 0.9645 for the public 3D CNN. No amount of training fixes the missing spatial context.
7. The two ~3000-voxel components in `lab_0101` are fused upper/lower arches: this model does **not** separate individual teeth, so a per-tooth component count is not a valid acceptance criterion (use it only for host/device determinism).
8. No DCMTK command-line tool exists on this host (`dcmdump` absent), so parser acceptance is argued from the encoding choice, not demonstrated against the app's build; the device load is the real test.
9. The learning-rate choice (0.05 -> 0.01) was not a controlled ablation; the radii/band-pass choice and the epoch count were (§4).

## 8. Reproduce

```
bash /tmp/ai_build/run_all.sh          # inputs: /tmp/dental_cbct only
python3 /tmp/ai_build/verify_all.py
```
