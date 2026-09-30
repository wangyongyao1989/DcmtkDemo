"""variants.py -- legitimate hyper-parameter study of the feature channel design.

Only the TRAIN-split cases are touched: fit on 0021+0047, score the calibrated Dice
on 0074 (that is exactly the phase-1 protocol of train_model.py).  dentvoxel_0101 is
never loaded here.  The winner becomes FEAT_RADII / BANDPASS_PAIR in prep.py.
"""
import json
import time
import numpy as np
import prep
import train_model as tm

VARIANTS = [
    {"radii": (1, 2, 4, 8), "bp": (3, 4)},    # current default
    {"radii": (1, 2, 4, 8), "bp": (1, 4)},
    {"radii": (1, 2, 4, 16), "bp": (1, 4)},
    {"radii": (1, 2, 4, 16), "bp": (3, 4)},
    {"radii": (1, 3, 6, 12), "bp": (1, 4)},
]
EPOCHS = 120
EVAL_EVERY = 10


def sample(cid, feat, yred, case_index):
    """Same documented sampling rule as train_model.sample_case."""
    x = feat.reshape(prep.MODEL_IN_CHANNELS, -1).T.copy()
    flat = yred.reshape(-1)
    n = flat.size
    i = np.arange(n)
    iz, iy, ix = i % 64, (i // 64) % 96, i // (64 * 96)
    ok = (ix % prep.TRAIN_STRIDE == 0) & (iy % prep.TRAIN_STRIDE == 0) & (iz % prep.TRAIN_STRIDE == 0)
    pos = np.nonzero(flat == 1)[0]
    pool = np.nonzero((flat == 0) & ok)[0]
    kmax = prep.NEG_PER_POS * len(pos)
    if len(pool) > kmax:
        rng = np.random.default_rng(prep.TRAIN_SEED + case_index)
        pool = np.sort(rng.permutation(pool)[:kmax])
    idx = np.concatenate([pos, pool])
    return x[idx].astype(np.float32), flat[idx].astype(np.uint8)


def main():
    cache = {}
    for cid in prep.CASES_TRAIN:                     # 0101 deliberately excluded
        vol, _ = prep.make_app_volume(cid)
        cache[cid] = vol
    yred = {cid: prep.downsample_gt_to_red(prep.gt_tooth_at_app_grid(cid))
            for cid in prep.CASES_TRAIN}

    results = []
    for var in VARIANTS:
        t0 = time.time()
        feats = {cid: prep.build_features(cache[cid], radii=var["radii"], bp=var["bp"])
                 for cid in prep.CASES_TRAIN}
        Xa, Ya = sample("dentvoxel_0021", feats["dentvoxel_0021"],
                        yred["dentvoxel_0021"], 0)
        Xb, Yb = sample("dentvoxel_0047", feats["dentvoxel_0047"],
                        yred["dentvoxel_0047"], 1)
        Xv, Yv = sample("dentvoxel_0074", feats["dentvoxel_0074"],
                        yred["dentvoxel_0074"], 2)
        X = np.concatenate([Xa, Xb]); Y = np.concatenate([Ya, Yb])
        model = tm.Mlp(prep.TRAIN_SEED)

        def val_cb(m):
            D, Yl = [], []
            feat, y = feats["dentvoxel_0074"], yred["dentvoxel_0074"]
            return tm.best_threshold(m.logit_diff(feat).reshape(-1),
                                     y.reshape(-1).astype(np.int8))[1]

        hist, iters = tm.train(model, X, Y, EPOCHS, val_cb=val_cb, eval_every=EVAL_EVERY)
        best = max(hist, key=lambda h: h["val_dice"])
        # also: what would the same recipe give on 0021 (in-sample sanity)
        entry = {"radii": list(var["radii"]), "bp": list(var["bp"]),
                 "best_epoch": best["epoch"], "best_val_dice_0074": best["val_dice"],
                 "best_val_prec": best["val_precision"], "best_val_rec": best["val_recall"],
                 "iterations": iters, "seconds": round(time.time() - t0, 1)}
        print("VARIANT", var, "->", json.dumps(entry))
        results.append(entry)
    results.sort(key=lambda r: -r["best_val_dice_0074"])
    json.dump(results, open(f"{prep.OUT_DIR}/work/variant_study.json", "w"), indent=1)
    print("\nWINNER:", json.dumps(results[0]))


if __name__ == "__main__":
    main()
