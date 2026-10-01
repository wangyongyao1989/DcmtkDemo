#!/usr/bin/env python3
"""check_device_dump.py -- AC-08 parity verdict between a real-device AiEngine::dumpParity
file and the host device-order fixtures in parity_ref/ (see make_parity_device.py).

Device dump layout (verified against cbctmeasure/src/main/cpp/core/AiEngine.cpp:143-177):
    64-byte header = 8 x int64 little-endian:
        featCount, probCount, labelBytes, modelDim0, modelDim1, modelDim2, redDim0, redDim1
    then:  float32 feat[featCount] | float32 prob[probCount] | uint8 label[labelBytes]
           | int16 inst[labelBytes]          (inst element count == labelBytes)

Exit codes: 0 = PASS, 2 = FAIL thresholds exceeded, 1 = format/size error.
numpy only. Thresholds via CLI flags.

NOTE: the header does NOT carry the probability threshold.  The device must have been
run with threshold = 0.49 (nativeRunSegment(handle, threshold, keepParity)); otherwise
label/inst mismatches are expected and this script will (correctly) report FAIL.
"""
import argparse
import hashlib
import os
import sys

import numpy as np

FIXTURES = {
    "feat": ("device_feat_0101.raw", "<f4"),
    "prob": ("device_prob_0101.raw", "<f4"),
    "label": ("device_label_0101.raw", "u1"),
    "inst": ("device_inst_0101.raw", "<i2"),
}
# expected header counts (case 0101, model grid 96x96x64)
EXP = {"featCount": 6 * 96 * 96 * 64, "probCount": 2 * 96 * 96 * 64,
       "labelBytes": 96 * 96 * 64, "modelDim": (96, 96, 64), "redDim": (96, 96)}


