# 第 7 轮：`:cbctmeasure` 全量功能真机回归报告

本轮需求（用户原文）：**「全量关于 cbctmeasure 模块功能真机再测试一遍。」**

性质：**纯回归轮 —— 本轮零代码改动**（`git status` 除会话副产物 `.qoder/` 外干净，HEAD 仍是 `f418b5d`）。
上一轮（round5/round6，见 [`UI_SPLIT_LAYOUT_REGRESSION_REPORT.md`](UI_SPLIT_LAYOUT_REGRESSION_REPORT.md)）修完的 D-01~D-07
在本轮复验；本轮新发现的缺陷延续编号为 **D-08 ~ D-12**，全部**只定位根因、未动代码**（其中 D-10/D-11
涉及临床判级口径，需你确认后再改）。

判定纪律沿用前几轮：**每个点击坐标取自当次 `uiautomator dump` 的节点 bounds 中心**（解析用
`.qoder/skills/android-adb-ui-regression-loop/scripts/nodes.py`，不用 grep/sed 抽属性）；
滚动、弹窗、切换 tab 后重新 dump；**没有真机日志或截图支撑的结论一律标注「未落盘」或「未取证」**。

---

## 0. 结论摘要

| # | 场景 | 结果 | 关键证据（当次落盘） |
|---|---|---|---|
| A | 开箱可用：牙科 CBCT 释放 + 序列解析 | **通过** | `logs/22_cold_start_parse_restore.txt`：`病例 dentvoxel_0101 已释放 128 张` → `bindVolume: 192 x 192 x 128, spacing 0.60/0.60/0.60` |
| B | 测量族（距离/角度/面积/体积/HU 阈值） | **通过**（读数与手算逐条吻合） | 结果列表 dump `d82/d83`，截图 `34_r7_24`、`35_r7_25`；体积/面积见 §2.2 算术核对 |
| C | ROI 量化与组合 ROI（R-05） | **通过**（数值对，性能超标 → D-12） | `logs/24_module_lines_now.txt`：`ROI #234 组合 (1∪4)−7 ... in 382 ms` |
| D | 种植体规划 + 安全判级 S-01~S-06 | **数值链路通过，判级口径发现 2 项缺陷** | `implant #73 eval: ... spacing=-1.00 level=0` vs `#177 eval: ... spacing=80.96 level=0`（同几何不同间距读数）→ D-10 |
| E | AI 链路（装载/分割/逐牙自动测量/推荐/清除） | **通过**，去重修复第 3 次复验成立 | `logs/23_ai_reload_remeasure.txt`：`aiAutoMeasure: 复用 ROI #1（aiLabel=1），清理旧自动测量 2 条` |
| F | 主机/真机奇偶校验（AC-08 复跑） | **PASS** | `logs/parity_check_0101.txt`：`label: bitwise=YES mismatch=0`、`inst: 100.0000%`、`VERDICT: PASS` |
| G | 归档 + PDF + DICOM SR | **通过** | `logs/21_export_report.txt`：`report ok pages=3 ... evidence=true`、`exportAll ... archive=3/3 pdf=true pages=3 sr=true verify=true` |
| H | 界面结构（tab 4/5、结果折叠、提示、按钮布局） | **通过**（D-07 叠加层标签仍只部分改善） | 截图 `29_r7_19_mpr_overlay_labels.png`、`31_r7_21_operator_dialog.png` |
| I | 性能 PC-01~PC-05 | **PC-01 不达标（掩膜大实例路径）**，其余达标或口径受限 | §3 |
| J | AI-01 精度 | **仍未达 PRD 门槛**（本轮未改模型，非回归） | 界面固定读数 `tv_ai_caveat`（§4.3 原文引用） |

一句话：**功能闭环，数据/数值/落盘三条链在真机上重复成立；PC-01 的超标点和上一轮同源（AI 掩膜 over-merging），本轮又给出它在「组合 ROI」上的新触发路径。**

---

## 1. 被测构型

| 项 | 值 |
|---|---|
| HEAD | `f418b5d`（blog：新增 cbctmeasure 的 CSDN 技术博客）；工作区无源码改动 |
| APK | `app/build/outputs/apk/debug/app-debug.apk` 177,581,194 B，md5 `83cec5ea92fc56eadec7d2accc431663` |
| 构建/安装 | `./gradlew :app:assembleDebug --offline` → `adb install -r` `Success` |
| 设备 | AGM3-W09HN（`AWTYCP2C27400666`），1920x1200 横屏，`ro.product.cpu.abi` = arm64-v8a |
| 病例 | `dentvoxel_0101`（assets 内置，128 张 → 应用私有目录，192×192×128 @0.6 mm） |
| 进程 | 前半轮 pid 26150，冷启动复测后 pid 30720（同一次安装、同一份归档） |
| 证据目录 | 主机 `/tmp/dcmtk_verify/round7/`（`d01`–`d83` dump、`logs/20`–`25`、`artifacts/`、`screens/`），编号截图 `/tmp/dcmtk_verify/10_r7_*` – `35_r7_*` |

