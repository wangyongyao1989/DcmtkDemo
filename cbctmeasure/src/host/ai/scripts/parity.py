"""parity.py -- host-side authority for the Android C++ <-> Python parity fixtures.

AXIS CONVENTION (verified here, documented in SPEC.md):
  The Android CbctSeriesParser.cpp sets  vol->width = Columns,  vol->height = Rows
  and lays the volume out as memory [depth][height][width], i.e. indexed
      data[d][h][w] = pixel(row h, column w) of slice d,  d sorted by ascending
      ImagePositionPatient z.
  The model tensor, however, is written in (i, j, k) = (device axis0, axis1, z)
  with  i = DICOM COLUMN (x),  j = DICOM ROW (y),  k = z -- exactly the order
  AiCore::buildFeatures produces.  Host canonical arrays from prep.py are
  (x=Rows, y=Columns, z), hence:
      feat_dev[c][i][j][k] = feat_canon[c][j][i][k]   ->  transpose(0, 2, 1, 3)
      yred_dev[j][i][k] ...yred_canon[j][i][k]        ->  transpose(1, 0, 2)
  verify_axis_order() below re-reads the DICOM series the way the parser does
  and proves the transposed cached features equal the rebuilt ones (this is the
  authority; train2.py aborts if they disagree).

Deliverables written by main():
  parity_ref/feat_0101.raw    float32 LE C-contiguous (6,96,96,64) 14,155,776 B
  parity_ref/prob_0101.raw    float32 LE C-contiguous (2,96,96,64)  4,718,592 B
  parity_ref/label_0101.raw   uint8  (96,96,64)                      589,824 B
  parity_ref/gt_0101.raw      uint8  (96,96,64)                      589,824 B
  parity_ref/parity_0101.json manifest (shapes/dtypes/sizes/rules)
  parity_ref/METRICS.md       human-readable summary (real numbers only)

NOTE: the task brief quoted prob_0101.raw as 294,912 bytes; the arithmetic of
2*96*96*64 float32 is 4,718,592 bytes -- the manifest records the real size.
"""
import json
import os
import types
import numpy as np
import prep

OUT = prep.OUT_DIR
CACHE = f"{OUT}/work/cache"
APPV = f"{OUT}/app_volume"
MODEL_DIR = f"{OUT}/model"
PARITY_DIR = f"{OUT}/parity_ref"
TEST_CASE = "dentvoxel_0101"


# ------------------------------------------------- parser-faithful DICOM read
def read_dicom_series_as_parser(series_dir):
    """Rebuild the volume EXACTLY like CbctSeriesParser.cpp:
       * sort slices by ascending ImagePositionPatient z;
       * width = Columns, height = Rows;
       * PixelData interpreted as signed int16 little-endian
         (PixelRepresentation==1, RescaleIntercept==0, RescaleSlope==1);
       * memory layout [depth][height=row][width=col].
    Returns (width, height, data) with data.shape == (128, 192, 192), int16."""
    import pydicom
    files = sorted(f for f in os.listdir(series_dir) if f.endswith(".dcm"))
    assert len(files) == prep.DICOM_SLICES, len(files)
    items = []
    for f in files:
        ds = pydicom.dcmread(os.path.join(series_dir, f))
        assert int(ds.PixelRepresentation) == 1, "expected signed pixels"
        assert int(ds.RescaleIntercept) == 0 and float(ds.RescaleSlope) == 1.0
        h, w = int(ds.Rows), int(ds.Columns)
        buf = np.frombuffer(bytes(ds.PixelData), dtype="<i2")   # signed LE int16
        assert buf.size == h * w, (f, buf.size, h * w)
        z = float(ds.ImagePositionPatient[2])
        items.append((z, buf.reshape(h, w)))
    items.sort(key=lambda t: t[0])                              # IPP z ascending
    data = np.stack([a for _, a in items], axis=0)              # [depth][row][col]
    return w, h, np.ascontiguousarray(data, dtype=np.int16)


