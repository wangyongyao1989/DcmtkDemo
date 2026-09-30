"""build_app_volumes.py -- step 1: canonical app volumes + GT masks + DICOM series.

Writes, for all four DentVoxel cases:
  /tmp/ai_build/app_volume/<case>/000001.dcm .. 000128.dcm   (16-bit signed HU)
  /tmp/ai_build/app_volume/<case>_hu.nii.gz                  (same array, NIfTI)
  /tmp/ai_build/app_volume/<case>_hu.raw                     (int16 LE, C order, 192*192*128*2 B)
  /tmp/ai_build/gt/<case>_tooth.nii.gz                       (uint8 0/1, 192x192x128)
  /tmp/ai_build/gt/<case>_tooth.raw                          (uint8, C order)
  /tmp/ai_build/work/crop_meta.json                          (crop numbers per case)
  /tmp/ai_build/work/dicom_roundtrip.json                    (round-trip assert results)
"""
import json
import os
import subprocess
import numpy as np
import nibabel as nib
import prep

OUT = prep.OUT_DIR
AFF = np.diag([prep.APP_SPACING_MM, prep.APP_SPACING_MM, prep.APP_SPACING_MM, 1.0])


def size_bytes(path):
    if os.path.isdir(path):
        return sum(os.path.getsize(os.path.join(path, f)) for f in os.listdir(path))
    return os.path.getsize(path)


def main():
    meta_all = {}
    rt_all = {}
    for cid in prep.ALL_CASES:
        vol, meta = prep.make_app_volume(cid)
        meta_all[cid] = meta
        gt = prep.gt_tooth_at_app_grid(cid)
        # ---- how much of the expert tooth annotation survived the crop
        gt_src = prep.gt_tooth_at_src_grid(cid)
        cov = float(gt.sum()) / max(1, int(gt_src.sum()))
        native = prep.gt_tooth_at_native_0p3(cid)
        meta["gt_tooth_voxels_native_0p3mm"] = int(native.sum())
        meta["gt_tooth_voxels_src_grid_0p6mm"] = int(gt_src.sum())
        meta["gt_tooth_voxels_in_app_crop"] = int(gt.sum())
        meta["tooth_volume_coverage_of_src_grid"] = cov
        meta["gt_tooth_voxels_red_grid"] = int(prep.downsample_gt_to_red(gt).sum())

        # ---- NIfTI + raw mirrors
        nib.save(nib.Nifti1Image(vol, AFF), f"{OUT}/app_volume/{cid}_hu.nii.gz")
        vol.tofile(f"{OUT}/app_volume/{cid}_hu.raw")
        nib.save(nib.Nifti1Image(gt, AFF), f"{OUT}/gt/{cid}_tooth.nii.gz")
        gt.tofile(f"{OUT}/gt/{cid}_tooth.raw")

        # ---- DICOM series
        series_dir = f"{OUT}/app_volume/{cid}"
        paths = prep.write_dicom_series(vol, series_dir, cid)
        nbytes = size_bytes(series_dir)
        assert len(paths) == prep.DICOM_SLICES

        # ---- round-trip verification (bit-exact)
        back = prep.read_dicom_series(series_dir)
        exact = np.array_equal(back, vol)
        assert exact, (cid, np.abs(back.astype(int) - vol.astype(int)).max())
        # also verify raw (unrescaled) pixel data equals HU, since Intercept is 0
        import pydicom
        d0 = pydicom.dcmread(paths[0])
        raw = d0.pixel_array
        raw_ok = np.array_equal(raw, vol[:, :, 0]) and raw.dtype == np.int16
        rt_all[cid] = {
            "series_dir": series_dir,
            "n_files": len(paths),
            "filename_pattern": prep.DICOM_FILENAME_FMT % 1,
            "total_bytes": nbytes,
            "total_mb": round(nbytes / 1e6, 2),
            "under_25mb": nbytes <= 25_000_000,
            "reassembled_array_equal_canonical": bool(exact),
            "raw_pixeldata_equals_hu_slice0": bool(raw_ok),
            "rescale_intercept": int(d0.RescaleIntercept),
            "rescale_slope": int(d0.RescaleSlope),
            "transfer_syntax": d0.file_meta.TransferSyntaxUID,
            "sop_class": d0.SOPClassUID,
            "modality": d0.Modality,
            "rows": int(d0.Rows), "columns": int(d0.Columns),
            "n_slices": len(paths),
            "bits_allocated": int(d0.BitsAllocated), "bits_stored": int(d0.BitsStored),
            "high_bit": int(d0.HighBit), "pixel_representation": int(d0.PixelRepresentation),
            "pixel_spacing": [float(x) for x in d0.PixelSpacing],
            "slice_thickness": float(d0.SliceThickness),
            "spacing_between_slices": float(d0.SpacingBetweenSlices),
            "image_orientation_patient": [float(x) for x in d0.ImageOrientationPatient],
            "ipp_first": [float(x) for x in d0.ImagePositionPatient],
            "ipp_last": None,
            "patient_name": str(d0.PatientName),
            "patient_id": str(d0.PatientID),
        }
        dlast = pydicom.dcmread(paths[-1])
        rt_all[cid]["ipp_last"] = [float(x) for x in dlast.ImagePositionPatient]
        print("%s crop=%s band=%d  gt_tooth=%d  series=%.2f MB  roundtrip_exact=%s" %
              (cid, meta["start"], meta["band_voxels"], gt.sum(), nbytes / 1e6, exact))

    json.dump(meta_all, open(f"{OUT}/work/crop_meta.json", "w"), indent=1)
    json.dump(rt_all, open(f"{OUT}/work/dicom_roundtrip.json", "w"), indent=1)
    print("wrote", f"{OUT}/work/crop_meta.json", f"{OUT}/work/dicom_roundtrip.json")


if __name__ == "__main__":
    main()
