"""
prep.py -- single source of truth for the host-side CBCT tooth-segmentation pipeline.

Every constant used by the Android integration is defined HERE and nowhere else.
See scripts/SPEC.md for the prose specification; this file is the executable
definition and the C++ port must match it bit-for-bit.

Axis convention (IMPORTANT)
---------------------------
All arrays in this pipeline are indexed in NIfTI *index order*:
    array.shape == (x, y, z)      # x = axis 0, y = axis 1, z = axis 2
The NIfTI affine (which for the source files flips patient x and y and increases z)
is DELIBERATELY IGNORED.  "x/y/z" below always mean array axes, never patient axes.

Dependencies: numpy only (scipy is used for connected components in other scripts).
"""

import json
import os

import numpy as np

# =============================================================================
# 0. Global constants
# =============================================================================
# Repo-relative defaults so a clean clone can regenerate every fixture without
# staging anything under /tmp.  Override when working out of tree:
#   DENTAL_SRC=/path/to/nifti  AI_BUILD=/path/to/out  bash scripts/run_all.sh
# The NIfTI inputs live in ../raw (see raw/README.md for provenance).
_AI_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DIR = os.environ.get("DENTAL_SRC") or os.path.join(_AI_ROOT, "raw")
OUT_DIR = os.environ.get("AI_BUILD") or _AI_ROOT

CASES_TRAIN = ["dentvoxel_0021", "dentvoxel_0047", "dentvoxel_0074"]
CASE_HELD_OUT = "dentvoxel_0101"
ALL_CASES = CASES_TRAIN + [CASE_HELD_OUT]

# --- source geometry (as actually downloaded; see SPEC.md section 2) ----------
SRC_SHAPE = (220, 220, 172)   # HU volume grid, index order (x, y, z)
SRC_SPACING_MM = 0.60         # isotropic, mm per voxel
GT_SHAPE = (440, 440, 344)    # ground-truth label grid, index order
GT_SPACING_MM = 0.30          # isotropic, mm per voxel
GT_TO_VOL_FACTOR = 2          # GT_TO_VOL_FACTOR * SRC_SPACING_MM == GT_SPACING_MM

# --- canonical app volume ----------------------------------------------------
APP_SHAPE = (192, 192, 128)   # (x, y, z)
APP_SPACING_MM = 0.60         # identical to source spacing => resample factor 1
RED_SHAPE = (96, 96, 64)      # app volume decimated by 2 (model grid)
RED_FACTOR = 2                # integer decimation factor app -> model grid

# --- jaw-centred crop, deterministic from HU ---------------------------------
BAND_HU_LO = 1200             # enamel/dentin band, lower HU bound (inclusive)
BAND_HU_HI = 4000             # enamel/dentin band, upper HU bound (inclusive)
BAND_PCT_LO = 1.0             #  1st percentile of band-voxel coordinates
BAND_PCT_HI = 99.0            # 99th percentile of band-voxel coordinates
CROP_MARGIN_VOX = (6, 6, 8)   # expand bbox on BOTH sides of each axis, in 0.6mm voxels

# --- HU clipping / normalisation ---------------------------------------------
HU_CLIP_LO = -1024
HU_CLIP_HI = 4095
HU_NORM_SCALE = 5120.0        # c0 = (clip(HU) + 1024) / 5120 - 0.5   -> [-0.5, +0.5]
HU_NORM_BIAS = 1024.0
HU_NORM_SHIFT = 0.5

# --- feature box radii (in REDUCED-grid voxels; box half-width, box = 2r+1) --
FEAT_RADII = (1, 2, 4, 8)     # c1, c2, c3, c4
BANDPASS_PAIR = (3, 4)        # c5 = c3 - c4   (radius 4 minus radius 8)
BOUNDARY_MODE = "replicate"   # np.pad(mode='edge'): clamp index to [0, n-1]

# --- GT resampling (0.30 mm label grid -> 0.60 mm app grid) ------------------
GT_VOTE_BLOCK = 8             # 2x2x2 source voxels per output voxel
GT_VOTE_THRESHOLD = 4         # output voxel is tooth iff (#tooth in the 8) >= 4
                              # (chosen on train cases only, see SPEC.md 5.3)

