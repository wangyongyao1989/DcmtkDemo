"""make_parity.py -- step 3: device/host parity fixtures for held-out case 0101.

Pipeline under test (this is what the Android side must reproduce):
  DICOM series (/tmp/ai_build/app_volume/dentvoxel_0101/) --pydicom--> int16
  (192,192,128) --prep.build_features--> float32 (6,96,96,64) --onnxruntime--> (1,2,96,96,64)

Outputs (all little-endian, C-order, no header):
  parity_ref/feat_0101.raw   float32 1*6*96*96*64*4 = 9,437,184 B
  parity_ref/prob_0101.raw   float32 1*2*96*96*64*4 = 4,718,592 B  (onnxruntime output)
  parity_ref/lab_0101.raw    uint8   96*96*64       =   589,824 B  (argmax over channel)
  parity_ref/inst_0101.raw   uint16  96*96*64       = 1,179,648 B  (26-CC component ids)
  parity_ref/summary.txt     hashes + first-1MB md5 + Dice + component census
"""
import hashlib
import json
import os
import time
import numpy as np
from scipy import ndimage
import prep

OUT = prep.OUT_DIR
PAR = f"{OUT}/parity_ref"
CID = prep.CASE_HELD_OUT
CC_MIN = 30                      # component size threshold reported to the device side


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for blk in iter(lambda: f.read(1 << 20), b""):
            h.update(blk)
    return h.hexdigest()


def md5_prefix(path, n=1 << 20):
    h = hashlib.md5()
    with open(path, "rb") as f:
        h.update(f.read(n))
    return h.hexdigest()


