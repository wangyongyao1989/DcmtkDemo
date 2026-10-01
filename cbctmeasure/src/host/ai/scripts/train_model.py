"""train_model.py -- step 2: numpy-only training of the 1x1x1 conv (per-voxel MLP)
tooth segmentation head, ONNX export, onnxruntime verification and metrics.json.

Contract (mandatory, see SPEC.md section 7):
    input  'feat'  float32 [1, 6, 96, 96, 64]
    output 'prob'  float32 [1, 2, 96, 96, 64]   softmax over channel axis
    opset 17, ops used: Conv(1x1x1), Relu, Softmax (Add/BiasAdd folded into Conv bias)

Split discipline:
    fit 0021+0047, monitor 0074 -> pick epoch count; then refit on 0021+0047+0074
    with that epoch count.  dentvoxel_0101 GT is NEVER touched before the final
    evaluation (its features/GT are only loaded in the `evaluate` phase).
"""
import hashlib
import json
import os
import shutil
import time
import numpy as np
import prep

OUT = prep.OUT_DIR
CACHE = f"{OUT}/work/cache"
os.makedirs(CACHE, exist_ok=True)


# ---------------------------------------------------------------- data cache
def case_arrays(cid):
    """feat (6,96,96,64) float32 and yred (96,96,64) uint8, cached as .npy."""
    fp = f"{CACHE}/{cid}_feat.npy"
    gp = f"{CACHE}/{cid}_yred.npy"
    if os.path.exists(fp) and os.path.exists(gp):
        return np.load(fp), np.load(gp)
    if cid == prep.CASE_HELD_OUT:
        # features are allowed (no GT involved); GT loaded only for reporting
        vol, _ = prep.make_app_volume(cid)
        feat = prep.build_features(vol)
    else:
        vol, _ = prep.make_app_volume(cid)
        feat = prep.build_features(vol)
    gt_app = prep.gt_tooth_at_app_grid(cid)
    yred = prep.downsample_gt_to_red(gt_app)
    feat.tofile(fp.replace(".npy", ".raw"))
    np.save(fp, feat)
    np.save(gp, yred)
    return feat, yred


