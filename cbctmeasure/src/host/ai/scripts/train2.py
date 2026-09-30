"""train2.py -- numpy-only training of a REAL 3x3x3 Conv3d tooth net, in DEVICE
AXIS ORDER, with a mandatory finite-difference gradient check, patch training,
full-grid validation, threshold calibration, ONNX export (opset<=17) and metrics.

Contract (see SPEC.md):
    input  'feat'  float32 [1, 6, 96, 96, 64]   -- DEVICE axis order
    output 'prob'  float32 [1, 2, 96, 96, 64]   softmax over channel axis, opset 17
    arch: Conv3d(6->8,k3,p1) Relu Conv3d(8->8,k3,p1) Relu Conv3d(8->2,k3,p1) Softmax

DEVICE AXIS ORDER (verified against a pydicom re-read of the DICOM series, which
is the authority -- see parity.py::read_dicom_series_as_parser):
    The Android CbctSeriesParser sets width=Columns, height=Rows and lays the
    volume out as [depth][height][width], so on device axis0 = host axis1 and
    axis1 = host axis0 (an in-plane transpose).  Host canonical arrays from
    prep.py are (x=Rows, y=Columns, z).  Therefore
        feat_dev = feat_canonical.transpose(0, 2, 1, 3)
        yred_dev = yred_canonical.transpose(1, 0, 2)
    i.e. feat_dev[c][i][j][k] with i = DICOM column, j = DICOM row, k = z.
    A 3x3x3 conv with trained (non-symmetric) kernels is NOT equivariant under
    that swap, so the model is TRAINED and EXPORTED in device order.

Split discipline: train 0021+0047, val 0074 (early stopping + threshold
calibration), test 0101 (single final evaluation).
"""
import json
import os
import sys
import time
import numpy as np
import prep

OUT = prep.OUT_DIR
CACHE = f"{OUT}/work/cache"
MODEL_DIR = f"{OUT}/model"
PARITY_DIR = f"{OUT}/parity_ref"

# ---- architecture / training hyper-parameters (this script's own contract) ----
CH_IN, CH_H1, CH_H2, CH_OUT = 6, 8, 8, 2
K = 3                          # 3x3x3 everywhere
PAD = 1
PATCH = 16                     # cubic training patch
PATCHES_PER_EPOCH = 512
PATCHES_PER_UPDATE = 8         # gradient accumulation: 128 Adam updates / epoch
LR = 0.02
BETA1, BETA2, AEPS = 0.9, 0.999, 1e-8
WEIGHT_DECAY = 1e-4
POS_CENTER_PROB = 0.5          # ~50% of patch centers on positive voxels
NEG_W = 4.0                    # NEG_PER_POS=4 style balance, via per-patch weights
MAX_EPOCHS = 150
PATIENCE = 20                  # epochs without full-grid val-Dice improvement
SEED = 20260101

TRAIN_CASES = ["dentvoxel_0021", "dentvoxel_0047"]
VAL_CASE = "dentvoxel_0074"
TEST_CASE = "dentvoxel_0101"


# ============================================================== conv machinery
# Layout: X has shape (C, H, W, D); H = device axis0 (DICOM column),
# W = device axis1 (DICOM row), D = axis2 (z).  Weights (Co, Ci, 3, 3, 3) in
# ONNX correlation orientation:
#   Y[o,x,y,z] = sum_{i,a,b,c} W[o,i,a,b,c] * Xpad[i,x+a,y+b,z+c], zero pad 1.

def _im2col(X):
    """Padded input -> (cols (27*Ci, N), Xp).  Row order: (a,b,c)-major, i-minor
    (matches _w2d).  Keep Xp: conv_backward accumulates dX in the same frame."""
    Ci, H, W_, D = X.shape
    Xp = np.pad(X, ((0, 0), (1, 1), (1, 1), (1, 1)))
    blocks = [Xp[:, a:a + H, b:b + W_, c:c + D].reshape(Ci, -1)
              for a in range(3) for b in range(3) for c in range(3)]
    return np.concatenate(blocks, axis=0), Xp


def _w2d(W):
    """(Co,Ci,3,3,3) -> (Co, 27*Ci) with cols in (a,b,c)-major, i-minor order."""
    return np.transpose(W, (0, 2, 3, 4, 1)).reshape(W.shape[0], -1)


