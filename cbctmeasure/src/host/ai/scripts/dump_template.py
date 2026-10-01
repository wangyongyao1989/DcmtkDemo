"""dump_template.py -- step 0: 从仓库自带的 neck_ct DICOM 里导出写盘模板。

prep.write_dicom_series 需要一份"真实 DICOM 的标签集合"当模板（病人号、序列号、
设备、方位等），否则它造出来的 .dcm 过不了 app 侧 dcmtk 的解析。这份模板原先是
手工跑出来的，只存在于 /tmp/ai_build/work/ 里 —— 新克隆的仓库没有它，
run_all.sh 会在 step 01 以 FileNotFoundError 失败（复现校验真踩到）。

模板的来源是仓库资产 app/src/main/assets/neck_ct/00000001.dcm，所以它可以随时
重新导出：本脚本就是那件手工事的代码化，输出路径与 prep 的默认值一致
（{AI_BUILD}/work/neckct_template.json）。它只取标签，不取像素，
因此不影响任何已提交夹具的字节。
"""
import json
import os
import sys

import pydicom
from pydicom.multival import MultiValue

HERE = os.path.dirname(os.path.abspath(__file__))
AI_ROOT = os.path.dirname(HERE)
REPO_ROOT = os.path.abspath(os.path.join(AI_ROOT, "..", "..", "..", ".."))
DEFAULT_SOURCE = os.path.join(
    REPO_ROOT, "app", "src", "main", "assets", "neck_ct", "00000001.dcm"
)


def enc(v):
    if isinstance(v, (list, tuple, MultiValue)):
        return "\\".join(str(x) for x in v)
    return str(v)


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_SOURCE
    if not os.path.isfile(src):
        print(f"FATAL: template source missing: {src}")
        return 2
    ds = pydicom.dcmread(src)
    out = {}
    for e in ds:
        if e.tag.is_private or e.keyword == "PixelData" or e.VR == "SQ":
            continue
        out["%04X,%04X" % (e.tag.group, e.tag.element)] = {
            "vr": e.VR, "kw": e.keyword, "val": enc(e.value)
        }
    meta = {
        "%04X,%04X" % (e.tag.group, e.tag.element): {"vr": e.VR, "kw": e.keyword, "val": enc(e.value)}
        for e in ds.file_meta if e.tag.group == 0x0002
    }
    # 与 prep.py 的默认参数保持一致：写在 OUT_DIR/work 下
    out_dir = os.environ.get("AI_BUILD") or AI_ROOT
    target_dir = os.path.join(out_dir, "work")
    os.makedirs(target_dir, exist_ok=True)
    target = os.path.join(target_dir, "neckct_template.json")
    with open(target, "w") as f:
        json.dump({
            "dataset": out,
            "file_meta": meta,
            "transfer_syntax_uid": ds.file_meta.TransferSyntaxUID,
            "sop_class": ds.SOPClassUID,
            "rows": ds.Rows,
            "columns": ds.Columns,
            "template_source_file": DEFAULT_SOURCE,
        }, f, indent=1)
    print(f"template: {len(out)} tags -> {target} (source {src})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
