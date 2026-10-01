#!/usr/bin/env python3
"""make_parity_device.py -- AC-08 host-side device-ORDER parity fixtures (case 0101).

The old parity_ref/{feat,prob,label,gt}_0101.raw fixtures were produced from
prep.build_features() which returns CANONICAL host axis order
(axis0 = DICOM Rows, axis1 = DICOM Columns, axis2 = z).  The Android device
(:cbctdeal CbctSeriesParser.cpp: vol->width = Columns, vol->height = Rows,
memory layout [depth][height][width]; AiEngine::runSegment: appDim =
{vol.width(), vol.height(), vol.depth()}; AiCore::buildFeatures indexes
vol[z*sliceSize + y*width + x]) works in DEVICE axis order
(axis0 = DICOM Column, axis1 = DICOM Row, axis2 = z).  The model was trained
and exported in DEVICE order (train2.py header + work/train2_full.json
"axis_order"), so the device comparison needs device-order fixtures.

This script emits (in parity_ref/, old canonical files kept untouched):
    device_feat_0101.raw    float32 (6,96,96,64)  14,155,776 B   C-order LE
    device_prob_0101.raw    float32 (2,96,96,64)   4,718,592 B
    device_label_0101.raw   uint8   (96,96,64)        589,824 B   prob[1] > thr (strict)
    device_inst_0101.raw    int16   (96,96,64)      1,179,648 B   AiCore::connectedComponents replica
    device_hu_0101.raw      float32 (96,96,64)      2,359,296 B   AiCore::buildReducedHu replica (589,824 float elements)
    device_gt_0101.raw      uint8   (96,96,64)        589,824 B   expert GT, device order
    device_parity.json                              all rules / gates / per-instance stats
No retraining.  Deterministic (no RNG).  Every number is computed here.
"""
import hashlib
import json
import math
import os
import platform
import sys

import numpy as np

_HERE = os.path.dirname(os.path.abspath(__file__))
_AI_ROOT = os.path.dirname(_HERE)
sys.path.insert(0, _HERE)
import prep  # canonical pipeline single source of truth

OUT = os.environ.get("AI_BUILD") or _AI_ROOT
PARITY = f"{OUT}/parity_ref"
CASE = "dentvoxel_0101"
MODEL = f"{OUT}/model/teeth_cnn.onnx"
TRAIN_JSON = f"{OUT}/work/train2_full.json"

MODEL_DIM = (96, 96, 64)      # ONNX 'feat' spatial dims, device order [col, row, z]
APP_DIM_DEV = (192, 192, 128)  # device appDim = {width=Columns, height=Rows, depth}
MIN_INSTANCE_VOXELS = 8       # AiConst::MIN_INSTANCE_VOXELS


def sha256_bytes(b):
    return hashlib.sha256(b).hexdigest()


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# ---------------------------------------------------------------- planGrid
def plan_grid(app_dim, app_spacing, model_dim):
    """Exact port of AiCore::planGrid (core/AiCore.cpp:30-47)."""
    f_, red, spacing = [], [], []
    for a in range(3):
        md = model_dim[a] if model_dim[a] > 0 else 1
        f = (app_dim[a] + md - 1) // md
        f = max(f, 1)
        rd = (app_dim[a] + f - 1) // f
        if rd > md:
            rd = md
        f_.append(f)
        red.append(rd)
        spacing.append(app_spacing[a] * f)
    return {"appDim": list(app_dim), "modelDim": list(model_dim),
            "factor": f_, "redDim": red, "spacing": spacing,
            "appSpacing": list(app_spacing)}


# ------------------------------------------------- redToWorld (AiTypes.h:130)
def red_world(i, j, k, spacing):
    # NOTE: C++ adds +0.5*spacing (voxel CENTER), not index*spacing.
    return ((i + 0.5) * spacing[0], (j + 0.5) * spacing[1], (k + 0.5) * spacing[2])


