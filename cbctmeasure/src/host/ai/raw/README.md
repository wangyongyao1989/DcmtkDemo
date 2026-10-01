# raw/ — 主机侧 AI 管线的输入数据（NIfTI 原始体积 + 分割标签）

这些 `.nii.gz` 是 `cbctmeasure/src/host/ai/scripts/` 里那条**离线**训练/取证管线唯一的输入。
它们放在这里，是为了让「克隆工程 → 重新生成模型与 parity 夹具」不再依赖某台机器上
`/tmp/dental_cbct` 这份临时目录（`prep.py` 之前硬编码那个路径，机器一清就得重新去网上找数据）。

```bash
cd cbctmeasure/src/host/ai
bash scripts/run_all.sh                  # 写入本目录（app_volume/gt/model/parity_ref/metrics.json）
AI_BUILD=/tmp/out bash scripts/run_all.sh  # 或写到别处，保持工作树干净
DENTAL_SRC=/some/other/nifti bash scripts/run_all.sh   # 换输入目录
```

## 来源与许可

公开基准 **DentVoxel**（VoxTooth 挑战赛），CBCT 牙分割数据集，随赛题公开发布用于科研与
算法评测；本目录只保留管线真正读到的 4 个病例，用于复现 AC-08 的奇偶校验夹具。

| 文件 | 作用 |
| --- | --- |
| `dentvoxel_{0021,0047,0074,0101}.nii.gz` | 强度体积（0.3mm 原生网格，int16 HU） |
| `..._seg_groundtruth.nii.gz` | 赛事给的逐牙标签（管线按标签 4..32 取牙齿） |
| `..._seg_dentalsegmentator.nii.gz` | 第三方配准标签，仅 0021/0047 有，用于交叉核对 |
| `cases.json` | `prep.py` 的病例清单与划分（训练 0021/0047/0074，留出 0101） |

0101 是**留出病例**：模型只在 0021/0047/0074 上训练，App 里跑的 F1/Dice 0.58285 就是它。

## 与 App 里那份牙科数据的关系

这里的是**训练/校验**用的原始网格（0.3mm，~500 层）。
App 直接加载的是另一份产物：`cbctmeasure/src/main/assets/dental_cbct/dentvoxel_{0021,0101}/`，
由 `scripts/build_app_volumes.py` 从本目录重采样成 192×192×128 @0.6mm 的 128 张 DICOM。
两者不是同一批文件，改这里不会改变 App 的内置数据，反之亦然；
要重建 App 的内置序列，跑 `python3 scripts/build_app_volumes.py` 后把
`$AI_BUILD/app_volume/<case>/` 覆盖到 `src/main/assets/dental_cbct/<case>/`。

## 刻意没放进来的东西

- `head_cbct*.nii.gz`：早期用来试配准的头部 CT，管线里没有任何脚本读它。
- `*_tooth.raw` / `app_volume/` / `work/`：`run_all.sh` 一跑就重新生成的派生物，
  体积大且与 `*.nii.gz` 等价，入库只会让仓库变胖（见 `.gitignore`）。