本轮全程只做只读观测 + 点击，**未 `pm clear`、未卸载、未点「保存归档」**；磁盘上的归档始终是
round6 那份 81 项状态（`loadAll ... ok=3/3` 反复证明），live session 里多出的 9 条测试记录
（#226–#234）随进程结束丢弃。轮末设备留存状态见 §6。

---

## 2. 逐项结果与真机取证

### 2.1 数据链路与解析（场景 A）

冷启动（`force-stop` 后 `am start`）到解析完成、归档自动回填的完整日志，逐字取自
`round7/logs/22_cold_start_parse_restore.txt`：

```
D/CbctMeasureAi(30720): 病例 dentvoxel_0101 已释放 128 张 -> /storage/emulated/0/Android/data/com.example.dcmtkdemo/files/dental_dentvoxel_0101
D/CbctMeasureJni(30720): cbct_measure JNI loaded（5 类 55 个原生方法 + AI 层）
D/CbctMeasure(30720): bindVolume: 192 x 192 x 128, spacing 0.60/0.60/0.60
D/CbctMeasureJni(30720): createSession: handle=0x76745f7880 volume=0x767466b8c0
D/CBCT_MEASURE_STORE(30720): listArchives key=1.2.826.0.1.3680043.10.474.900000.10101_..._20101 found=3
D/CbctMeasure(30720): measures loaded: 51 records, 26 rois
D/CbctMeasure(30720): plan loaded: 3 implants, 1 nerve paths
D/CbctMeasure(30720): annotations loaded: 0 items
I/CBCT_MEASURE_STORE(30720): loadAll key=... ok=3/3
```

要点：**clone 后不联网、不手动放文件即可解析**（需求 1 的核心验收）；解析成功即自动挂归档
（`found=3` → `ok=3/3`），51 条数值 + 26 个 ROI + 3 枚种植体 + 1 条神经管一次性回来，
不需要用户再点「恢复归档」。三张截图独立复现同一状态：
`22_r7_12_restored_after_kill.png`（杀进程后回填）、`32_r7_22_cold_start_reparse.png`（冷启动重解析）、
`33_r7_23_third_refill_81.png`（第三次回填后仍是 81 项）。

### 2.2 测量族（场景 B）：读数 + 独立算术核对

本轮在 live session 里重新驱动了 5 类工具（新记录 #226–#234，编号即 UI 结果列表标题前缀）：

| 记录 | 工具 | 真机读数 | 主机侧独立核对 | 结论 |
|---|---|---|---|---|
| `#226` | 距离（MPR 两点） | 82.6041 mm | 按两次 pick 的体素坐标 ×0.6 mm 手算 √(Δx²+Δy²+Δz²) | 一致 |
| `#228` | 角度（三点） | 129.3213° | 两向量点积反算 | 一致 |
| `#229` | 平面裁剪 截面 3 点 | — | dump `d83` 可见行标题 `#229 平面裁剪 截面 3 点` | 建了 ROI |
| `#230` | 面积（多边形） | 3.4722 cm² | 同三点鞋带公式，逐位吻合 | 一致 |
| `#231` | 裁剪盒 | — | dump `d82` 可见行标题 `#231 裁剪盒 ROI 盒` | 建了 ROI |
| `#232` | 体积（VR 拖框） | 7.0589 cm³ | coverage × 0.216 mm³ 反算 | 一致 |
| `#233` | HU 阈值 ROI | HU 200~3000 | dump `d82` 行标题 `#233 HU 阈值 HU 200~3000`，滑条区间原样进名称 | 一致 |
| `#234` | 组合 ROI（R-05） | 13.421 cm³ / 2230.9 HU | 见 §2.3 | 数值口径正确 |

**证据落盘状态（如实标注）**：`#229 / #231 / #233 / #234` 的行标题来自当次 dump（`d82_results_90_items.xml`、
`d83_results_scrolled_up.xml`）；`#234` 的量化日志行落在 `logs/24_module_lines_now.txt`；
`#226 / #228 / #230 / #232` 的 **logcat 原文已被厂商噪声（`legacy_power`/`hwcomposer`/`libPowerHal` 各 400+ 行）冲出缓冲区**，
读数当时在结果列表与截图上可见，算术核对在本会话内完成 —— 这几条按「有 UI 读数 + 有核对、日志未持久化」定级，
不当作 log 级取证。教训写进 §5。

VR 体积测量的第一次尝试是失败的，而且是**有用的失败**：拖框位置落在渲染体之外，logcat 连续 10 条
`mode=0 plane=0/0 ... hit=false ray misses volume`，界面不提交任何记录（截图 `34_r7_24_vr_volume_attempt.png`）。
改用截图定位颅骨投影区后重拖即成功 —— 说明 VR 表面的射线-体积相交判定与「拾取不到就不提交」的守卫是对的。