# ------------------------------------------------- Jacobi port (pcaPrincipalAxis)
def pca_principal_axis(cov6):
    """Exact port of AiCore::pcaPrincipalAxis (core/AiCore.cpp:470-523).
    cov6 = [a00,a01,a02,a11,a12,a22] upper triangle of the symmetric 3x3.
    Same sweeps, same pivoting, same sign convention (z >= 0)."""
    a00, a01, a02, a11, a12, a22 = cov6
    m = [[a00, a01, a02], [a01, a11, a12], [a02, a12, a22]]
    v = [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]]
    for _sweep in range(24):
        p, q = 0, 1
        off = abs(m[0][1])
        if abs(m[0][2]) > off:
            off, p, q = abs(m[0][2]), 0, 2
        if abs(m[1][2]) > off:
            off, p, q = abs(m[1][2]), 1, 2
        if off < 1e-18:
            break
        app, aqq, apq = m[p][p], m[q][q], m[p][q]
        theta = (aqq - app) / (2.0 * apq)
        t = (1.0 if theta >= 0.0 else -1.0) / (abs(theta) + math.sqrt(theta * theta + 1.0))
        cs = 1.0 / math.sqrt(t * t + 1.0)
        sn = cs * t
        for kk in range(3):
            mkp, mkq = m[kk][p], m[kk][q]
            m[kk][p] = cs * mkp - sn * mkq
            m[kk][q] = sn * mkp + cs * mkq
        for kk in range(3):
            mpk, mqk = m[p][kk], m[q][kk]
            m[p][kk] = cs * mpk - sn * mqk
            m[q][kk] = sn * mpk + cs * mqk
        for kk in range(3):
            vkp, vkq = v[kk][p], v[kk][q]
            v[kk][p] = cs * vkp - sn * vkq
            v[kk][q] = sn * vkp + cs * vkq
    best = 0
    if m[1][1] > m[best][best]:
        best = 1
    if m[2][2] > m[best][best]:
        best = 2
    ax = [v[0][best], v[1][best], v[2][best]]
    nrm = math.sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2])
    if nrm > 0.0:
        ax = [c / nrm for c in ax]
    if nrm < 0.5:
        ax = [0.0, 0.0, 1.0]
    if ax[2] < 0.0:
        ax = [-c for c in ax]
    return ax


# ------------------------------------------------- Neighbor26 table (C++ order)
def neighbor26():
    """core/AiCore.cpp:280-297: a outermost, b middle, c innermost, skip (0,0,0)."""
    out = []
    for a in (-1, 0, 1):
        for b in (-1, 0, 1):
            for c in (-1, 0, 1):
                if a == 0 and b == 0 and c == 0:
                    continue
                out.append((a, b, c))
    assert len(out) == 26
    return out


