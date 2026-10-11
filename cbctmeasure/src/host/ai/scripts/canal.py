"""canal.py -- P-01 下颌管（下牙槽神经管）自动分割：主机侧训练 + 准入指标 + ONNX 导出。

与牙齿链路（train2.py）的关系
-------------------------------------------------------------------------------
本脚本**不重新发明**任何预处理：特征张量走 `prep.read_dicom_series` +
`prep.build_features`（与设备侧 C++ 同一套常量，也是 AC-08 奇偶校验的权威入口），
网络与反向传播直接 import train2（Conv3d 6->8->8->2，3x3x3，opset 17），
导出用 export2.export_onnx。于是"第二个模型"在设备上的可信度依据和第一个模型
完全同源：同一份特征构造、同一套轴序约定、同一种后处理纪律。

标签来源（已逐例核实，见 cbctmeasure/doc/ROUND8_*）：
    raw/cases.json 里 groundtruth 的标签表
      1 = maxilla   2 = mandible   3..34 = tooth_11..tooth_48（FDI 命名）
      35 = r_mandibular_canal   36 = l_mandibular_canal      <-- 本脚本的正样本
      37 = r_maxillary_sinus    38 = l_maxillary_sinus       （留给 P-05）
    下颌管在 0.60 mm 网格每侧 1472~2208 体素，2x2x2 多数票（阈值 4）抽稀到
    96x96x64（1.2 mm）后每侧 185~279 体素；4 例全部完整落在内置 app 体积的 FOV 内。

划分纪律与牙齿一致：train 0021+0047 / val 0074（早停 + 阈值标定）/ test 0101
（只做一次最终评估，0101 就是 App 里跑的那一例）。

输出（全部留在 src/host/ai/，**不进 APK**；决策 2）：
    model/canal_cnn.onnx            设备用的权重（几十 KB）
    model/canal_cnn.json            阈值 + 契约（设备侧读取，同 teeth_cnn.json 的角色）
    model/canal_cnn_weights.npz     权重原值（复现/审计）
    parity_ref/metrics_canal.json   全部指标（Dice/HD95/中心线一致性/门禁判定）
    parity_ref/canal_{gt,pred}_0101.{raw,nii.gz}  掩膜夹具（设备奇偶校验用）

用法：
    cd cbctmeasure/src/host/ai && python3 scripts/canal.py
    CANAL_EPOCHS=80 python3 scripts/canal.py        # 覆盖训练轮数上限
"""
import json
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import prep            # noqa: E402
import train2          # noqa: E402
import export2         # noqa: E402

OUT = prep.OUT_DIR
MODEL_DIR = f"{OUT}/model"
PARITY_DIR = f"{OUT}/parity_ref"
APP_VOL_DIR = f"{OUT}/app_volume"

CANAL_LABELS = (35, 36)          # r_mandibular_canal / l_mandibular_canal
TRAIN_CASES = ["dentvoxel_0021", "dentvoxel_0047"]
VAL_CASE = "dentvoxel_0074"
TEST_CASE = "dentvoxel_0101"

SEED = 20260101
MAX_EPOCHS = int(os.environ.get("CANAL_EPOCHS", "100"))
PATIENCE = int(os.environ.get("CANAL_PATIENCE", "25"))
PATCHES_PER_EPOCH = int(os.environ.get("CANAL_PATCHES", "256"))
PATCHES_PER_UPDATE = train2.PATCHES_PER_UPDATE
LR = float(os.environ.get("CANAL_LR", "0.02"))

# ---- 类别不平衡参数：与牙齿不同，且必须说明为什么不同 ----
# 牙齿在模型网格上约占 1% 体素，train2 用 neg_w=4 + 50% 正样本中心补丁就能学到
# dice@0.5>0.5。下颌管只占 0.06~0.09%（每例 374~536 / 589824 体素），同样配比下
# 首轮实测 16 个 epoch 全网格 dice@0.5 恒为 0（模型把所有体素都判成背景，
# 只有把阈值降到 0.05 附近才有输出）—— 那不是一个可用的模型。因此这里
# 只调"采样与损失配比"这两个明确与稀有类相关的量，网络、优化器、特征都不变：
#   NEG_W       16.0  （负样本总损失质量 : 正样本 = 16 : 1）
#   POS_CENTER  0.85  （85% 的补丁中心落在管腔体素上）
# train2.POS_CENTER_PROB 是在 sample() 运行时从模块全局读的，所以这里覆盖它
# 不会改动 train2.py 本身，牙齿那条已验收的流水线保持原样。
NEG_W = float(os.environ.get("CANAL_NEG_W", "16.0"))
POS_CENTER_PROB = float(os.environ.get("CANAL_POS_CENTER", "0.85"))
train2.POS_CENTER_PROB = POS_CENTER_PROB