def conv_forward(X, W, b):
    Ci = X.shape[0]
    cols, _ = _im2col(X)
    Y = (_w2d(W) @ cols + b[:, None])
    if Ci > 4:                       # free the big col matrix before the view
        cols = None
    return Y.reshape(W.shape[0], *X.shape[1:])


def conv_backward_from_cols(Xp, W, dY, cols):
    """Backward given the padded input, its im2col matrix and upstream dY.
    Xp is NOT modified (it is a view chain of the input in forward mode)."""
    Ci, Hp, Wp, Dp = Xp.shape
    H, W_, D = Hp - 2, Wp - 2, Dp - 2
    Co = W.shape[0]
    dt = np.float64 if Xp.dtype == np.float64 else np.float32
    dY2 = dY.reshape(Co, -1)
    dW2 = dY2 @ cols.T                                   # (Co, 27*Ci)
    db = dY2.sum(axis=1)
    dcols = _w2d(W).T.astype(dt, copy=False) @ dY2.astype(dt, copy=False)
    dXp = np.zeros_like(Xp, dtype=dt)
    n = 0
    for a in range(3):
        for b in range(3):
            for c in range(3):
                dXp[:, a:a + H, b:b + W_, c:c + D] += dcols[n * Ci:(n + 1) * Ci].reshape(Ci, H, W_, D)
                n += 1
    dX = dXp[:, 1:-1, 1:-1, 1:-1]
    dW = dW2.reshape(Co, 3, 3, 3, Ci).transpose(0, 4, 1, 2, 3)
    return (dX.astype(Xp.dtype, copy=False), dW.astype(W.dtype, copy=False),
            db.astype(W.dtype, copy=False))


def conv_backward(X, W, dY):
    """Standalone (Ci,H,W,D) version used by the gradient check."""
    cols, Xp = _im2col(X)
    return conv_backward_from_cols(Xp, W, dY, cols)


def conv_stage_slabs(X, W, b, relu, slab=16):
    """Full-grid one conv (+relu), exact, batched along the D axis, halo of 1."""
    C = W.shape[0]
    Y = np.zeros((C, *X.shape[1:]), dtype=np.float32)
    D = X.shape[3]
    for z0 in range(0, D, slab):
        z1 = min(D, z0 + slab)
        a0, a1 = max(0, z0 - 1), min(D, z1 + 1)
        Ys = conv_forward(X[:, :, :, a0:a1], W, b)
        if relu:
            Ys = np.maximum(Ys, 0)
        Y[:, :, :, z0:z1] = Ys[:, :, :, (z0 - a0):(z1 - a0)]
    return Y


class Net:
    def __init__(self, seed):
        rng = np.random.default_rng(seed)
        self.W1 = (rng.standard_normal((CH_H1, CH_IN, K, K, K)) * np.sqrt(2.0 / (CH_IN * 27))).astype(np.float32)
        self.b1 = np.zeros(CH_H1, np.float32)
        self.W2 = (rng.standard_normal((CH_H2, CH_H1, K, K, K)) * np.sqrt(2.0 / (CH_H1 * 27))).astype(np.float32)
        self.b2 = np.zeros(CH_H2, np.float32)
        self.W3 = (rng.standard_normal((CH_OUT, CH_H2, K, K, K)) * np.sqrt(2.0 / (CH_H2 * 27))).astype(np.float32)
        self.b3 = np.zeros(CH_OUT, np.float32)

    def params(self):
        return [self.W1, self.b1, self.W2, self.b2, self.W3, self.b3]

    def n_params(self):
        return sum(p.size for p in self.params())

    def forward_grid(self, feat):
        z1 = conv_stage_slabs(feat, self.W1, self.b1, False)
        a1 = np.maximum(z1, 0)
        z2 = conv_stage_slabs(a1, self.W2, self.b2, False)
        a2 = np.maximum(z2, 0)
        return conv_stage_slabs(a2, self.W3, self.b3, False)   # logits (2,96,96,64)

    def predict_proba(self, feat):
        Z = self.forward_grid(feat).astype(np.float64)
        E = np.exp(Z - Z.max(axis=0, keepdims=True))
        return (E / E.sum(axis=0, keepdims=True)).astype(np.float32)


