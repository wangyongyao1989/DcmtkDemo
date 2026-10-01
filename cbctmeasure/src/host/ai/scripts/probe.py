"""探针：确认 HU 体积与 GT mask 的空间对齐关系、1.2mm 网格上的类别占比。"""
import numpy as np, nibabel as nib, json, glob, os

_AI_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.environ.get("DENTAL_SRC") or os.path.join(_AI_ROOT, "raw")
OUT = os.environ.get("AI_BUILD") or _AI_ROOT
CASES = ["0021", "0047", "0074", "0101"]

for c in CASES:
    v = nib.load(f"{SRC}/dentvoxel_{c}.nii.gz")
    m = nib.load(f"{SRC}/dentvoxel_{c}_seg_groundtruth.nii.gz")
    va, ma = np.asanyarray(v.dataobj), np.asanyarray(m.dataobj)
    print(f"--- case {c} vol{va.shape}{v.header.get_zooms()} mask{ma.shape}{m.header.get_zooms()}")
    print("   vol aff=\n", np.round(v.affine, 3))
    print("   msk aff=\n", np.round(m.affine, 3))
    # mask 每维除 2 == volume 尺寸？
    print("   mask/2 =", tuple(np.array(ma.shape) // 2), " vol =", va.shape)
    labs = np.unique(ma)
    print("   labels:", labs.tolist())
    counts = {int(l): int((ma == l).sum()) for l in labs}
    print("   counts:", counts)
    hu = va.astype(np.int32)
    print("   HU min/max/mean:", int(hu.min()), int(hu.max()), int(hu.mean()))
    break

# 网格对齐：在 1.2mm 网格上（vol 抽稀 2、mask 抽稀 4）统计牙齿体素占比
def decim(a, k):
    s = tuple(0 for _ in a.shape)
    sl = tuple(slice(None, None, k) for _ in a.shape)
    return a[sl]

tot = {}
for c in CASES:
    va = np.asanyarray(nib.load(f"{SRC}/dentvoxel_{c}.nii.gz").dataobj)
    ma = np.asanyarray(nib.load(f"{SRC}/dentvoxel_{c}_seg_groundtruth.nii.gz").dataobj)
    vd, md = decim(va, 2), decim(ma, 4)
    print(f"case {c}: vol2={vd.shape} mask4={md.shape} match={vd.shape == md.shape}")
    tooth = np.isin(md, np.arange(4, 33))
    print("   tooth frac = %.4f  bone(1,2) frac = %.4f  canal(35..38) frac = %.4f" % (
        tooth.mean(), np.isin(md, [1, 2]).mean(), np.isin(md, [35, 36, 37, 38]).mean()))
    tot[c] = dict(shape=list(md.shape), tooth=float(tooth.mean()),
                  hu=[float(vd.min()), float(vd.max()), float(np.median(vd))])
json.dump(tot, open(f"{OUT}/work/probe.json", "w"), indent=1)
print(json.dumps(tot, indent=1))