# ---- 后处理常量：必须与设备侧 cpp/include/AiTypes.h 的 AiConst 同名同值 ----
MIN_CANAL_VOXELS = int(os.environ.get("CANAL_MIN_VOX", "12"))
CANAL_MAX_CANDIDATES = 6         # 只取前若干大连通域，其余按散点丢弃

# ---- 准入门禁（P-01 上线口径；来自建议需求清单，任一不满足即标记为"不上线"）----
GATE = {
    "test_dice_min": 0.70,
    "test_hd95_mm_max": 3.0,
    "nerve_distance_abs_err_mm_max": 1.0,
    "inference_seconds_max": 5.0,     # PC-05，真机侧另行取证
}


# ============================================================== 1. 数据装载
def canal_mask_at_app_grid(cid, labels=CANAL_LABELS):
    """GT 下颌管（并集）-> 设备内置体积的同一片 192x192x128 网格（canonical 序）。

    与 prep.gt_tooth_at_app_grid 同一条链：0.30 mm 标签 --2x2x2 多数票-->
    0.60 mm 源网格 --同一个 crop_start--> app 网格。这里刻意在本脚本内实现而
    不改 prep.py：牙齿那条流水线的产物（gt/*.raw、parity_ref/*）已经冻结，
    AC-08 依赖它逐位复现，任何"顺手重构"都可能把已验收的夹具挪走。
    """
    import nibabel as nib
    g = np.ascontiguousarray(np.asanyarray(
        nib.load(f"{prep.SRC_DIR}/{cid}_seg_groundtruth.nii.gz").dataobj), dtype=np.uint8)
    assert g.shape == prep.GT_SHAPE, (cid, g.shape)
    hit = np.isin(g, list(labels)).astype(np.uint8)
    c = hit.reshape(prep.SRC_SHAPE[0], prep.GT_TO_VOL_FACTOR,
                    prep.SRC_SHAPE[1], prep.GT_TO_VOL_FACTOR,
                    prep.SRC_SHAPE[2], prep.GT_TO_VOL_FACTOR).sum(axis=(1, 3, 5))
    src = (c >= prep.GT_VOTE_THRESHOLD).astype(np.uint8)          # (220,220,172)
    sx, sy, sz = prep.crop_start(prep.load_volume(cid))[0]
    sub = src[sx:sx + prep.APP_SHAPE[0], sy:sy + prep.APP_SHAPE[1],
              sz:sz + prep.APP_SHAPE[2]]
    assert sub.shape == prep.APP_SHAPE
    return np.ascontiguousarray(sub, dtype=np.uint8)


def case_arrays(cid):
    """设备同源数据：feat (6,96,96,64) 设备轴序 f32 + yred (96,96,64) 设备轴序 u8。

    DICOM 重读是权威（与 make_parity.py 一致），不是 NIfTI；轴序交换与
    train2.dev_case_arrays 逐字符相同，因此第二个模型与第一个模型在同一坐标系里。

    返回的 app / red 掩膜一律已换到**设备轴序**（axis0=Columns, axis1=Rows, axis2=z）。
    这一步不能省：`upsample_nearest2` 作用在设备轴序的预测上得到的是设备轴序的
    192x192x128，而 prep 的 GT 是 canonical（axis0=Rows, axis1=Columns）；app 网格
    的 0/1 轴正好都是 192，形状检查发现不了转置错误，只有比对本身会静默变差。
    """
    vol = prep.read_dicom_series(f"{APP_VOL_DIR}/{cid}")
    assert vol.shape == prep.APP_SHAPE and vol.dtype == np.int16
    feat_canon = prep.build_features(vol)
    feat = np.ascontiguousarray(feat_canon.transpose(0, 2, 1, 3))
    app_canon = canal_mask_at_app_grid(cid)
    red_canon = prep.downsample_gt_to_red(app_canon)
    app = np.ascontiguousarray(app_canon.transpose(1, 0, 2), dtype=np.uint8)
    yred = np.ascontiguousarray(red_canon.transpose(1, 0, 2), dtype=np.uint8)
    return vol, feat, yred, app


# ==================================================== 2. 中心线（与 C++ 同规则）
#
# 规则（必须与 core/NerveAutoTracker.cpp 逐位一致，报告里两边互相印证）：
#   a) 26 邻域连通域，体素数 < MIN_CANAL_VOXELS 的散点丢弃；
#   b) 按体素数降序（并列时按首次出现的最小线性索引升序）取前 MAX 个；
#   c) 每个连通域取 "bbox extent 最大的轴" 为描记轴（并列按 x<y<z 顺序取第一个）；
#   d) 沿描记轴按坐标分组，每组取该组体素球心的算术平均 -> 折线一点；组按坐标升序；
#   e) 半径 = 各组截面积 sqrt(area/pi) 的中位数，面积 = 组内体素数 * 垂直面体素面积。
def _components(mask):
    from scipy import ndimage
    lab, n = ndimage.label(mask, structure=np.ones((3, 3, 3), int))
    if n == 0:
        return []
    out = []
    for cid in range(1, n + 1):
        idx = np.argwhere(lab == cid)
        if len(idx) < MIN_CANAL_VOXELS:
            continue
        lin = (idx[:, 0] * mask.shape[1] + idx[:, 1]) * mask.shape[2] + idx[:, 2]
        out.append({"idx": idx, "voxels": len(idx), "min_lin": int(lin.min())})
    out.sort(key=lambda c: (-c["voxels"], c["min_lin"]))
    return out[:CANAL_MAX_CANDIDATES]