MPR 侧的空点路径同样有日志（`logs/20_logcat_tail_after_restore.txt`，逐字）：

```
D/CbctMeasureCtrl(26150): pick disp=(1380.0,429.0) mode=PLANE plane=0/63 -> hit=false outside current slice cost=1.0ms
D/CbctVtk (26150): displayToSliceWorld: mode=1 plane=0 pos=63 ... in=(176.15,22.46) lim=(114.6,114.6) -> point outside slice
```

即：点击落在当前层四边形（±114.6 mm）之外 → 不生成记录 → 只提示。`cost=0~1.0 ms` 说明拾取本身极快。

### 2.3 ROI 量化与组合 ROI（场景 C）

掩膜 ROI 的量化日志（`logs/23_ai_reload_remeasure.txt`，逐字，取头部）：

```
D/CbctMeasureCore(30720): ROI #1 AI 牙 L2(#1) 掩膜 analyze: scanned=1121796 coverage=39216.0 vol=8.471cm3 mean=2193.3HU in 117 ms
D/CbctMeasure(30720): addMeasure #178 type=3 value=8.4707 cm3 ok=1
D/CbctMeasureCore(30720): ROI #1 AI 牙 L2(#1) 掩膜 analyze: scanned=1121796 coverage=39216.0 vol=8.471cm3 mean=2193.3HU in 108 ms
D/CbctMeasure(30720): addMeasure #179 type=7 value=2193.2742 HU ok=1
D/CbctMeasureCore(30720): ROI #4 AI 牙 L5(#2) 掩膜 analyze: scanned=325080 coverage=22916.0 vol=4.950cm3 mean=2295.3HU in 30 ms
D/CbctMeasureCore(30720): ROI #7 AI 牙 L16(#3) 掩膜 analyze: scanned=58240 coverage=4684.0 vol=1.012cm3 mean=2104.7HU in 5 ms
```

三条口径检查全部成立：
1. **体积 = coverage × 0.216 mm³**：39,216 × 0.216 = 8,470.7 mm³ = 8.4707 cm³，与 `addMeasure` 落库值逐位相同；
2. **R-06 与 AI 实例体素数一致**：`coverage 39216 / 8 ≈ 4902` 个红网格体素，对照 parity 夹具里 `id 1: 4927`（差值即体积边界裁剪掉的贴边体素），两处口径同源；
3. **同一 ROI 两次 analyze 数值完全一致**（117/108 ms 只是耗时抖动）。

组合 ROI `#234`（A=ROI#1 掩膜 `L2(#1)` ∪ B=ROI#4 掩膜 `L5(#2)`，再减去 C=ROI#7 掩膜 `L16(#3)`；
三个操作数都是掩膜 ROI，编号是 ROI id 不是实例 id）逐字：

```
D/CbctMeasureCore(30720): ROI #234 组合 (1∪4)−7 analyze: scanned=1494600 coverage=62132.0 vol=13.421cm3 mean=2230.9HU in 382 ms
```

算术核对（覆盖量按 0.216 mm³ 换算）：

| 量 | 值 |
|---|---|
| A = `#1` 掩膜覆盖 | 39,216 |
| B = `#4` 掩膜覆盖 | 22,916 |
| C = `#7` 掩膜覆盖 | 4,684 |
| A + B | **62,132** |
| `(A∪B)−C` 实测覆盖 | **62,132** |

**两者逐位相等**，即 A 与 B 无交集、C 与 A∪B 也无交集。这是**正确的**：连通域标记出来的实例彼此体素互斥，
所以不同掩膜 ROI 之间的 `∪` 不重复计数、`−` 也减不出东西；体积 13.421 cm³ = 62,132 × 0.216 逐项吻合，
且恰等于 8.471 + 4.950。反过来说，组合 ROI 在**不同牙实例之间**只能表达"牙组"，表达不了"重叠区扣除"——
要验 `∩`/`−` 的真减除，得拿空间盒 ROI（R-05 修的那条路径）来组合。耗时问题另列 D-12，不影响数值口径。

### 2.4 种植体规划与安全判级（场景 D）

自动评估日志（`logs/22_cold_start_parse_restore.txt` 与 `20_*` 均有同一份，逐字）：

```
D/CbctMeasureCore(30720): implant #73 nerve distance: axis=38.87mm radius=1.50 -> 37.37mm
D/CbctMeasureCore(30720): implant #73 eval: height=19.50 width=13.20 nerve=37.37(measured=1) spacing=-1.00 level=0
D/CbctMeasureCore(30720): implant #127 nerve distance: axis=58.31mm radius=1.50 -> 56.81mm
D/CbctMeasureCore(30720): implant #127 eval: height=2.10 width=0.00 nerve=56.81(measured=1) spacing=80.96 level=2
D/CbctMeasureCore(30720): implant #177 nerve distance: axis=38.87mm radius=1.50 -> 37.37mm
D/CbctMeasureCore(30720): implant #177 eval: height=19.50 width=13.20 nerve=37.37(measured=1) spacing=80.96 level=0
```

