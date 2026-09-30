"""verify_all.py -- definition-of-done checks.  Prints PASS/FAIL per check and
exits non-zero if anything fails.  Nothing here is asserted in the report unless it
actually ran: run `python3 verify_all.py` to reproduce.
"""
import json
import os
import sys
import time
import numpy as np
import prep

OUT = prep.OUT_DIR
FAIL = []
NCHK = [0]


def chk(name, ok, detail=""):
    NCHK[0] += 1
    print("%-4s %s%s" % ("PASS" if ok else "FAIL", name, ("  |  " + detail) if detail else ""))
    if not ok:
        FAIL.append(name)
    return ok


def main():
    # ---------- 1. ONNX contract
    import onnx
    import onnxruntime as ort
    p = f"{OUT}/model/teeth_cnn.onnx"
    m = onnx.load(p)
    try:
        onnx.checker.check_model(m)
        chk("onnx.checker.check_model", True, p)
    except Exception as e:
        chk("onnx.checker.check_model", False, str(e))
    ins, outs = m.graph.input, m.graph.output
    chk("exactly one input / one output", len(ins) == 1 and len(outs) == 1,
        "%d in / %d out" % (len(ins), len(outs)))
    din = [d.dim_value for d in ins[0].type.tensor_type.shape.dim]
    dout = [d.dim_value for d in outs[0].type.tensor_type.shape.dim]
    chk("input named 'feat'", ins[0].name == "feat", ins[0].name)
    chk("input dims [1,6,96,96,64]", din == [1, 6, 96, 96, 64], str(din))
    chk("input dtype float32",
        ins[0].type.tensor_type.elem_type == onnx.TensorProto.FLOAT)
    chk("output named 'prob'", outs[0].name == "prob", outs[0].name)
    chk("output dims [1,2,96,96,64]", dout == [1, 2, 96, 96, 64], str(dout))
    chk("output dtype float32",
        outs[0].type.tensor_type.elem_type == onnx.TensorProto.FLOAT)
    chk("opset <= 17", m.opset_import[0].version <= 17, "opset %d" % m.opset_import[0].version)
    ops = sorted({n.op_type for n in m.graph.node})
    chk("ops are trivially CPU-supported", set(ops) <= {"Conv", "Relu", "Softmax", "Add",
                                                        "Reshape", "Concat"}, str(ops))
    nparam = sum(int(np.prod(d)) for init in m.graph.initializer
                 for d in [init.dims])
    chk("weight payload small", nparam < 5000, "%d scalars, file %d B"
        % (nparam, os.path.getsize(p)))
    # kernel shapes must be 1x1x1 (contract: only Conv(1x1x1)) -- catches a foreign
    # 3x3x3 model being dropped onto the same path by another process.
    kshapes = sorted({tuple(attr.i for attr in n.attribute if attr.name == "kernel_shape")
                      for n in m.graph.node if n.op_type == "Conv"})
    chk("every Conv has kernel_shape [1,1,1]", kshapes == [(1, 1, 1)], str(kshapes))
    import hashlib
    sha = hashlib.sha256(open(p, "rb").read()).hexdigest()
    M0 = json.load(open(f"{OUT}/metrics.json"))
    want = M0["model"].get("onnx_sha256")
    chk("model/teeth_cnn.onnx bytes == the exported model recorded in metrics.json",
        want is not None and sha == want,
        "sha256 %s%s" % (sha[:16], (" expected " + want[:16]) if want else " (no recorded sha)"))
    bkp = f"{OUT}/model/teeth_cnn_pointwise_mlp.onnx"
    chk("backup copy model/teeth_cnn_pointwise_mlp.onnx present and identical",
        os.path.exists(bkp) and hashlib.sha256(open(bkp, "rb").read()).hexdigest() == sha)

    # ---------- 2. parity fixture: ORT on feat_0101.raw == prob_0101.raw
    fb = open(f"{OUT}/parity_ref/feat_0101.raw", "rb").read()
    pb = open(f"{OUT}/parity_ref/prob_0101.raw", "rb").read()
    sess = ort.InferenceSession(p, providers=["CPUExecutionProvider"])
    got = sess.run(None, {sess.get_inputs()[0].name:
                          np.frombuffer(fb, np.float32).reshape(1, 6, 96, 96, 64)})[0]
    ref = np.frombuffer(pb, np.float32).reshape(1, 2, 96, 96, 64)
    d = float(np.abs(got - ref).max())
    chk("ORT(feat_0101.raw) == prob_0101.raw to 1e-6", d <= 1e-6, "max abs diff %.3e" % d)
    chk("prob sums to 1 over channel axis (softmax)",
        float(np.abs(got.sum(axis=1) - 1.0).max()) < 1e-5,
        "max dev %.3e" % float(np.abs(got.sum(axis=1) - 1.0).max()))

    # lab = argmax(prob) must reproduce lab_0101.raw
    lab = np.frombuffer(open(f"{OUT}/parity_ref/lab_0101.raw", "rb").read(),
                        np.uint8).reshape(96, 96, 64)
    chk("lab_0101.raw == argmax(prob_0101.raw)",
        np.array_equal(lab, got[0].argmax(axis=0).astype(np.uint8)))

    # connected-component census recomputed
    from scipy import ndimage
    inst, nc = ndimage.label(lab, structure=np.ones((3, 3, 3), np.uint8))
    sizes = np.bincount(inst.ravel().astype(np.int64))[1:]
    saved = np.frombuffer(open(f"{OUT}/parity_ref/inst_0101.raw", "rb").read(),
                          np.uint16).reshape(96, 96, 64)
    chk("inst_0101.raw == recomputed 26-CC labels", np.array_equal(saved, inst.astype(np.uint16)),
        "n_components %d, >=30: %d" % (nc, int((sizes >= 30).sum())))

    # ---------- 3. fixture byte sizes
    chk("feat_0101.raw size == 1*6*96*96*64*4", len(fb) == 14_155_776, "%d B" % len(fb))
    chk("prob_0101.raw size == 2*96*96*64*4", len(pb) == 4_718_592, "%d B" % len(pb))
    chk("lab_0101.raw size == 96*96*64", lab.size == 589_824)
    chk("inst_0101.raw size == 96*96*64*2", saved.size * 2 == 1_179_648)
    sz_hu = os.path.getsize(f"{OUT}/app_volume/dentvoxel_0101_hu.raw")
    chk("canonical int16 volume raw == 9,437,184 B (the number quoted in the brief)",
        sz_hu == 9_437_184, "%d B = 192*192*128*2" % sz_hu)

    # ---------- 4. DICOM series round-trip, all four cases
    rt = json.load(open(f"{OUT}/work/dicom_roundtrip.json"))
    for cid in prep.ALL_CASES:
        v = prep.read_dicom_series(f"{OUT}/app_volume/{cid}")
        canon, _ = prep.make_app_volume(cid)
        chk("DICOM series round-trip bit-exact: %s" % cid, np.array_equal(v, canon),
            "%d files, %.2f MB, TS %s" % (rt[cid]["n_files"], rt[cid]["total_mb"],
                                          rt[cid]["transfer_syntax"]))
        chk("series <= 25 MB: %s" % cid, rt[cid]["total_bytes"] <= 25_000_000,
            "%.2f MB" % rt[cid]["total_mb"])

    # ---------- 5. GT artefacts
    for cid in prep.ALL_CASES:
        ok = (os.path.exists(f"{OUT}/gt/{cid}_tooth.nii.gz") and
              os.path.getsize(f"{OUT}/gt/{cid}_tooth.raw") == 192 * 192 * 128)
        chk("GT tooth mask files present: %s" % cid, ok)
    import nibabel as nib
    hold = prep.CASE_HELD_OUT
    g = np.asanyarray(nib.load(f"{OUT}/gt/{hold}_tooth.nii.gz").dataobj)
    chk("GT nii.gz shape/dtype/values", g.shape == (192, 192, 128) and g.dtype == np.uint8
        and set(np.unique(g).tolist()) <= {0, 1}, "%s %s %s" % (g.shape, g.dtype, np.unique(g)))
    chk("GT nii.gz == GT raw == in-process GT",
        np.array_equal(g, prep.gt_tooth_at_app_grid(hold)) and
        np.array_equal(np.fromfile(f"{OUT}/gt/{hold}_tooth.raw", np.uint8)
                       .reshape(192, 192, 128), g))

    # ---------- 6. features reproducible from the canonical volume
    canon_ho, _ = prep.make_app_volume(prep.CASE_HELD_OUT)
    f2 = prep.build_features(canon_ho)
    chk("build_features(canonical volume) bit-exact == feat_0101.raw",
        np.array_equal(f2, np.frombuffer(fb, np.float32).reshape(6, 96, 96, 64)))

    # ---------- 7. metrics.json
    M = json.load(open(f"{OUT}/metrics.json"))
    h = M["held_out"]
    chk("metrics.json holds the held-out case measured", h == "dentvoxel_0101" and
        h in M["cases"] and M["cases"][h]["split"] == "held_out")
    for cid in prep.CASES_TRAIN:
        chk("metrics.json marks %s as train split" % cid,
            M["cases"][cid]["split"] == "train")
    chk("metrics.json has the DentalSegmentator reference",
        "dentalsegmentator_labels_3_plus_4_vs_gt_tooth_union_native_0p30mm" in M["reference"])

    c = M["cases"][h]
    print("\nHEADLINE (held-out %s):" % h)
    for k in ("grid_96x96x64", "grid_192x192x128_nearest", "vs_native_0p30mm_gt"):
        s = c[k]
        print("  %-26s dice %.4f  precision %.4f  recall %.4f  f1 %.4f  tp %d fp %d fn %d"
              % (k, s["dice"], s["precision"], s["recall"], s["f1"], s["tp"], s["fp"], s["fn"]))
    print("  reference DentalSegmentator Dice:", ", ".join(
        "%s %.4f" % (k, v["dice"]) for k, v in
        M["reference"]["dentalsegmentator_labels_3_plus_4_vs_gt_tooth_union_native_0p30mm"].items()))
    print("  model: %d params, %d B onnx, %d Adam iterations, wall %s s"
          % (M["model"]["parameter_count"], M["model"]["onnx_bytes"],
             M["model"]["total_iterations"], M["model"]["wall_clock_seconds"]))
    n = NCHK[0]
    print("\n%d checks run, %d passed, %d failed" % (n, n - len(FAIL), len(FAIL)))
    json.dump({"utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
               "checks_run": n, "passed": n - len(FAIL), "failed": len(FAIL),
               "failures": FAIL},
              open(f"{OUT}/work/verify_summary.json", "w"), indent=1)
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