def centerlines(mask, spacing=(1.2, 1.2, 1.2)):
    """(96,96,64) 设备轴序掩膜 -> [{points:[(x,y,z) mm], voxels, radius_mm, trace_axis}]"""
    res = []
    for comp in _components(mask):
        idx = comp["idx"].astype(np.float64)
        lo = idx.min(0)
        hi = idx.max(0)
        extent = hi - lo + 1.0
        axis = int(np.argmax(extent))                    # 并列时取第一个最大（x<y<z）
        coords = idx[:, axis]
        order = np.argsort(coords, kind="stable")
        radii = []
        pts = []
        perp = [a for a in range(3) if a != axis]
        area_unit = spacing[perp[0]] * spacing[perp[1]]
        for c in np.unique(coords):
            sel = idx[coords == c]
            p = [0.0, 0.0, 0.0]
            for a in range(3):
                # 体素球心 = (index + 0.5) * spacing（与 AiGrid::redToWorld 同源）
                p[a] = (sel[:, a].mean() + 0.5) * spacing[a]
            pts.append(tuple(p))
            radii.append(np.sqrt(len(sel) * area_unit / np.pi))
        radii.sort()
        med = radii[len(radii) // 2] if len(radii) % 2 else 0.5 * (
            radii[len(radii) // 2 - 1] + radii[len(radii) // 2])
        res.append({"points": pts, "voxels": comp["voxels"], "radius_mm": float(med),
                    "trace_axis": axis, "extent": [float(x) for x in extent],
                    "bbox_min_vox": [int(x) for x in lo], "bbox_max_vox": [int(x) for x in hi]})
    return res


def polyline_segment_distance(poly, a, b):
    """与 MeasureMath::polylineSegmentDistance 同义：折线各段与线段 ab 的最小距离。

    注意 ab 退化成一个点（a==b）时就是"点到折线"的距离，探针与中心线比对都靠这一点。
    """
    a = np.asarray(a, float)
    b = np.asarray(b, float)
    best = float("inf")
    for i in range(len(poly) - 1):
        p = np.asarray(poly[i], float)
        q = np.asarray(poly[i + 1], float)
        best = min(best, _seg_seg(p, q, a, b))
    return best


def _seg_seg(p0, p1, q0, q1):
    """两线段最短距离（Eberly），与 C++ MeasureMath::segmentSegment 同一算法。"""
    d1 = p1 - p0
    d2 = q1 - q0
    r = p0 - q0
    a = float(d1 @ d1)
    e = float(d2 @ d2)
    f = float(d2 @ r)
    EPS = 1e-12
    if a <= EPS and e <= EPS:
        return float(np.linalg.norm(p0 - q0))
    if a <= EPS:
        t = 0.0
        s = min(1.0, max(0.0, f / e))
    else:
        c = float(d1 @ r)
        if e <= EPS:
            s = 0.0
            t = min(1.0, max(0.0, -c / a))
        else:
            b = float(d1 @ d2)
            denom = a * e - b * b
            t = min(1.0, max(0.0, (b * f - c * e) / denom)) if denom > EPS else 0.0
            s = min(1.0, max(0.0, (a * f - b * c) / (a * e - b * b))) if denom > EPS else 0.0
            if s < 0.0:
                s = 0.0
                t = min(1.0, max(0.0, -c / a))
            elif s > 1.0:
                s = 1.0
                t = min(1.0, max(0.0, (b - c) / a))
    return float(np.linalg.norm((p0 + d1 * t) - (q0 + d2 * s)))


def polyline_polyline_distance(pa, pb):
    best = float("inf")
    for i in range(len(pa) - 1):
        for j in range(len(pb) - 1):
            # 两折线段最短距离：对每条线段做 33 点采样近似（口径在报告里写明）
            for t in np.linspace(0, 1, 33):
                q = np.asarray(pa[i], float) * (1 - t) + np.asarray(pa[i + 1], float) * t
                best = min(best, polyline_segment_distance(pb, q, q))
    return best


def hd95(pred_mask, gt_mask, sampling=0.6):
    """双向 HD95（app 网格 0.60 mm）；与临床口径一致，用 app 网格而不是 1.2 mm 模型网格。"""
    from scipy import ndimage
    if pred_mask.sum() == 0 or gt_mask.sum() == 0:
        return None
    dp = ndimage.distance_transform_edt(1 - pred_mask, sampling=sampling)
    dg = ndimage.distance_transform_edt(1 - gt_mask, sampling=sampling)
    a = np.percentile(dp[gt_mask.astype(bool)], 95)
    b = np.percentile(dg[pred_mask.astype(bool)], 95)
    return {"hd95_mm": float(max(a, b)), "hd95_pred_to_gt": float(a),
            "hd95_gt_to_pred": float(b),
            "asd_mm": float(0.5 * (a + b))}


# ==================================================== 3. 训练（复用 train2 机器）
def main():
    t0 = time.time()
    os.makedirs(MODEL_DIR, exist_ok=True)
    os.makedirs(PARITY_DIR, exist_ok=True)

    gc = train2.gradient_check(patch=12, eps=1e-4, n_per=3)
    print("[gradcheck] max rel err = %.3e ok=%s" % (gc["max_rel_err"], gc["ok"]))
    if not gc["ok"]:
        print("[gradcheck] FAILED -- backward 有问题，训练前中止（与 train2 同一纪律）")
        sys.exit(3)

    data, extras = {}, {}
    for cid in TRAIN_CASES + [VAL_CASE, TEST_CASE]:
        vol, feat, yred, app = case_arrays(cid)      # 全部设备轴序
        data[cid] = (feat, yred)
        extras[cid] = {"vol": vol, "app": app, "red": yred}
        print("[data] %s feat %s canal red voxels %d (%.4f%% of grid) app 0.6mm %d"
              % (cid, feat.shape, int(yred.sum()), 100.0 * yred.sum() / yred.size,
                 int(app.sum())))

    net = train2.Net(SEED)
    print("[net] params", net.n_params(), "lr", LR, "epochs<=", MAX_EPOCHS)
    samplers = {cid: train2.make_patch_sampler(*data[cid]) for cid in TRAIN_CASES}
    m, v = train2.adam_init(net.params())
    t = 0
    history = []
    best = {"val_dice": -1.0, "epoch": -1, "threshold": None, "state": None}
    since = 0
    upd_per_epoch = PATCHES_PER_EPOCH // PATCHES_PER_UPDATE
    # 早停指标：不用 dice@0.5，用"粗扫（0.05~0.95，步长 0.05）里的最大 val Dice"。
    # 原因写在上面那组常量里：稀有类在 0.5 处可以长时间一个体素都不越过，
    # 但概率排序其实已经在变好 —— 拿恒 0 的指标早停会停在第 16 轮，白跑。
    # 仍然只用 VAL 例（0074）选参数与标定阈值，0101 只做一次最终评估。
    COARSE = [round(float(x), 4) for x in np.arange(0.05, 0.9501, 0.05)]

    for ep in range(1, MAX_EPOCHS + 1):
        rng = np.random.default_rng(SEED + ep)
        losses = []
        for u in range(upd_per_epoch):
            accG = None
            for bidx in range(PATCHES_PER_UPDATE):
                cid = TRAIN_CASES[(u * PATCHES_PER_UPDATE + bidx) % len(TRAIN_CASES)]
                feat, y = data[cid]
                s0, s1, s2 = (int(x) for x in samplers[cid](rng, 1)[0])
                Xp = np.ascontiguousarray(
                    feat[:, s0:s0 + train2.PATCH, s1:s1 + train2.PATCH, s2:s2 + train2.PATCH])
                yp = y[s0:s0 + train2.PATCH, s1:s1 + train2.PATCH, s2:s2 + train2.PATCH]
                loss, G, _ = train2.patch_loss_grads(net, Xp, yp, NEG_W)
                losses.append(loss)
                accG = [g.astype(np.float32) for g in G] if accG is None else \
                    [a + g for a, g in zip(accG, G)]
            t += 1
            train2.adam_update(net, [g / PATCHES_PER_UPDATE for g in accG], m, v, t)
        fv, yv = data[VAL_CASE]
        p1 = net.predict_proba(fv)[1]
        st05 = prep.dice_stats(p1 > 0.5, yv.astype(bool))
        cur_th, cur_dice = 0.05, -1.0
        for th in COARSE:
            d = prep.dice_stats(p1 > th, yv.astype(bool))["dice"]
            if d > cur_dice:
                cur_dice, cur_th = d, th
        history.append({"epoch": ep, "loss": float(np.mean(losses)),
                        "val_dice_0p5": st05["dice"], "val_prec": st05["precision"],
                        "val_rec": st05["recall"], "val_pmax": float(p1.max()),
                        "val_coarse_best_dice": cur_dice,
                        "val_coarse_best_threshold": cur_th})
        improved = cur_dice > best["val_dice"]
        if improved:
            best = {"val_dice": cur_dice, "epoch": ep, "threshold": cur_th,
                    "state": [p.copy() for p in net.params()]}
            since = 0
        else:
            since += 1
        if ep % 5 == 0 or improved or since == PATIENCE:
            print("[ep %3d] loss %.4f | VAL %s dice@0.5 %.4f | coarse-best dice %.4f @t %.2f "
                  "pmax %.3f | best %.4f@%d since %d"
                  % (ep, float(np.mean(losses)), VAL_CASE, st05["dice"], cur_dice, cur_th,
                     float(p1.max()), best["val_dice"], best["epoch"], since), flush=True)
        if since >= PATIENCE:
            print("[stop] early stopping at epoch %d" % ep, flush=True)
            break
    for p, q in zip(net.params(), best["state"]):
        p[:] = q
    print("[restore] best params epoch %d val coarse dice %.4f (coarse t %s)"
          % (best["epoch"], best["val_dice"], best["threshold"]))

    # ---------------- 阈值标定（只在 VAL 上做，与牙齿一致）----------------
    fv, yv = data[VAL_CASE]
    prob_val = net.predict_proba(fv)
    curve = []
    for th in [round(float(x), 4) for x in np.arange(0.05, 0.9501, 0.01)]:
        s = prep.dice_stats(prob_val[1] > th, yv.astype(bool))
        curve.append({"threshold": th, "dice": s["dice"], "precision": s["precision"],
                      "recall": s["recall"], "f1": s["f1"]})
    chosen = max(curve, key=lambda c: (c["dice"], -c["threshold"]))
    T = chosen["threshold"]
    print("[calib] val-best threshold %.2f dice %.4f" % (T, chosen["dice"]))

    # ---------------- 最终评估 ----------------
    def eval_case(cid):
        f, y = data[cid]
        pr = net.predict_proba(f)
        lab = (pr[1] > T).astype(np.uint8)
        red_stats = prep.dice_stats(lab.astype(bool), y.astype(bool))
        # 上采样到 app 网格（与设备侧 R-06 统计同一口径），并加连通域后处理
        up = prep.upsample_nearest2(lab)
        up_gt = extras[cid]["app"]
        app_stats = prep.dice_stats(up.astype(bool), up_gt.astype(bool))
        comps = _components(lab)                    # 设备侧后处理：散点剔除 + 前 N 大
        kept = np.zeros_like(lab)
        for c in comps:
            ix = c["idx"]
            kept[ix[:, 0], ix[:, 1], ix[:, 2]] = 1
        post = prep.dice_stats(kept.astype(bool), y.astype(bool))
        return {"case": cid, "at_chosen": {"threshold": T, **red_stats},
                "after_cc_min%d" % MIN_CANAL_VOXELS: {"dice": post["dice"],
                                                      "precision": post["precision"],
                                                      "recall": post["recall"]},
                "app_0p6mm_nearest_x2": app_stats,
                "components_kept": len(comps)}

    res = {cid: eval_case(cid) for cid in TRAIN_CASES + [VAL_CASE, TEST_CASE]}
    te = res[TEST_CASE]
    print("\n== TEST %s ==" % TEST_CASE)
    s = te["at_chosen"]
    print("  red 1.2mm dice %.4f prec %.4f rec %.4f (tp %d fp %d fn %d)"
          % (s["dice"], s["precision"], s["recall"], s["tp"], s["fp"], s["fn"]))
    print("  app 0.6mm dice %.4f  components kept %d"
          % (te["app_0p6mm_nearest_x2"]["dice"], te["components_kept"]))

    # ---------------- HD95 + 中心线一致性（准入指标的真身）----------------
    f_te, _ = data[TEST_CASE]
    pred_lab = (net.predict_proba(f_te)[1] > T).astype(np.uint8)
    pred_app = prep.upsample_nearest2(pred_lab)
    gt_app = extras[TEST_CASE]["app"]
    hd = hd95(pred_app, gt_app)
    spacing = (1.2, 1.2, 1.2)
    cl_pred = centerlines(pred_lab, spacing)
    cl_gt = centerlines(extras[TEST_CASE]["red"], spacing)
    print("  HD95 %s" % (None if hd is None else
          "pred->gt %.2fmm gt->pred %.2fmm max %.2fmm"
          % (hd["hd95_pred_to_gt"], hd["hd95_gt_to_pred"], hd["hd95_mm"])))

    # 双侧配对：按描记轴首点的 x 坐标（设备 axis1 ~ Rows）分不出左右时，
    # 用"质心 x"排序后与 GT 同序配对（GT 侧同理），并记录配对距离。
    def cx(cl):
        return float(np.mean([p[0] for p in cl["points"]])) if cl["points"] else 0.0
    pair = []
    for g in sorted(cl_gt, key=cx):
        cand = min(cl_pred, key=lambda p: abs(cx(p) - cx(g))) if cl_pred else None
        if cand is None:
            pair.append({"matched": False, "gt_voxels": g["voxels"]})
            continue
        d_pp = polyline_polyline_distance(g["points"], cand["points"])
        # 逐点最近距离的最大值（临床更关心"最坏偏多少"）
        worst = 0.0
        for p in g["points"]:
            worst = max(worst, polyline_segment_distance(cand["points"], p, p))
        pair.append({"matched": True, "gt_voxels": g["voxels"], "pred_voxels": cand["voxels"],
                     "gt_radius_mm": round(g["radius_mm"], 2),
                     "pred_radius_mm": round(cand["radius_mm"], 2),
                     "polyline_min_dist_mm": round(d_pp, 3),
                     "gt_to_pred_max_mm": round(worst, 3),
                     "pred_to_gt_max_mm": round(max(
                         polyline_segment_distance(g["points"], p, p)
                         for p in cand["points"]), 3)})
    print("  centerline pairs: %s" % json.dumps(pair, ensure_ascii=False))

    # 种植轴到管线的距离一致性：用下颌牙（GT 牙标签 19..34 = tooth_3x/4x）的
    # 牙冠端作入口、朝该例 GT 下颌管的 z 均值方向推进 11mm 合成探针轴，
    # 比较"GT 管线"与"预测管线"的距离差 —— 这才是 S-05 会不会被自动下颌管带偏的直接答案。
    probe = probe_axis_agreement(extras[TEST_CASE], cl_gt, cl_pred)
    mx = probe["max_abs_err_mm"]
    print("  probe axes n=%d  mean|dGT-dPred| %s  max %s  over1mm %s  %s"
          % (probe["n"], probe["mean_abs_err_mm"], mx, probe["n_over_1mm"],
             "n/a" if mx is None or mx != mx else
             ("OK" if mx <= GATE["nerve_distance_abs_err_mm_max"] else "OVER GATE")))

    # ---------------- 导出 ----------------
    onnx_path = f"{MODEL_DIR}/canal_cnn.onnx"
    export2.export_onnx(net, onnx_path)
    onnx_bytes = os.path.getsize(onnx_path)
    np.savez(f"{MODEL_DIR}/canal_cnn_weights.npz",
             W1=net.W1, b1=net.b1, W2=net.W2, b2=net.b2, W3=net.W3, b3=net.b3,
             threshold=np.float32(T), arch=np.array([6, 8, 8, 2, 3]))
    spec = {
        "model": "canal_cnn.onnx", "task": "AI-02 下颌管（下牙槽神经管）自动分割",
        "threshold": T, "threshold_chosen_by": "max Dice on val case %s full grid" % VAL_CASE,
        "inChannels": 6, "outChannels": 2, "positiveClass": 1,
        "minInstanceVoxels": MIN_CANAL_VOXELS, "maxCandidates": CANAL_MAX_CANDIDATES,
        "grid": "device axis order (96,96,64) @1.2mm == app 192x192x128 @0.6mm decimated x2",
        "labels": {"35": "r_mandibular_canal", "36": "l_mandibular_canal"},
        "split": {"train": TRAIN_CASES, "val": VAL_CASE, "test": TEST_CASE},
        "shipped_in_apk": False,
        "delivery": "adb push 到 <外置 filesDir>/ai_models/canal_cnn.onnx，或在 AI 页选择模型文件",
        "metrics_test": {"dice_red": s["dice"], "dice_app": te["app_0p6mm_nearest_x2"]["dice"],
                         "hd95_mm": None if hd is None else hd["hd95_mm"]},
    }
    json.dump(spec, open(f"{MODEL_DIR}/canal_cnn.json", "w"), indent=1, ensure_ascii=False)
    parity_ort = ort_check(onnx_path, f_te, net.predict_proba(f_te))
    # .raw 一律是**设备轴序**的扁平副本（C++ 侧读它做奇偶校验，顺序必须与推理输出一致）；
    # .nii.gz 是同一掩膜的 canonical 副本，只为在 ITK-SNAP 里肉眼核对解剖左右。
    pred_lab.astype(np.uint8).tofile(f"{PARITY_DIR}/canal_pred_0101.raw")
    extras[TEST_CASE]["red"].astype(np.uint8).tofile(f"{PARITY_DIR}/canal_gt_0101.raw")
    pred_app.astype(np.uint8).tofile(f"{PARITY_DIR}/canal_pred_0101_app.raw")
    gt_app.astype(np.uint8).tofile(f"{PARITY_DIR}/canal_gt_0101_app.raw")
    try:
        import nibabel as nib
        aff = np.diag([prep.APP_SPACING_MM, prep.APP_SPACING_MM, prep.APP_SPACING_MM, 1.0])
        for arr, name in ((pred_app, "canal_pred_0101_app"), (gt_app, "canal_gt_0101_app")):
            nib.save(nib.Nifti1Image(np.ascontiguousarray(arr.transpose(1, 0, 2)), aff),
                     f"{PARITY_DIR}/{name}.nii.gz")
    except Exception as e:                                # nii 只是给人看的，写失败不影响门禁
        print("[warn] nii.gz mirror skipped:", repr(e))

    gates = {
        "dice_red_min": {"value": round(s["dice"], 4), "limit": GATE["test_dice_min"],
                         "pass": bool(s["dice"] >= GATE["test_dice_min"])},
        "hd95_max": {"value": None if hd is None else round(hd["hd95_mm"], 3),
                     "limit": GATE["test_hd95_mm_max"],
                     "pass": bool(hd is not None and hd["hd95_mm"] <= GATE["test_hd95_mm_max"])},
        "nerve_distance_abs_err_max": {
            "value": probe["max_abs_err_mm"],
            "limit": GATE["nerve_distance_abs_err_mm_max"],
            "pass": bool(probe["max_abs_err_mm"] is not None and
                         probe["max_abs_err_mm"] <= GATE["nerve_distance_abs_err_mm_max"])},
    }
    out = {
        "generated_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "task": "P-01 mandibular canal auto-segmentation (host training)",
        "architecture": "Conv3d(6->8,k3,p1) Relu Conv3d(8->8,k3,p1) Relu Conv3d(8->2,k3,p1) Softmax",
        "axis_order": spec["grid"],
        "parameter_count": net.n_params(),
        "onnx_path": onnx_path, "onnx_bytes": onnx_bytes,
        "onnx_check": parity_ort,
        "label_source": "raw/cases.json groundtruth 35/36（DentVoxel 专家标注）",
        "gradient_check": {"max_rel_err": gc["max_rel_err"], "ok": gc["ok"]},
        "training": {"train_cases": TRAIN_CASES, "val_case": VAL_CASE, "test_case": TEST_CASE,
                     "lr": LR, "seed": SEED, "patch": train2.PATCH,
                     "patches_per_epoch": PATCHES_PER_EPOCH, "adam_updates": t,
                     "neg_weight_per_positive": NEG_W, "pos_center_prob": POS_CENTER_PROB,
                     "early_stop_metric": "max val Dice over coarse sweep 0.05..0.95 step 0.05",
                     "positive_fraction_of_grid": {
                         cid: round(float(data[cid][1].mean()), 6)
                         for cid in TRAIN_CASES + [VAL_CASE, TEST_CASE]},
                     "epochs_run": len(history), "best_epoch": best["epoch"],
                     "best_val_coarse_dice": best["val_dice"],
                     "best_val_coarse_threshold": best["threshold"],
                     "history": history},
        "threshold_calibration": {"chosen_threshold": T, "val_curve_best": chosen},
        "metrics": res,
        "hd95_app_0p6mm": hd,
        "centerline": {"pred_components": len(cl_pred), "gt_components": len(cl_gt),
                       "pairs": pair,
                       "pred_radius_mm": [round(c["radius_mm"], 2) for c in cl_pred],
                       "gt_radius_mm": [round(c["radius_mm"], 2) for c in cl_gt],
                       "pred_points": [len(c["points"]) for c in cl_pred],
                       "gt_points": [len(c["points"]) for c in cl_gt]},
        "probe_axis_agreement": probe,
        "admission_gate": gates,
        "gate_all_pass": bool(all(g["pass"] for g in gates.values())),
        "wall_clock_seconds": round(time.time() - t0, 1),
    }
    json.dump(out, open(f"{PARITY_DIR}/metrics_canal.json", "w"), indent=1, ensure_ascii=False)
    print("\n[gate] %s" % json.dumps(gates, ensure_ascii=False, indent=1))
    print("[done] wall %.1fs -> %s / %s" % (time.time() - t0, onnx_path,
                                            f"{PARITY_DIR}/metrics_canal.json"))


def ort_check(onnx_path, feat, np_prob):
    """ORT 与 numpy 前向必须逐位接近（与 parity.ort_check 同一目的，本脚本自带一份，
    避免把牙齿流水线拖进来）。算子类型从 onnx 图读，不是 InferenceSession ——
    ORT 1.23 的 session 对象没有 get_nodes()，写成那样会在训练跑完后才崩，
    白丢二十分钟，所以这里沿用 parity.py 里已经验证过的写法。"""
    import onnx
    try:
        import onnxruntime as ort
    except Exception as e:
        return {"available": False, "reason": repr(e)}
    m = onnx.load(onnx_path)
    assert [i.name for i in m.graph.input] == ["feat"], "输入名必须是 feat"
    assert [o.name for o in m.graph.output] == ["prob"], "输出名必须是 prob"
    ishape = [d.dim_value for d in m.graph.input[0].type.tensor_type.shape.dim]
    oshape = [d.dim_value for d in m.graph.output[0].type.tensor_type.shape.dim]
    assert ishape == [1, 6, 96, 96, 64] and oshape == [1, 2, 96, 96, 64], (ishape, oshape)
    so = ort.SessionOptions()
    so.inter_op_num_threads = 1
    so.intra_op_num_threads = 1
    sess = ort.InferenceSession(onnx_path, sess_options=so,
                                providers=["CPUExecutionProvider"])
    got = sess.run(["prob"], {"feat": np.ascontiguousarray(feat, np.float32)[np.newaxis, ...]})[0]
    diff = float(np.abs(got[0] - np_prob).max())
    ver = m.opset_import[0].version
    return {"available": True, "ort_max_abs_diff_vs_numpy": diff,
            "opset": int(ver), "input_shape": ishape, "output_shape": oshape,
            "n_nodes": len(m.graph.node),
            "ort_ops": sorted({n.op_type for n in m.graph.node}),
            "ort_providers": sess.get_providers()[0]}


def lower_fdi_tooth_labels(cid):
    """cases.json 里该例的下颌牙标签号（FDI 3x/4x，即 tooth_31..tooth_48）。"""
    for c in json.load(open(f"{prep.SRC_DIR}/cases.json")):
        if c.get("id") != cid:
            continue
        for r in c.get("results", []):
            if r.get("key") == "groundtruth":
                return sorted(int(k) for k, v in r["labels"].items()
                              if v.startswith("tooth_3") or v.startswith("tooth_4"))
    return []


def probe_axis_agreement(ex, cl_gt, cl_pred):
    """合成探针轴（模拟种植体轴线），比较 S-05 距离在"GT 管线"与"预测管线"下的差。

    探针来源不用模型：直接用 GT 的下颌牙标签（cases.json 里 tooth_31..tooth_48
    即 label 19..34）的冠端与根端中点作入口，沿 -Z 走 11 mm —— 于是这条指标衡量的是
    "自动下颌管会不会把神经距离带偏"，不含分割误差以外的噪声源。
    """
    import nibabel as nib
    cid = TEST_CASE
    g = np.ascontiguousarray(np.asanyarray(
        nib.load(f"{prep.SRC_DIR}/{cid}_seg_groundtruth.nii.gz").dataobj), dtype=np.uint8)
    ids = lower_fdi_tooth_labels(cid)
    sx, sy, sz = prep.crop_start(prep.load_volume(cid))[0]
    # 探针要真的"跨过"管区才有意义：用 GT 下颌管的 z 均值决定每根轴的推进方向
    canal_z_mean = float(np.argwhere(ex["app"].astype(bool))[:, 2].mean())
    axes = []
    for tid in ids:
        m = (g == tid)
        c = m.reshape(220, 2, 220, 2, 172, 2).sum(axis=(1, 3, 5))
        app = (c >= 4).astype(np.uint8)
        sub = app[sx:sx + 192, sy:sy + 192, sz:sz + 128]
        idx = np.argwhere(sub)
        if len(idx) < 4:
            continue
        # canonical(x=Rows,y=Columns,z) -> 设备轴序(i=Columns, j=Rows, k=z)
        cen = idx.mean(0)
        toward = 1.0 if canal_z_mean > cen[2] else -1.0
        z_entry = idx[:, 2].max() if toward > 0 else idx[:, 2].min()
        entry_dev = np.asarray(((cen[1] + 0.5) * 0.6, (cen[0] + 0.5) * 0.6,
                                (z_entry + 0.5) * 0.6))
        tip_dev = entry_dev + np.asarray((0.0, 0.0, toward * 11.0))
        axes.append((entry_dev, tip_dev))
    if not axes or not cl_gt:
        return {"n": len(axes), "mean_abs_err_mm": None,
                "max_abs_err_mm": None, "n_over_1mm": None,
                "note": "无探针轴或无 GT 管线"}

    def dist(cl_list, a, b):
        best = float("inf")
        for c in cl_list:
            best = min(best, polyline_segment_distance(c["points"], a, b))
        return best

    errs = []
    for a, b in axes:
        dg = dist(cl_gt, a, b)
        dp = dist(cl_pred, a, b) if cl_pred else float("nan")
        errs.append({"entry": [round(float(x), 2) for x in a],
                     "nerve_mm_gt": round(dg, 3),
                     "nerve_mm_pred": round(dp, 3) if dp == dp else None,
                     "abs_err_mm": round(abs(dp - dg), 3) if dp == dp else None})
    real = [e["abs_err_mm"] for e in errs if e["abs_err_mm"] is not None]
    return {"n": len(axes),
            "method": "探针轴 = GT 下颌牙（tooth_3x/4x）牙冠端沿该例下颌管 z 均值方向推进 11mm；"
                      "距离 = 线段到折线的最小距离，两侧都不减半径（同口径），"
                      "因此 |dGT-dPred| 度量的就是"
                      "「换成自动下颌管后，S-05 神经距离会偏多少」的几何下界",
            "mean_abs_err_mm": round(float(np.mean(real)), 3) if real else None,
            "max_abs_err_mm": round(float(np.max(real)), 3) if real else None,
            "n_over_1mm": int(sum(1 for x in real if x > 1.0)) if real else None,
            "per_axis": errs}


if __name__ == "__main__":
    main()