# --------------------------------------------------------- patch fwd/bwd + loss
def patch_loss_grads(net, X, yv, neg_w=NEG_W):
    """X (6,P,P,P) float32/64 patch, yv (P,P,P) 0/1.
    Weighted softmax-CE: positive voxels weight 1, negatives weight
    neg_w*npos/nnege so each patch contributes pos:neg loss mass exactly 1:neg_w
    (NEG_PER_POS=4 style class balancing).  Returns (loss, param_grads, dX).
    im2col matrices from the forward pass are reused by the backward pass."""
    cols1, Xp1 = _im2col(X)
    z1 = (_w2d(net.W1) @ cols1 + net.b1[:, None]).reshape(CH_H1, *X.shape[1:])
    a1 = np.maximum(z1, 0)
    cols2, Xp2 = _im2col(a1)
    z2 = (_w2d(net.W2) @ cols2 + net.b2[:, None]).reshape(CH_H2, *X.shape[1:])
    a2 = np.maximum(z2, 0)
    cols3, Xp3 = _im2col(a2)
    z3 = (_w2d(net.W3) @ cols3 + net.b3[:, None]).reshape(CH_OUT, *X.shape[1:])
    Z = z3.reshape(2, -1).astype(np.float64)
    N = Z.shape[1]
    E = np.exp(Z - Z.max(axis=0, keepdims=True))
    P = E / E.sum(axis=0, keepdims=True)
    lab = yv.reshape(-1)
    pos = int(lab.sum())
    neg = N - pos
    if pos > 0 and neg > 0:
        w = np.where(lab == 1, 1.0, neg_w * pos / neg)
    else:
        w = np.ones(N)
    Wsum = w.sum()
    l = -np.log(np.clip(P[lab, np.arange(N)], 1e-12, None))
    loss = float((w * l).sum() / Wsum)
    dZ = P.copy()
    dZ[lab, np.arange(N)] -= 1.0
    dZ *= (w / Wsum)
    dz3 = dZ.reshape(z3.shape).astype(X.dtype, copy=False)
    dx3, dW3, db3 = conv_backward_from_cols(Xp3, net.W3, dz3, cols3)
    dz2 = dx3 * (a2 > 0)
    dx2, dW2, db2 = conv_backward_from_cols(Xp2, net.W2, dz2, cols2)
    dz1 = dx2 * (a1 > 0)
    _, dW1, db1 = conv_backward_from_cols(Xp1, net.W1, dz1, cols1)
    return loss, [dW1, db1, dW2, db2, dW3, db3], dx3


# ------------------------------------------------------------ gradient check
def gradient_check(net=None, patch=8, eps=1e-3, n_per=3):
    """Central finite-difference check of the whole conv backward chain in
    float64.  Checks 3 random entries of every parameter tensor."""
    if net is None:
        net = Net(SEED)
    net64 = Net(7)
    for p, q in zip(net64.params(), net.params()):
        p[:] = q.astype(np.float64)
    rng = np.random.default_rng(1234)
    X = rng.standard_normal((CH_IN, patch, patch, patch))
    yv = (rng.random((patch, patch, patch)) < 0.3).astype(np.int64)
    loss, G, _ = patch_loss_grads(net64, X, yv)
    results = []
    max_rel = 0.0
    for pi, g in enumerate(G):
        idxs = np.random.default_rng(100 + pi).choice(
            g.size, min(n_per, g.size), replace=False)
        P = net64.params()[pi]
        for ti in idxs:
            orig = float(P.flat[ti])
            P.flat[ti] = orig + eps
            lp, _, _ = patch_loss_grads(net64, X, yv)
            P.flat[ti] = orig - eps
            lm, _, _ = patch_loss_grads(net64, X, yv)
            P.flat[ti] = orig
            num = (lp - lm) / (2 * eps)
            ana = float(g.flat[ti])
            rel = abs(num - ana) / max(1e-12, abs(num) + abs(ana))
            max_rel = max(max_rel, rel)
            results.append({"param": pi, "flat_index": int(ti), "analytic": ana,
                            "numeric": float(num), "rel_err": float(rel)})
    return {"ok": max_rel < 1e-4, "max_rel_err": max_rel, "eps": eps,
            "patch": patch, "loss_at_check": float(loss), "checks": results}