# ------------------------------------------------- connectedComponents replica
def connected_components(label_flat, hu_flat, grid):
    """Exact port of AiCore::connectedComponents (core/AiCore.cpp:299-468).

    label_flat: np.uint8, size modelDim product, C-order (i slowest, then j, k).
    hu_flat:    np.float32, size redDim product.
    Returns (inst_flat int16, instances list in final id order, tie flags).
    """
    md0, md1, md2 = grid["modelDim"]
    rd0, rd1, rd2 = grid["redDim"]
    sp = grid["spacing"]
    n = md0 * md1 * md2
    assert label_flat.size == n
    inst = np.zeros(n, dtype=np.int16)
    nb = neighbor26()
    raws = [None]  # index 0 placeholder (labels start at 1)
    n_raw = 0

    def decode(t):
        i = t // (md1 * md2)
        rem = t - i * md1 * md2
        j = rem // md2
        k = rem - j * md2
        return i, j, k

    for t0 in range(n):
        if label_flat[t0] == 0 or inst[t0] != 0:
            continue
        i0, j0, k0 = decode(t0)
        n_raw += 1
        comps = []
        st_i, st_j, st_k = [i0], [j0], [k0]
        cur_id = len(raws)
        inst[t0] = np.int16(cur_id)
        count = 0
        while st_i:
            i = st_i[-1]
            j = st_j[-1]
            k = st_k[-1]
            st_i.pop()
            st_j.pop()
            st_k.pop()
            comps.append(i * md1 * md2 + j * md2 + k)
            count += 1
            for (a, b, c) in nb:
                x = i + a
                y = j + b
                z = k + c
                if x < 0 or y < 0 or z < 0 or x >= md0 or y >= md1 or z >= md2:
                    continue
                s = (x * md1 + y) * md2 + z
                if label_flat[s] == 0 or inst[s] != 0:
                    continue
                inst[s] = np.int16(cur_id)
                st_i.append(x)
                st_j.append(y)
                st_k.append(z)
        if count < MIN_INSTANCE_VOXELS:
            for q in comps:
                inst[q] = 0
            continue
        # raw statistics (double accumulation, comps discovery order)
        vox = 0
        sx = sy = sz = 0.0
        sxx = syy = szz = sxy = sxz = syz = 0.0
        m_min = [float("inf")] * 3
        m_max = [-float("inf")] * 3
        hu_sum = hu_sq = 0.0
        hu_min = float("inf")
        hu_max = -float("inf")
        for idx in comps:
            i, j, k = decode(idx)
            if i >= rd0 or j >= rd1 or k >= rd2:  # zero-padded high end: clipped
                continue
            wx = (i + 0.5) * sp[0]
            wy = (j + 0.5) * sp[1]
            wz = (k + 0.5) * sp[2]
            sx += wx
            sy += wy
            sz += wz
            sxx += wx * wx
            syy += wy * wy
            szz += wz * wz
            sxy += wx * wy
            sxz += wx * wz
            syz += wy * wz
            bb = (wx - sp[0] * 0.5, wy - sp[1] * 0.5, wz - sp[2] * 0.5)
            tt = (wx + sp[0] * 0.5, wy + sp[1] * 0.5, wz + sp[2] * 0.5)
            for a in range(3):
                if bb[a] < m_min[a]:
                    m_min[a] = bb[a]
                if tt[a] > m_max[a]:
                    m_max[a] = tt[a]
            rv = ((i * rd1) + j) * rd2 + k
            hv = float(hu_flat[rv])
            hu_sum += hv
            hu_sq += hv * hv
            if hv < hu_min:
                hu_min = hv
            if hv > hu_max:
                hu_max = hv
            vox += 1
        raws.append({
            "vox": vox, "sx": sx, "sy": sy, "sz": sz,
            "sxx": sxx, "syy": syy, "szz": szz, "sxy": sxy, "sxz": sxz, "syz": syz,
            "min": m_min, "max": m_max,
            "hu_sum": hu_sum, "hu_sq": hu_sq, "hu_min": hu_min, "hu_max": hu_max,
            "discovery": t0,
        })

    # renumber: descending clipped voxel count; std::sort is NOT stable in C++,
    # we use a stable sort here (== first-discovery order for ties) and flag ties.
    order = list(range(1, len(raws)))
    order.sort(key=lambda t: -raws[t]["vox"])
    tie_pairs = []
    for q in range(1, len(order)):
        if raws[order[q]]["vox"] == raws[order[q - 1]]["vox"]:
            tie_pairs.append(int(raws[order[q]]["vox"]))
    remap = [0] * len(raws)
    instances = []
    voxel_mm3 = sp[0] * sp[1] * sp[2]
    for q, old in enumerate(order):
        new_id = q + 1
        remap[old] = new_id
        raw = raws[old]
        vox = raw["vox"]
        cx = raw["sx"] / vox
        cy = raw["sy"] / vox
        cz = raw["sz"] / vox
        mean_hu = raw["hu_sum"] / vox
        var = raw["hu_sq"] / vox - mean_hu * mean_hu
        sd = math.sqrt(var) if var > 0.0 else 0.0
        inv = 1.0 / vox
        cov = [raw["sxx"] * inv - cx * cx, raw["sxy"] * inv - cx * cy,
               raw["sxz"] * inv - cx * cz, raw["syy"] * inv - cy * cy,
               raw["syz"] * inv - cy * cz, raw["szz"] * inv - cz * cz]
        axis = pca_principal_axis(cov)
        instances.append({
            "id": new_id, "voxels": int(vox),
            "volumeMm3": vox * voxel_mm3,
            "centroidWorldMm": [cx, cy, cz],
            "bboxMinWorldMm": raw["min"], "bboxMaxWorldMm": raw["max"],
            "meanHu": mean_hu, "minHu": raw["hu_min"], "maxHu": raw["hu_max"],
            "sdHu": sd, "pcaAxisDeviceOrder": axis,
            "pcaNote": "axis in device order (x=col,y=row,z); comparison tolerance is on |axis| up to sign; device uses the same Jacobi port replicated here",
        })
    # apply remap in one pass (C++ line 460-463)
    lut = np.array(remap, dtype=np.int16)
    mask = inst > 0
    inst[mask] = lut[inst[mask]]
    return inst, instances, sorted(set(tie_pairs)), n_raw