神经距离的减半径口径可复核：`38.87 − 1.50 = 37.37`（轴心距 − 半径，S-03 定义），日志自洽。
`#127` 的 `height=2.10 width=0.00` → `level=2`（红）与阈值（骨高 ≥10 mm、骨宽 ≥6 mm）一致，
说明 DANGER 分支在真机上触发。

但 `#73` 与 `#177` 这两行暴露两个缺陷：**几何与神经距离完全相同（19.50/13.20/37.37），
间距一个读 `-1.00`、一个读 `80.96`** —— 见 D-10；而这两条本来就是**同一个推荐候选被重复采纳的
两枚位点**（名字都是「AI推荐 上颌18-6」）—— 见 D-11。

### 2.5 AI 链路（场景 E）

`logs/23_ai_reload_remeasure.txt` 的开头几行是本轮最有价值的复验证据 —— 恢复归档后重跑分割 +
逐牙自动测量，**没有产生双份记录**（round6 修的 #27 缺陷，本轮第 3 次复验）：

```
D/CbctMeasure(30720): aiAutoMeasure: 复用 ROI #1（aiLabel=1），清理旧自动测量 2 条
D/CbctMeasure(30720): aiAutoMeasure: 复用 ROI #4（aiLabel=2），清理旧自动测量 2 条
D/CbctMeasure(30720): aiAutoMeasure: 复用 ROI #7（aiLabel=3），清理旧自动测量 2 条
```

结果列表在自动测量前 81 项、自动测量后仍 81 项（截图 `23_r7_13_refill_81_items.png`、
`24_r7_14_second_refill_81.png`、`33_r7_23_third_refill_81.png` —— 三次独立冷启动/恢复，同一数字）。
24 个实例、9,958 牙齿体素、阈值 0.49 在三个不同进程里逐次复现，见 §2.6 的 parity 表。

位点推荐与叠加：`#73 AI推荐 上颌18-6（安全）`、`#177 AI推荐 上颌18-6（安全）`（dump `d82` 行标题原文，
两条文案逐字相同）—— 这是 D-11 的直接证据。禁忌提示文案同样在当次 dump 里逐字拿到（UI 节点
`tv_ai_caveat`，取自 `AccessibilityNodeInfoDumper` 转写行）：

> ⚠ 粗筛不是结论：AI-01 实测 F1 0.58（PRD 门槛 0.85）。掩膜常把相邻牙合并成一个实例，最大实例的「体积」要按牙组读；推荐的间隙候选可能超出解剖可能，采纳前必须自行核对。

### 2.6 主机/真机奇偶校验（场景 F，AC-08 复跑）

`logs/parity_check_0101.txt`（本轮从真机重新拉取 20,643,904 B dump 后离线比对）：

```
header: featCount=3538944 probCount=1179648 labelBytes=589824 modelDim=(96,96,64) redDim0/1=(96,96)
dump sha256[:12]=ddc22d9e5bb3 bytes=20643904
feat : bitwise=no  max|diff|=2.980e-08 differing=799/3538944
prob : bitwise=no  max|diff|=1.192e-07 differing=2887/1179648
label: bitwise=YES mismatch=0 voxels (0.000000% of 589824) ; device tooth voxels=9958
inst : bitwise=YES exact-match-rate=100.0000% mismatch=0
  id   1:   4927 ->   4927      id   2:   2855 ->   2855      id   3:    586 ->    586 ...
VERDICT: PASS - device dump matches host device-order fixtures within tolerances
```

特征图/概率图的非逐位差异在 1e-8/1e-7 量级（NN 重采样与 SIMD 求和顺序），**不改变任何下游整数结论**
（label 逐位相同、实例集合与每实例体素数逐项相同）。与 round5/round6 的 dump **字节级同一份**
（sha256 前 12 位 `ddc22d9e5bb3`，20,643,904 B）—— 推理确定性再一次成立。

### 2.7 归档、PDF、DICOM SR（场景 G）

`logs/21_export_report.txt` + `logs/20_*` 关键行（逐字）：