# --- DICOM series ------------------------------------------------------------
DICOM_ROWS = 192              # pixel rows    <-> canonical axis 0 (x)
DICOM_COLUMNS = 192           # pixel columns <-> canonical axis 1 (y)
DICOM_SLICES = 128            # one file per z index
DICOM_FILENAME_FMT = "%06d.dcm"   # 1-based: 000001.dcm ... 000128.dcm
DICOM_XY_PITCH_MM = 0.6
DICOM_SLICE_THICKNESS_MM = 0.6
DICOM_IPP_X0 = 0.0            # synthetic, patient-frame origin of slice 1
DICOM_IPP_Y0 = 0.0
DICOM_IPP_Z0 = 0.0            # IPP z of slice n (1-based) = Z0 + (n-1)*0.6
DICOM_ROW_COSINE = (1.0, 0.0, 0.0)   # identical to neck_ct/00000001.dcm
DICOM_COL_COSINE = (0.0, 1.0, 0.0)   # identical to neck_ct/00000001.dcm
DICOM_TRANSFER_SYNTAX = "1.2.840.10008.1.2.1"   # Explicit VR Little Endian (uncompressed)
DICOM_SOP_CLASS_UID = "1.2.840.10008.5.1.4.1.1.2"  # CT Image Storage (same as neck_ct)
DICOM_PATIENT_NAME = "DENTAL^CBCT^TEST"
DICOM_PATIENT_ID_FMT = "DENTVOXEL%04d"          # -> DENTVOXEL0021 ... DENTVOXEL0101
DICOM_PATIENT_SEX = "O"
DICOM_PATIENT_AGE = "030Y"
DICOM_STUDY_DATE = "20260101"
DICOM_STUDY_DESC = "SYNTHETIC CBCT DENTAL"
DICOM_SERIES_DESC = "DENTAL CBCT 0.6MM 192X192X128"
DICOM_UID_ROOT = "1.2.826.0.1.3680043.10.474.900000"  # + case digits + role -> unique UIDs
DICOM_WINDOW_CENTER = 1500    # display only, does NOT affect the model
DICOM_WINDOW_WIDTH = 3000
DICOM_RESCALE_INTERCEPT = 0   # stored int16 == HU exactly  (see SPEC.md 6.4)
DICOM_RESCALE_SLOPE = 1
DICOM_BITS_ALLOCATED = 16
DICOM_BITS_STORED = 16
DICOM_HIGH_BIT = 15
DICOM_PIXEL_REPRESENTATION = 1        # signed
DICOM_PIXEL_PADDING_VALUE = -2000
DICOM_PHOTOMETRIC = "MONOCHROME2"

# --- model / training --------------------------------------------------------
MODEL_IN_NAME = "feat"
MODEL_OUT_NAME = "prob"
MODEL_IN_CHANNELS = 6
MODEL_OUT_CHANNELS = 2
MODEL_HIDDEN = 24
OPSET = 17
TRAIN_SEED = 20260101
TRAIN_STRIDE = 1              # negative pool thinning: 1 = every negative voxel eligible
NEG_PER_POS = 4               # class balancing: 4 sampled negatives per positive
MINIBATCH = 4096
LEARNING_RATE = 0.01
ADAM_BETA1 = 0.9
ADAM_BETA2 = 0.999
ADAM_EPS = 1e-8
EVAL_EVERY = 50


# =============================================================================
# 1. loading
# =============================================================================
def load_case_ids(cid):
    """Return {label_int: 'tooth_XX'} for the expert ground truth of case cid."""
    d = json.load(open(f"{SRC_DIR}/cases.json"))
    for c in d:
        if c["id"] == cid:
            for r in c["results"]:
                if r["key"] == "groundtruth":
                    return {int(k): v for k, v in r["labels"].items()
                            if v.startswith("tooth_")}
    raise KeyError(cid)


def load_volume(cid):
    """HU volume, int16, index order (220,220,172), 0.60 mm."""
    import nibabel as nib
    a = np.asanyarray(nib.load(f"{SRC_DIR}/{cid}.nii.gz").dataobj)
    assert a.shape == SRC_SHAPE, (cid, a.shape)
    return np.ascontiguousarray(a, dtype=np.int16)