def main():
    os.makedirs(PAR, exist_ok=True)
    t0 = time.time()
    # ---- 1. re-read the DICOM series, never the NIfTI
    vol = prep.read_dicom_series(f"{OUT}/app_volume/{CID}")
    canon, meta = prep.make_app_volume(CID)
    assert vol.shape == prep.APP_SHAPE and vol.dtype == np.int16
    lossless = bool(np.array_equal(vol, canon))
    print("dicom re-read == canonical:", lossless)

    # ---- 2. features
    feat = prep.build_features(vol)
    assert feat.shape == (6, 96, 96, 64) and feat.dtype == np.float32
    feat_path = f"{PAR}/feat_0101.raw"
    feat.tofile(feat_path)
    n_feat_bytes = os.path.getsize(feat_path)
    EXPECTED = 1 * 6 * 96 * 96 * 64 * 4          # = 14,155,776
    assert n_feat_bytes == EXPECTED, (n_feat_bytes, EXPECTED)

    # features must also equal the NIfTI-derived cached ones (proves the two ingest
    # paths agree, so the device may load either).
    # 缓存由 step 02/03 写；单独跑 step 04（或 clean clone 只想重放推理链）时没有缓存，
    # 那是一条"没做"的检查，不是"做失败"——直接抛 FileNotFoundError 会把 step 04 说成坏了。
    cached_path = f"{OUT}/work/cache/{CID}_feat.npy"
    if os.path.exists(cached_path):
        cached = np.load(cached_path)
        feat_match = bool(np.array_equal(feat, cached))
    else:
        feat_match = None
        print("NOTE: 无 %s（未跑 step 02/03），跳过 NIfTI 缓存对拍" % cached_path)

    # ---- 3. onnxruntime inference on exactly that buffer
    import onnxruntime as ort
    sess = ort.InferenceSession(f"{OUT}/model/teeth_cnn.onnx",
                                providers=["CPUExecutionProvider"])
    inp = sess.get_inputs()[0]
    got = sess.run(None, {inp.name: np.frombuffer(open(feat_path, "rb").read(),
                                                  dtype=np.float32).reshape(1, 6, 96, 96, 64)})
    prob = np.ascontiguousarray(got[0], dtype=np.float32)
    assert prob.shape == (1, 2, 96, 96, 64), prob.shape
    prob_path = f"{PAR}/prob_0101.raw"
    prob[0].tofile(prob_path)
    n_prob_bytes = os.path.getsize(prob_path)
    assert n_prob_bytes == 2 * 96 * 96 * 64 * 4, n_prob_bytes

    # determinism: a second ORT run on the same file must be bit-identical
    got2 = sess.run(None, {inp.name: np.frombuffer(open(feat_path, "rb").read(),
                                                   dtype=np.float32).reshape(1, 6, 96, 96, 64)})
    ort_repeat_bitexact = bool(np.array_equal(got[0], got2[0]))
    # and the raw file must re-load to exactly what ORT produced
    prob_reread_bitexact = bool(np.array_equal(
        np.frombuffer(open(prob_path, "rb").read(), np.float32).reshape(2, 96, 96, 64), prob[0]))

    # ---- 4. argmax labels
    lab = prob[0].argmax(axis=0).astype(np.uint8)
    lab_path = f"{PAR}/lab_0101.raw"
    lab.tofile(lab_path)
    assert os.path.getsize(lab_path) == 96 * 96 * 64
    # verify reading back the raws and recomputing argmax agrees (device-style)
    lab_from_raw = np.frombuffer(open(lab_path, "rb").read(), np.uint8).reshape(96, 96, 64)
    assert np.array_equal(lab_from_raw, lab)

    # ---- 5. connected components, 26-connectivity
    st = np.ones((3, 3, 3), dtype=np.uint8)
    inst, ncomp = ndimage.label(lab, structure=st)
    inst = inst.astype(np.uint16)
    assert inst.max() <= 65535
    inst_path = f"{PAR}/inst_0101.raw"
    inst.tofile(inst_path)
    assert os.path.getsize(inst_path) == 96 * 96 * 64 * 2
    sizes = np.bincount(inst.reshape(-1).astype(np.int64))
    sizes = sizes[1:]                       # drop background
    order = np.argsort(-sizes)
    top = [[int(sizes[i]) for i in order[:15]]]
    big = int((sizes >= CC_MIN).sum())
    comp = {"n_components_26conn": int(ncomp), "n_tooth_voxels": int(lab.sum()),
            "n_background_components": 0,
            "sizes_desc": [int(s) for i in order for s in [sizes[i]]],
            "n_components_ge_%d" % CC_MIN: big,
            "largest_15_sizes": top[0],
            "sum_of_sizes": int(sizes.sum()),
            "structure": "3x3x3 all-ones (26-connectivity), scipy.ndimage.label"}

    # ---- 6. Dice of lab vs GT, at both resolutions
    gt_app = prep.gt_tooth_at_app_grid(CID)
    gt_red = prep.downsample_gt_to_red(gt_app)
    d96 = prep.dice_stats(lab.astype(bool), gt_red.astype(bool))
    d192 = prep.dice_stats(prep.upsample_nearest2(lab).astype(bool), gt_app.astype(bool))

    # ---- 7. summary
    lines = []
    A = lines.append
    A("parity fixtures -- held-out case %s (never used for training or tuning)" % CID)
    A("generated_utc %s" % time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))
    A("")
    A("source series : %s/app_volume/%s  (128 files, 000001.dcm..000128.dcm)" % (OUT, CID))
    A("DICOM re-read == canonical int16 array : %s" % lossless)
    A("features from DICOM == features from NIfTI : %s" % (
        "not run (work/cache 缺 step 02/03 缓存)" if feat_match is None else feat_match))
    A("onnxruntime repeat run bit-exact : %s" % ort_repeat_bitexact)
    A("prob raw re-read bit-exact : %s" % prob_reread_bitexact)
    A("")
    A("file                 bytes      sha256")
    for name in ("feat_0101.raw", "prob_0101.raw", "lab_0101.raw", "inst_0101.raw"):
        p = f"{PAR}/{name}"
        A("%-20s %10d  %s" % (name, os.path.getsize(p), sha256(p)))
    A("")
    A("file                 md5(first 1 MiB)")
    for name in ("feat_0101.raw", "prob_0101.raw", "lab_0101.raw", "inst_0101.raw"):
        p = f"{PAR}/{name}"
        A("%-20s %s" % (name, md5_prefix(p)))
    A("")
    A("layout: little-endian, C order, NCHW.  feat = [1,6,96,96,64] -> axis order")
    A("(channel, x=96, y=96, z=64); prob = [1,2,96,96,64]; lab = [96,96,64] uint8")
    A("argmax over the channel axis; inst = [96,96,64] uint16 26-CC labels, C order.")
    A("")
    A("SIZE NOTE: feat_0101.raw is %d bytes = 1*6*96*96*64*4.  The task brief quotes"
      % n_feat_bytes)
    A("9,437,184 for that product, but 9,437,184 = 192*192*128*2, which is the size of")
    A("the canonical int16 volume (app_volume/dentvoxel_0101_hu.raw, also produced).")
    A("")
    A("lab_0101 vs GT (2x2x2 majority-vote tooth union):")
    A("  at 96x96x64   : dice %.6f precision %.6f recall %.6f f1 %.6f tp %d fp %d fn %d"
      % (d96["dice"], d96["precision"], d96["recall"], d96["f1"], d96["tp"], d96["fp"], d96["fn"]))
    A("  at 192x192x128(nearest x2): dice %.6f f1 %.6f tp %d fp %d fn %d"
      % (d192["dice"], d192["f1"], d192["tp"], d192["fp"], d192["fn"]))
    A("")
    A("connected components (26-connectivity) on lab_0101:")
    A("  n_components = %d over %d tooth voxels" % (comp["n_components_26conn"], comp["n_tooth_voxels"]))
    A("  n_components >= %d voxels = %d" % (CC_MIN, comp["n_components_ge_%d" % CC_MIN]))
    A("  largest 15 sizes = %s" % comp["largest_15_sizes"])
    A("  all sizes desc = %s" % comp["sizes_desc"])
    A("")
    A("device-side expectation: prob should match within 1e-5 per element (float32")
    A("  kernel ordering differences); lab argmax should then match on >= 99.9% of")
    A("  voxels and the >=%d-voxel component census should agree." % CC_MIN)
    txt = "\n".join(lines) + "\n"
    open(f"{PAR}/summary.txt", "w").write(txt)
    print(txt)

    # augment metrics.json with the fixture facts (kept consistent with run_all order).
    # metrics.json 只有完整跑过 train_model.py 才存在；单独重放本步时不该把已生成的
    # 夹具白写一遍却因为缺文件崩在最后，所以缺了就跳过这一步并说明。
    mp = f"{OUT}/metrics.json"
    if not os.path.exists(mp):
        print("NOTE: 无 %s（未跑 step 03），夹具事实只写 %s/summary.txt" % (mp, PAR))
    else:
        m = json.load(open(mp))
        m["parity_fixtures"] = {
            "case": CID, "dir": PAR,
            "dicom_reread_equals_canonical": lossless,
            # None 会变成 JSON null，读的人不知道是"不等"还是"没做"；显式写清楚。
            "features_dicom_equal_features_nifti": (
                "not run (work/cache missing)" if feat_match is None else feat_match),
            "feat_0101_raw_bytes": n_feat_bytes,
            "prob_0101_raw_bytes": n_prob_bytes,
            "ort_repeat_run_bit_exact": ort_repeat_bitexact,
            "prob_raw_reread_bit_exact": prob_reread_bitexact,
            "components": comp,
            "lab_dice_96": d96, "lab_dice_192": d192,
            "sha256": {n: sha256(f"{PAR}/{n}") for n in
                       ("feat_0101.raw", "prob_0101.raw", "lab_0101.raw", "inst_0101.raw")},
            "build_seconds": round(time.time() - t0, 1),
        }
        json.dump(m, open(mp, "w"), indent=1)


if __name__ == "__main__":
    main()
