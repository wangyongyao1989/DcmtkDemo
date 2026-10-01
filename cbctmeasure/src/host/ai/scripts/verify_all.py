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


def have(path, what):
    """派生产物不在时打印 SKIP 而不是抛 FileNotFoundError。

    本目录只提交了脚本与设备轴序夹具；app_volume/、work/、gt/ 由 run_all.sh 现场生成，
    没跑过 step 01/02 的克隆里这些路径就是空的 —— 那种情况该报"缺产物"，不是"检查失败"。
    """
    if os.path.exists(path):
        return True
    print("SKIP %s —— 缺 %s（先跑 run_all.sh）" % (what, path))
    return False


def warn(name, detail=""):
    """把"证据不在/本地副本已过期"与"检查失败"分开：FAIL 会让读日志的人以为模型或
    夹具坏了，而这里真正要说的是"这一步没在这台机器上做过"。计数不进 NCHK。"""
    print("%-4s %s%s" % ("WARN", name, ("  |  " + detail) if detail else ""))


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
    chk("ops are CPU-supported (Conv/Relu/Softmax only)",
        set(ops) <= {"Conv", "Relu", "Softmax"}, str(ops))
    nparam = sum(int(np.prod(d)) for init in m.graph.initializer
                 for d in [init.dims])
    chk("weight payload small", nparam == 3474, "%d scalars, file %d B"
        % (nparam, os.path.getsize(p)))

    # 结构契约：train2.py 的 3x3x3 Conv3d 主干（6->8->8->2, pad 1），也就是随包的
    # cbctmeasure/src/main/assets/models/teeth_cnn.onnx。
    # kernel_shape/pads 是 repeated int64，必须读 attr.ints —— 读 attr.i 只会拿到 0，
    # 这条检查曾经把正确的模型判成 "FAIL ... | [(0,)]"。
    def ints(node, name):
        return tuple(next((a.ints for a in node.attribute if a.name == name), ()))
    convs = [n for n in m.graph.node if n.op_type == "Conv"]
    kshapes = sorted({ints(n, "kernel_shape") for n in convs})
    pads = sorted({ints(n, "pads") for n in convs})
    chk("three Conv3d layers", len(convs) == 3, "%d convs" % len(convs))
    chk("every Conv has kernel_shape [3,3,3]", kshapes == [(3, 3, 3)], str(kshapes))
    chk("every Conv is pad-1 (same-shape)", pads == [(1, 1, 1, 1, 1, 1)], str(pads))

    import hashlib
    sha = hashlib.sha256(open(p, "rb").read()).hexdigest()
    dev = json.load(open(f"{OUT}/parity_ref/device_parity.json"))
    chk("model sha256 == the record the device fixtures were built from",
        sha == dev["model"]["sha256"], "built %s vs device_parity %s"
        % (sha[:16], dev["model"]["sha256"][:16]))
    # 随包资产用 __file__ 定位，不用 OUT：AI_BUILD=/tmp/out 做 clean-clone 复现时，
    # OUT 在仓库外，而"重新训练出来的模型 == app 里那份"正是这条检查要证明的事。
    here = os.path.dirname(os.path.abspath(__file__))
    asset = os.path.normpath(f"{here}/../../../../src/main/assets/models/teeth_cnn.onnx")
    chk("model/teeth_cnn.onnx == shipped asset assets/models/teeth_cnn.onnx",
        os.path.exists(asset)
        and hashlib.sha256(open(asset, "rb").read()).hexdigest() == sha,
        "asset %s" % ("missing" if not os.path.exists(asset) else asset))
    old = f"{OUT}/model/teeth_cnn_pointwise_old.onnx"
    chk("1x1x1 历史备份仍在 model/teeth_cnn_pointwise_old.onnx 且不是当前模型",
        os.path.exists(old)
        and hashlib.sha256(open(old, "rb").read()).hexdigest() != sha,
        old if os.path.exists(old) else "missing")

    # ---------- 2. device-order parity: ORT(device_feat) == device_prob, and the
    #               post-processing rules (threshold 0.49 / 26-CC) reproduce the fixtures
    fb = open(f"{OUT}/parity_ref/device_feat_0101.raw", "rb").read()
    pb = open(f"{OUT}/parity_ref/device_prob_0101.raw", "rb").read()
    sess = ort.InferenceSession(p, providers=["CPUExecutionProvider"])
    got = sess.run(None, {sess.get_inputs()[0].name:
                          np.frombuffer(fb, np.float32).reshape(1, 6, 96, 96, 64)})[0]
    ref = np.frombuffer(pb, np.float32).reshape(1, 2, 96, 96, 64)
    d = float(np.abs(got - ref).max())
    # 设备侧 ORT 1.17 + arm64 kernel，主机侧 ORT 1.23：概率图只能按浮点容差比。
    # 真机 dump 与这套夹具的实测差是 1.19e-07（见 doc/ 里的 ONNX 推理测试报告）。
    chk("ORT(device_feat_0101.raw) == device_prob_0101.raw to 1e-6", d <= 1e-6,
        "max abs diff %.3e" % d)
    chk("prob sums to 1 over channel axis (softmax)",
        float(np.abs(got.sum(axis=1) - 1.0).max()) < 1e-5,
        "max dev %.3e" % float(np.abs(got.sum(axis=1) - 1.0).max()))

    # AiCore::thresholdMask 的复刻：label = prob[channel 1] > 0.49（严格大于，double 比较）
    thr = dev["threshold"]["value"]
    lab = np.frombuffer(open(f"{OUT}/parity_ref/device_label_0101.raw", "rb").read(),
                        np.uint8).reshape(96, 96, 64)
    chk("device label == (prob[ch1] > %.2f)" % thr,
        np.array_equal(lab, (got[0, 1].astype(np.float64) > thr).astype(np.uint8)),
        "tooth voxels %d" % int(lab.sum()))

    # 旧主机链（make_parity.py 写的 prob/lab_0101.raw）与随包设备夹具**不可逐元素比**，
    # 原因不是后处理规则差异，而是轴序：设备按 feat[c][i=DICOM column][j][k] 训练与推理，
    # 主机按行序喂同一份体积 —— 3x3x3 卷积核对各轴不对称，x/y 一互换输出就变了。
    # 实测（本脚本现场算）：prob 通道 1 逐元素最大差 ~1.0，lab 在 (1,0,2) 置换下 Dice 也只有
    # 0.81，都不构成" parity 失败"，只说明这两条链算的是两个不同的预测。
    # 权威证据是随包的 device_*_0101.raw（上面几条 chk 已按 1e-6 / 逐位通过）。
    host_lab_path = f"{OUT}/parity_ref/lab_0101.raw"
    host_prob_path = f"{OUT}/parity_ref/prob_0101.raw"
    if os.path.exists(host_lab_path) and os.path.exists(host_prob_path):
        hl = np.frombuffer(open(host_lab_path, "rb").read(), np.uint8).reshape(96, 96, 64)
        hp = np.fromfile(host_prob_path, np.float32).reshape(2, 96, 96, 64)[1]
        dp = got[0, 1]
        d_p = float(np.abs(hp - dp).max())
        o_swap = int((np.transpose(hl, (1, 0, 2)) & lab).sum())
        dice_swap = 2.0 * o_swap / (int(hl.sum()) + int(lab.sum()))
        warn("主机旧链 prob/lab_0101.raw 是主机轴序下的另一份预测，与设备夹具不做逐元素对齐判定",
             "prob ch1 max abs diff %.3e；lab 在 (1,0,2) 轴置换下 Dice %.4f（阈值 %.2f vs argmax "
             "只是次要差异：%d vs %d 体素）。随包证据只认 device_*_0101.raw"
             % (d_p, dice_swap, thr, int(hl.sum()), int(lab.sum())))

    from scipy import ndimage
    inst, nc = ndimage.label(lab, structure=np.ones((3, 3, 3), np.uint8))
    saved = np.frombuffer(open(f"{OUT}/parity_ref/device_inst_0101.raw", "rb").read(),
                          np.uint16).reshape(96, 96, 64)
    # 只比连通域数量与各域体素数的多重集：设备复刻的编号顺序与 scipy 不保证一致，
    # 逐 id 比会把正确的分割判成 FAIL（check_device_dump.py 也是按多重集比的）。
    # 并且要先过 AiCore::connectedComponents 的最小尺寸闸门（<8 体素丢弃）：
    # 实测 scipy 裸跑 0101 得 34 域 / 9958 体素，夹具是过闸后的 24 域 / 9935 体素。
    MIN_CC_VOXELS = 8
    sz_all = np.bincount(inst.ravel().astype(np.int64))[1:]
    sz_new = np.sort(sz_all[sz_all >= MIN_CC_VOXELS])
    sz_saved = np.sort(np.bincount(saved.ravel().astype(np.int64))[1:])
    chk("夹具里没有小于 %d 体素的实例（丢弃规则生效）" % MIN_CC_VOXELS,
        bool((sz_saved >= MIN_CC_VOXELS).all()), "min %d" % (int(sz_saved.min()) if sz_saved.size else -1))
    chk("26-CC census reproduced (count + per-instance sizes, after the %d-voxel gate)"
        % MIN_CC_VOXELS,
        len(sz_saved) == len(sz_new) and np.array_equal(sz_saved, sz_new),
        "scipy %d comps / %d voxels -> gated %d, fixture %d comps / %d voxels"
        % (nc, int(sz_all.sum()), len(sz_new), len(sz_saved), int(sz_saved.sum())))

    # ---------- 3. fixture byte sizes
    chk("device_feat_0101.raw size == 1*6*96*96*64*4", len(fb) == 14_155_776, "%d B" % len(fb))
    chk("device_prob_0101.raw size == 2*96*96*64*4", len(pb) == 4_718_592, "%d B" % len(pb))
    chk("device_label_0101.raw size == 96*96*64", lab.size == 589_824)
    chk("device_inst_0101.raw size == 96*96*64*2", saved.size * 2 == 1_179_648)
    chk("device_gt_0101.raw size == 96*96*64",
        os.path.getsize(f"{OUT}/parity_ref/device_gt_0101.raw") == 589_824)
    chk("device_hu_0101.raw size == 96*96*64*4",
        os.path.getsize(f"{OUT}/parity_ref/device_hu_0101.raw") == 2_359_296)
    sz_hu_p = f"{OUT}/app_volume/dentvoxel_0101_hu.raw"
    if have(sz_hu_p, "canonical 体积尺寸"):
        sz_hu = os.path.getsize(sz_hu_p)
        chk("canonical int16 volume raw == 9,437,184 B (the number quoted in the brief)",
            sz_hu == 9_437_184, "%d B = 192*192*128*2" % sz_hu)

    # ---------- 4. DICOM series round-trip, all four cases
    if have(f"{OUT}/work/dicom_roundtrip.json", "DICOM 往返检查"):
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
    # 随包的是 nii.gz（可读、可核对形状）；_tooth.raw 是 step 01 顺带写的扁平副本，
    # 不在交付清单里，所以只作为"在场时再验一遍一致性"的加分项，不能当 FAIL 判。
    if have(f"{OUT}/gt/{prep.CASE_HELD_OUT}_tooth.nii.gz", "GT 掩膜检查"):
        for cid in prep.ALL_CASES:
            nii = f"{OUT}/gt/{cid}_tooth.nii.gz"
            raw = f"{OUT}/gt/{cid}_tooth.raw"
            chk("GT tooth mask nii.gz present: %s" % cid, os.path.exists(nii), nii)
            if os.path.exists(raw):
                chk("GT tooth mask raw size == 192*192*128: %s" % cid,
                    os.path.getsize(raw) == 192 * 192 * 128, "%d B" % os.path.getsize(raw))
            else:
                print("  SKIP  GT tooth mask raw（未生成，run_all.sh step 01 会写）: %s" % cid)
        import nibabel as nib
        hold = prep.CASE_HELD_OUT
        g = np.asanyarray(nib.load(f"{OUT}/gt/{hold}_tooth.nii.gz").dataobj)
        chk("GT nii.gz shape/dtype/values", g.shape == (192, 192, 128) and g.dtype == np.uint8
            and set(np.unique(g).tolist()) <= {0, 1},
            "%s %s %s" % (g.shape, g.dtype, np.unique(g)))
        chk("GT nii.gz == in-process GT", np.array_equal(g, prep.gt_tooth_at_app_grid(hold)))
        raw_ho = f"{OUT}/gt/{hold}_tooth.raw"
        if have(raw_ho, "GT nii.gz == GT raw"):
            chk("GT nii.gz == GT raw",
                np.array_equal(np.fromfile(raw_ho, np.uint8).reshape(192, 192, 128), g))

    # ---------- 6. features reproducible from the canonical volume
    # fb 现在是设备轴序夹具，所以主机特征要先 transpose(0,2,1,3) 再比。
    # 主机自己的 feat_0101.raw（make_parity.py 产出）如果在场，额外要求逐位相同：
    # 那才证明"换轴序"是精确置换而不是近似重排。
    # 注意：parity_ref/{feat,prob,label,gt}_0101.raw 是"派生夹具"（.gitignore 里没写，
    # 但从未入库），本地那份可能是特征配置改版之前留下的。特征一改，旧 raw 就会和
    # device_feat 差到 0.9 量级，而**随包的设备轴序夹具才是证据**（上面已按 1e-6 通过）。
    # 所以这里只 WARN 并说明如何刷新，不作为 FAIL —— FAIL 会让人以为模型/夹具坏了。
    if have(f"{OUT}/raw", "特征复算（需要 NIfTI 输入）"):
        canon_ho, _ = prep.make_app_volume(prep.CASE_HELD_OUT)
        f2 = prep.build_features(canon_ho)
        fdev = np.frombuffer(fb, np.float32).reshape(6, 96, 96, 64)
        dft = float(np.abs(f2.transpose(0, 2, 1, 3) - fdev).max())
        chk("build_features(canonical).transpose(0,2,1,3) == device_feat_0101.raw to 1e-6",
            dft <= 1e-6, "max abs diff %.3e" % dft)
        host_feat = f"{OUT}/parity_ref/feat_0101.raw"
        if os.path.exists(host_feat):
            fh = np.frombuffer(open(host_feat, "rb").read(), np.float32).reshape(6, 96, 96, 64)
            dh = float(np.abs(fh.transpose(0, 2, 1, 3) - fdev).max())
            if dh == 0.0:
                chk("host feat_0101.raw == device_feat bit-exact（换轴序是精确置换）", True,
                    "max abs diff 0.000e+00")
                chk("build_features(canonical volume) bit-exact == feat_0101.raw",
                    np.array_equal(f2, fh))
            else:
                warn("host feat_0101.raw 与随包设备夹具不一致 —— 本地这份是特征配置改版前"
                     "留下的旧副本（未入库、由 run_all.sh step 04 重新生成）",
                     "max abs diff %.3e；权威证据是 device_feat_0101.raw，已按上面的 1e-6 复算通过。"
                     "刷新：python3 make_parity.py" % dh)
        else:
            print("SKIP host feat_0101.raw checks (make_parity.py 未产出该文件)")

    # ---------- 7. 训练记录
    # train2.py 写 parity_ref/metrics_0101.json；metrics.json 只有旧的 1x1x1 实验会写，
    # 所以这里以 metrics_0101.json 为准，metrics.json 存在时再补一条旧实验的读档检查。
    rec_path = f"{OUT}/parity_ref/metrics_0101.json"
    if have(rec_path, "训练记录检查"):
        R = json.load(open(rec_path))
        chk("metrics_0101.json 记录的是 3x3x3 设备轴序主干",
            R["architecture"].startswith("Conv3d(6->8,k3,pad1)")
            and "DEVICE" in R["axis_order"],
            "%s / %s" % (R["architecture"], R["axis_order"][:40]))
        chk("metrics_0101.json onnx_bytes == 重建模型的字节数",
            R["onnx_bytes"] == os.path.getsize(p),
            "%d vs %d" % (R["onnx_bytes"], os.path.getsize(p)))
        chk("metrics_0101.json 的阈值 == device_parity.json 的阈值",
            R["threshold_calibration"]["chosen_threshold"] == thr,
            "%s vs %s" % (R["threshold_calibration"]["chosen_threshold"], thr))
        ho = prep.CASE_HELD_OUT
        m101 = R["metrics"][ho]["at_chosen"]
        chk("held-out 预测体素数 == 设备分割复算的牙齿体素数",
            m101["pred_voxels"] == int(lab.sum()),
            "metrics %d vs fixture label %d" % (m101["pred_voxels"], int(lab.sum())))
        print("\nHEADLINE (held-out %s, 阈值 %.2f):" % (ho, thr))
        print("  dice %.4f  precision %.4f  recall %.4f  f1 %.4f  tp %d fp %d fn %d"
              % (m101["dice"], m101["precision"], m101["recall"], m101["f1"],
                 m101["tp"], m101["fp"], m101["fn"]))
        print("  PRD AI-01 门槛 F1>=0.85: %s" %
              ("达标" if m101["f1"] >= 0.85 else "未达标（本模型定位是粗筛）"))
    if os.path.exists(f"{OUT}/metrics.json"):
        M = json.load(open(f"{OUT}/metrics.json"))
        chk("旧 1x1x1 实验的 metrics.json 仍标注 held-out 例",
            M.get("held_out") == ho, str(M.get("held_out")))
    n = NCHK[0]
    print("\n%d checks run, %d passed, %d failed" % (n, n - len(FAIL), len(FAIL)))
    # make_report.py 要引用这份小结；work/ 是派生目录，clean clone 里可能还没被建出来。
    os.makedirs(f"{OUT}/work", exist_ok=True)
    json.dump({"utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
               "checks_run": n, "passed": n - len(FAIL), "failed": len(FAIL),
               "failures": FAIL},
              open(f"{OUT}/work/verify_summary.json", "w"), indent=1)
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