def dev_volume_to_canonical(data):
    """[depth=z][height=row][width=col] -> prep.py canonical (x=row, y=col, z)."""
    return np.ascontiguousarray(data.transpose(1, 2, 0), dtype=np.int16)


def build_features_dev(series_dir):
    """Parser-faithful device-order feature build:
    DICOM -> dev volume -> canonical -> prep.build_features -> transpose(0,2,1,3).
    Returns (feat_dev (6,96,96,64) f32, dev_data, diagnostics)."""
    w, h, data = read_dicom_series_as_parser(series_dir)
    canon = dev_volume_to_canonical(data)
    feat_canon = prep.build_features(canon)
    feat_dev = np.ascontiguousarray(feat_canon.transpose(0, 2, 1, 3), dtype=np.float32)
    diag = {"width": w, "height": h, "depth": int(data.shape[0]),
            "ipp_z_first": None, "n_slices": int(data.shape[0])}
    return feat_dev, data, diag


# --------------------------------------------------- axis-order verification
def verify_axis_order(cache_dir=CACHE, cids=("dentvoxel_0021", "dentvoxel_0101")):
    """Prove cached-canonical.transpose(0,2,1,3) == parser-reread device features
    and the reread volume is bit-identical to the _hu.raw mirror.  The DICOM
    re-read is the authority; any disagreement fails the check."""
    out = {"ok": True, "max_abs_diff_feat": 0.0, "cases": {}}
    for cid in cids:
        feat_dev, data, diag = build_features_dev(f"{APPV}/{cid}")
        cached_dev = np.ascontiguousarray(
            np.load(f"{cache_dir}/{cid}_feat.npy").transpose(0, 2, 1, 3))
        d = float(np.abs(feat_dev - cached_dev).max())
        raw = np.fromfile(f"{APPV}/{cid}_hu.raw", dtype="<i2").reshape(prep.APP_SHAPE)
        vol_exact = bool(np.array_equal(dev_volume_to_canonical(data), raw))
        out["cases"][cid] = {"feat_max_abs_diff": d, "volume_bitexact_to_hu_raw": vol_exact,
                             **diag}
        out["max_abs_diff_feat"] = max(out["max_abs_diff_feat"], d)
        out["ok"] = out["ok"] and d < 1e-6 and vol_exact
    return out


# ------------------------------------------------------- onnxruntime helpers
def ort_check(onnx_path, feat_dev, np_prob=None):
    """Load the exported model, check the interface contract and (optionally)
    compare ORT output with a numpy forward pass.  NOTE: the attribute is
    `opset_import`, not `opset`."""
    import onnx
    import onnxruntime as ort
    m = onnx.load(onnx_path)
    assert [i.name for i in m.graph.input] == ["feat"]
    assert [o.name for o in m.graph.output] == ["prob"]
    ishape = [d.dim_value for d in m.graph.input[0].type.tensor_type.shape.dim]
    oshape = [d.dim_value for d in m.graph.output[0].type.tensor_type.shape.dim]
    assert ishape == [1, 6, 96, 96, 64] and oshape == [1, 2, 96, 96, 64]
    ver = m.opset_import[0].version
    assert ver <= 17, ver
    sess = ort.InferenceSession(onnx_path, providers=["CPUExecutionProvider"])
    got = sess.run(None, {"feat": np.ascontiguousarray(feat_dev[None], np.float32)})[0]
    res = {"input_shape": ishape, "output_shape": oshape, "opset_import_version": int(ver),
           "opset": int(ver), "ort_ops": sorted({n.op_type for n in m.graph.node}),
           "n_nodes": len(m.graph.node), "onnx_bytes": os.path.getsize(onnx_path)}
    if np_prob is not None:
        res["ort_max_abs_diff_vs_numpy"] = float(np.abs(got[0] - np_prob).max())
    return res, got[0]