```
D/CbctMeasureView(26150): captureEvidence 1880x600 frame=209.0ms overlay=9.0ms total=218.0ms
D/CbctMeasureSR(26150): SR dictionary injected: /data/user/0/com.example.dcmtkdemo/files/dicom.dic
I/CBCT_MEASURE_REPORT(26150): report ok pages=3 measures=51 rois=26 implants=3 nerves=1 annos=0 evidence=true -> .../reports/1.2.826...10101_1790829482651.pdf
D/CbctMeasureSR(26150): SR series created inside study 1.2.826.0.1.3680043.10.474.900000.10101
D/CbctMeasureSR(26150): SR NUM Distance=91.0934 mm
D/CbctMeasureSR(26150): SR NUM Volume of ROI=8.4707 cm3
D/CbctMeasureSR(26150): SR NUM Density=2193.2742 [hnsf]
D/CbctMeasureSR(26150): SR NUM Distance=-1.0 mm          ← D-10 的哨兵值一路写进了 SR
I/CBCT_MEASURE_REPORT(26150): exportAll ... archive=3/3 pdf=true pages=3 sr=true verify=true
I/CbctMeasureFragment(26150): exportAll cost=1615ms
```

拉回主机的产物：`artifacts/r7_pdf_final.pdf`（828,331 B）、`artifacts/r7_sr_final.dcm`（33,922 B），
离线 `dcmdump`/`pdfinfo` 可读；SR 数值与结果列表逐条对应（同一份 `Volume of ROI=8.4707 cm3`）。

导出前有一步**模态「报告署名」对话框**（`btn_export_report` 之后必须确认 `导出` 才会落盘）。
本轮第一次点完以为「没产出文件」，实际是对话框待确认（`d81_dialog.xml` + 截图
`31_r7_21_operator_dialog.png`）—— 交互上是正确的（署名是报告的法定字段），但**没有任何超时/取消提示**，
操作者容易误判成按钮失灵；记为 D-09 的同类观察，不单列新缺陷。

`exportAll cost=1615ms` 里 `captureEvidence` 只占 218 ms，其余为 JSON 落盘 + PDF 渲染 + SR 写盘，
不在 PC-01「单次测量计算」口径内，不计超标。

### 2.8 界面结构、折叠与提示（场景 H）

- **tab 4/5 剥离**：测量页与 AI 页切换后共享同一 Native 会话（`createSession: handle=0x76745f7880` 全程只出现一次），
  没有出现「两个 Fragment 各持一份会话」的翻倍现象。
- **结果折叠（需求 3）**：90 项时列表占满 `[20,176][1900,1080]`（dump `d82` 实测 bounds）；
  用户手动展开后，**切 tab 不再自动折叠**（`resultsUserToggled` 生效），与前一轮结论一致 ——
  并修正了一个我自己写错的笔记：「折叠态在切 tab 时会重置」是错的，实测 `▾ / 点击折叠` 跨 tab 保持。
- **按钮不遮挡（需求 4）**：AI 页读数在按钮之后，跑完分割按钮不位移；快捷行 5 按钮一行。
- **D-07 仍未闭环**：MPR 叠加层标签避让在 3 条以上密集记录时还会互压（`29_r7_19_mpr_overlay_labels.png`），
  维持上一轮的「部分改善」定级，本轮未改代码。

---

## 3. 性能取证 PC-01 ~ PC-05

| 编号 | 预算 | 本轮真机数字 | 判定 |
|---|---|---|---|
| PC-01 单次测量计算 | ≤ 50 ms | 距离/角度/面积 0~1 ms（`pick ... cost=0.0~1.0ms`）；小掩膜 ROI **0~5 ms**；`#4`（22,916 覆盖）**30 ms**；`#1`（39,216 覆盖）**117 / 108 ms**；组合 `#234`（1,494,600 遍历）**382 ms** | **不达标**：3 条超标，全部落在「AI 掩膜大实例 → 包围盒横跨牙弓」这一条路径上（D-12） |
| PC-02 ROI 渲染帧率 | ≥ 30 FPS | `overlay fps=13 avgDraw=0.93~1.32ms frames=17 window≈1200ms prims=56`；静置时 `fps=0 frames=1` | **口径受限**：绘制本身 ~1 ms（等价 >900 FPS），`fps` 数字受合成 swipe 事件密度限制，不能当作 30 FPS 的反证 —— 沿用前轮「未充分验证」标注 |
| PC-03 证据截图 | ≤ 500 ms | `captureEvidence 1880x600 frame=209.0ms overlay=9.0ms total=218.0ms` | **达标**（余量 ~2.3×） |
| PC-04 模块内存增量 | ≤ 100 MB | 本轮落盘：全量状态（90 项 + 掩膜 + 组合 ROI）`TOTAL PSS = 302,189 KB`（Native Heap 133,032 / Graphics 99,266 / Java 16,876），见 `logs/25_meminfo_full_state_now.txt`。本轮实时读取（**未落盘**）：冷启动 67,729 KB → 解析后 194,127 KB → AI 重跑后 261,173 KB，模块净增量 ~67 MB | **达标（判定依据需补）**：增量 <100 MB，但本轮只有全量状态一档进了文件，前两档待下轮补 `dumpsys` 存档 |
| PC-05 单次推理 | ≤ 5 s | 本轮未重跑带时间戳的 `runSegment` 行（缓冲冲掉）；同构型 round6 存档：`prep=676.8ms infer=329.6ms post=7.5ms total=1014.1ms alloc=21.9MB` | **达标**（余量 ~5×），但本轮数字是引用存档，不是当次取证 |
| PC-05 模型 + 运行时 | ≤ 80 MB | `libonnxruntime.so` 16,033,712 B + `teeth_cnn.onnx` 14,477 B = **16.05 MB**；本轮日志复现 dlopen 路径 `base.apk!/lib/arm64-v8a/libonnxruntime.so`、`ORT 1.17.0 api v17` | **达标** |

