#!/usr/bin/env python3
"""check_canal_dump.py -- AC-08-2 的下颌管（P-01 / AI-02）分支：真机 dumpParity 与主机复算逐元素比对。

为什么需要单独一个脚本（而不是复用 check_device_dump.py）
-------------------------------------------------------------------------------
check_device_dump.py 比对的是**牙齿**通道，它依赖 parity_ref/device_*_0101.raw 那批
已冻结的夹具（make_parity_device.py 生成）。下颌管是同一个 AiEngine::runSegment
的第二次使用，但权重、阈值、后处理常量都不同，而且**主机侧从来没有落过 feat/prob/inst
夹具**（canal.py 只写了 pred/gt 掩膜）。所以这里改成"当场复算"：
特征用 prep.build_features + 设备轴序转置（canal.case_arrays，与训练时同一行代码），
概率用 canal.py 导出的那份 onnx（model/canal_cnn.onnx，就是 adb push 给真机的同一个文件），
标签阈值用 canal_cnn.json 里的 threshold（真机 CanalEngine.readSpec 读的也是它）。
"两端各自独立算、结果必须重合"比"两端比同一份夹具"更强，因为夹具本身不会跟着
权重一起被重新生成，而这条链路会。

比对的四段（dump 布局见 AiEngine.cpp:144-178）
    1) feat   (6,96,96,64) f32  —— 主机 prep.build_features 复算
    2) prob   (2,96,96,64) f32  —— onnxruntime 复算
    3) label  (96,96,64) u8     —— prob[:,1] > spec.threshold，另与
                                   parity_ref/canal_pred_0101.raw 夹具对照
    4) inst   (96,96,64) i16    —— 26 邻域连通域（>=MIN_INSTANCE_VOXELS=8）后按
                                   体素数降序重编号；并列体素数的编号交换是合法的
                                   （C++ std::sort 不稳定），所以只比"尺寸序列 +
                                   尺寸唯一时的点集"
    5) 中心线 —— 用**真机的 label** 跑 canal.centerlines()，与
                 parity_ref/metrics_canal.json 的 centerline 段比半径/点数/体素数，
                 这就是"设备中心线 == 主机中心线"的证据（NerveAutoTracker 规则对账）

用法：
    adb shell run-as com.example.dcmtkdemo cat files/ai_parity/canal_device.parity.bin \\
        > /tmp/dcmtk_r8/canal_device.parity.bin
    cd cbctmeasure/src/host/ai && python3 scripts/check_canal_dump.py /tmp/dcmtk_r8/canal_device.parity.bin

退出码：0 = PASS，2 = 超出容差（FAIL），1 = 格式/尺寸/依赖问题。
"""
import argparse
import hashlib
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import canal            # noqa: E402  (case_arrays / centerlines / _components)
import prep             # noqa: E402

HEADER_INTS = 8
# 与 cpp/include/AiTypes.h 同名同值：inst 由共用的 AiCore::connectedComponents 产出，
# 所以散点阈值是牙齿的 8，不是下颌管追踪用的 12（12 只作用于 NerveAutoTracker）。
MIN_INSTANCE_VOXELS = 8
# 26 邻域偏移，与 AiCore::connectedComponents / NerveAutoTracker 的遍历顺序无关，
# 只要求"同一连通域内的体素集合相同"，因此用 scipy 的 structure 即可。
STRUCT26 = np.ones((3, 3, 3), int)


def sha12(b):
    return hashlib.sha256(b).hexdigest()[:12]


def parse_dump(path):
    """64B 头 + feat + prob + label + inst；返回 (head, feat, prob, label, inst, raw)"""
    raw = open(path, "rb").read()
    if len(raw) < HEADER_INTS * 8:
        raise ValueError("dump 比 64 字节头还短，不像是 dumpParity 的产物")
    head = np.frombuffer(raw[:64], dtype="<i8")
    feat_n, prob_n, label_n = (int(head[0]), int(head[1]), int(head[2]))
    need = 64 + 4 * feat_n + 4 * prob_n + label_n + 2 * label_n
    if len(raw) != need:
        raise ValueError(f"dump 实际 {len(raw)} 字节 != 头部推算 {need} 字节")
    off = 64
    feat = np.frombuffer(raw, dtype="<f4", count=feat_n, offset=off); off += 4 * feat_n
    prob = np.frombuffer(raw, dtype="<f4", count=prob_n, offset=off); off += 4 * prob_n
    label = np.frombuffer(raw, dtype=np.uint8, count=label_n, offset=off); off += label_n
    inst = np.frombuffer(raw, dtype="<i2", count=label_n, offset=off)
    return head, feat, prob, label, inst, raw