# ---------------------------------------------------------------------- data
def dev_case_arrays(cid):
    """Cached canonical arrays -> DEVICE order. feat (6,96,96,64) f32, y u1."""
    feat = np.load(f"{CACHE}/{cid}_feat.npy")
    yred = np.load(f"{CACHE}/{cid}_yred.npy")
    fd = np.ascontiguousarray(feat.transpose(0, 2, 1, 3))
    yd = np.ascontiguousarray(yred.transpose(1, 0, 2)).astype(np.uint8)
    return fd, yd


def make_patch_sampler(feat, y):
    pos = np.argwhere(y == 1)
    shape = np.array(feat.shape[1:])
    lo = np.zeros(3, int)
    hi = shape - PATCH

    def sample(rng, n):
        starts = np.empty((n, 3), int)
        npos = int(round(n * POS_CENTER_PROB))
        for i in range(n):
            if i < npos and len(pos):
                c = pos[rng.integers(len(pos))]
            else:
                c = rng.integers(shape)
            starts[i] = np.clip(c - PATCH // 2, lo, hi)
        return starts
    return sample


def adam_init(params):
    return ([np.zeros_like(p, np.float64) for p in params],
            [np.zeros_like(p, np.float64) for p in params])


def adam_update(net, grads, m, v, t):
    ps = net.params()
    for i in range(len(ps)):
        p = ps[i]
        gi = grads[i].astype(np.float64) + WEIGHT_DECAY * p.astype(np.float64)
        m[i] = BETA1 * m[i] + (1 - BETA1) * gi
        v[i] = BETA2 * v[i] + (1 - BETA2) * gi * gi
        mh = m[i] / (1 - BETA1 ** t)
        vh = v[i] / (1 - BETA2 ** t)
        newp = p.astype(np.float64) - LR * mh / (np.sqrt(vh) + AEPS)
        p[:] = newp.astype(np.float32)


# --------------------------------------------------------------------- metrics
def dice_at(prob1, y, t):
    return prep.dice_stats(prob1 > t, y.astype(bool))


def fbeta(p, r, beta=0.5):
    if p + r == 0:
        return 0.0
    b2 = beta * beta
    return (1 + b2) * p * r / (b2 * p + r)


def cc_stats(label, vox_mm3=1.728, min_size=8):
    """26-connectivity CC stats after dropping components < min_size voxels,
    renumbered by descending size (documented post-processing)."""
    from scipy import ndimage
    lab, n = ndimage.label(label, structure=np.ones((3, 3, 3), int))
    if n == 0:
        return {"n_components_raw": 0, "min_size_filter": min_size,
                "n_components_filtered": 0, "voxel_size_mm": 1.2,
                "voxel_volume_mm3": vox_mm3, "largest_cm3": 0.0, "smallest_cm3": 0.0,
                "component_volumes_cm3": [],
                "plausible_single_tooth_range_cm3": [0.15, 2.5],
                "n_plausible_0p15_2p5_cm3": 0}
    sizes = np.bincount(lab.ravel())[1:]
    keep_sizes = sizes[sizes >= min_size]
    vols = np.sort(keep_sizes)[::-1] * vox_mm3 / 1000.0
    plaus = int(((keep_sizes * vox_mm3 / 1000.0 >= 0.15) &
                 (keep_sizes * vox_mm3 / 1000.0 <= 2.5)).sum())
    return {"n_components_raw": int(n), "min_size_filter": min_size,
            "n_components_filtered": int(len(keep_sizes)),
            "voxel_size_mm": 1.2, "voxel_volume_mm3": vox_mm3,
            "largest_cm3": float(vols[0]) if len(vols) else 0.0,
            "smallest_cm3": float(vols[-1]) if len(vols) else 0.0,
            "component_volumes_cm3": [float(x) for x in vols],
            "plausible_single_tooth_range_cm3": [0.15, 2.5],
            "n_plausible_0p15_2p5_cm3": plaus}


# ------------------------------------------------------------------ main flow
def main():
    t0 = time.time()
    os.makedirs(MODEL_DIR, exist_ok=True)
    os.makedirs(PARITY_DIR, exist_ok=True)

    # ---------- step 0: axis-order verification against the DICOM re-read ----
    import parity
    axis_check = parity.verify_axis_order(CACHE)
    print("[axis] canonical->device transpose vs DICOM-parser re-read: "
          "max|diff| = %.3e  ok=%s" % (axis_check["max_abs_diff_feat"], axis_check["ok"]))
    if not axis_check["ok"]:
        print("[axis] DISCREPANCY -- the DICOM re-read is the authority; aborting "
              "so the cache/transpose assumption can be fixed. Details:")
        print(json.dumps(axis_check, indent=1))
        sys.exit(2)

    # ---------- step 1: gradient check (MANDATORY) ----------------------------
    gc = gradient_check(patch=12, eps=1e-4, n_per=3)
    print("[gradcheck] max rel err = %.3e  ok=%s (%d params checked, loss %.4f)"
          % (gc["max_rel_err"], gc["ok"], len(gc["checks"]), gc["loss_at_check"]))
    for c in gc["checks"][:6]:
        print("   param %d idx %8d analytic %+.6e numeric %+.6e rel %.2e"
              % (c["param"], c["flat_index"], c["analytic"], c["numeric"], c["rel_err"]))
    if not gc["ok"]:
        print("[gradcheck] FAILED -- backward is wrong, aborting before training.")
        sys.exit(3)

    # ---------- step 2: data --------------------------------------------------
    data = {}
    for cid in TRAIN_CASES + [VAL_CASE, TEST_CASE]:
        data[cid] = dev_case_arrays(cid)
        print("[data] %s feat %s  positive voxels %d" %
              (cid, data[cid][0].shape, int(data[cid][1].sum())))

    net = Net(SEED)
    print("[net] params:", net.n_params())
    samplers = {cid: make_patch_sampler(*data[cid]) for cid in TRAIN_CASES}

    m, v = adam_init(net.params())
    t = 0
    history = []
    best = {"val_dice": -1.0, "epoch": -1, "state": None}
    since_best = 0
    upd_per_epoch = PATCHES_PER_EPOCH // PATCHES_PER_UPDATE

    for ep in range(1, MAX_EPOCHS + 1):
        rng = np.random.default_rng(SEED + ep)
        losses = []
        for u in range(upd_per_epoch):
            accG = None
            for bidx in range(PATCHES_PER_UPDATE):
                cid = TRAIN_CASES[(u * PATCHES_PER_UPDATE + bidx) % len(TRAIN_CASES)]
                feat, y = data[cid]
                s0, s1, s2 = (int(x) for x in samplers[cid](rng, 1)[0])
                Xp = np.ascontiguousarray(feat[:, s0:s0 + PATCH, s1:s1 + PATCH, s2:s2 + PATCH])
                yp = y[s0:s0 + PATCH, s1:s1 + PATCH, s2:s2 + PATCH]
                loss, G, _ = patch_loss_grads(net, Xp, yp)
                losses.append(loss)
                accG = [g.astype(np.float32) for g in G] if accG is None else \
                    [a + g for a, g in zip(accG, G)]
            t += 1
            adam_update(net, [g / PATCHES_PER_UPDATE for g in accG], m, v, t)

        # ---- full-grid validation (exact, no subsampling) ----
        fv, yv = data[VAL_CASE]
        prob = net.predict_proba(fv)
        st05 = prep.dice_stats(prob[1] > 0.5, yv.astype(bool))
        cur = {"epoch": ep, "loss": float(np.mean(losses)),
               "val_dice_0p5": st05["dice"], "val_prec": st05["precision"],
               "val_rec": st05["recall"], "val_tp": st05["tp"], "val_fp": st05["fp"],
               "val_fn": st05["fn"]}
        if ep % 5 == 0 or ep == 1:
            ft, yt = data[TRAIN_CASES[0]]
            cur["train0021_dice_0p5"] = prep.dice_stats(
                net.predict_proba(ft)[1] > 0.5, yt.astype(bool))["dice"]
        history.append(cur)
        improved = st05["dice"] > best["val_dice"]
        if improved:
            best = {"val_dice": st05["dice"], "epoch": ep,
                    "state": [p.copy() for p in net.params()]}
            since_best = 0
        else:
            since_best += 1
        if ep % 5 == 0 or improved or since_best == PATIENCE:
            print("[ep %3d] loss %.4f | VAL 0074 dice@0.5 %.4f prec %.4f rec %.4f "
                  "(tp %d fp %d fn %d) | train0021 %.4f | best %.4f@%d since %d"
                  % (ep, cur["loss"], st05["dice"], st05["precision"], st05["recall"],
                     st05["tp"], st05["fp"], st05["fn"],
                     cur.get("train0021_dice_0p5", float("nan")),
                     best["val_dice"], best["epoch"], since_best), flush=True)
        if since_best >= PATIENCE:
            print("[stop] early stopping at epoch %d (best %.4f @ epoch %d)"
                  % (ep, best["val_dice"], best["epoch"]), flush=True)
            break
    epochs_run = ep
    if best["state"] is not None:
        for p, q in zip(net.params(), best["state"]):
            p[:] = q
    print("[restore] best params from epoch %d, val dice@0.5 %.4f"
          % (best["epoch"], best["val_dice"]))

    # ---------- step 3: threshold calibration on VAL (0074) ------------------
    fv, yv = data[VAL_CASE]
    prob_val = net.predict_proba(fv)
    thresholds = [round(float(x), 4) for x in np.arange(0.05, 0.9501, 0.01)]
    curve = []
    for th in thresholds:
        s = dice_at(prob_val[1], yv, th)
        curve.append({"threshold": th, "dice": s["dice"], "precision": s["precision"],
                      "recall": s["recall"], "f1": s["f1"]})
    bestc = max(curve, key=lambda c: (c["dice"], -c["threshold"]))
    chosen_t = bestc["threshold"]
    bestf1 = max(curve, key=lambda c: (c["f1"], -c["threshold"]))
    print("[calib] val-best threshold %.2f dice %.4f prec %.4f rec %.4f f1 %.4f "
          "| f1-best %.2f (%.4f)" % (chosen_t, bestc["dice"], bestc["precision"],
          bestc["recall"], bestc["f1"], bestf1["threshold"], bestf1["f1"]))

    # ---------- step 4: final evaluation --------------------------------------
    def eval_case(cid, with_cc=False):
        f, y = data[cid]
        pr = net.predict_proba(f)
        r05 = dice_at(pr[1], y, 0.5)
        rch = dice_at(pr[1], y, chosen_t)
        e = {"case": cid, "grid": "dev (96,96,64) [col,row,z]",
             "at_0p5": r05, "at_chosen": rch,
             "f1_beta0p5_at_chosen": fbeta(rch["precision"], rch["recall"], 0.5),
             "iou_at_chosen": rch["tp"] / max(1, rch["tp"] + rch["fp"] + rch["fn"])}
        if with_cc:
            lab = (pr[1] > chosen_t).astype(np.uint8)
            e["cc_26conn_min8_at_chosen"] = cc_stats(lab)
            e["cc_gt_reference_26conn_min8"] = cc_stats(y.astype(np.uint8))
        return e

    res = {cid: eval_case(cid, with_cc=(cid == TEST_CASE))
           for cid in TRAIN_CASES + [VAL_CASE, TEST_CASE]}
    te = res[TEST_CASE]
    print("\n== TEST %s (chosen threshold %.2f) ==" % (TEST_CASE, chosen_t))
    s = te["at_chosen"]
    print("  dice %.4f prec %.4f rec %.4f f1 %.4f (tp %d fp %d fn %d)"
          % (s["dice"], s["precision"], s["recall"], s["f1"], s["tp"], s["fp"], s["fn"]))
    print("  @0.5:      dice %.4f prec %.4f rec %.4f f1 %.4f" %
          (te["at_0p5"]["dice"], te["at_0p5"]["precision"],
           te["at_0p5"]["recall"], te["at_0p5"]["f1"]))
    print("  f1(beta=0.5) %.4f   IoU %.4f" % (te["f1_beta0p5_at_chosen"], te["iou_at_chosen"]))
    cc = te["cc_26conn_min8_at_chosen"]
    print("  CC: raw %d -> kept %d (>=8 vox), largest %.4f cm3, smallest %.4f cm3, "
          "plausible(0.15-2.5cm3) %d" % (cc["n_components_raw"], cc["n_components_filtered"],
          cc["largest_cm3"], cc["smallest_cm3"], cc["n_plausible_0p15_2p5_cm3"]))
    f1v = te["at_chosen"]["f1"]
    print("  PRD AI-01 target F1>=0.85: %s (F1=%.4f @ t=%.2f)"
          % ("MET" if f1v >= 0.85 else "MISSED", f1v, chosen_t))

    # ---------- step 5: ONNX export + weights --------------------------------
    onnx_path = f"{MODEL_DIR}/teeth_cnn.onnx"
    old_backup = f"{MODEL_DIR}/teeth_cnn_pointwise_old.onnx"
    if os.path.exists(onnx_path) and not os.path.exists(old_backup):
        import shutil
        shutil.copy2(onnx_path, old_backup)
        print("[export] old pointwise model backed up to", old_backup)
    import export2
    export2.export_onnx(net, onnx_path)
    onnx_bytes = os.path.getsize(onnx_path)
    np.savez(f"{MODEL_DIR}/teeth_cnn_weights.npz",
             W1=net.W1, b1=net.b1, W2=net.W2, b2=net.b2, W3=net.W3, b3=net.b3,
             threshold=np.float32(chosen_t),
             arch=np.array([CH_IN, CH_H1, CH_H2, CH_OUT, K]))
    np_prob = net.predict_proba(data[TEST_CASE][0])
    ort_info, _ort_got = parity.ort_check(onnx_path, data[TEST_CASE][0], np_prob)
    print("[onnx] opset %d  ort-vs-numpy max|diff| %.3e  ops %s  bytes %d"
          % (ort_info["opset"], ort_info["ort_max_abs_diff_vs_numpy"],
             ",".join(ort_info["ort_ops"]), onnx_bytes))
    assert ort_info["ort_max_abs_diff_vs_numpy"] < 1e-4

    # ---------- step 6: dump artifacts ----------------------------------------
    out = {
        "generated_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "architecture": "Conv3d(6->8,k3,pad1)+Relu+Conv3d(8->8,k3,pad1)+Relu+Conv3d(8->2,k3,pad1)+Softmax(axis=1)",
        "axis_order": "DEVICE: feat[c][i][j][k], i=DICOM column (host axis1), j=DICOM row (host axis0), k=z",
        "parameter_count": net.n_params(),
        "onnx_path": onnx_path, "onnx_bytes": onnx_bytes,
        "onnx_check": ort_info,
        "training": {
            "optimizer": "Adam lr=%g betas=(%g,%g) eps=%g l2=%g" % (LR, BETA1, BETA2, AEPS, WEIGHT_DECAY),
            "patches_per_epoch": PATCHES_PER_EPOCH, "patch": PATCH,
            "patches_per_update": PATCHES_PER_UPDATE,
            "pos_center_prob": POS_CENTER_PROB,
            "loss": "weighted softmax-CE, per-patch pos:neg loss mass 1:4 (NEG_PER_POS=4 style)",
            "train_cases": TRAIN_CASES, "val_case": VAL_CASE, "test_case": TEST_CASE,
            "max_epochs": MAX_EPOCHS, "patience": PATIENCE, "seed": SEED,
            "epochs_run": epochs_run, "best_epoch": best["epoch"],
            "best_val_dice_0p5": best["val_dice"], "adam_updates": t,
            "history": history},
        "gradient_check": gc,
        "axis_verification_dicom_reread": axis_check,
        "threshold_calibration": {"swept_from_to_step": [0.05, 0.95, 0.01],
                                  "chosen_threshold": chosen_t,
                                  "chosen_by": "max Dice on val case 0074 full grid",
                                  "val_curve": curve,
                                  "f1_best_threshold": bestf1["threshold"],
                                  "f1_best_value": bestf1["f1"]},
        "metrics": res,
        "prd_ai01_target_f1_0p85": {"f1_at_chosen_on_test": f1v,
                                    "met": bool(f1v >= 0.85)},
        "wall_clock_seconds": round(time.time() - t0, 1),
    }
    json.dump(out, open(f"{PARITY_DIR}/metrics_0101.json", "w"), indent=1)
    json.dump(out, open(f"{OUT}/work/train2_full.json", "w"), indent=1)
    print("[done] wall %.1fs  -> parity_ref/metrics_0101.json" % (time.time() - t0))


if __name__ == "__main__":
    main()