---

## 4. 本轮缺陷清单（延续上一轮编号，全部**未修**）

### D-08 神经管描记的「撤销取点」是结构性空操作

- **现象**：NERVE_TRACE 下连点若干层后点「撤销取点」，描记点数不变、神经管叠加不减一段；界面无报错。
- **取证**：撤销时 logcat **没有任何对应行**（`grep CbctMeasureCtrl.*undo` 无命中，这本身即证据）；
  点数变化判定来自当次结果列表读数 + 截图 `28_r7_18_nerve_4pts_after_restore.png`。
- **根因（代码走查定位）**：`MeasureToolController.kt:164` 的 `undoPendingPoint()` 只弹 `pending`
  （未提交的草稿点），而 `commitNervePoint()`（同文件 `:696`）每点一层就**直接** `appendNervePoint`
  进 Native、从不写 `pending`。Native 侧 `SurgeryPlanJni.removeNervePoint`（`SurgeryPlanJni.kt:47`）
  与 C++ 实现、JNI 注册（`measure-native-lib.cpp:603`、`:964`）**全都在**，但
  `grep -rn NervePoint --include=*.kt` 显示**零个 Kotlin 调用点** —— 删除通道存在，UI 没接。
- **影响**：描记错一个点只能整条作废重来（本轮就被污染过一次 5 点路径，靠「恢复归档」回到 4 点）。
- **建议修法**：`undoPendingPoint()` 在 `NERVE_TRACE` 分支改调 `removeNervePoint(handle, nervePathId, last)`，
  并补一条 `Log.d` 便于取证；纯 UI 层改动，不碰几何与判级。

### D-09 物理返回键直接销毁测量会话，无确认

- **现象**：测量页按 BACK，Native 会话连同未归档记录一起没了。
- **状态**：本轮**遵守规避策略**（测量页一律用 `KEYCODE_ESCAPE` 收输入法，不碰 BACK），
  因此只有前一轮的事故记录，**本轮未复现取证**，不新定级。风险仍在。
- **建议**：`onBackPressed` 前判「有未归档改动」再弹确认，或干脆在本页吞掉 BACK。

### D-10 S-06 间距判级丢弃负值 + `minSpacing` 复用哨兵（两个 bug 叠加）

- **现象**（真机日志，§2.4 逐字）：同几何的 `#73` 报 `spacing=-1.00`，`#177` 报 `spacing=80.96`；
  并且 `-1.0` 这个**哨兵值被当作真实测量值写进了 DICOM SR**（`SR NUM Distance=-1.0 mm`）。
- **根因**（`ImplantPlanner.cpp`）：
  1. `minSpacing()`（`:144`）用 `best = -1.0` 同时表示「还没有候选」和「表面间距为负」：
     `if (best < 0.0 || surf < best) best = surf;` —— 一旦算出 `surf < 0`（两植体相交，表面间距为负），
     后续任何候选都会被 `best < 0.0` 判成「第一个候选」而**无条件覆盖**它，最小间距被后面的大值吃掉；
  2. `evaluate()`（`:214`）的 `if (sp >= 0.0) { s.spacingMm = sp; s.spacingMeasured = true; }` ——
     负间距（= 最该报红的重叠）**根本不参与 S-06 判级**，`spacingMeasured` 保持 false，
     `:224` 回填 `im.minSpacingMm = -1.0`，UI/SR 就把哨兵读成数值。
- **影响**：**临床安全侧漏报**。两植体贴在一起时，S-06 要么不判、要么被后来的大间距顶掉，
  `level` 停留在 SAFE（本轮 `#73` 即 `spacing=-1.00 level=0`）。