def sha12(b):
    return hashlib.sha256(b).hexdigest()[:12]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dump", help="path to device .parity.bin from AiEngine::dumpParity")
    _ai_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument("--fixtures-dir",
                    default=os.environ.get("AI_BUILD") or os.path.join(_ai_root, "parity_ref"))
    # feat 的默认容差不可能是 0：真机是 ORT 1.17 + arm64 kernel，主机夹具是 ORT 1.23 + x86，
    # 同一条 2x2x2 抽稀/特征链在 float32 上就会差到 1e-8 量级。
    # 实测（2026-10-01 round6 拉回的 dump）：max|diff| = 2.980e-08，799/3538944 个元素非零差；
    # prob 同批实测 1.192e-07，label/inst 逐位相同。默认取 1e-6 让「正常设备」直接 PASS，
    # 而 0.0 这个默认值会让任何人都量出 FAIL，把"判定"变成"必须记得加 flag 的仪式"。
    ap.add_argument("--feat-max-abs", type=float, default=1e-6,
                    help="max allowed |feat diff| (default 1e-6: 跨 ORT 版本/架构不可能逐位相同)")
    ap.add_argument("--prob-max-abs", type=float, default=1e-5,
                    help="max allowed |prob diff| (default 1e-5)")
    ap.add_argument("--label-max-rate", type=float, default=0.001,
                    help="max allowed label mismatch voxel rate (default 0.001 = 0.1%%)")
    ap.add_argument("--inst-require-exact", action="store_true",
                    help="also fail unless the inst array matches the fixture bitwise")
    args = ap.parse_args()

    if not os.path.isfile(args.dump):
        print(f"ERROR: dump not found: {args.dump}")
        return 1
    raw = open(args.dump, "rb").read()
    if len(raw) < 64:
        print("ERROR: dump shorter than 64-byte header")
        return 1
    head = np.frombuffer(raw[:64], dtype="<i8")
    feat_n, prob_n, label_n, md0, md1, md2, rd0, rd1 = (int(x) for x in head)
    print(f"header: featCount={feat_n} probCount={prob_n} labelBytes={label_n} "
          f"modelDim=({md0},{md1},{md2}) redDim0/1=({rd0},{rd1})")
    if (feat_n, prob_n, label_n) != (EXP["featCount"], EXP["probCount"], EXP["labelBytes"]) \
            or (md0, md1, md2) != EXP["modelDim"] or (rd0, rd1) != EXP["redDim"]:
        print("ERROR: header counts/dims do not match the case-0101 device fixtures "
              f"(expected feat {EXP['featCount']}, prob {EXP['probCount']}, label {EXP['labelBytes']}, "
              f"modelDim {EXP['modelDim']}, redDim {EXP['redDim']})")
        return 1
    need = 64 + 4 * feat_n + 4 * prob_n + label_n + 2 * label_n
    if len(raw) != need:
        print(f"ERROR: dump size {len(raw)} != header-implied {need}")
        return 1
    off = 64
    feat = np.frombuffer(raw, dtype="<f4", count=feat_n, offset=off);      off += 4 * feat_n
    prob = np.frombuffer(raw, dtype="<f4", count=prob_n, offset=off);      off += 4 * prob_n
    label = np.frombuffer(raw, dtype=np.uint8, count=label_n, offset=off); off += label_n
    inst = np.frombuffer(raw, dtype="<i2", count=label_n, offset=off)
    print(f"dump sha256[:12]={sha12(raw)} bytes={len(raw)}")

    ok = True
    for key, arr in (("feat", feat), ("prob", prob), ("label", label), ("inst", inst)):
        fname, dt = FIXTURES[key]
        path = os.path.join(args.fixtures_dir, fname)
        host = np.fromfile(path, dtype=dt)
        if host.size != arr.size:
            print(f"ERROR: fixture {fname} size {host.size} != dump {arr.size}")
            return 1
        same = np.array_equal(host, arr)
        if key == "feat":
            d = np.abs(host - arr)
            nd = int((host != arr).sum())
            print(f"feat : bitwise={'YES' if same else 'no '} max|diff|={d.max():.3e} "
                  f"differing={nd}/{host.size}")
            if float(d.max()) > args.feat_max_abs:
                ok = False
                print(f"  FAIL: feat max|diff| {float(d.max()):.3e} > {args.feat_max_abs:.3e}")
        elif key == "prob":
            d = np.abs(host - arr)
            print(f"prob : bitwise={'YES' if same else 'no '} max|diff|={d.max():.3e} "
                  f"differing={int((host != arr).sum())}/{host.size}")
            if float(d.max()) > args.prob_max_abs:
                ok = False
                print(f"  FAIL: prob max|diff| {float(d.max()):.3e} > {args.prob_max_abs:.3e}")
        elif key == "label":
            nd = int((host != arr).sum())
            rate = nd / host.size
            thr_hits = int(arr.sum())
            print(f"label: bitwise={'YES' if same else 'no '} mismatch={nd} voxels "
                  f"({rate * 100:.6f}% of {host.size}) ; device tooth voxels={thr_hits}")
            if rate > args.label_max_rate:
                ok = False
                print(f"  FAIL: label mismatch rate {rate:.6f} > {args.label_max_rate:.6f}")
        else:  # inst
            nd = int((host != arr).sum())
            print(f"inst : bitwise={'YES' if same else 'no '} exact-match-rate="
                  f"{100.0 * (host.size - nd) / host.size:.4f}% mismatch={nd}")
            hx = np.bincount(host.astype(np.int64))
            dx = np.bincount(arr.astype(np.int64))
            ids = sorted(set(np.nonzero(hx)[0]) | set(np.nonzero(dx)[0]))
            print("per-instance voxel counts (host -> device):")
            inst_count_bad = False
            for i in ids[1:] if 0 in ids else ids:
                h, dev = int(hx[i]) if i < hx.size else 0, int(dx[i]) if i < dx.size else 0
                flag = "" if h == dev else "   <-- DIFFERS"
                if h != dev:
                    inst_count_bad = True
                print(f"  id {i:3d}: {h:6d} -> {dev:6d}{flag}")
            if inst_count_bad:
                ok = False
                print("  FAIL: per-instance voxel counts differ "
                      "(remember std::sort instability: ids of EQUAL-sized components "
                      "may legally swap; sizes multiset must still match)")
            hm, dm = sorted(hx.tolist(), reverse=True)[:8], sorted(dx.tolist(), reverse=True)[:8]
            print(f"  host size multiset (top8 nonbg): {hm}  device: {dm}")
            if args.inst_require_exact and not same:
                ok = False
                print("  FAIL: --inst-require-exact set but inst differs bitwise")

    print("VERDICT:", "PASS - device dump matches host device-order fixtures within tolerances"
          if ok else "FAIL - see FAIL lines above")
    return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main())