def main():
    import onnxruntime as ort
    gates = {}

    # ---- 1. DICOM -> canonical volume -> features -> device order ----
    vol_can = prep.read_dicom_series(f"{OUT}/app_volume/{CASE}")
    assert vol_can.shape == (192, 192, 128) and vol_can.dtype == np.int16, vol_can.shape
    import pydicom
    from pydicom import dcmread
    d0 = dcmread(f"{OUT}/app_volume/{CASE}/000001.dcm")
    rows, cols = int(d0.Rows), int(d0.Columns)
    ps = [float(x) for x in d0.PixelSpacing]
    thick = float(d0.SliceThickness)
    assert (rows, cols) == (192, 192)
    # device: spacingX = PixelSpacing[1] (col), spacingY = PixelSpacing[0] (row), spacingZ = thickness
    app_spacing_dev = (ps[1], ps[0], thick)
    grid = plan_grid((cols, rows, 128), app_spacing_dev, MODEL_DIM)
    assert grid["redDim"] == list(MODEL_DIM) and grid["factor"] == [2, 2, 2], grid

    feat_can = prep.build_features(vol_can)                     # (6,96,96,64) canonical
    feat_dev = np.ascontiguousarray(feat_can.transpose(0, 2, 1, 3))
    # independent rebuild: run the SAME host feature pipeline on the device-ordered
    # volume; because every stage (normalize, 2x2x2 decimation, separable replicate
    # box means) commutes with an axis permutation, this must be BIT-identical to
    # the transpose above -> proves transpose(0,2,1,3) is the correct host->device map.
    vol_dev_can_build = np.ascontiguousarray(vol_can.transpose(1, 0, 2))  # [col,row,z]
    feat_dev_rebuilt = prep.build_features(vol_dev_can_build)
    d_tr = float(np.max(np.abs(feat_dev - feat_can.transpose(0, 2, 1, 3))))
    d_rb = float(np.max(np.abs(feat_dev - feat_dev_rebuilt)))
    old_feat = np.fromfile(f"{PARITY}/feat_0101.raw", dtype="<f4").reshape(6, 96, 96, 64)
    fix_eq = bool(np.array_equal(feat_dev, old_feat))
    gates["canonical_feat_is_exactly_transpose_of_device_feat"] = {
        "pass": bool(d_tr == 0.0 and fix_eq),
        "max_abs_diff_transpose_identity": d_tr,
        "device_feat_equals_shipped_feat_0101_raw_bitwise": fix_eq,
    }
    # TRUTH FOUND DURING DEVELOPMENT (verified bitwise, recorded in gates below):
    # the shipped *_0101.raw fixtures (19:55) are ALREADY device-order:
    # feat_0101.raw == transpose(prep.build_features(read_dicom_series(vol)))
    # == (bit-identical) our device_feat_0101.raw.  The previous agent evidently
    # regenerated them from the train2.py device-order pipeline before finishing;
    # the "canonical order, unusable" premise was stale.  The old files are kept
    # untouched as historical evidence; the device_ files are an independent
    # rebuild from raw DICOM + the frozen ONNX, bit-identical for
    # feat/prob/label/gt, and add the missing inst + hu fixtures.
    gates["independent_device_order_rebuild"] = {
        # NOT required to be bitwise: building features from the transposed VOLUME
        # runs np.cumsum over a different memory layout, so the separable box-mean
        # channels c1..c4 can differ by <= 2^-25 (1 float32 ulp).  c0 (normalize +
        # 2x2x2 decimate) is bitwise identical.  This is exactly the documented
        # host/device divergence in AiCore.cpp:142-149 ("parity is judged by
        # max-abs-diff + label agreement, not memcmp").
        "pass": bool(d_rb < 1e-6),
        "max_abs_diff": d_rb,
        "n_float32_elements_differing": int((feat_dev != feat_dev_rebuilt).sum()),
        "n_total_elements": int(feat_dev.size),
        "note": "addition-order-only divergence at the last float32 bit; not a contract violation",
    }

    # ---- 2. ORT inference in device order, determinism check ----
    so = ort.SessionOptions()
    sess = ort.InferenceSession(MODEL, so, providers=["CPUExecutionProvider"])
    feed = {"feat": feat_dev[None]}
    prob1 = sess.run(["prob"], feed)[0].reshape(2, 96, 96, 64)   # ORT emits (1,2,96,96,64)
    prob2 = sess.run(["prob"], feed)[0].reshape(2, 96, 96, 64)
    det = sha256_bytes(np.ascontiguousarray(prob1, dtype="<f4").tobytes()) == \
        sha256_bytes(np.ascontiguousarray(prob2, dtype="<f4").tobytes())
    gates["ort_determinism_two_runs_bit_identical"] = {"pass": bool(det)}
    prob_dev = np.ascontiguousarray(prob1.astype(np.float32), dtype="<f4")
    gates["prob_shape"] = {
        "pass": bool(prob_dev.shape == (2, 96, 96, 64) and prob_dev.dtype == np.dtype("<f4")),
        "shape": list(prob_dev.shape), "dtype": "float32 little-endian C-order",
    }

    # ---- threshold from the frozen training run ----
    tj = json.load(open(TRAIN_JSON))
    thr = float(tj["threshold_calibration"]["chosen_threshold"])
    thr_src = ("work/train2_full.json threshold_calibration.chosen_threshold "
               "(max val Dice on case dentvoxel_0074)")
    assert thr == 0.49, thr

    # ---- 3a. label: AiCore::thresholdMask rule (strict >, float32 vs double) ----
    n = 96 * 96 * 64
    tooth_p = prob_dev[1].astype(np.float64)   # (double)tooth[t] > threshold
    label_dev = (tooth_p > thr).astype(np.uint8)
    # np.argmax over channel axis (ties -> channel 0 == background)
    argmax_lab = (np.argmax(prob_dev, axis=0) == 1).astype(np.uint8)
    n_diff = int(np.count_nonzero(label_dev != argmax_lab))
    gates["label_vs_argmax_diff_count"] = {
        "pass": True,
        "count": n_diff,
        "note": "device rule is prob[1] > 0.49 (thresholdMask), NOT np.argmax(prob) (== prob[1] > 0.5); voxels with prob[1] in (0.49, 0.5] differ",
    }

    # ---- 3b. reduced HU: AiCore::buildReducedHu on the DEVICE-ordered volume ----
    # integer HU values summed over a 2x2x2 block and divided by 8 -> exact in
    # float regardless of accumulation order, so the numpy replica is bit-safe.
    hu_dev = (vol_dev_can_build.astype(np.float64)
              .reshape(96, 2, 96, 2, 64, 2).sum(axis=(1, 3, 5)) / 8.0).astype(np.float32)
    hu_dev = np.ascontiguousarray(hu_dev, dtype="<f4")

    # ---- 3c. GT at reduced grid, device order ----
    try:
        gt_app_can = prep.gt_tooth_at_app_grid(CASE)          # (192,192,128) canonical
        gt_red_can = prep.downsample_gt_to_red(gt_app_can)    # (96,96,64) canonical
        gt_source = "prep.gt_tooth_at_app_grid + prep.downsample_gt_to_red (canonical), then transpose(1,0,2)"
    except Exception as e:  # noqa: BLE001 -- keep going from the frozen canonical fixture
        gt_red_can = np.fromfile(f"{PARITY}/gt_0101.raw", dtype=np.uint8).reshape(96, 96, 64)
        gt_source = (f"canonical parity_ref/gt_0101.raw re-used "
                     f"(direct GT regeneration failed: {e}), then transpose(1,0,2)")
    gt_dev = np.ascontiguousarray(gt_red_can.transpose(1, 0, 2), dtype=np.uint8)

    # gate: are the shipped 19:55 fixtures already device-order? (bitwise)
    old_prob = np.fromfile(f"{PARITY}/prob_0101.raw", dtype="<f4").reshape(2, 96, 96, 64)
    old_label = np.fromfile(f"{PARITY}/label_0101.raw", dtype=np.uint8).reshape(96, 96, 64)
    old_gt = np.fromfile(f"{PARITY}/gt_0101.raw", dtype=np.uint8).reshape(96, 96, 64)
    gates["shipped_1955_fixtures_already_device_order_bit_equal"] = {
        "pass": bool(fix_eq
                     and np.array_equal(np.ascontiguousarray(old_prob, dtype="<f4").tobytes(),
                                        prob_dev.tobytes())
                     and np.array_equal(old_label, label_dev)
                     and np.array_equal(old_gt, gt_dev)),
        "feat": bool(fix_eq),
        "prob": bool(np.array_equal(np.ascontiguousarray(old_prob, dtype="<f4").tobytes(),
                                    prob_dev.tobytes())),
        "label": bool(np.array_equal(old_label, label_dev)),
        "gt": bool(np.array_equal(old_gt, gt_dev)),
        "note": ("independent rebuild from raw DICOM + frozen ONNX is bit-identical to the "
                 "shipped feat/prob/label/gt fixtures => those were already device order; "
                 "this script adds the missing inst + hu fixtures and the per-instance table"),
    }

    # ---- 3d. instance ids: connectedComponents replica ----
    lf = label_dev.reshape(-1)
    inst_dev, instances, tie_sizes, n_raw = connected_components(
        lf, hu_dev.reshape(-1), grid)
    inst_dev = np.ascontiguousarray(inst_dev.astype("<i2"))
    # scipy cross-check: unique 26-components + kept size multiset vs train2_full.json
    from scipy import ndimage as ndi
    _, n_uniq = ndi.label(label_dev, structure=ndi.generate_binary_structure(3, 3))
    json_cc = tj["metrics"][CASE]["cc_26conn_min8_at_chosen"]["component_volumes_cm3"]
    json_sizes = sorted([round(v * 1000.0 / 1.728) for v in json_cc], reverse=True)
    my_sizes = sorted([c["voxels"] for c in instances], reverse=True)
    gates["kept_instances_match_train2_full_json_cc"] = {
        "pass": bool(my_sizes == json_sizes and len(instances) ==
                     tj["metrics"][CASE]["cc_26conn_min8_at_chosen"]["n_components_filtered"]),
        "my_kept_sizes_desc": my_sizes,
        "json_kept_sizes_desc": json_sizes,
        "scipy_unique_26_components": int(n_uniq),
    }

    # ---- write fixtures (old canonical files untouched) ----
    files = {
        "device_feat_0101.raw": (feat_dev.astype("<f4"), (6, 96, 96, 64), "float32"),
        "device_prob_0101.raw": (prob_dev, (2, 96, 96, 64), "float32"),
        "device_label_0101.raw": (label_dev, (96, 96, 64), "uint8"),
        "device_inst_0101.raw": (inst_dev, (96, 96, 64), "int16 (signed LE, C++ short)"),
        "device_hu_0101.raw": (hu_dev, (96, 96, 64), "float32"),
        "device_gt_0101.raw": (gt_dev, (96, 96, 64), "uint8"),
    }
    expected_bytes = {"device_feat_0101.raw": 14155776, "device_prob_0101.raw": 4718592,
                      "device_label_0101.raw": 589824, "device_inst_0101.raw": 1179648,
                      # float32 on the (96,96,64) reduced grid = 589,824 ELEMENTS x 4 bytes.
                      # (The task brief's "589,824 bytes" is the element count, not the
                      # float32 byte size; uint8 label/GT and int16 inst line up with
                      # 589,824 / 1,179,648 bytes exactly, only HU needed this correction.)
                      "device_hu_0101.raw": 2359296, "device_gt_0101.raw": 589824}
    files_meta = {}
    for name, (arr, shape, dt) in files.items():
        raw = np.ascontiguousarray(arr).tobytes()
        assert len(raw) == expected_bytes[name], (name, len(raw))
        with open(f"{PARITY}/{name}", "wb") as f:
            f.write(raw)
        files_meta[name] = {"bytes": len(raw), "sha256": sha256_bytes(raw),
                            "dtype": dt, "shape": list(shape),
                            "order": "C-contiguous, little-endian"}

    # ---- 4/5. metrics vs GT + gate against train2_full.json ----
    tp = int(np.count_nonzero((gt_dev == 1) & (label_dev == 1)))
    fp = int(np.count_nonzero((gt_dev == 0) & (label_dev == 1)))
    fn = int(np.count_nonzero((gt_dev == 1) & (label_dev == 0)))
    dice = 2.0 * tp / (2 * tp + fp + fn)
    prec = tp / (tp + fp)
    rec = tp / (tp + fn)
    jm = tj["metrics"][CASE]["at_chosen"]
    gates["dice_agrees_with_train2_full_json"] = {
        "pass": bool(abs(dice - jm["dice"]) < 1e-6 and abs(prec - jm["precision"]) < 1e-6
                     and abs(rec - jm["recall"]) < 1e-6 and tp == jm["tp"]
                     and fp == jm["fp"] and fn == jm["fn"]),
        "computed": {"dice": dice, "precision": prec, "recall": rec,
                     "tp": tp, "fp": fp, "fn": fn,
                     "pred_voxels": int(label_dev.sum()), "gt_voxels": int(gt_dev.sum())},
        "json_reference": jm,
        "note": ("Dice/precision/recall are axis-order invariant aggregate counts, so "
                 "device-order fixtures must reproduce the json exactly (same prob, same GT). "
                 "If this fails, the prob or GT arrays differ and the json stays as authority."),
    }

    out = {
        "case": CASE,
        "generated_utc": "RUN_UTC",
        "purpose": ("AC-08 host-side parity fixtures in DEVICE axis order "
                    "(axis0=DICOM Column, axis1=DICOM Row, axis2=z), mirroring "
                    ":cbctdeal CbctSeriesParser + cbctmeasure AiCore/AiEngine semantics"),
        "axis_order_verdict": {
            "canonical_host": "prep.py arrays: axis0=Rows, axis1=Columns, axis2=z",
            "device": "AiCore/AiEngine + CbctSeriesParser: axis0=width=Columns, axis1=height=Rows, axis2=z; layout [depth][height][width]",
            "conversion": "feat: transpose(0,2,1,3); volumes/masks/GT: transpose(1,0,2); exact bitwise in-plane permutation of the shipped canonical fixtures (see gates); the device-ordered conv output is NOT the transposed canonical output (3x3x3 kernels are not permutation-equivariant) -- hence this script feeds feat_dev itself to ORT",
            "evidence": [
                "cbctdeal/src/main/cpp/CbctSeriesParser.cpp:436-441 vol->width=cols, vol->height=rows",
                "cbctmeasure/src/main/cpp/include/VolumeRef.h:24-27 world=index*spacing, layout [depth][height][width]",
                "cbctmeasure/src/main/cpp/core/AiEngine.cpp:73 appDim={width,height,depth}",
                "cbctmeasure/src/main/cpp/core/AiCore.cpp:107-140 vol[z*sliceSize + y*width + x]",
                "/tmp/ai_build/train2.py header + work/train2_full.json axis_order=DEVICE",
            ],
        },
        "model": {"path": MODEL, "sha256": sha256_file(MODEL),
                  "bytes": os.path.getsize(MODEL),
                  "input": "feat [1,6,96,96,64] float32", "output": "prob [1,2,96,96,64] float32",
                  "ops": "Conv3d 6->8->8->2 k3 p1 + Relu + Softmax(axis=1), opset 17",
                  "parameters": tj["parameter_count"]},
        "environment": {
            "python": platform.python_version(),
            "onnxruntime": ort.__version__,
            "onnxruntime_providers": "CPUExecutionProvider",
            "numpy": np.__version__,
            "scipy": None,
            "ort_version_in_android_app": "ONNX Runtime 1.17 (ORT_API_VERSION 17, third_party/onnxruntime)",
        },
        "threshold": {"value": thr, "source": thr_src,
                      "comparison_semantics": "float32 prob promoted to double, strict >",
                      "device_note": "device passes threshold via nativeRunSegment(handle, threshold, keepParity); must be configured with 0.49 for parity"},
        "rules": {
            "features": "prep.build_features(canonical int16 (192,192,128)) then transpose(0,2,1,3); equals device AiCore::buildFeatures(vol) up to <=1 float32 ulp in the box-mean channels (summation order; c0 bitwise equal) -- parity judged by max-abs-diff per AiCore.cpp:142-149",
            "label": "label[i,j,k] = 1 iff double(prob_dev[1,i,j,k]) > 0.49 (AiCore::thresholdMask; strict >, ties->background; NOT np.argmax)",
            "inst": ("AiCore::connectedComponents replica: scan linear index t C-order (i slowest, then j, then k); LIFO stack flood over the 26 neighbour offsets in Neighbor26 table order "
                     "(a = axis0 offset outermost -1..1, b = axis1 middle, c = axis2 inner, skipping (0,0,0)); drop components with RAW flood-fill count < AiConst::MIN_INSTANCE_VOXELS=8; "
                     "renumber 1..N by DESCENDING CLIPPED voxel count (voxels counted only inside redDim via the C++ `if (i>=redDim[0]...) continue;`); "
                     "std::sort is unstable in C++ -> for equal sizes only the multiset of sizes is guaranteed; here stable sort = first-discovery order, ambiguity flagged"),
            "hu": "mean raw HU of each 2x2x2 app block in device order (AiCore::buildReducedHu on vol.transpose(1,0,2)); exact because HU integers / 8 are dyadic",
            "gt": gt_source,
            "centroid_world": "mean of AiGrid::redToWorld(i,j,k) = ((i+0.5)*1.2, (j+0.5)*1.2, (k+0.5)*1.2) mm, device-order red indices; NOTE: +0.5*spacing voxel-center offset per AiTypes.h:130-134, NOT index*spacing",
            "bbox": "per-voxel [center - 0.5*spacing, center + 0.5*spacing] union (AiCore.cpp:398-405)",
            "volumeMm3": "clipped voxels * spacing product = voxels * 1.2^3 = voxels * 1.728",
            "hu_stats": "double accumulation; sdHu = sqrt(max(E[x^2]-E[x]^2, 0)) population sd",
            "pca": "cov = E[xx^T] - mean*mean^T from origin moments (parallel-axis), Jacobi sweeps exactly as AiCore::pcaPrincipalAxis; sign convention z>=0; compare |axis| up to sign",
        },
        "neighbor26_table_order": [[int(x) for x in t] for t in neighbor26()],
        "grid": grid,
        "notes": [
            "device_hu_0101.raw is float32 over the (96,96,64) reduced grid = 589,824 elements = 2,359,296 BYTES (AiResult.hu is float)",
            "ONNX 'prob' output is declared [1,2,96,96,64]; the device prob buffer and this fixture are the same 4,718,592-byte flat C-order float32 payload",
        ],
        "files": files_meta,
        "instances": instances,
        "instance_census": {
            "n_raw_components": n_raw,
            "n_raw_components_note": ("C++-faithful seed count: AiCore::connectedComponents zeroes the inst of "
                                      "sub-MIN_INSTANCE_VOXELS components, so the linear scan RE-SEEDS those same voxels "
                                      f"(|A|-1 extra floods per dropped component of size |A|). Unique 26-components is the "
                                      "scipy-equivalent count below; BOTH give the identical kept-instance census (sizes/ids), "
                                      "identical to train2_full.json cc_26conn_min8_at_chosen"),
            "min_size_filter": MIN_INSTANCE_VOXELS,
            "n_kept": len(instances),
            "scipy_unique_26_components": int(n_uniq),
            "component_size_ties_detected": tie_sizes,
            "tie_ambiguity_note": ("C++ std::sort is not stable; if ties exist only the multiset of "
                                   "component sizes is guaranteed identical across implementations"),
        },
        "tooth_voxels_total": int(label_dev.sum()),
        "dice_block": {"dice": dice, "precision": prec, "recall": rec,
                       "tp": tp, "fp": fp, "fn": fn,
                       "threshold": thr, "grid": "device-order reduced (96,96,64)"},
        "device_dump_format": {
            "producer": "AiEngine::dumpParity (cbctmeasure/src/main/cpp/core/AiEngine.cpp:143-177)",
            "header_bytes": 64,
            "header": "8 x int64 little-endian: featCount, probCount, labelBytes, modelDim0, modelDim1, modelDim2, redDim0, redDim1",
            "payload_order": ["float32 feat[featCount]", "float32 prob[probCount]",
                              "uint8 label[labelBytes]", "int16 inst[labelBytes]"],
            "expected_counts": {"featCount": 6 * 96 * 96 * 64, "probCount": 2 * 96 * 96 * 64,
                                "labelBytes": 96 * 96 * 64,
                                "modelDim": [96, 96, 64], "redDim0": 96, "redDim1": 96},
            "expected_total_bytes": 64 + 4 * 6 * n + 4 * 2 * n + n + 2 * n,
            "note": "inst element count == label count (both modelDim volume); dump does NOT carry the threshold or the HU/GT arrays",
        },
        "gates": gates,
        "old_canonical_fixtures_kept": sorted(k for k in os.listdir(PARITY)
                                              if not k.startswith("device_")),
    }
    # scipy version if present
    try:
        import scipy
        out["environment"]["scipy"] = scipy.__version__
    except Exception:  # noqa: BLE001
        pass
    import datetime
    out["generated_utc"] = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

    with open(f"{PARITY}/device_parity.json", "w") as f:
        json.dump(out, f, indent=1)

    print(json.dumps({
        "gates": {k: v["pass"] for k, v in gates.items()},
        "label_argmax_diff": n_diff,
        "instances_kept": len(instances),
        "tie_sizes": tie_sizes,
        "dice": dice, "precision": prec, "recall": rec,
        "tooth_voxels": int(label_dev.sum()),
        "json": f"{PARITY}/device_parity.json",
    }, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