def load_gt(cid):
    """Expert multi-label ground truth, uint8, index order (440,440,344), 0.30 mm."""
    import nibabel as nib
    a = np.asanyarray(nib.load(f"{SRC_DIR}/{cid}_seg_groundtruth.nii.gz").dataobj)
    assert a.shape == GT_SHAPE, (cid, a.shape)
    return np.ascontiguousarray(a, dtype=np.uint8)


def load_ds(cid):
    """DentalSegmentator reference output, uint8, index order (440,440,344), 0.30 mm."""
    import nibabel as nib
    a = np.asanyarray(nib.load(f"{SRC_DIR}/{cid}_seg_dentalsegmentator.nii.gz").dataobj)
    return np.ascontiguousarray(a, dtype=np.uint8)


# =============================================================================
# 2. jaw-centred crop (deterministic, HU-driven)
# =============================================================================
def band_bbox(vol):
    """1st..99th percentile bounding box of voxels with BAND_HU_LO<=HU<=BAND_HU_HI.

    Returns (lo[3], hi[3], center[3], n_band_voxels); lo/hi are inclusive int
    indices into `vol`, computed per axis with floor(percentile1) / ceil(percentile99).
    """
    m = (vol >= BAND_HU_LO) & (vol <= BAND_HU_HI)
    idx = np.argwhere(m)
    n = len(idx)
    if n < 16:                                   # documented degenerate fallback
        return np.zeros(3, int), np.array(SRC_SHAPE) - 1, np.array(SRC_SHAPE) // 2, n
    pc = np.percentile(idx, [BAND_PCT_LO, BAND_PCT_HI], axis=0)
    lo = np.floor(pc[0]).astype(np.int64)
    hi = np.ceil(pc[1]).astype(np.int64)
    return lo, hi, None, n


def crop_start(vol, fov=APP_SHAPE, margin=CROP_MARGIN_VOX):
    """Deterministic crop start index (3 ints) for a `fov` cuboid.

    Algorithm (exactly 5 steps, all integer after the percentile):
      1. band mask  = (HU >= 1200) & (HU <= 4000)
      2. lo_d = floor(P1(coord_d)), hi_d = ceil(P99(coord_d))     per axis d
      3. expand: lo_d -= margin_d ; hi_d += margin_d
      4. clamp to the source grid: lo_d in [0, N_d-1], hi_d in [0, N_d-1]
         (this clamping is the only thing that can make the window asymmetric)
      5. center_d = (lo_d + hi_d) // 2 ;  start_d = center_d - fov_d // 2
         then clamp start_d into [0, N_d - fov_d]
    """
    lo, hi, _, n = band_bbox(vol)
    shape = np.array(vol.shape, dtype=np.int64)
    fov = np.array(fov, dtype=np.int64)
    marg = np.array(margin, dtype=np.int64)
    if n < 16:
        lo = np.zeros(3, dtype=np.int64)
        hi = shape - 1
    lo = np.clip(lo - marg, 0, shape - 1)
    hi = np.clip(hi + marg, 0, shape - 1)
    center = (lo + hi) // 2
    start = center - fov // 2
    start = np.clip(start, 0, shape - fov)
    return start, lo, hi, center, n


def make_app_volume(cid, want_int16=True):
    """Canonical 192x192x128 app volume (int16 HU) + the crop parameters used."""
    vol = load_volume(cid)
    start, lo, hi, center, nband = crop_start(vol)
    sx, sy, sz = (int(v) for v in start)
    crop = vol[sx:sx + APP_SHAPE[0], sy:sy + APP_SHAPE[1], sz:sz + APP_SHAPE[2]]
    assert crop.shape == APP_SHAPE, crop.shape
    crop = np.ascontiguousarray(crop, dtype=np.int16)
    assert crop.shape == (192, 192, 128) and crop.dtype == np.int16
    meta = {"case": cid, "start": [sx, sy, sz], "bbox_lo": lo.tolist(),
            "bbox_hi": hi.tolist(), "bbox_center": center.tolist(),
            "band_voxels": int(nband)}
    return crop, meta