- **本轮日志能逐值复算这两条分支**（`round7/plan_0101.json`：`#73` 与 `#177` 的 `entry`/`tip`/`axis`/
  `diaMm`/`lengthMm` 完全相同，`implants` 数组顺序为 `[73, 127, 177]`）：
  * 对 `im=73`：先比 `127` → `surf=80.96` 记入 `best`；再比 `177`（同位置，`axisDist≈0`，
    `tipI = entry + axis*lengthMm` 也重合）→ `surf = 0 − (5+5)/2 = −5` < 80.96 → `best=−5`；
    返回 −5 → `evaluate` 的 `sp >= 0.0` 不成立 → `spacingMeasured=false` → `minSpacingMm=-1`，
    **正是日志里的 `spacing=-1.00` 与 JSON 里的 `"minSpacingMm": -1`**；
  * 对 `im=177`：先比 `73` → `best=−5`；再比 `127` → 因 `best < 0.0` 被判定为「第一个候选」而
    **无条件覆盖**成 80.96 → 返回 80.96 → `spacingMeasured=true`，判级用了一个明显错误的大间距。
  同一个重叠事实，两颗牙一个变成哨兵、一个变成 80.96mm，**顺序决定结果**，这就是 D-10 的两个 bug 叠加后的可观测签名。
- **本轮处置**：**未改**。判级阈值属于临床口径，改动需要你确认预期语义（建议：负值 = 直接 DANGER_RED，
  并把「未测量」与「负值」用 `bool measured` 而非 -1.0 区分；SR 导出对 `measured=false` 不写 NUM 项）。

### D-11 AI 位点推荐采纳了重复位点，且两条都标「安全」

- **现象**：结果列表里 `#73 AI推荐 上颌18-6（安全）` 与 `#177 AI推荐 上颌18-6（安全）` 并存，
  叠加层标签与 PDF 报告同样各出现一次（截图 `27_r7_17_duplicate_ai_rows_d10.png`、`25_r7_15_implant_rows_d10.png`）。
- **取证**：`round7/plan_0101.json` 里 `#73` 与 `#177` 除 `id`/`minSpacingMm` 外**字段全等** ——
  `entry=[73.2739130435, 13.5014492754, 73.6]`、`tip=[83.9918346747, 6.17360744883, 74.2548969942]`、
  `axis=[0.824455510089, -0.563680140505, 0.0503766918644]`、`pitchDeg=-34.3106880239`、
  `yawDeg=-93.496595824`、`depthMm=13`、`diaMm=5`、`lengthMm=13`、`nerveDistMm=37.3672961421`、
  `level=0`、`warnText=各指标均在安全范围内（骨高 19.5mm / 骨宽 13.2mm）` 两条逐位相同；
  只有 `minSpacingMm` 一个是 `-1`、一个是 `80.9615695796`（即 D-10 的顺序效应）。
- **根因（已定位到行）**：**采纳通道没有去重**。候选生成侧 `AiPlanner::recommend`
  只在**同一 arch 的相邻实例对**上产候选（`AiPlanner.cpp:113-134`，`t` 与 `t+1` 各只配对一次），
  而两条记录的名字/`entry`/`axis` 逐位相同 → 它们是**同一个候选被放置了两次**。
  放置函数 `CbctAiFragment.placeRecommended()`（`:241-259`）直接 `addImplant` + `recomputePlan`，
  既不查「该位置是否已存在位点」，也不在放置后把候选标灰；推荐对话框（`:232-236`）每次重新列出
  同一份确定性候选，于是连点两次（或本轮"清除后重推荐"再点一次）就是两枚重叠种植体。
- **可复现**：AI 页「位点推荐」→ 选任一候选 → 再次「位点推荐」→ 选同一候选 → 结果列表出现两行同名
  同坐标位点；本轮真机即此状态（`#73` / `#177`）。
- **影响**：报告里出现两枚同位置种植体，其中一枚的间距读数还是 D-10 的哨兵 —— 两个缺陷叠加会让
  「安全」结论完全不成立。**本轮只定位，未改**（同样需要确认语义：建议按位置容差去重 + 采纳前提示）。

### D-12 掩膜大实例的包围盒横跨牙弓，把 PC-01 顶穿（含组合 ROI 新触发路径）

- **现象**：`#1` 体积/骨密度各 117 / 108 ms；组合 `(1∪4)−7` **382 ms**（预算 50 ms 的 7.6 倍）。
- **取证**：`logs/23_ai_reload_remeasure.txt`、`logs/24_module_lines_now.txt`；遍历量/覆盖量比
  `1121796 / 39216 = 28.6×`，`1494600 / 62132 = 24.1×`；parity 表给出实例真实大小
  `id1=4927 / id2=2855 / 总牙体素 9958` —— **单个实例占了全部牙齿体素的 49.5%**，
  即两颗牙弓段的体素被合并成了一个「牙」。
- **根因**：`MeasurementManager::boundsMm`（`:648`）如实返回实例的轴对齐包围盒，
  `RoiExtractor::spatialBoundsAt` 据此裁剪扫描范围。包围盒大不是 boundsMm 的错，是**上游分割
  把相邻牙连成一整块**（低分辨率 + 阈值 0.49 下的连通域合并）；量化又要按 app 网格逐体素做
  中心归属判定（0/1 权重，非角点比例），所以剪不掉 —— 遍历量是覆盖量的 20~30 倍。
  **组合 ROI 是新触发路径**：两个大盒取并，扫描量直接叠到 149 万体素。
