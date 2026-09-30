# CBCT tooth segmentation — host-side pipeline contract for the C++/Android side (v2)

Author of record: QoderCN agent (host-side ML pipeline), 2026-09-30.
This file is the **normative** contract. Everything here was executed on this
machine; every number comes from real runs (no estimates, no placeholders
except values explicitly pointing to `parity_ref/metrics_0101.json`).
If this file and the code disagree, the code is right.

**v2 change (supersedes the v1 model sections):** the shipped model is now a REAL
3x3x3 Conv3d network **trained and exported in DEVICE AXIS ORDER**. The v1 model
(three 1x1x1 pointwise convs; VAL Dice 0.50, precision 0.361, recall 0.815 — too
weak) is archived as `model/teeth_cnn_pointwise_old.onnx`. Sections 1–6 below
(geometry, crop, GT vote, DICOM) are unchanged from v1 and remain binding.

Executable sources of truth:
* `/tmp/ai_build/prep.py` — constants, crop, GT vote, `build_features`, DICOM tags
* `/tmp/ai_build/train2.py` — trainer (device order, gradient check, calibration)
* `/tmp/ai_build/export2.py` — ONNX exporter (opset 17, initializer weights)
* `/tmp/ai_build/parity.py` — DICOM-parser-faithful re-read + fixture generation
* `/tmp/ai_build/parity_ref/metrics_0101.json` — all metrics (machine-readable)
* `/tmp/ai_build/parity_ref/METRICS.md` — all metrics (human-readable)

---

## 1. Geometry & spacing (binding)

| constant | value | notes |
|---|---|---|
| SRC_SPACING_MM | 0.60 | app volume isotropic mm/voxel |
| APP_SHAPE | (192, 192, 128) | canonical host grid, signed int16 HU |
| RED_FACTOR | 2 | 2x2x2 box-average decimation app → model grid |
| RED_SHAPE | (96, 96, 64) | model grid |
| **reduced voxel edge** | **1.2 mm** = 0.60 mm x RED_FACTOR | ALL volume statistics use this |
| **reduced voxel volume** | **1.728 mm³ = 0.001728 cm³** | per-instance volume = voxel_count x 1.728 mm³ |
| GT native | (440, 440, 344) @ 0.30 mm | expert labels; 2x2x2 vote (>=4/8) → 0.6 mm grid, crop, 2x2x2 vote (>=4/8) → model grid |

HU facts measured: DentVoxel *volumes* are 220x220x172 @0.6 mm (NOT 440³/0.3 mm —
only the label files are); the app volume is therefore a pure integer crop with no
interpolation. GT vote threshold 4/8 was chosen on train cases only (§5.3 of the
v1 spec reasoning still holds).

## 2. Axis convention (CRITICAL — the v1 spec's "single index order" is superseded for the MODEL TENSOR)

Host canonical arrays (`prep.py`, NIfTI index order): `canon[x][y][z]`,
x = axis0, y = axis1. The canonical→DICOM mapping (as written by
`prep.write_dicom_series` and verified bit-exact in v1): **pixel row index =
canonical axis 0 (x); pixel column index = canonical axis 1 (y); slice = z**.

The Android `CbctSeriesParser.cpp` sets `vol->width = Columns`,
`vol->height = Rows` and lays memory out as `[depth][height][width]`, slices
sorted by ascending `ImagePositionPatient` z. The model tensor used on device
(`AiCore::buildFeatures` output) is therefore:

```
feat_dev[c][i][j][k]:   i = device axis0 = DICOM COLUMN  = host canonical axis 1
                        j = device axis1 = DICOM ROW     = host canonical axis 0
                        k = z (unchanged)
```

Host→device conversion: `feat_dev = feat_canon.transpose(0, 2, 1, 3)`;
labels: `y_dev = y_canon.transpose(1, 0, 2)`.

**Why the model is trained in device order:** a 3x3x3 conv with learned
(non-symmetric) kernels is NOT equivariant under an in-plane transpose:
`conv(x^T) != conv(x)^T`. Training in one order and feeding the other silently
corrupts the model — this is exactly the class of bug that capped v1 quality
(v1 used 1x1x1 kernels where the swap is harmless). v2 therefore trains,
validates, calibrates, exports and generates fixtures ENTIRELY in device order.

**Verification (authority = the DICOM re-read, not the cache):**
`parity.verify_axis_order()` re-reads `app_volume/<case>/000001..000128.dcm`
exactly like the parser (IPP-z sort, signed int16 LE from the raw PixelData
buffer, `[z][row][col]`), builds features via `prep.build_features` and
transposes; result equals `cached_feat.transpose(0,2,1,3)` with
**max |diff| = 0.000e+00** for dentvoxel_0021 and dentvoxel_0101, and the
re-read volume is **bit-identical** to `app_volume/<case>_hu.raw`. train2.py
refuses to train if this check ever disagrees. Recorded in
`parity_ref/metrics_0101.json → axis_verification_dicom_reread`.