def numpy_forward_from_npz(npz_path, feat_dev):
    """Re-run the forward pass without retraining, using the saved weights."""
    import train2
    z = np.load(npz_path)
    net = object.__new__(train2.Net)      # no re-init; attach the saved tensors
    net.W1, net.b1 = z["W1"], z["b1"]
    net.W2, net.b2 = z["W2"], z["b2"]
    net.W3, net.b3 = z["W3"], z["b3"]
    return net.predict_proba(feat_dev)


# ------------------------------------------------------------------ fixtures
def main():
    os.makedirs(PARITY_DIR, exist_ok=True)
    cid = TEST_CASE
    print("[parity] building device-order features from the DICOM series (authority)")
    feat_dev, dev_data, diag = build_features_dev(f"{APPV}/{cid}")
    assert feat_dev.shape == (6, 96, 96, 64) and feat_dev.dtype == np.float32
    ac = verify_axis_order(cids=(cid,))
    print("[parity] axis check %s: feat max|diff| %.3e, volume bit-exact to _hu.raw: %s"
          % (cid, ac["max_abs_diff_feat"], ac["cases"][cid]["volume_bitexact_to_hu_raw"]))
    assert ac["ok"], ac

    z = np.load(f"{MODEL_DIR}/teeth_cnn_weights.npz")
    thresh = float(z["threshold"])
    np_prob = numpy_forward_from_npz(f"{MODEL_DIR}/teeth_cnn_weights.npz", feat_dev)
    onnx_path = f"{MODEL_DIR}/teeth_cnn.onnx"
    info, prob = ort_check(onnx_path, feat_dev, np_prob)
    d_ort_np = info["ort_max_abs_diff_vs_numpy"]
    print("[parity] opset %d, ORT-vs-numpy max|diff| %.3e" % (info["opset"], d_ort_np))
    assert d_ort_np < 1e-4

    prob = np.ascontiguousarray(prob, np.float32)              # (2,96,96,64) dev order
    label = np.ascontiguousarray((prob[1] > thresh).astype(np.uint8))
    gt_dev = np.ascontiguousarray(np.load(f"{CACHE}/{cid}_yred.npy").transpose(1, 0, 2)
                                  .astype(np.uint8))

    fp = f"{PARITY_DIR}/feat_0101.raw"
    prob_fp = f"{PARITY_DIR}/prob_0101.raw"
    lab_fp = f"{PARITY_DIR}/label_0101.raw"
    gt_fp = f"{PARITY_DIR}/gt_0101.raw"
    feat_dev.tofile(fp)
    prob.tofile(prob_fp)
    label.tofile(lab_fp)
    gt_dev.tofile(gt_fp)

    # metrics recap (computed by train2.py; only read here)
    mfile = f"{PARITY_DIR}/metrics_0101.json"
    recap = None
    if os.path.exists(mfile):
        mj = json.load(open(mfile))
        tm = mj["metrics"][cid]
        recap = {"chosen_threshold": mj["threshold_calibration"]["chosen_threshold"],
                 "dice_at_chosen": tm["at_chosen"]["dice"],
                 "precision_at_chosen": tm["at_chosen"]["precision"],
                 "recall_at_chosen": tm["at_chosen"]["recall"],
                 "f1_at_chosen": tm["at_chosen"]["f1"],
                 "f1_beta0p5_at_chosen": tm["f1_beta0p5_at_chosen"],
                 "iou_at_chosen": tm["iou_at_chosen"],
                 "dice_at_0p5": tm["at_0p5"]["dice"]}
        # cross-check: metrics were computed on the same dev-order arrays
        from train2 import dice_at
        s = dice_at(prob[1], gt_dev, thresh)
        assert abs(s["dice"] - tm["at_chosen"]["dice"]) < 1e-9, (s["dice"], tm)
        recap["fixture_label_dice_vs_gt"] = s["dice"]

    manifest = {
        "case": cid,
        "generated_utc": __import__("time").strftime("%Y-%m-%dT%H:%M:%SZ", __import__("time").gmtime()),
        "axis_convention": (
            "DEVICE order, verified against the Android CbctSeriesParser.cpp contract: "
            "the parser sets width=Columns, height=Rows and lays data as [depth][height][width] "
            "with slices sorted by ascending ImagePositionPatient z; PixelData is signed int16 LE "
            "(PixelRepresentation=1, RescaleIntercept=0, Slope=1). The model/label tensors are "
            "feat_dev[c][i][j][k] with i=device axis0=DICOM COLUMN (=host canonical axis1), "
            "j=device axis1=DICOM ROW (=host canonical axis0), k=z. Host canonical arrays are "
            "converted by transpose(0,2,1,3) (features) and transpose(1,0,2) (labels)."),
        "files": {
            "feat_0101.raw": {"dtype": "float32", "byteorder": "little", "order": "C-contiguous",
                              "shape": list(feat_dev.shape), "bytes": os.path.getsize(fp),
                              "content": "6-channel features rebuilt from the DICOM series exactly "
                                         "as AiCore::buildFeatures does (channel defs: SPEC.md)"},
            "prob_0101.raw": {"dtype": "float32", "byteorder": "little", "order": "C-contiguous",
                              "shape": list(prob.shape), "bytes": os.path.getsize(prob_fp),
                              "content": "onnxruntime output of teeth_cnn.onnx on feat_0101.raw"},
            "label_0101.raw": {"dtype": "uint8", "shape": list(label.shape),
                               "bytes": os.path.getsize(lab_fp),
                               "rule": "label[i,j,k] = 1 iff prob[1,i,j,k] > %.4f else 0 "
                                       "(plain calibrated-threshold argmax of the 2-channel softmax; "
                                       "NO connected-component filtering, NO min-size filter -- the "
                                       "C++ side must reproduce this exact rule)" % thresh},
            "gt_0101.raw": {"dtype": "uint8", "shape": list(gt_dev.shape),
                            "bytes": os.path.getsize(gt_fp),
                            "content": "expert GT voted to reduced grid (canonical transpose(1,0,2))"},
        },
        "threshold": thresh,
        "numpy_forward_from_weights_npz": f"{MODEL_DIR}/teeth_cnn_weights.npz",
        "ort_vs_numpy_max_abs_diff": d_ort_np,
        "onnx_opset_import_version": info["opset"],
        "axis_verification": ac,
        "metrics_recap": recap,
        "note_on_task_byte_count": ("task brief quoted prob_0101.raw as 294,912 B; the true "
                                    "size of (2,96,96,64) float32 is 4,718,592 B"),
    }
    json.dump(manifest, open(f"{PARITY_DIR}/parity_0101.json", "w"), indent=1)
    write_metrics_md(manifest, recap)
    print("[parity] wrote", ", ".join(os.path.basename(k) for k in
          [fp, prob_fp, lab_fp, gt_fp, f"{PARITY_DIR}/parity_0101.json",
           f"{PARITY_DIR}/METRICS.md"]))