- **与上一轮的关系**：**同一根因，不是回归**。`AI_ONNX_TEST_REPORT.md` §8.3 已记 178 / 162 ms 两条。
  注意口径差异，别读成"同一颗牙变了大小"：那两条来自**训练例 0021**（`0021_measure.json`，
  遍历 1,686,672 / 覆盖 100,696 app 体素 = 12,587 红体素），本轮 117 / 108 ms 来自**测试例 0101**
  （遍历 1,121,796 / 覆盖 39,216 = 4,927 红体素，与 `parity_check_0101.txt` 的 `id 1: 4927` 逐项一致，
  4,927 × 8 = 39,416 ≈ 39,216，差值即体积边界裁剪）。**两个病例的巨型实例是各自病例的 #1，不是同一颗牙。**
- **可选缓解**（本轮**未做**）：① 按实例包围盒分块 + 只遍历非零掩膜体素（把 O(盒) 降成 O(掩膜)）；
  ② 组合 ROI 用「子 ROI 已算出的体素集合」做并/交，而不是重新扫盒；
  ③ 分割侧加形态学分离或提高分辨率。③ 动模型/数据，成本最高。

---

## 5. 未覆盖 / 证据薄弱项（不粉饰）

1. **安全判级的「临界黄档」[thr, 1.2×thr) 在真机上没出现** —— 本轮三条位点分别是 SAFE(0) 与 DANGER(2)。
   黄档只有主机单测覆盖（`cbctmeasure/src/host/test_main.cpp:912-941`）。真机侧属**未取证**。
2. **PC-02 帧率仍不能定论**：合成 `input swipe` 的事件密度决定了 `fps` 上报值，
   `avgDraw ~1 ms` 只能证明绘制不慢。要真正判定 30 FPS，需要连续手势注入或自动回放（AC-10 同源问题）。
3. **PC-04 只有全量状态一档落盘**，冷启动/解析后两档是本轮实时读数未存档 —— 下轮把
   `dumpsys meminfo` 直接重定向进 `logs/`。
4. **PC-05 本轮引用 round6 存档数字**（`total=1014.1ms alloc=21.9MB`），因为本轮 `logcat -c` 之后
   缓冲被厂商噪声挤掉。**教训**：`logcat -d -v brief | grep TAG` 这类抽查必须在关键动作**之后立刻**
   重定向到文件，而不是先看一眼再存 —— 本轮有 4 条测量日志因此丢失（§2.2）。
5. **D-07 叠加层标签互压**：维持「部分改善」，本轮未动 `MeasureOverlayView` 的避让策略。
6. **D-09 BACK 事故**：本轮主动规避未复现，属「已知风险未验证」，不是「已修好」。
7. **AI-01 精度未动**：held-out F1 0.5828 < 0.85，界面读数禁忌文案已按需求固定展示（§2.5 原文）。

---

## 6. 设备与数据留存状态

- 磁盘归档 = round6 那份 **81 项状态**（`loadAll ok=3/3` 在本轮被 3 次冷启动独立证明）；
  **本轮从未点「保存归档」**，live session 新增的 9 条测试记录（#226–#234）不属于任何归档。
- 轮末把进程留在原状或 `force-stop` 均可（不点保存就不会污染归档）；本报告定稿时执行了
  `$ADB shell am force-stop com.example.dcmtkdemo`，丢弃测试记录，磁盘状态不变。
- 主机证据：`/tmp/dcmtk_verify/round7/`（dump `d01`–`d83`、`logs/20`–`25`、`parity_check_0101.txt`、
  `artifacts/r7_pdf_final.pdf`、`artifacts/r7_sr_final.dcm`、`measure_0101.json`、`plan_0101.json`），
  截图 `/tmp/dcmtk_verify/10_r7_00_*` ~ `35_r7_25_*`（`round7/screens/` 内有前 22 张副本）。

## 7. 建议的下一步（按风险/收益排序）

1. 修 D-08（纯 UI 层，把已存在的 Native 删除通道接上）+ D-11 的采纳前去重 —— 都不碰判级公式。
2. 就 D-10 的语义给结论：负间距是否 = 直接 DANGER_RED、`measured` 与哨兵是否分离、SR 对未测量项不写 NUM。
   确认后单独一个小提交，并补真机取证（再造一对重叠位点）。
3. D-12 选 ① 或 ②（性能，不改数值），改完用同一份 parity dump 复跑，证明 R-06 体积逐位不变。
4. 把 §5 列的取证空洞在下轮一次性补上：关键动作后**立刻**重定向落盘 logcat（本轮丢了 4 条测量日志）、
   `dumpsys meminfo` 三档全存、黄档（level=1）构造数据上真机、PC-05 的 `runSegment` 时间戳行当次重取、
   0021 侧的 PC-01 超标条数与 0101 一起各记一次（本轮只回归了 0101）。