# =============================================================================
# 3. GT resampling: 0.30 mm label grid -> binary tooth mask
# =============================================================================
def gt_tooth_at_src_grid(cid):
    """Tooth union of the expert GT resampled to the 0.60 mm source grid
    (220,220,172) by 2x2x2 majority vote, threshold GT_VOTE_THRESHOLD of 8."""
    g = load_gt(cid)
    ids = sorted(load_case_ids(cid))
    t = np.isin(g, ids).astype(np.uint8)
    assert t.shape == GT_SHAPE
    c = t.reshape(SRC_SHAPE[0], GT_TO_VOL_FACTOR,
                  SRC_SHAPE[1], GT_TO_VOL_FACTOR,
                  SRC_SHAPE[2], GT_TO_VOL_FACTOR).sum(axis=(1, 3, 5))
    return (c >= GT_VOTE_THRESHOLD).astype(np.uint8)      # (220,220,172)


def gt_tooth_at_app_grid(cid):
    """Binary tooth mask (uint8 0/1) on the canonical 192x192x128 grid."""
    m = gt_tooth_at_src_grid(cid)
    sx, sy, sz = crop_start(load_volume(cid))[0]
    sub = m[sx:sx + APP_SHAPE[0], sy:sy + APP_SHAPE[1], sz:sz + APP_SHAPE[2]]
    assert sub.shape == APP_SHAPE
    return np.ascontiguousarray(sub, dtype=np.uint8)


def gt_tooth_at_native_0p3(cid):
    """Raw expert tooth union at full 0.30 mm resolution, (440,440,344) uint8."""
    g = load_gt(cid)
    return np.isin(g, sorted(load_case_ids(cid))).astype(np.uint8)


# =============================================================================
# 4. decimation + feature channels  (the C++ port target)
# =============================================================================
def decimate2_boxavg(arr):
    """Generic 2x2x2 box-average decimation (192,192,128) -> (96,96,64).
    out[I,J,K] = sum(in[2I:2I+2, 2J:2J+2, 2K:2K+2]) / 8, accumulated in float64.
    Accepts int16 or float32 input; returns float32.  No interpolation."""
    a = np.asarray(arr)
    assert a.shape == APP_SHAPE, a.shape
    s = a.astype(np.float64).reshape(96, 2, 96, 2, 64, 2).sum(axis=(1, 3, 5))
    return (s / 8.0).astype(np.float32)


def decimate2_boxavg_int16(vol):
    """2x2x2 box-average decimation of the canonical (192,192,128) int16 HU volume
    to the (96,96,64) model grid (returns float32 HU, not normalised)."""
    assert vol.dtype == np.int16, vol.dtype
    return decimate2_boxavg(vol)


def normalize_hu(vol):
    """clip -> [(-0.5,+0.5)] float32 on the APP grid."""
    h = np.clip(vol.astype(np.float64), HU_CLIP_LO, HU_CLIP_HI)
    return ((h + HU_NORM_BIAS) / HU_NORM_SCALE - HU_NORM_SHIFT).astype(np.float32)


def box_mean_replicate(a, r):
    """Separable running-sum box mean, box half-width r (box width 2r+1),
    replicate/clamped boundary on every axis.  `a` float32 (nx,ny,nz).

    Per axis: pad with `r` copies of the edge slice (np.pad mode='edge'),
    prefix-sum in float64, out[i] = (P[i+2r+1] - P[i]) / (2r+1).
    Result is cast back to float32 after all three axes.
    """
    if r <= 0:
        return a.astype(np.float32)
    x = a.astype(np.float64)
    w = 2 * r + 1
    for axis in (0, 1, 2):
        pad = [(0, 0), (0, 0), (0, 0)]
        pad[axis] = (r, r)
        x = np.pad(x, pad, mode="edge")
        cs = np.cumsum(x, axis=axis, dtype=np.float64)
        zshape = list(cs.shape)
        zshape[axis] = 1
        cs = np.concatenate([np.zeros(zshape, np.float64), cs], axis=axis)  # cs[i] = sum_{j<i} x[j]
        n = a.shape[axis]
        sl_hi = [slice(None)] * 3
        sl_hi[axis] = slice(w, w + n)
        sl_lo = [slice(None)] * 3
        sl_lo[axis] = slice(0, n)
        x = (cs[tuple(sl_hi)] - cs[tuple(sl_lo)]) / w
    return x.astype(np.float32)


