"""repro_check.py -- prove the preprocessing is exactly reproducible WITHOUT deleting
the shipped artefacts (another host process works in this same tree, so we rebuild
into work/repro_check/ and byte-compare).

Checks, for all four cases:
  * canonical int16 volume recomputed == shipped app_volume/<case>_hu.raw bytes
  * DICOM series rewritten into work/repro_check/<case>/ is byte-for-byte identical
    to the shipped app_volume/<case>/0000NN.dcm files (same UIDs, same tags, same
    pixel data) -- i.e. the device assets can be regenerated bit-exactly
  * re-read of the *rebuilt* series == canonical array (np.array_equal)
  * GT mask recomputed == shipped gt/<case>_tooth.raw bytes
  * build_features(canonical) == shipped parity_ref/feat_0101.raw (held-out case)
Writes work/repro_check.json and prints PASS/FAIL.  Exits non-zero on any failure.
"""
import hashlib
import json
import os
import sys
import numpy as np
import prep

OUT = prep.OUT_DIR
REPRO = f"{OUT}/work/repro_check"
FAIL = []
NCHK = [0]


def sha(b):
    return hashlib.sha256(b).hexdigest()


def shafile(p):
    return sha(open(p, "rb").read())


def chk(name, ok, detail=""):
    NCHK[0] += 1
    print("%-4s %s%s" % ("PASS" if ok else "FAIL", name, ("  |  " + detail) if detail else ""))
    if not ok:
        FAIL.append(name)
    return ok


def main():
    os.makedirs(REPRO, exist_ok=True)
    res = {}
    for cid in prep.ALL_CASES:
        vol, meta = prep.make_app_volume(cid)
        gt = prep.gt_tooth_at_app_grid(cid)
        # 1. raw HU mirror
        shipped_raw = f"{OUT}/app_volume/{cid}_hu.raw"
        b = vol.tobytes(order="C")
        chk("canonical volume bytes == shipped %s_hu.raw" % cid,
            sha(b) == shafile(shipped_raw), "%d B" % len(b))
        # 2. GT mirror
        gt_b = gt.tobytes(order="C")
        chk("GT mask bytes == shipped gt/%s_tooth.raw" % cid,
            sha(gt_b) == shafile(f"{OUT}/gt/{cid}_tooth.raw"), "%d B" % len(gt_b))
        # 3. rewrite the whole DICOM series and compare file bytes
        d = f"{REPRO}/{cid}"
        paths = prep.write_dicom_series(vol, d, cid)
        same, diffs = True, []
        for p in paths:
            q = f"{OUT}/app_volume/{cid}/{os.path.basename(p)}"
            if not os.path.exists(q) or shafile(p) != shafile(q):
                same = False
                diffs.append(os.path.basename(p))
        chk("rebuilt DICOM series byte-identical to shipped series: %s" % cid, same,
            "%d files, first mismatches %s" % (len(paths), diffs[:3]))
        # 4. round-trip the rebuilt series
        back = prep.read_dicom_series(d)
        chk("rebuilt DICOM series re-read == canonical: %s" % cid, np.array_equal(back, vol))
        res[cid] = {"n_files": len(paths), "series_bytes":
                    sum(os.path.getsize(p) for p in paths),
                    "dicom_files_byte_identical": same,
                    "raw_hu_sha256": shafile(shipped_raw),
                    "gt_raw_sha256": shafile(f"{OUT}/gt/{cid}_tooth.raw")}
    # 5. feature parity against the shipped fixture
    fb = f"{OUT}/parity_ref/feat_0101.raw"
    if os.path.exists(fb):
        v, _ = prep.make_app_volume(prep.CASE_HELD_OUT)
        f = prep.build_features(v)
        chk("build_features(canonical 0101) == shipped feat_0101.raw",
            sha(f.tobytes(order="C")) == shafile(fb),
            "%d B, sha %s" % (len(fb), shafile(fb)[:16]))
        res["feat_0101_sha256"] = shafile(fb)
    json.dump({"identical": not FAIL, "cases": res},
              open(f"{OUT}/work/repro_check.json", "w"), indent=1)
    print("\n%d checks run, %d failed" % (NCHK[0], len(FAIL)))
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