def host_prob_from_onnx(onnx_path, feat):
    """同一个 .onnx、同一份 feat -> 概率图；ORT 不可用时返回 None（不降级成"跳过就通过"）"""
    try:
        import onnxruntime as ort
    except Exception as e:                                  # 缺依赖 = 无法判定，不是通过
        print("[warn] onnxruntime 不可用: %r" % (e,))
        return None
    ses = ort.InferenceSession(onnx_path, providers=["CPUExecutionProvider"])
    out = ses.run(None, {ses.get_inputs()[0].name: feat[None].astype(np.float32)})[0]
    return np.ascontiguousarray(out[0])                     # (2,96,96,64)


def components_26(label, shape):
    """真机 label -> 连通域体素集合列表（>=MIN_INSTANCE_VOXELS），按体素数降序"""
    from scipy import ndimage
    lab, n = ndimage.label(label.reshape(shape), structure=STRUCT26)
    out = []
    for cid in range(1, n + 1):
        idx = np.argwhere(lab == cid)
        if len(idx) >= MIN_INSTANCE_VOXELS:
            out.append(idx)
    out.sort(key=lambda ix: -len(ix))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dump", help="真机 canal_device.parity.bin（AiEngine::dumpParity 产物）")
    _ai = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument("--spec", default=os.path.join(_ai, "model", "canal_cnn.json"))
    ap.add_argument("--onnx", default=os.path.join(_ai, "model", "canal_cnn.onnx"))
    ap.add_argument("--metrics", default=os.path.join(_ai, "parity_ref", "metrics_canal.json"))
    ap.add_argument("--label-fixture", default=os.path.join(_ai, "parity_ref",
                                                            "canal_pred_0101.raw"))
    ap.add_argument("--case", default=canal.TEST_CASE, help="主机复算用哪一例（默认 test 例）")
    ap.add_argument("--feat-max-abs", type=float, default=1e-6,
                    help="feat 允许的最大绝对差（默认 1e-6：真机 arm64/ORT1.17 与主机 x86/ORT1.23 "
                         "的浮点求和顺序不同，逐位相同不是必要条件）")
    ap.add_argument("--prob-max-abs", type=float, default=1e-5)
    ap.add_argument("--expect-voxels", type=int, default=522,
                    help="真机 logcat 的掩膜体素数（adoptCanalTracks 那行），0 = 不比")
    ap.add_argument("--expect-tracks", type=int, default=6,
                    help="真机 logcat 的中心线条数，0 = 不比")
    args = ap.parse_args()

    for p, what in ((args.dump, "真机 dump"), (args.spec, "规格文件"), (args.onnx, "onnx 权重")):
        if not os.path.isfile(p):
            print(f"ERROR: 找不到{what}: {p}")
            return 1
    spec = json.load(open(args.spec))
    head, feat, prob, label, inst, raw = parse_dump(args.dump)
    md = tuple(int(x) for x in head[3:6])
    rd = (int(head[6]), int(head[7]))
    in_ch, out_ch = int(spec["inChannels"]), int(spec["outChannels"])
    thr = float(spec["threshold"])
    print(f"dump   : {args.dump}")
    print(f"         sha256[:12]={sha12(raw)} bytes={len(raw)}")
    print(f"header : feat={int(head[0])} prob={int(head[1])} label={int(head[2])} "
          f"modelDim={md} redDim={rd}")
    print(f"spec   : threshold={thr} in={in_ch} out={out_ch} "
          f"minInstanceVoxels={spec['minInstanceVoxels']} maxCandidates={spec['maxCandidates']}")

    if md != (96, 96, 64) or rd != (96, 96):
        print("ERROR: 头部网格不是下颌管约定的 (96,96,64)@1.2mm —— 这台设备跑的不是这个模型")
        return 1
    if feat.size != in_ch * md[0] * md[1] * md[2] or prob.size != out_ch * md[0] * md[1] * md[2]:
        print("ERROR: feat/prob 元素数与 inChannels/outChannels × modelDim 不符")
        return 1
    ok = True

    # ---- 1) feat：主机按同一套特征公式复算 ----
    print("\n[1] feat 主机复算（case=%s, prep.build_features + 设备轴序转置）" % args.case)
    vol, hfeat, hyred, happ = canal.case_arrays(args.case)
    hfeat = np.ascontiguousarray(hfeat)
    if hfeat.size != feat.size:
        print(f"ERROR: 主机 feat {hfeat.size} != 真机 {feat.size}")
        return 1
    # 两端都是"设备轴序的扁平副本"，reshape 成 (C,96,96,64) 之后才是可喂给 ORT 的 5D 输入
    hf = hfeat.reshape((in_ch,) + md)
    d = np.abs(hf.ravel() - feat)
    print(f"    shape={hf.shape} bitwise={'YES' if np.array_equal(hf.ravel(), feat) else 'no '}"
          f" max|diff|={d.max():.3e} differing={int((hf.ravel() != feat).sum())}/{hf.size}")
    if float(d.max()) > args.feat_max_abs:
        ok = False
        print(f"    FAIL: feat 差 {float(d.max()):.3e} > {args.feat_max_abs:.3e}"
              " —— 输入不同则概率图不可比，后面的比对全部失去意义")

    # ---- 2) prob：同一份 .onnx，主机 ORT vs 真机 ORT ----
    print("\n[2] prob 主机复算（onnxruntime 跑 %s）" % os.path.basename(args.onnx))
    hp = host_prob_from_onnx(args.onnx, hf)
    if hp is None:
        ok = False
        print("    FAIL: 主机侧没有 onnxruntime，无法独立复算概率图（判定不能靠跳过）")
    else:
        pf = prob.reshape(out_ch, *md)
        d = np.abs(hp - pf)
        print(f"    shape={hp.shape} bitwise={'YES' if np.array_equal(hp, pf) else 'no '}"
              f" max|diff|={d.max():.3e} differing={int((hp != pf).sum())}/{hp.size}")
        print(f"    正类概率 max 主机 {hp[1].max():.6f} / 真机 {pf[1].max():.6f}")
        if float(d.max()) > args.prob_max_abs:
            ok = False
            print(f"    FAIL: prob 差 {float(d.max()):.3e} > {args.prob_max_abs:.3e}")

    # ---- 3) label：阈值判定 + 与 canal.py 冻结夹具对照 ----
    print("\n[3] label（prob[:,1] > %.2f）" % thr)
    dev_vox = int(label.sum())
    hlab = (hp[1] > thr) if hp is not None else None
    if hlab is not None:
        nd = int((hlab != label.reshape(md)).sum())
        print(f"    vs 主机 ORT 复算: mismatch={nd} voxels "
              f"(真机 {dev_vox} / 主机 {int(hlab.sum())})")
        if nd:
            ok = False
            print("    FAIL: 阈值判定后的掩膜不一致 —— 检查真机是否用了 spec 里的 threshold"
                  "（CanalEngine.run 传 model.threshold，日志 thr= 就是它）")
    fx = np.fromfile(args.label_fixture, dtype=np.uint8)
    if fx.size == label.size:
        nd = int((fx != label).sum())
        print(f"    vs 冻结夹具 canal_pred_0101.raw: mismatch={nd} "
              f"(夹具 {int(fx.sum())} / 真机 {dev_vox})")
        if nd:
            ok = False
            print("    FAIL: 与训练导出时落盘的掩膜夹具不同（权重/阈值/轴序三者之一变了）")
    else:
        print(f"    [warn] 夹具尺寸 {fx.size} != {label.size}，跳过（夹具来自另一次训练）")
    if args.expect_voxels:
        match = dev_vox == args.expect_voxels
        print(f"    vs 真机 logcat adoptCanalTracks 掩膜体素 {args.expect_voxels}: "
              f"{'一致' if match else '不一致'}")
        if not match:
            ok = False

    # ---- 4) inst：连通域编号（AiCore::connectedComponents 共用牙齿阈值 8）----
    print("\n[4] inst（26 邻域、>= %d 体素、按体素数降序重编号）" % MIN_INSTANCE_VOXELS)
    dev_hist = np.bincount(inst.astype(np.int64))
    dev_ids = [i for i in range(1, len(dev_hist)) if dev_hist[i] > 0]
    comps = components_26(label, md)
    print(f"    真机实例数={len(dev_ids)} 主机复算={len(comps)}；"
          f"label 有 {dev_vox} 体素，inst 覆盖 {int(inst.astype(bool).sum())} 体素")
    dev_sizes = [int(dev_hist[i]) for i in dev_ids]
    host_sizes = [len(c) for c in comps]
    if dev_sizes == host_sizes:
        print(f"    尺寸序列（降序）一致: {dev_sizes}")
    else:
        ok = False
        print(f"    FAIL: 尺寸序列 真机 {dev_sizes} != 主机 {host_sizes}")
    # 尺寸唯一时编号可以逐条对上；有并列时只比集合（std::sort 不稳定，并列块换号合法）
    if len(set(dev_sizes)) == len(dev_sizes) and dev_sizes == host_sizes:
        bad = 0
        for q, c in enumerate(comps):
            dev_pts = np.argwhere(inst.reshape(md) == dev_ids[q])
            if dev_pts.shape != c.shape or not np.array_equal(
                    np.sort(dev_pts, 0), np.sort(c, 0)):
                bad += 1
        print(f"    点集逐实例比对: {'全部一致' if bad == 0 else f'{bad} 条不一致'}")
        if bad:
            ok = False
    elif dev_sizes == host_sizes:
        print("    （存在体素数并列的块，编号顺序按 C++ std::sort 属未定义，只比尺寸序列）")
    stray = int(((inst != 0) & (label == 0)).sum())
    print(f"    inst 非零但 label 为零的体素: {stray}（必须为 0）")
    if stray:
        ok = False

    # ---- 5) 中心线：真机 label 走 canal.centerlines，与 metrics_canal.json 对账 ----
    print("\n[5] 中心线（NerveAutoTracker 规则 vs canal.py::centerlines）")
    tracks = canal.centerlines(label.reshape(md), spacing=(1.2, 1.2, 1.2))
    print(f"    条数={len(tracks)} 体素数={[t['voxels'] for t in tracks]}")
    print(f"    半径 mm={[round(t['radius_mm'], 4) for t in tracks]}")
    print(f"    点数={[len(t['points']) for t in tracks]} 描记轴={[t['trace_axis'] for t in tracks]}")
    if args.expect_tracks:
        match = len(tracks) == args.expect_tracks
        print(f"    vs 真机 logcat「canal track: {args.expect_tracks} 条」: "
              f"{'一致' if match else '不一致'}")
        if not match:
            ok = False
    if os.path.isfile(args.metrics):
        mc = json.load(open(args.metrics)).get("centerline", {})
        hp_r = [round(x, 2) for x in mc.get("pred_radius_mm", [])]
        hp_p = list(mc.get("pred_points", []))
        dev_r = [round(t["radius_mm"], 2) for t in tracks]
        dev_p = [len(t["points"]) for t in tracks]
        print(f"    metrics_canal.json 半径 {hp_r} / 真机 label 复算 {dev_r}")
        print(f"    metrics_canal.json 点数 {hp_p} / 真机 label 复算 {dev_p}")
        if hp_r and hp_r != dev_r:
            ok = False
            print("    FAIL: 半径不同 —— 掩膜相同却半径不同，说明两端中位数/截面积口径分叉")
        if hp_p and hp_p != dev_p:
            ok = False
            print("    FAIL: 折线点数不同 —— 分组（沿描记轴）口径分叉")
    else:
        print("    [warn] 没有 metrics_canal.json，跳过第 5 段对账")

    print("\nVERDICT:", "PASS - 下颌管通道的 feat/prob/label/inst/中心线与主机复算一致"
          if ok else "FAIL - 见上面的 FAIL 行")
    return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main())