# ------------------------------------------------------------ sampling rule
def sample_case(cid, case_index):
    """X (N,6) float32, y (N,) uint8.  Rule (documented in SPEC.md 7.3):
      positives = every reduced-grid voxel with tooth label 1;
      negatives = reduced-grid voxels with label 0 whose reduced indices pass the
                  TRAIN_STRIDE test (TRAIN_STRIDE = 1 -> all of them);
                  if that pool is larger than NEG_PER_POS * #positives, keep the
                  sorted first NEG_PER_POS * #positives entries of
                  np.random.default_rng(TRAIN_SEED + case_index).permutation(pool).
    """
    feat, yred = case_arrays(cid)
    x = feat.reshape(prep.MODEL_IN_CHANNELS, -1).T.copy()      # (N,6) C order = z fastest
    flat = yred.reshape(-1)
    # reduced-grid indices for the flattened (96,96,64) C-order layout
    n = flat.size
    iz = np.arange(n) % 64
    iy = (np.arange(n) // 64) % 96
    ix = np.arange(n) // (64 * 96)
    stride_ok = (ix % prep.TRAIN_STRIDE == 0) & (iy % prep.TRAIN_STRIDE == 0) & (iz % prep.TRAIN_STRIDE == 0)
    pos = np.nonzero(flat == 1)[0]
    negpool = np.nonzero((flat == 0) & stride_ok)[0]
    kmax = prep.NEG_PER_POS * len(pos)
    if len(negpool) > kmax:
        rng = np.random.default_rng(prep.TRAIN_SEED + case_index)
        neg = np.sort(rng.permutation(negpool)[:kmax])
    else:
        neg = negpool
    idx = np.concatenate([pos, neg])
    return x[idx].astype(np.float32), flat[idx].astype(np.uint8)


# ------------------------------------------------------------- the MLP itself
class Mlp:
    """Per-voxel MLP == Conv(6->24,1^3) Relu Conv(24->24,1^3) Relu Conv(24->2,1^3) Softmax."""

    def __init__(self, seed):
        rng = np.random.default_rng(seed)
        h = prep.MODEL_HIDDEN
        self.W1 = (rng.standard_normal((6, h)) * np.sqrt(2.0 / 6)).astype(np.float32)
        self.b1 = np.zeros(h, np.float32)
        self.W2 = (rng.standard_normal((h, h)) * np.sqrt(2.0 / h)).astype(np.float32)
        self.b2 = np.zeros(h, np.float32)
        self.W3 = (rng.standard_normal((h, 2)) * np.sqrt(2.0 / h)).astype(np.float32)
        self.b3 = np.zeros(2, np.float32)

    def params(self):
        return [self.W1, self.b1, self.W2, self.b2, self.W3, self.b3]

    def n_params(self):
        return sum(p.size for p in self.params())

    def forward(self, X, want_probs=False):
        A1 = np.maximum(X @ self.W1 + self.b1, 0, dtype=np.float32)
        A2 = np.maximum(A1 @ self.W2 + self.b2, 0, dtype=np.float32)
        Z = (A2 @ self.W3 + self.b3).astype(np.float64)
        Z = Z - Z.max(axis=1, keepdims=True)
        E = np.exp(Z)
        P = E / E.sum(axis=1, keepdims=True)
        if want_probs:
            return A1, A2, Z, P.astype(np.float32)
        return A1, A2, Z, P

    def loss_and_grads(self, X, Yv):
        N = X.shape[0]
        A1, A2, Z, P = self.forward(X)
        logp = -np.log(np.clip(P[np.arange(N), Yv], 1e-12, 1.0)).mean()
        dP = P.astype(np.float64)
        dP[np.arange(N), Yv] -= 1.0
        dP /= N
        dW3 = A2.T.astype(np.float64) @ dP
        db3 = dP.sum(axis=0)
        dA2 = dP @ self.W3.T.astype(np.float64)
        dA2 *= (A2 > 0)
        dW2 = A1.T.astype(np.float64) @ dA2
        db2 = dA2.sum(axis=0)
        dA1 = dA2 @ self.W2.T.astype(np.float64)
        dA1 *= (A1 > 0)
        dW1 = X.T.astype(np.float64) @ dA1
        db1 = dA1.sum(axis=0)
        return logp, [dW1.astype(np.float32), db1.astype(np.float32),
                      dW2.astype(np.float32), db2.astype(np.float32),
                      dW3.astype(np.float32), db3.astype(np.float32)]

    def logit_diff(self, feat):
        """(96,96,64) float64 array of  z1 - z0  (class-1 minus class-0 logit).
        Adding a constant to b3[1] shifts this array uniformly, which is exactly a
        move of the 0.5 argmax threshold -- that is how prior calibration is folded
        into the exported model, so device-side post-processing stays pure argmax."""
        X = feat.reshape(prep.MODEL_IN_CHANNELS, -1).T
        out = np.empty(X.shape[0], np.float64)
        for s in range(0, X.shape[0], 65536):
            xb = X[s:s + 65536]
            A1 = np.maximum(xb @ self.W1 + self.b1, 0)
            A2 = np.maximum(A1 @ self.W2 + self.b2, 0)
            Z = A2 @ self.W3 + self.b3
            out[s:s + 65536] = Z[:, 1] - Z[:, 0]
        return out.reshape(*prep.RED_SHAPE)

    def predict_grid(self, feat):
        """feat (6,96,96,64) -> prob (2,96,96,64) float32, full-grid batched."""
        X = feat.reshape(prep.MODEL_IN_CHANNELS, -1).T
        out = np.empty((2, X.shape[0]), np.float32)
        for s in range(0, X.shape[0], 65536):
            _, _, _, p = self.forward(X[s:s + 65536], want_probs=True)
            out[:, s:s + 65536] = p.T
        return out.reshape(2, *prep.RED_SHAPE)


def adam_step(model, grads, m, v, t):
    for i, (p, g) in enumerate(zip(model.params(), grads)):
        gi = g.astype(np.float64)
        m[i] = prep.ADAM_BETA1 * m[i] + (1 - prep.ADAM_BETA1) * gi
        v[i] = prep.ADAM_BETA2 * v[i] + (1 - prep.ADAM_BETA2) * gi * gi
        mh = m[i] / (1 - prep.ADAM_BETA1 ** t)
        vh = v[i] / (1 - prep.ADAM_BETA2 ** t)
        p -= prep.LEARNING_RATE * mh / (np.sqrt(vh) + prep.ADAM_EPS)


def train(model, X, Y, epochs, val_cb=None, eval_every=10):
    """Minibatch Adam.  Returns (history, iterations).  `val_cb(model)` is called on
    epoch 1 and every `eval_every` epochs; it must return a prep.dice_stats dict
    computed on the FULL 96x96x64 grid of a monitoring case."""
    N = X.shape[0]
    m = [np.zeros_like(p, np.float64) for p in model.params()]
    v = [np.zeros_like(p, np.float64) for p in model.params()]
    t = 0
    history = []
    for ep in range(1, epochs + 1):
        rng = np.random.default_rng(prep.TRAIN_SEED + ep)
        order = rng.permutation(N)
        for s in range(0, N, prep.MINIBATCH):
            b = order[s:s + prep.MINIBATCH]
            t += 1
            _, g = model.loss_and_grads(X[b], Y[b])
            adam_step(model, g, m, v, t)
        if val_cb is not None and (ep % eval_every == 0 or ep == 1):
            _, _, _, P2 = model.forward(X)
            tr = prep.dice_stats(P2.argmax(axis=1) == 1, Y.astype(bool))
            st = val_cb(model)
            history.append({"epoch": ep, "iterations": t, "train_sampled_dice": tr["dice"],
                            "val_dice": st["dice"], "val_precision": st["precision"],
                            "val_recall": st["recall"], "val_tp": st["tp"],
                            "val_fp": st["fp"], "val_fn": st["fn"]})
            print("  ep %3d it %5d train_dice %.4f | VAL full-grid dice %.4f prec %.4f "
                  "rec %.4f (tp %d fp %d fn %d)" % (ep, t, tr["dice"], st["dice"],
                  st["precision"], st["recall"], st["tp"], st["fp"], st["fn"]))
    return history, t


# ------------------------------------------------------------- ONNX export
def export_onnx(model, path):
    import onnx
    from onnx import helper, numpy_helper, TensorProto
    h = prep.MODEL_HIDDEN

    def conv(name, X, W, B, Y):
        return helper.make_node("Conv", [X, W, B], [Y], name=name + "_conv",
                                kernel_shape=[1, 1, 1], strides=[1, 1, 1],
                                pads=[0, 0, 0, 0, 0, 0])
    inits = [
        numpy_helper.from_array(model.W1.transpose(1, 0).reshape(h, 6, 1, 1, 1).astype(np.float32), "W1"),
        numpy_helper.from_array(model.b1.astype(np.float32), "b1"),
        numpy_helper.from_array(model.W2.T.reshape(h, h, 1, 1, 1).astype(np.float32), "W2"),
        numpy_helper.from_array(model.b2.astype(np.float32), "b2"),
        numpy_helper.from_array(model.W3.T.reshape(2, h, 1, 1, 1).astype(np.float32), "W3"),
        numpy_helper.from_array(model.b3.astype(np.float32), "b3"),
    ]
    nodes = [
        conv("l1", prep.MODEL_IN_NAME, "W1", "b1", "z1"),
        helper.make_node("Relu", ["z1"], ["a1"], name="r1"),
        conv("l2", "a1", "W2", "b2", "z2"),
        helper.make_node("Relu", ["z2"], ["a2"], name="r2"),
        conv("l3", "a2", "W3", "b3", "z3"),
        helper.make_node("Softmax", ["z3"], [prep.MODEL_OUT_NAME], name="sm", axis=1),
    ]
    gin = helper.make_tensor_value_info(prep.MODEL_IN_NAME, TensorProto.FLOAT,
                                        [1, prep.MODEL_IN_CHANNELS] + list(prep.RED_SHAPE))
    gout = helper.make_tensor_value_info(prep.MODEL_OUT_NAME, TensorProto.FLOAT,
                                         [1, prep.MODEL_OUT_CHANNELS] + list(prep.RED_SHAPE))
    graph = helper.make_graph(nodes, "teeth_cnn", [gin], [gout], inits)
    mdl = helper.make_model(graph, opset_imports=[helper.make_opsetid("", prep.OPSET)])
    mdl.ir_version = 8
    onnx.checker.check_model(mdl)
    onnx.save(mdl, path)
    return mdl


def ort_check(path, feat, prob_np):
    import onnxruntime as ort
    import onnx
    m = onnx.load(path)
    assert [i.name for i in m.graph.input] == ["feat"], [i.name for i in m.graph.input]
    assert [o.name for o in m.graph.output] == ["prob"], [o.name for o in m.graph.output]
    din = m.graph.input[0].type.tensor_type.shape.dim
    dout = m.graph.output[0].type.tensor_type.shape.dim
    ishape = [d.dim_value for d in din]
    oshape = [d.dim_value for d in dout]
    assert ishape == [1, 6, 96, 96, 64], ishape
    assert oshape == [1, 2, 96, 96, 64], oshape
    assert m.opset_import[0].version <= 17
    sess = ort.InferenceSession(path, providers=["CPUExecutionProvider"])
    got = sess.run(None, {"feat": feat.reshape(1, *feat.shape).astype(np.float32)})[0]
    d = float(np.abs(got - prob_np[None]).max())
    return {"input_shape": ishape, "output_shape": oshape,
            "opset": m.opset_import[0].version,
            "ort_ops": sorted({n.op_type for n in m.graph.node}),
            "n_nodes": len(m.graph.node),
            "ort_max_abs_diff_vs_numpy": d}


def full_grid_dice(model, cid):
    """Dice of the model's argmax over the WHOLE 96x96x64 grid of `cid` against the
    2x2x2-voted GT -- i.e. exactly the metric reported in metrics.json, computed on a
    case whose GT is allowed to be used for hyper-parameter choice (train cases only)."""
    feat, yred = case_arrays(cid)
    prob = model.predict_grid(feat)
    lab = prob.argmax(axis=0).astype(np.uint8)
    return prep.dice_stats(lab.astype(bool), yred.astype(bool))


def pooled_counts(model, cids):
    """Concatenate the class-1-minus-class-0 logit and the GT over `cids`."""
    dz, ys = [], []
    for cid in cids:
        feat, yred = case_arrays(cid)
        dz.append(model.logit_diff(feat).reshape(-1))
        ys.append(yred.reshape(-1).astype(np.int8))
    return np.concatenate(dz), np.concatenate(ys)


def best_threshold(D, Y):
    """Threshold t* on the logit difference maximising Dice of (D >= t*) vs Y.
    Deterministic: full sort, single sweep, ties resolved by the sorted order."""
    order = np.argsort(-D, kind="stable")
    Ds = D[order]; Ys = Y[order]
    pos = (Ys == 1).astype(np.int64)
    cum_tp = np.cumsum(pos)
    cum_fp = np.cumsum(1 - pos)
    ptot = int(pos.sum())
    dice = np.where(2 * cum_tp + cum_fp + (ptot - cum_tp) > 0,
                    2.0 * cum_tp / np.maximum(2 * cum_tp + cum_fp + (ptot - cum_tp), 1), 0.0)
    k = int(np.argmax(dice))
    t = float(Ds[k])
    tp = int(cum_tp[k]); fp = int(cum_fp[k]); fn = ptot - tp
    den = 2 * tp + fp + fn
    return t, {"dice": 2.0 * tp / den, "precision": tp / (tp + fp),
               "recall": tp / (tp + fn), "f1": 2.0 * tp / den,
               "tp": tp, "fp": fp, "fn": fn, "pred_voxels": tp + fp, "gt_voxels": ptot,
               "total_voxels": int(D.size)}


def calibrated_dice(model, cids):
    """Non-mutating: Dice of the logit-difference-optimal threshold on `cids`."""
    D, Y = pooled_counts(model, cids)
    return best_threshold(D, Y)[1]


def calibrate(model, cids):
    """Fold the prior correction into b3[1] so that argmax(prob) reproduces the
    Dice-optimal threshold found on `cids` (train-split cases only)."""
    D, Y = pooled_counts(model, cids)
    t, st = best_threshold(D, Y)
    before = [float(x) for x in model.b3]
    model.b3[1] = np.float32(model.b3[1] - t)     # new dz = dz - t  -> argmax <=> dz > t
    return {"threshold_on_logit_diff": t, "dice_at_threshold_on_calibration_cases": st,
            "calibration_cases": list(cids), "b3_after": [float(x) for x in model.b3],
            "b3_before": before}


def dice_at_native_0p3(cid, lab, crop_start):
    """Prediction (reduced grid, 1.2 mm effective) nearest-upsampled x4 into the
    native 0.30 mm expert GT grid (440,440,344) and scored there.  Everything
    outside the app crop is predicted background."""
    ox, oy, oz = 2 * int(crop_start[0]), 2 * int(crop_start[1]), 2 * int(crop_start[2])
    up = np.repeat(np.repeat(np.repeat(lab.astype(np.uint8), 4, 0), 4, 1), 4, 2)
    pred = np.zeros(prep.GT_SHAPE, bool)
    pred[ox:ox + up.shape[0], oy:oy + up.shape[1], oz:oz + up.shape[2]] = up.astype(bool)
    del up
    native = prep.gt_tooth_at_native_0p3(cid).astype(bool)
    tp = int(np.count_nonzero(pred & native))
    fp = int(np.count_nonzero(pred & ~native))
    fn = int(np.count_nonzero(~pred & native))
    gt_outside = int(np.count_nonzero(native)) - tp - fn
    del pred, native
    den = 2 * tp + fp + fn
    return {"dice": 2.0 * tp / den, "precision": tp / (tp + fp), "recall": tp / (tp + fn),
            "f1": 2 * (tp / (tp + fp)) * (tp / (tp + fn)) / ((tp / (tp + fp)) + (tp / (tp + fn))),
            "tp": tp, "fp": fp, "fn": fn, "pred_voxels": tp + fp, "gt_voxels": tp + fn,
            "total_voxels": int(np.prod(prep.GT_SHAPE)),
            "gt_tooth_voxels_outside_app_crop": gt_outside,
            "voxel_size_mm": 0.30}


# ------------------------------------------------------------------ metrics
def evaluate(model, onnx_path, cal_t=None):
    res = {}
    for cid in prep.ALL_CASES:
        feat, yred = case_arrays(cid)
        prob = model.predict_grid(feat)
        lab = prob.argmax(axis=0).astype(np.uint8)
        gt_red = yred.astype(bool)
        r96 = prep.dice_stats(lab.astype(bool), gt_red)
        up = prep.upsample_nearest2(lab).astype(bool)
        gt_app = prep.gt_tooth_at_app_grid(cid).astype(bool)
        r192 = prep.dice_stats(up, gt_app)
        # native 0.30 mm expert annotation vs prediction upsampled x4 (1.2 mm -> 0.3 mm)
        start = prep.crop_start(prep.load_volume(cid))[0]
        r_native = dice_at_native_0p3(cid, lab, start)
        entry = {"case": cid, "split": "train" if cid in prep.CASES_TRAIN else "held_out",
                 "crop_start_0p6mm": [int(s) for s in start],
                 "grid_96x96x64": r96, "grid_192x192x128_nearest": r192,
                 "vs_native_0p30mm_gt": r_native,
                 "oracle_ceiling_gt_vs_gt_at_192": None}
        if cal_t is not None:
            dz_raw = model.logit_diff(feat) + cal_t        # undo the folded offset
            entry["grid_96x96x64_uncalibrated_prob_half"] = prep.dice_stats(
                dz_raw > 0.0, gt_red)
        if cid == prep.CASE_HELD_OUT:
            entry["onnx_numpy_prob_agreement"] = ort_check(onnx_path, feat, prob)
        res[cid] = entry
    return res


def dentalsegmentator_reference():
    """Independent accuracy reference: DS labels 3+4 (upper/lower teeth) vs the
    expert GT tooth union, both at native 0.30 mm resolution."""
    out = {}
    import nibabel as nib
    for cid in ["dentvoxel_0021", "dentvoxel_0047"]:
        ds = prep.load_ds(cid)
        dsm = np.isin(ds, [3, 4]).astype(bool)
        gt = prep.gt_tooth_at_native_0p3(cid).astype(bool)
        st = prep.dice_stats(dsm, gt)
        st["ds_labels_used"] = [3, 4]
        st["ds_label_names"] = {"3": "Upper Teeth", "4": "Lower Teeth"}
        out[cid] = st
    return out


def naive_hu_baseline():
    """Honest trivial reference: HU in the enamel/dentin band inside the crop."""
    out = {}
    for cid in prep.ALL_CASES:
        vol, _ = prep.make_app_volume(cid)
        gt = prep.gt_tooth_at_app_grid(cid).astype(bool)
        band = ((vol >= prep.BAND_HU_LO) & (vol <= prep.BAND_HU_HI)).astype(bool)
        band_red = prep.downsample_gt_to_red(band.astype(np.uint8)).astype(bool)
        _, yred = case_arrays(cid)
        out[cid] = {"at_96_grid": prep.dice_stats(band_red, yred.astype(bool))}
    return out


def main():
    t0 = time.time()
    log = []
    # ---- phase 1: fit on 0021+0047, monitor 0074 (epoch/hyper-parameter choice)
    Xa, Ya = sample_case("dentvoxel_0021", 0)
    Xb, Yb = sample_case("dentvoxel_0047", 1)
    Xv, Yv = sample_case("dentvoxel_0074", 2)
    Xtr = np.concatenate([Xa, Xb]); Ytr = np.concatenate([Ya, Yb])
    print("phase1 fit=%d val_sampled=%d" % (len(Ytr), len(Yv)))
    model = Mlp(prep.TRAIN_SEED)
    MAX_EPOCHS = 120
    val_cb = lambda m: calibrated_dice(m, ["dentvoxel_0074"])
    hist, iters1 = train(model, Xtr, Ytr, MAX_EPOCHS, val_cb=val_cb, eval_every=5)
    best = max(hist, key=lambda h: h["val_dice"])
    chosen_epochs = best["epoch"]
    print("chosen epochs (by calibrated val Dice on 0074) =", chosen_epochs, best)
    log.append("phase1: fit on 0021+0047 (%d sampled voxels), monitor 0074 (%d sampled "
               "voxels / full 96^3 grid scored), %d Adam iterations, best val Dice %.4f "
               "at epoch %d.  val metric = Dice at the per-case Dice-optimal logit "
               "threshold, i.e. the same threshold calibration folded into b3 later."
               % (len(Ytr), len(Yv), iters1, best["val_dice"], chosen_epochs))

    # ---- phase 2: refit on all three training cases for the chosen epoch count
    Xall = np.concatenate([Xtr, Xv]); Yall = np.concatenate([Ytr, Yv])
    final = Mlp(prep.TRAIN_SEED)
    hist2, iters2 = train(final, Xall, Yall, chosen_epochs, val_cb=None)
    log.append("phase2: final refit on 0021+0047+0074 (%d sampled voxels), %d Adam "
               "iterations, %d epochs (chosen in phase 1)" % (len(Yall), iters2, chosen_epochs))

    # ---- prior calibration: single scalar folded into the exported b3 so that the
    #      device-side argmax(prob) already implements the corrected decision.
    cal = calibrate(final, prep.CASES_TRAIN)
    log.append("calibration: threshold t*=%.6f on (z1-z0) maximises pooled Dice over "
               "0021+0047+0074 (%.4f); b3 = [%.6f, %.6f] -> [%.6f, %.6f].  dentvoxel_0101 "
               "was not used for this or for the epoch choice."
               % (cal["threshold_on_logit_diff"],
                  cal["dice_at_threshold_on_calibration_cases"]["dice"],
                  cal["b3_before"][0], cal["b3_before"][1],
                  cal["b3_after"][0], cal["b3_after"][1]))

    onnx_path = f"{OUT}/model/teeth_cnn.onnx"
    os.makedirs(f"{OUT}/model", exist_ok=True)
    # Another host process in this workspace has been writing the SAME path with a
    # non-compliant 3x3x3 Conv3d variant.  Snapshot whatever is there before we
    # overwrite it, so nothing is destroyed, and keep our own byte-identical backup
    # copy at model/teeth_cnn_pointwise_mlp.onnx.  metrics.json records the sha256.
    if os.path.exists(onnx_path):
        prev_sha = hashlib.sha256(open(onnx_path, "rb").read()).hexdigest()
        shutil.copy2(onnx_path, f"{OUT}/work/pre_existing_teeth_cnn_snapshot.onnx")
        print("pre-existing %s sha256 %s (snapshotted to work/)" % (onnx_path, prev_sha))
    export_onnx(final, onnx_path)
    onnx_sha = hashlib.sha256(open(onnx_path, "rb").read()).hexdigest()
    backup_path = f"{OUT}/model/teeth_cnn_pointwise_mlp.onnx"
    shutil.copy2(onnx_path, backup_path)
    fsz = os.path.getsize(onnx_path)
    print("exported %s (%d B, sha256 %s)" % (onnx_path, fsz, onnx_sha))

    metrics = evaluate(final, onnx_path, cal_t=cal["threshold_on_logit_diff"])
    # oracle ceiling: GT at 0.30 mm voted down to 0.60 mm vs GT at 192 grid == 1.0 by
    # construction; the meaningful ceiling is the 2x2x2 vote information loss, reported
    # as the Dice of the *training target itself* (yred upsampled vs gt_app).
    for cid, e in metrics.items():
        _, yred = case_arrays(cid)
        e["oracle_ceiling_gt_vs_gt_at_192"] = prep.dice_stats(
            prep.upsample_nearest2(yred).astype(bool),
            prep.gt_tooth_at_app_grid(cid).astype(bool))
        e["feature_file"] = f"{CACHE}/{cid}_feat.npy"

    out_dsr = dentalsegmentator_reference()
    out_nhb = naive_hu_baseline()
    out = {
        "generated_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "model": {
            "onnx_path": onnx_path, "onnx_bytes": fsz, "opset": prep.OPSET,
            "onnx_sha256": onnx_sha, "onnx_backup_path": backup_path,
            "parameter_count": final.n_params(),
            "architecture": "Conv(6->24,1x1x1)+Relu+Conv(24->24,1x1x1)+Relu+Conv(24->2,1x1x1)+Softmax",
            "optimizer": "Adam, lr=%g, betas=(%g,%g), eps=%g" % (prep.LEARNING_RATE, prep.ADAM_BETA1,
                                                                 prep.ADAM_BETA2, prep.ADAM_EPS),
            "minibatch": prep.MINIBATCH, "epochs_final": chosen_epochs,
            "iterations_phase1": iters1, "iterations_final": iters2,
            "total_iterations": iters1 + iters2,
            "sampling_rule": "all positives + negative pool (TRAIN_STRIDE=%d -> every "
                             "label-0 reduced-grid voxel) capped at NEG_PER_POS=%d x "
                             "#positives via sorted(rng(TRAIN_SEED+case_index)."
                             "permutation)[:k]" % (prep.TRAIN_STRIDE, prep.NEG_PER_POS),
            "sample_counts": {"0021": int(len(Ya)), "0047": int(len(Yb)), "0074": int(len(Yv)),
                              "final_fit": int(len(Yall)), "final_positives": int(Yall.sum())},
            "calibration": cal,
            "epoch_choice": {"criterion": "Dice at the Dice-optimal logit threshold on the "
                                         "full 96x96x64 grid of dentvoxel_0074",
                             "max_epochs_searched": MAX_EPOCHS, "chosen": chosen_epochs,
                             "phase1_history": hist},
            "wall_clock_seconds": None,
        },
        "held_out": prep.CASE_HELD_OUT,
        "feature_variant_study": (json.load(open(f"{OUT}/work/variant_study.json"))
                                  if os.path.exists(f"{OUT}/work/variant_study.json") else None),
        "cases": metrics,
        "reference_dentalsegmentator_vs_gt": out_dsr,
        "reference_naive_hu_band": out_nhb,
        "reference": {"dentalsegmentator_labels_3_plus_4_vs_gt_tooth_union_native_0p30mm":
                      out_dsr,
                      "naive_hu_band_1200_4000_at_96_grid": out_nhb,
                      "comment": "independent accuracy references measured with the same "
                                 "GT definition; the DentalSegmentator numbers are the "
                                 "achievable level for a real 3D CNN"},
        "notes": log,
    }
    out["model"]["wall_clock_seconds"] = round(time.time() - t0, 1)
    json.dump(out, open(f"{OUT}/metrics.json", "w"), indent=1)
    np.savez(f"{OUT}/work/model_weights.npz", W1=final.W1, b1=final.b1, W2=final.W2,
             b2=final.b2, W3=final.W3, b3=final.b3)
    # human/C++ readable copy of the exact exported weights
    json.dump({"note": "exact float32 values stored in %s.  numpy Wk is (in,out); "
                       "the ONNX Conv initialiser is its transpose reshaped to "
                       "(out,in,1,1,1).  prob = softmax(z3, axis=1)." % onnx_path,
               "numpy_layout": {"z1": "relu(feat . W1 + b1)", "z2": "relu(z1 . W2 + b2)",
                                "z3": "z2 . W3 + b3"},
               "shapes": {"W1": list(final.W1.shape), "b1": list(final.b1.shape),
                          "W2": list(final.W2.shape), "b2": list(final.b2.shape),
                          "W3": list(final.W3.shape), "b3": list(final.b3.shape)},
               "W1": final.W1.tolist(), "b1": final.b1.tolist(),
               "W2": final.W2.tolist(), "b2": final.b2.tolist(),
               "W3": final.W3.tolist(), "b3": final.b3.tolist()},
              open(f"{OUT}/work/model_weights.json", "w"), indent=1)
    h = out["held_out"]
    c = metrics[h]
    print("\n== HELD-OUT %s ==" % h)
    print("  96^3 grid   : dice %.4f  precision %.4f  recall %.4f  f1 %.4f  (tp %d fp %d fn %d)"
          % (c["grid_96x96x64"]["dice"], c["grid_96x96x64"]["precision"],
             c["grid_96x96x64"]["recall"], c["grid_96x96x64"]["f1"],
             c["grid_96x96x64"]["tp"], c["grid_96x96x64"]["fp"], c["grid_96x96x64"]["fn"]))
    print("  192 grid    : dice %.4f  f1 %.4f" % (c["grid_192x192x128_nearest"]["dice"],
                                                  c["grid_192x192x128_nearest"]["f1"]))
    print("  native 0.30 : dice %.4f" % c["vs_native_0p30mm_gt"]["dice"])
    print("  onnx vs numpy prob maxdiff: %.3e" % c["onnx_numpy_prob_agreement"]["ort_max_abs_diff_vs_numpy"])
    for cid, st in out["reference_dentalsegmentator_vs_gt"].items():
        print("  DS-vs-GT %s : dice %.4f" % (cid, st["dice"]))
    print("onnx bytes", fsz, "params", final.n_params(), "wall", out["model"]["wall_clock_seconds"])


if __name__ == "__main__":
    main()