## 3. DICOM parsing rules (must match; unchanged from v1)

* `000001.dcm … 000128.dcm`, Explicit VR Little Endian `1.2.840.10008.1.2.1`,
  CT Image Storage; 75,396 B/file, 9,650,862 B/series.
* sort slices by **ImagePositionPatient z ascending** (slice n IPP z = 0.6*(n-1)).
* `width = Columns` (192), `height = Rows` (192), depth = 128.
* PixelData = **signed int16 little-endian**: PixelRepresentation 1, BitsAllocated
  16, BitsStored 16, HighBit 15; RescaleIntercept 0, RescaleSlope 1 ⇒ stored
  value IS HU (identity modality LUT — do not double-apply any offset).
* Rows == Columns == 192, so the (col,row) convention is NOT detectable from
  shapes — section 2 is the definition.

## 4. Crop / GT (unchanged from v1, binding)

Jaw-centred deterministic crop (HU band 1200–4000, 1st/99th percentile bbox,
margins (6,6,8), clamp, center = (lo+hi)//2, start = center - fov//2 clamped);
exact starts: 0021 (13,22,23), 0047 (9,23,27), 0074 (11,16,22), 0101 (13,19,24).
GT: native 0.30 mm tooth-union → 2x2x2 majority vote >= 4/8 → crop → 2x2x2
majority vote >= 4/8 → reduced-grid training target. Positive counts on the
reduced grid: 0021 9584 / 0047 12800 / 0074 8613 / 0101 12089 voxels
(~1.6–2.2 % of 589,824).

## 5. Feature channels — words AND pseudocode (unchanged; only the tensor axis order changed)

All on the reduced 96x96x64 grid. In words:

* **c0**: HU clipped to [-1024, 4095], affinely mapped by (h+1024)/5120 − 0.5 to
  [-0.5, +0.5], then 2x2x2 box-AVERAGE decimation from the app grid.
* **c1..c4**: box MEAN of c0 with half-width radii 1, 2, 4, 8 reduced voxels
  (box width 2r+1 ⇒ physical 1.8/3.0/5.4/10.2 mm), boundary = replicate (clamp
  index to [0, n-1]) on EVERY axis for EVERY radius.
* **c5**: band-pass c5 = box(c0, r=4) − box(c0, r=8).

Pseudocode (matches `prep.build_features` exactly):

```
n[x,y,z]  = (clip(V_int16[x,y,z], -1024, 4095) + 1024.0) / 5120.0 - 0.5   # float32
c0[X,Y,Z] = (sum over a,b,c in {0,1} of n[2X+a, 2Y+b, 2Z+c]) / 8          # float64 accum -> f32
box(a, r):  for axis in (0,1,2):  pad r edge copies; prefix-sum float64;
            out[i] = (P[i+2r+1] - P[i]) / (2r+1)
c1=box(c0,1); c2=box(c0,2); c3=box(c0,4); c4=box(c0,8); c5=c3-c4
feat_canon = stack([c0..c5])                    # (6,96,96,64) float32 C-contiguous
feat_dev   = feat_canon.transpose(0,2,1,3)      # THE MODEL INPUT (device order, section 2)
```

On device the same quantities are built from `data[d][h][w]` with the roles of
the two in-plane axes assigned per section 2 (the box ops are per-axis and
order-preserving under the documented mapping).

## 6. Model contract (v2)

| item | value |
|---|---|
| file | `/tmp/ai_build/model/teeth_cnn.onnx` (overwrites v1; v1 archived as `teeth_cnn_pointwise_old.onnx`) |
| input | name `feat`, float32, shape **[1, 6, 96, 96, 64]** — DEVICE axis order |
| output | name `prob`, float32, shape **[1, 2, 96, 96, 64]**; channel 0 = background, channel 1 = tooth |
| graph | Conv3d(6→8, kernel 3x3x3, pads [1,1,1,1,1,1], strides 1) → Relu → Conv3d(8→8, k3, pad1) → Relu → Conv3d(8→2, k3, pad1) → Softmax(**axis=1**) |
| padding semantics | ONNX Conv zero padding, correlation, weights stored [Cout,Cin,3,3,3] directly (numpy conv uses the identical orientation — verified against onnxruntime) |
| weights | initializers W1,b1,W2,b2,W3,b3 only; no training-only ops |
| opset | `opset_import[0].version` = **17** (Android ORT 1.17 ⇒ ORT_API_VERSION 17; opset 18 fails to load). NOTE for scripts: the protobuf attribute is `opset_import`, NOT `opset`. |
| parameters | **3,474** (1296+8 + 1728+8 + 432+2) |
| compute | 2.04 GMAC full-grid (conv1 1.30 + conv2 1.73 + conv3 0.43 GMAC-equivalent: 3456 MAC/voxel) |
| file size / verification | **14,477 bytes**; `opset_import[0].version` = 17, ir_version 8, 6 nodes, ops = Conv, Relu, Softmax; numpy-vs-onnxruntime max \|diff\| on 0101 device features = **1.252e-06** (< 1e-4 gate); ORT re-run on the shipped fixture reproduced it bit-exactly (0.0) |
| weights mirror | `/tmp/ai_build/model/teeth_cnn_weights.npz`: W1,b1,W2,b2,W3,b3 (float32, ONNX orientation), `threshold` (float32 calibrated T*), `arch=[6,8,8,2,3]` |

## 7. Training recipe (v2, numpy only — torch/TF are not available on this host and their use is deliberately excluded)

* Split: **train 0021+0047; val 0074; test 0101**. 0101 GT untouched until the
  single final evaluation; 0074 used ONLY for full-grid early stopping and
  threshold calibration.
* 16³ patches, 512 per epoch, **~50 % of patch centers on positive voxels**
  (`POS_CENTER_PROB=0.5`), rest uniform; seed 20260101, per-epoch rng
  `default_rng(seed+epoch)`.
* Loss: softmax cross-entropy, per-patch class weights giving positive:negative
  loss mass exactly **1:4** (NEG_PER_POS=4 style balancing: positives weight 1,
  negatives weight 4·npos/nnege). L2 weight decay 1e-4 added to gradients.
* Adam lr 0.02, betas (0.9, 0.999), eps 1e-8, gradient accumulation over 8
  patches (64 updates/epoch), float32 params/grads, float64 moments.
* **MANDATORY gradient check** before any training: central finite differences in
  float64 on a 12³ patch (eps 1e-4), 3 random entries of every parameter tensor;
  gate max rel err < 1e-4; training aborts otherwise. Result of this run:
  recorded in `metrics_0101.json → gradient_check` (max rel err **8.48e-06**, PASS).
* Early stopping: FULL-GRID inference on all 96×96×64 val voxels every epoch
  (exact — never subsampled), Dice at 0.5, patience 20, best checkpoint restored.

## 8. Decision threshold & post-processing (binding for device + fixtures)

* **Decision rule: `tooth iff prob[1,i,j,k] > T*`**, T* calibrated by sweeping
  0.05..0.95 step 0.01 on the FULL val grid of 0074 and taking the Dice argmax.
  The numeric T* for the shipped model is stored in
  `model/teeth_cnn_weights.npz["threshold"]` and
  `parity_ref/metrics_0101.json → threshold_calibration.chosen_threshold`
  (also in `parity_0101.json` and METRICS.md). **0.5 is not hardcoded anywhere**
  in the final numbers; both @0.5 and @T* are reported.
* Post-processing for instance counting (display/metrics):
  1. thresholded binary label map — plain per-voxel rule above (equivalent to
     calibrated-threshold argmax of the 2-channel softmax);
  2. 26-connectivity connected components (`structure = ones(3,3,3)`;
     scipy.ndimage.label reference; first-pass scan-order labels);
  3. drop components with **< 8 voxels** (noise filter);
  4. renumber remaining components by **descending voxel count** (largest → 1).
* Per-instance statistics: volume = voxel_count × **1.728 mm³**; largest/smallest
  kept volume; count of components within the plausible single-tooth range
  **0.15–2.5 cm³**; components < 8 voxels are excluded from all of these.

## 9. Parity fixtures (`/tmp/ai_build/parity_ref/`, case dentvoxel_0101 — the shipped Android asset)

All raw binary, little-endian, C-contiguous, no header. Built by `parity.py`
from the **DICOM series re-read** (section 2 authority), NOT from the cache.
Stale v1 fixtures are moved to `parity_ref/archive_v1_pointwise/` (canonical
axis order + old model — do not compare against those).

| file | dtype | shape (device order) | bytes |
|---|---|---|---|
| `feat_0101.raw` | float32 | (6, 96, 96, 64) | **14,155,776** |
| `prob_0101.raw` | float32 | (2, 96, 96, 64) | **4,718,592** |
| `label_0101.raw` | uint8 | (96, 96, 64) | **589,824** |
| `gt_0101.raw` | uint8 | (96, 96, 64) | **589,824** |
| `parity_0101.json` | manifest | — | — |
| `metrics_0101.json` | full metrics | — | — |
| `METRICS.md` | human summary | — | — |

Flatten order (C): `offset = ((c*96 + i)*96 + j)*64 + k` with i=column, j=row,
k=z. Byte-size note: the v2 task brief quoted `prob_0101.raw` as 73,728 floats /
294,912 B — arithmetically wrong; 2·96·96·64 = 1,179,648 floats = 4,718,592 B,
which is what the files and manifest contain (consistent with the mandatory
[1,2,96,96,64] float32 tensor contract).

**`label_0101.raw` generation rule (reproduce bit-exactly in C++):**
`label[i,j,k] = uint8( prob[1,i,j,k] > T* ? 1 : 0 )` — plain thresholded argmax
of the exported `prob`; **NO** connected-component or min-size filtering is
applied to this file (the C++ comparison is argmax evidence only). CC filtering
is a display-stage rule (section 8) and deliberately NOT baked into fixtures.

## 10. Parity procedure (Android ↔ host)

1. Parse `assets/.../dentvoxel_0101/` with CbctSeriesParser (section 3).
2. `AiCore::buildFeatures` → float32 buffer of 3,538,944 values in the
   section-2 layout; compare vs `feat_0101.raw`: expect bitwise/near-bit equality
   (max |Δ| < 1e-6; float-accumulation order only).
3. ORT session run of `model/teeth_cnn.onnx` → `prob_dev` (2,96,96,64);
   compare with **max |prob_dev − prob_0101| < 1e-4** (PASS gate; host-side
   onnxruntime-vs-numpy was measured far below this — see
   `metrics_0101.json → onnx_check.ort_max_abs_diff_vs_numpy`).
4. Apply T* → `label_dev`; compare vs `label_0101.raw` with **agreement > 99.9 %**
   (PASS gate). Ties/edge flips at exactly T* are expected at the 1e-4 float level.
5. Optional end-to-end: run section-8 post-processing on `label_dev` and compare
   component census against `metrics_0101.json → metrics.dentvoxel_0101.cc_26conn_min8_at_chosen`.

## 11. Metrics & PRD verdict (measured, this run)

Machine-readable: `parity_ref/metrics_0101.json`; human summary:
`parity_ref/METRICS.md`. Chosen threshold **T* = 0.49** (Dice argmax on the FULL
val grid of 0074; stored in the weights npz as float32 0.4900000095367432 — the
manifest records it at full precision).

Headline at T* on the **unseen test case dentvoxel_0101** (full 96×96×64 grid,
device order, vs the voted expert mask):

| metric | value |
|---|---|
| Dice | 0.5828 |
| Precision | 0.6452 |
| Recall | 0.5315 |
| F1 | **0.5828** |
| F1 (β=0.5) | 0.6187 |
| IoU | 0.4113 |
| TP / FP / FN | 6425 / 3533 / 5664 |
| CC (26-conn, min 8 vox) | 34 raw → 24 kept; largest 8.51 cm³; 5 in 0.15–2.5 cm³ |
| @0.5 for reference | Dice 0.5815, prec 0.6484, rec 0.5270, F1 0.5815 |
| val 0074 @T* | Dice 0.6225 (prec 0.5409, rec 0.7330) — threshold selected here, not on test |
| train cases @T* | 0021 0.6778, 0047 0.6083 (optimistic, fitted) |

**PRD AI-01 target F1 ≥ 0.85: MISSED** (0.5828 on the unseen test case). Plain
statement: with 2 real training cases, 3,474 parameters and no augmentation this
small CNN reaches ~58–62 % Dice/F1; the previous pointwise model reached
0.50 val Dice at 0.36 precision — v2 is substantially better (val Dice 0.6225,
precision 0.54) but nowhere near 0.85. Host ORT full-grid inference: 0.14 s
(macOS x86_64), so tablet runtime is comfortably in the seconds range.


## 12. Reproduce

```
cd /tmp/ai_build
python3 parity.py && python3 train2.py   # train2 re-verifies axes + gradients first, then trains/exports
python3 parity.py                        # regenerate fixtures + METRICS.md from the final model
```
`train2.py` writes `parity_ref/metrics_0101.json`, `model/teeth_cnn.onnx`,
`model/teeth_cnn_weights.npz`; `parity.py` writes the four raw fixtures,
`parity_0101.json`, `METRICS.md`. Inputs read: `/tmp/ai_build/app_volume/*`,
`/tmp/ai_build/gt/*`, `/tmp/ai_build/work/cache/*` only. Nothing inside the
Android repo was read or written by the v2 step.