def write_metrics_md(manifest, recap):
    mj = json.load(open(f"{PARITY_DIR}/metrics_0101.json"))
    tr = mj["training"]
    gc = mj["gradient_check"]
    te = mj["metrics"][manifest["case"]]
    va = mj["metrics"]["dentvoxel_0074"]
    tc = mj["threshold_calibration"]
    th = tc["chosen_threshold"]
    prd = mj["prd_ai01_target_f1_0p85"]
    cc = te["cc_26conn_min8_at_chosen"]
    ccg = te["cc_gt_reference_26conn_min8"]
    lines = []
    A = lines.append
    A("# teeth_cnn v2 -- 3x3x3 Conv3d in device axis order: metrics report\n")
    A("Generated by parity.py from real computed numbers (metrics_0101.json). UTC %s.\n"
      % mj["generated_utc"])
    A("## Dataset provenance\n")
    A("- 4 dental CBCT cases derived from the public **DentVoxel** dataset (expert tooth "
      "annotations), converted host-side to a canonical 192x192x128 @0.6 mm signed-int16 "
      "DICOM series (Explicit VR LE, RescaleIntercept=0, PixelRepresentation=1).")
    A("- Expert GT: native 0.30 mm multi-label masks (440,440,344), voted 2x2x2 to 0.6 mm "
      "(threshold 4/8), jaw-centred crop, then voted 2x2x2 again to the 96x96x64 model grid.")
    A("- **DentalSegmentator** published outputs on the same cases serve as an independent "
      "published-accuracy reference (its published mean Dice on DentVoxel is ~0.96+ at native "
      "resolution; our tiny 4-case CNN is not comparable to it -- see 'what this is NOT').")
    A("\n## Split\n")
    A("| split | case | used for |\n|---|---|---|"
      "\n| train | dentvoxel_0021, dentvoxel_0047 | patch training |"
      "\n| val   | dentvoxel_0074 (unseen) | early stopping on full-grid val Dice, threshold calibration |"
      "\n| test  | dentvoxel_0101 (unseen, ships as Android asset) | single final evaluation + parity fixtures |\n")
    A("\n## Architecture / model\n")
    A("- %s" % mj["architecture"])
    A("- input `feat` [1,6,96,96,64] f32, output `prob` [1,2,96,96,64] f32, Softmax axis=1, opset %d" % mj["onnx_check"]["opset"])
    A("- **DEVICE axis order**: axis0 = DICOM column, axis1 = DICOM row, axis2 = z "
      "(train/export in device order because 3x3x3 convs are not equivariant under the "
      "host<->device in-plane transpose; verified by DICOM re-read, feat max|diff| %.3e)."
      % mj["axis_verification_dicom_reread"]["max_abs_diff_feat"])
    A("- parameters: %d;  onnx file: %d bytes;  compute: ~2.04 GMAC full grid."
      % (mj["parameter_count"], mj["onnx_bytes"]))
    A("- training: %s; %d patches/epoch of 16^3 (50%% centers on positive voxels, per-patch "
      "pos:neg loss mass 1:4); %d Adam updates; epochs run %d, best epoch %d (early stop, patience %d); seed %d."
      % (tr["optimizer"], tr["patches_per_epoch"], tr["adam_updates"], tr["epochs_run"],
         tr["best_epoch"], tr["patience"], tr["seed"]))
    A("- **gradient check (pre-training gate)**: central finite differences, float64, max rel err %.3e -> %s (threshold 1e-4)."
      % (gc["max_rel_err"], "PASS" if gc["ok"] else "FAIL"))
    A("- numpy-vs-onnxruntime on 0101 device features: max|diff| = %.3e (< 1e-4 gate)."
      % mj["onnx_check"]["ort_max_abs_diff_vs_numpy"])
    A("\n## Threshold calibration (on VAL dentvoxel_0074, full grid)\n")
    A("- swept %s..%s step %s on prob[class 1]; chosen t* = **%.2f** (max Dice); F1-best t = %.2f."
      % (tc["swept_from_to_step"][0], tc["swept_from_to_step"][1], tc["swept_from_to_step"][2], th, tc["f1_best_threshold"]))
    A("- val 0074 @t*: Dice %.4f, precision %.4f, recall %.4f, F1 %.4f  (@0.5: Dice %.4f)"
      % (va["at_chosen"]["dice"], va["at_chosen"]["precision"], va["at_chosen"]["recall"],
         va["at_chosen"]["f1"], va["at_0p5"]["dice"]))
    curve = tc["val_curve"]
    A("\n| t | Dice | Precision | Recall |\n|---|---|---|---|")
    for c in curve[::5]:
        A("| %.2f | %.4f | %.4f | %.4f |" % (c["threshold"], c["dice"], c["precision"], c["recall"]))
    A("\n## TEST dentvoxel_0101 (unseen) vs PRD AI-01\n")
    A("| metric | @0.5 | @t*=%.2f | PRD target | met? |" % th)
    A("|---|---|---|---|---|")
    A("| Dice | %.4f | %.4f | - | - |" % (te["at_0p5"]["dice"], te["at_chosen"]["dice"]))
    A("| Precision | %.4f | %.4f | - | - |" % (te["at_0p5"]["precision"], te["at_chosen"]["precision"]))
    A("| Recall | %.4f | %.4f | - | - |" % (te["at_0p5"]["recall"], te["at_chosen"]["recall"]))
    A("| F1 | %.4f | %.4f | >= 0.85 | **%s** |" % (te["at_0p5"]["f1"], te["at_chosen"]["f1"],
      "MET" if prd["met"] else "MISSED"))
    A("| F1 (beta=0.5) | - | %.4f | - | - |" % te["f1_beta0p5_at_chosen"])
    A("| IoU | - | %.4f | - | - |" % te["iou_at_chosen"])
    A("\nPer-instance (26-connectivity, drop components < 8 voxels; reduced voxel edge 1.2 mm, "
      "volume %.3f mm3 = %.6f cm3):" % (cc["voxel_volume_mm3"], cc["voxel_volume_mm3"] / 1000.0))
    A("- predicted: %d raw components -> %d kept; largest %.4f cm3, smallest kept %.4f cm3; "
      "%d in plausible single-tooth range 0.15-2.5 cm3 (voxel 1.2 mm = 1.728 mm3 = 0.001728 cm3)."
      % (cc["n_components_raw"], cc["n_components_filtered"], cc["largest_cm3"],
         cc["smallest_cm3"], cc["n_plausible_0p15_2p5_cm3"]))
    A("- GT reference: %d raw -> %d kept, largest %.4f cm3." %
      (ccg["n_components_raw"], ccg["n_components_filtered"], ccg["largest_cm3"]))
    A("\n**PRD AI-01 F1 >= 0.85: %s** (F1 = %.4f at the calibrated threshold on the unseen test case)."
      % ("MET" if prd["met"] else "MISSED", prd["f1_at_chosen_on_test"]))
    A("\n## Parity fixtures (parity_ref/)\n")
    for name, info in manifest["files"].items():
        A("- `%s`: %s, shape %s, %d bytes" % (name, info["dtype"],
          info.get("shape"), info["bytes"]))
    A("- label rule: %s" % manifest["files"]["label_0101.raw"]["rule"])
    A("- ORT-vs-numpy: max|diff| %.3e; feat rebuild-vs-cache: %.3e"
      % (manifest["ort_vs_numpy_max_abs_diff"], manifest["axis_verification"]["max_abs_diff_feat"]))
    A("\n## What this is NOT\n")
    A("- It is NOT nnU-Net and not any published architecture: a hand-written numpy "
      "3-layer Conv3d (6->8->8->2) trained on **4 cases** with no augmentation. "
      "nnU-Net v2.2 Dataset112 weights exist under CC-BY-4.0 but cannot be exported on this "
      "host: torch is not installable here, tf2onnx installs are blocked, and the Zenodo "
      "download runs at ~20 KB/s. Do not compare these numbers to nnU-Net/DentalSegmentator "
      "leaderboard claims.")
    A("- Validation metrics are FULL-grid and exact (96x96x64), never subsampled.")
    A("- The exported model contains only Conv/Relu/Softmax initializers (opset %d) so it "
      "loads under ONNX Runtime 1.17 on the tablet (ORT_API_VERSION 17)." % mj["onnx_check"]["opset"])
    open(f"{PARITY_DIR}/METRICS.md", "w").write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