def build_features(vol_int16, radii=None, bp=None):
    """Canonical app volume -> model input.

    Args:
        vol_int16: np.int16 array, shape (192,192,128), index order (x,y,z), HU.
    Returns:
        np.float32 array, shape (6,96,96,64), C-contiguous, ready for the ONNX
        input 'feat' with dims [1,6,96,96,64] (append a leading 1 and ravel in
        C order for the raw fixture).

    Channels (all on the 96x96x64 reduced grid):
        c0 = decimate2_boxavg( (clip(HU,-1024,4095)+1024)/5120 - 0.5 )
        c1 = boxmean(c0, r=1)
        c2 = boxmean(c0, r=2)
        c3 = boxmean(c0, r=4)
        c4 = boxmean(c0, r=8)
        c5 = c3 - c4
    """
    assert vol_int16.dtype == np.int16 and vol_int16.shape == APP_SHAPE
    rad = tuple(FEAT_RADII) if radii is None else tuple(radii)
    pair = tuple(BANDPASS_PAIR) if bp is None else tuple(bp)
    n = normalize_hu(vol_int16)
    c0 = np.ascontiguousarray(decimate2_boxavg(n), dtype=np.float32)
    chans = [c0]
    boxes = {}
    for i, r in enumerate(rad, start=1):
        b = box_mean_replicate(c0, r)
        boxes[i] = b
        chans.append(b)
    chans.append(boxes[pair[0]] - boxes[pair[1]])
    feat = np.stack(chans, axis=0).astype(np.float32)
    assert feat.shape == (MODEL_IN_CHANNELS,) + RED_SHAPE
    return np.ascontiguousarray(feat)


def upsample_nearest2(mask):
    """(96,96,64) -> (192,192,128) by integer division mapping:
    out[x,y,z] = in[x//2, y//2, z//2].  No interpolation."""
    a = np.asarray(mask)
    assert a.shape == RED_SHAPE, a.shape
    return np.repeat(np.repeat(np.repeat(a, 2, axis=0), 2, axis=1), 2, axis=2)


def downsample_gt_to_red(gt_app):
    """(192,192,128) tooth mask -> (96,96,64) with the same 2x2x2 vote rule
    (threshold GT_VOTE_THRESHOLD of 8) used for the 0.30 mm -> 0.60 mm GT vote."""
    c = gt_app.astype(np.int32).reshape(96, 2, 96, 2, 64, 2).sum(axis=(1, 3, 5))
    return (c >= GT_VOTE_THRESHOLD).astype(np.uint8)


# =============================================================================
# 5. metrics
# =============================================================================
def dice_stats(pred, gt):
    pred = np.asarray(pred).astype(bool)
    gt = np.asarray(gt).astype(bool)
    assert pred.shape == gt.shape
    tp = int((pred & gt).sum()); fp = int((pred & ~gt).sum()); fn = int((~pred & gt).sum())
    denom = 2 * tp + fp + fn
    d = (2.0 * tp / denom) if denom else 1.0
    p = tp / (tp + fp) if (tp + fp) else 0.0
    r = tp / (tp + fn) if (tp + fn) else 0.0
    f1 = 2 * p * r / (p + r) if (p + r) else 0.0
    return {"dice": d, "precision": p, "recall": r, "f1": f1, "tp": tp, "fp": fp,
            "fn": fn, "pred_voxels": int(pred.sum()), "gt_voxels": int(gt.sum()),
            "total_voxels": int(pred.size)}


# =============================================================================
# 6. DICOM series
# =============================================================================
def _uid(role, case_digits, instance=None):
    """Numeric-only DICOM UID (every component is digits, total length <= 64).
    role: 1=StudyInstanceUID, 2=SeriesInstanceUID, 3=FrameOfReferenceUID,
          4=SOPInstanceUID (requires `instance`, 1..128)."""
    parts = [DICOM_UID_ROOT, "%d%04d" % (role, case_digits)]
    if instance is not None:
        parts.append(str(instance))
    s = ".".join(parts)
    assert len(s) <= 64 and s.replace(".", "").isdigit(), s
    return s


def write_dicom_series(vol_int16, out_dir, cid, template_path=f"{OUT_DIR}/work/neckct_template.json"):
    """Write 128 uncompressed Explicit-VR-Little-Endian CT slices mirroring the
    tag set of the project's neck_ct/00000001.dcm (see SPEC.md section 6).

    Slice n (1-based, file %06d) = canonical z index n-1; pixel row index =
    canonical axis 0 (x), pixel column index = canonical axis 1 (y).
    """
    import os
    import pydicom
    from pydicom.dataset import Dataset, FileMetaDataset
    from pydicom.dataelem import DataElement
    from pydicom.uid import UID
    from pydicom.valuerep import DS, IS

    tmpl = json.load(open(template_path))["dataset"]
    assert vol_int16.shape == APP_SHAPE and vol_int16.dtype == np.int16
    os.makedirs(out_dir, exist_ok=True)
    digits = int(cid.replace("dentvoxel_", ""))

    file_meta = FileMetaDataset()
    file_meta.MediaStorageSOPClassUID = UID(DICOM_SOP_CLASS_UID)
    file_meta.TransferSyntaxUID = UID(DICOM_TRANSFER_SYNTAX)
    file_meta.ImplementationClassUID = UID("1.2.826.0.1.3680043.10.474.9.1")
    file_meta.ImplementationVersionName = "AIBUILD1"

    SKIP_KW = ("SOPInstanceUID", "InstanceNumber", "ImagePositionPatient", "PixelSpacing",
               "SeriesInstanceUID", "StudyInstanceUID", "FrameOfReferenceUID",
               "SliceLocation", "ImageType", "PatientName", "PatientID", "PatientSex",
               "PatientAge", "PatientBirthDate", "StudyDate", "SeriesDate",
               "AcquisitionDate", "ContentDate", "StudyDescription", "SeriesDescription",
               "DeidentificationMethod", "Modality", "SeriesNumber", "AcquisitionNumber",
               "SamplesPerPixel", "PhotometricInterpretation", "Rows", "Columns",
               "SliceThickness", "SpacingBetweenSlices", "ImageOrientationPatient",
               "PatientPosition", "BitsAllocated", "BitsStored", "HighBit",
               "PixelRepresentation", "PixelPaddingValue", "WindowCenter", "WindowWidth",
               "RescaleIntercept", "RescaleSlope", "RescaleType",
               "ReconstructionDiameter", "DataCollectionDiameter", "SpecificCharacterSet")
    NUMERIC = {"US": int, "SS": int, "UL": int, "SL": int, "AT": None,
               "FD": float, "FL": float}
    ds = Dataset()
    # 1. copy every non-private, non-override tag of the reference series verbatim ...
    copied = []
    for key, rec in tmpl.items():
        tag = pydicom.tag.Tag(int(key.split(",")[0], 16), int(key.split(",")[1], 16))
        if rec["kw"] in SKIP_KW or rec["vr"] == "SQ" or rec["vr"] == "AT":
            continue
        vr, val = rec["vr"], rec["val"]
        if "\\" in val:
            val = val.split("\\")
        if vr in NUMERIC and NUMERIC[vr] is not None:
            val = [NUMERIC[vr](float(v)) for v in (val if isinstance(val, list) else [val])]
            if len(val) == 1:
                val = val[0]
        ds.add(DataElement(tag, vr, val))
        copied.append(key)
    ds.SpecificCharacterSet = "ISO_IR 100"
    # 2. ... then override the tags that must describe OUR geometry/identity
    ds.PatientName = DICOM_PATIENT_NAME
    ds.PatientID = DICOM_PATIENT_ID_FMT % digits
    ds.PatientSex = DICOM_PATIENT_SEX
    ds.PatientAge = DICOM_PATIENT_AGE
    ds.PatientBirthDate = ""
    ds.StudyDate = DICOM_STUDY_DATE
    ds.SeriesDate = DICOM_STUDY_DATE
    ds.AcquisitionDate = DICOM_STUDY_DATE
    ds.ContentDate = DICOM_STUDY_DATE
    ds.StudyDescription = DICOM_STUDY_DESC
    ds.SeriesDescription = DICOM_SERIES_DESC
    ds.DeidentificationMethod = "SYNTHETIC DATA - NOT A REAL PATIENT"
    ds.Modality = "CT"
    ds.ImageType = ["ORIGINAL", "PRIMARY", "AXIAL"]
    ds.StudyInstanceUID = UID(_uid(1, digits))
    ds.SeriesInstanceUID = UID(_uid(2, digits))
    ds.FrameOfReferenceUID = UID(_uid(3, digits))
    ds.SeriesNumber = IS("1")
    ds.AcquisitionNumber = IS("1")
    ds.SamplesPerPixel = 1
    ds.PhotometricInterpretation = DICOM_PHOTOMETRIC
    ds.Rows = DICOM_ROWS
    ds.Columns = DICOM_COLUMNS
    ds.PixelSpacing = [DS(str(DICOM_XY_PITCH_MM)), DS(str(DICOM_XY_PITCH_MM))]
    ds.SliceThickness = DS(str(DICOM_SLICE_THICKNESS_MM))
    ds.SpacingBetweenSlices = DS(str(DICOM_SLICE_THICKNESS_MM))
    ds.ImageOrientationPatient = [DS("1"), DS("0"), DS("0"), DS("0"), DS("1"), DS("0")]
    ds.PatientPosition = "FFS"
    ds.BitsAllocated = DICOM_BITS_ALLOCATED
    ds.BitsStored = DICOM_BITS_STORED
    ds.HighBit = DICOM_HIGH_BIT
    ds.PixelRepresentation = DICOM_PIXEL_REPRESENTATION
    ds.PixelPaddingValue = DICOM_PIXEL_PADDING_VALUE
    ds.WindowCenter = DS(str(DICOM_WINDOW_CENTER))
    ds.WindowWidth = DS(str(DICOM_WINDOW_WIDTH))
    ds.RescaleIntercept = DS(str(DICOM_RESCALE_INTERCEPT))
    ds.RescaleSlope = DS(str(DICOM_RESCALE_SLOPE))
    ds.RescaleType = "HU"
    ds.ReconstructionDiameter = DS("115.2")     # 192 * 0.6 mm FOV in-plane
    ds.DataCollectionDiameter = DS("132.0")     # source FOV 220 * 0.6 mm

    paths = []
    for k in range(APP_SHAPE[2]):
        pixels = np.ascontiguousarray(vol_int16[:, :, k], dtype="<u2")
        ds.PixelData = pixels.tobytes()
        ds.SOPInstanceUID = UID(_uid(4, digits, k + 1))
        ds.InstanceNumber = IS(str(k + 1))
        z = DICOM_IPP_Z0 + k * DICOM_SLICE_THICKNESS_MM
        ds.ImagePositionPatient = [DS(str(DICOM_IPP_X0)), DS(str(DICOM_IPP_Y0)), DS("%.3f" % z)]
        ds.SliceLocation = DS("%.3f" % z)
        file_meta.MediaStorageSOPInstanceUID = ds.SOPInstanceUID
        ds.file_meta = file_meta
        ds.is_little_endian = True
        ds.is_implicit_VR = False
        p = os.path.join(out_dir, DICOM_FILENAME_FMT % (k + 1))
        pydicom.dcmwrite(p, ds, write_like_original=False, enforce_file_format=True)
        paths.append(p)
    return paths


def read_dicom_series(in_dir):
    """Reassemble a written series back into the canonical (192,192,128) int16 HU
    array.  Slices are ordered by increasing ImagePositionPatient z (equivalently
    by InstanceNumber / filename).  Rows -> axis 0, columns -> axis 1, z -> axis 2.
    """
    import os
    import pydicom
    files = sorted(f for f in os.listdir(in_dir) if f.endswith(".dcm"))
    slices = []
    for f in files:
        ds = pydicom.dcmread(os.path.join(in_dir, f))
        arr = ds.pixel_array                       # int16 (signed, PixelRepresentation 1)
        if int(ds.RescaleIntercept) != 0 or float(ds.RescaleSlope) != 1.0:
            arr = arr.astype(np.int32) * int(ds.RescaleSlope) + int(ds.RescaleIntercept)
        slices.append((float(ds.SliceLocation), int(ds.InstanceNumber), arr.astype(np.int16)))
    slices.sort(key=lambda t: (t[0], t[1]))
    assert len(slices) == DICOM_SLICES, len(slices)
    vol = np.stack([s[2] for s in slices], axis=2)     # (rows=192, cols=192, z=128)
    return np.ascontiguousarray(vol, dtype=np.int16)


if __name__ == "__main__":
    for cid in ALL_CASES:
        v, meta = make_app_volume(cid)
        print(cid, meta, "app", v.shape, v.dtype, v.min(), v.max())
