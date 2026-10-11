# 第 8 轮：P-01 / P-09 / P-10 落地 + D-08 / D-10 / D-11 修复的真机验证

本轮需求（延续 [`CBCT_APPLICATION_UPGRADE_PROPOSALS.md`](CBCT_APPLICATION_UPGRADE_PROPOSALS.md) 的立项条目）：
实现 **P-01（下颌管 AI-02 自动分割/追踪）**、**P-09（植体型号库 + crown-down S-11/S-12）**、
**P-10（第三磨牙 ↔ 下牙槽神经风险度量）**，并修掉第 7 轮定位的 **D-08 / D-10 / D-11**，然后真机全覆盖回归。

性质：**改动轮**。主机侧 C++ 与 Kotlin/UI 都有改动，且**尚未提交**
（`git status` 定稿时 49 项：25 处工作区修改 + 1 个已 `git add` 的新文档 + 22 个未跟踪新文件（含本报告）+ `.qoder/`）。
HEAD 仍是 `f0b85a7`，本轮全部工作位于工作区。

判定纪律沿用前几轮，本轮额外加强一点：**全程 `adb logcat -v brief` 落到文件**
（`/tmp/dcmtk_r8/logcat_p01_run.txt`，**定稿 121,464 行 / 19,371,102 B**，报告定稿时已停止采集），
所以下面每条结论都能给出**文件行号**，不再有第 7 轮「读到了但没落盘」那种证据空洞。该文件无时间戳前缀，但**行序即时序**；
两个进程号正好是两个构型（见 §1），构成同一份日志里的改前/改后对照。
定稿后校验过尾部：第 116,962–121,464 行里本模块 TAG（`CbctMeasure*` / `CBCT_MEASURE*`）**0 命中**，
按 tag 统计全是 `hwservicemanager` / `thermal_repeater` / `bms_behavior` / `AALLightSensor` 一类的 ROM 后台噪声，
所以下面引用的行号区间已经覆盖本轮全部模块动作，定稿之后没有遗漏的诊断信息。

**§2.3.1 是唯一的例外**：它属于第 9 轮补录，日志是另一份文件（`/tmp/dcmtk_r9/logcat_r9.txt`，
`-v threadtime` 带时间戳，**定稿 32,548 行 / 5,990,678 B**，主机 16:15 停止采集后同样做过尾部校验：
第 22,556–32,548 行本模块 TAG **0 命中**），界面原文取 `/tmp/dcmtk_r9/ui_*.xml`（33 份）。
第 9 轮**不写盘**：动作只有滑动、dump、建/删会话内 ROI、对话框「应用并重算」，
**没有点「保存归档」也没有点「导出报告」**（`exportReport() → ex.exportAll(...)` 会连归档一起写，会覆写 §7 记录的末态）。
**磁盘末态未变**（§7 有 `run-as ls -l` 的 mtime 证据），但**会话内存态已变**（`#359` 被删、`#177` 的 `measured` 变 0），
两者不能混为一谈 —— 详见 §7。
**一个必须记住的坑**：本机设备时钟比主机慢 **71 s**（定稿时 `adb shell date +%s` 与 `date +%s` 相减实测），
logcat 时间戳是设备时间、dump/截图的 mtime 是主机时间，两者不能直接比 —— §2.3.1 末尾那张换算表就是踩完之后补的。

---

## 0. 结论摘要

| # | 场景 | 结果 | 关键证据（文件:行 / 落盘产物） |
|---|---|---|---|
| 主机 | 纯 C++ 单测全量 | **778 PASS / 0 FAIL / 4 FINDING**，本轮新增 4 组共 **392 条断言全过** | `round8/logs/host_tests.txt`（组12=102、组13=33、组14=131、组15=126） |
| 主机 | PRD §6 精度门禁 AC-01~AC-04 | **四条全部达标** | `AC-01 8.882e-16 mm`、`AC-02 7.105e-15 度`、`AC-03 0.1244%`、`AC-04 0.000 HU` |
| P-01 | 外置权重通道 + 下颌管分割 + 掩膜转中心线 | **通过**（功能闭环；**模型质量门禁未过 ⇒ 默认只预览不判级**） | `logcat_p01_run.txt:57570`（装载 14,477 B 外部模型）、`:58566`（522 体素 thr=0.660）、`:58604`（6 条中心线）、`canal_parity.txt` **VERDICT: PASS** |
| P-09 | 型号库（20 个通用型号）+ 型号入选 + 报告/SR/归档回写 | **通过**；crown-down（S-11/S-12）第 8 轮真机未设冠方参考（如实显示「未测量」），**第 9 轮已补真机数值取证**：`measured=1` 出数值、与主机复算逐位吻合、失效引用正确降级 | 第 8 轮：`:81609`（20 models loaded, builtin=false）、`:82488`（选中 GEN-5.8-15）、`r8_out/new.txt` 第四章、`new.dcm` 的 `S-11/S-12 未测量：…` TEXT 项<br>第 9 轮：`logcat_r9.txt:16742`–`:16745`（`offset=41.11 angle=48.37 along=-36.54` / `measured=1`）、`:21456`–`:21458`（失效降级 `measured=0`）、§2.3.1 ④ 主机复算表 |
| P-10 | 一键风险评估 + 取证导出 | **通过**：18 下颌弓实例 × 8 路径 → **126 条**，41 ms；18 次跳过**各有原因行** | `:87504`、`:87508`、`:87249`–`:87503`、`r8_out/risk.json`（126 条 / 125,487 B） |
| D-08 | 神经管「撤销取点」接上原生删除通道 | **已修**：4→3→2→1 逐点删除，空路径给出「无点可撤」 | `:74435`、`:74624`、`:74803`、`:74971`、`:75273`；`plan.json` 路径 #184 `pts=0` |
| D-10 | S-06 负间距参与判级 + 未测量与哨兵分离 | **已修**，同日志内改前/改后对照成立 | 改前 `:3616` `spacing=-1.00 level=0`；改后 `:54992` `spacing=-5.00(measured=1) level=2`；`:82647` `-5.40` |
| D-11 | AI 位点重复采纳 | **已修**：候选标「已放置」，重复放置被拒并回显既有 id | `:61069` `已存在为 id=73，拒绝重复放置`；`r8_rec.xml` 「上颌 18-6 …（已放置）」 |
| 复验 | 归档/重跑推理/自动测量去重（#27 修复第 4 次复验） | **通过**：48 行先「待重算」，重跑后全部恢复实值，总数仍 51 | `:97697`–（复用 ROI 清理旧测量）、`r8_out/measure.json` vs `measure2.json` |
| 导出 | PDF + DICOM SR + 归档 | **通过**：3 页 PDF、ComprehensiveSR `num=70 text=82`、`archive=3/3`、`verify=true` | `:94905`、`:94983`、`:94984` |
| 新缺陷 | **D-13**：SR 把「待重算」行写成 `NUM 0.0`（PDF 已是「待重算」） | **本轮发现，未修** | `:94913`–（24×`Volume of ROI=0.0` + 24×`Density=0.0`）、`new.dcm` 解析、`SrReport.cpp:624-636` |
| 新缺陷 | **D-14**：删除被方案引用的 ROI / 标注**不触发方案重算**，列表继续显示旧数值（神经管分支不受影响，core 内部已重算） | **第 9 轮发现，未修** | `logcat_r9.txt:20255`/`:20274`（删除后 0 条重算日志）、`:21456`（直到手动「应用并重算」才降级）、`ui_177_after_del.xml`（悬空引用 + 旧数值同屏）、`CbctMeasureFragment.kt:1310-1327`、`MeasurementManager.cpp:545-563` |
| 性能 | PC-01~PC-05 | PC-03/PC-04/PC-05 达标；**PC-01 仍被巨型掩膜顶穿（D-12，123 ms）**；PC-02 口径受限 | `:94458`（222 ms）、`meminfo_pid12764_full_state.txt`（TOTAL 325,885 KB Pss）、`:57155`（968.8 ms / 21.9 MB） |

一句话：**三个新需求在真机上端到端跑通并全部落盘可复核，三个老缺陷的修复都有「同日志改前/改后」级证据；
P-01 的短板不在工程链路而在模型本身（Dice 0.146 / HD95 42.5 mm，门禁未过），代码用「默认只预览」把这个事实挡住了。**
第 9 轮补录把 P-09 唯一没验的 S-11/S-12 数值面补上了（与主机复算逐位吻合），代价是又挖出一条新缺陷 **D-14**：
删除被引用的对象不会触发方案重算，界面上会有一段**安全读数过期**的窗口。

---

## 1. 被测构型（两个构型在同一份日志里）

| 项 | 值 |
|---|---|
| HEAD | `f0b85a7`；工作区 49 项改动未提交（25 modified + 1 已暂存新文档 + 22 untracked（含本报告）+ `.qoder/`） |
| 最终 APK | `app/build/outputs/apk/debug/app-debug.apk` **177,852,514 B**，md5 `d3096b222304e6838bb412046c5d07ca` |
| 设备侧 APK | `/data/app/com.example.dcmtkdemo-k4GPjAbseLSUduPy3yxyUg==/base.apk`，md5 **与主机逐位相同**（`adb shell md5sum`） |
| 构建/安装 | `./gradlew :app:assembleDebug --offline` → `adb install -r` `Success` |
| 设备 | AGM3-W09HN（`AWTYCP2C27400666`），1920×1200 横屏，arm64-v8a |
| 病例 | `dentvoxel_0101`（assets 内置 128 张 → 应用私有目录，192×192×128 @0.6 mm） |
| **构型 A** | pid **9713**（12:48–13:25）：P-01 设备侧已接，**P-09/P-10/D-08/D-10/D-11 尚未安装** |
| **构型 B** | pid **12764**（14:44–15:14）：最终 APK，含全部三项实现与三项修复 |
| 证据目录 | 日志 `/tmp/dcmtk_r8/logcat_p01_run.txt`、`/tmp/dcmtk_r8/canal_device.parity.bin`；过滤件 `/tmp/dcmtk_verify/round8/logs/`（`host_tests.txt`、`canal_parity.txt`、`meminfo_pid12764_full_state.txt`、`module_lines_all.txt`、`full_buffer.txt`）；产物 `/tmp/dcmtk_verify/r8_out/`；截图 `/tmp/dcmtk_verify/05_r8_01_*` – `15_r8_11_*`；dump `r8_dump/rec/list/nerve/dlg/lib/lib2/arch.xml` |

构型 A/B 的区分不是推测：`grep loadFromJson|assessAllNerveRisks|undoPendingPoint` 在 pid 9713 上 **0 命中**、在 pid 12764 上 **1/1/5 命中**；
两者的安装目录名也不同。因此 §2.6 的改前/改后对照来自**同一台设备、同一份归档、同一份病例**，只有代码不同 —— 这是本轮最干净的一组证据。

本轮全程只读观测 + 点击：**未 `pm clear`、未卸载**。轮末**主动点了一次「保存归档」**（`:94140`/`:94873` `saveAll ok=3/3`），
磁盘归档已从第 7 轮的 81 项状态变成本轮末态，详见 §7。

---

## 2. 逐项结果与真机取证

### 2.1 主机侧回归（改动前先跑，改动后再跑）

`round8/logs/host_tests.txt`（`cbctmeasure/src/host/build_and_run.sh`，macOS clang，C++11）：

```
  AC-01 距离  最坏绝对误差 = 8.882e-16 mm（要求 <= 0.1 mm）[达标]
  AC-02 角度  最坏绝对误差 = 7.105e-15 度（要求 <= 0.5 度）[达标]
  AC-03 体积  最坏相对误差 = 0.1244%（要求 <= 1%）[达标]
  AC-04 HU    最坏绝对误差 = 0.000e+00 HU（要求精确值）[达标]
====================================================
RESULT: PASSED 778 / FAILED 0 / FINDINGS 4
```

本轮新增的四组（每组都含边界与退化路径）：

| 组 | 主题 | 断言数 |
|---|---|---|
| 组 12 | AI-02 下颌管中心线追踪 + 与 S-05 判级联动（P-01） | 102 |
| 组 13 | D-10：S-06 负间距判级 / 未测量哨兵分离 | 33 |
| 组 14 | P-09：植体型号库 / crown-down 偏差（S-11 · S-12） | 131 |
| 组 15 | P-10：第三磨牙 ↔ 下颌管风险度量（距离/接触/管壁皮质骨） | 126 |

4 条 FINDING **不是失败**，是主动记下的口径边界（与前几轮同源，本轮未新增）：
AC-03 球 r=2/3/6 mm 三例体积误差 5.42% / 1.76% / 1.35% 超 1%（角点加权在 `r/voxel ≤ 15` 时的量化偏差，
需亚体素体积积分才能压进 1%；计入 PRD 判定的最坏值是 **0.1244%**），外加持久化 13 位有效数字（毫秒时间戳）会被 `%.12g` 截断。
构型 B 的 clang 编译**零警告**（上一轮那条 `-Wunused-variable` 已清）。

### 2.2 P-01 下颌管 AI-02：权重外置 → 分割 → 中心线 → 判级开关（场景 A）

**决策 2（权重不进 APK）在产物层面自证**，不是靠注释：

```
$ unzip -l app-debug.apk | grep -i canal        → 无命中
  assets/models/teeth_cnn.onnx   14,477 B      ← AI-01 牙齿模型仍在 APK 内（前几轮既定口径）
  assets/models/teeth_cnn.json    2,420 B
  assets/implant_models.json      6,431 B      ← P-09 型号库（纯目录数据，非权重）

$ adb shell md5sum <外置>/ai_models/canal_cnn.onnx
  fde39f12a6ff3a8e65ec24e8e3ff44da5  =  主机 cbctmeasure/src/host/ai/model/canal_cnn.onnx 的 md5
```

真机装载与推理（`:57570`、`:58566`、`:58604`、`:58605`，逐字）：

```
D/CbctMeasureAi(12764): 模型 /storage/emulated/0/Android/data/com.example.dcmtkdemo/files/ai_models/canal_cnn.onnx（14477 字节），运行时 so=/data/app/.../base.apk!/lib/arm64-v8a/libonnxruntime.so，ORT 1.17.0 api v17
D/CbctMeasureAi(12764): runSegment ok: inst=15 toothVox=522 thr=0.6600 prep=590.7ms infer=353.5ms post=3.7ms total=947.9ms alloc=21.9MB
D/CbctMeasure(12764):  adoptCanalTracks: 掩膜体素 522 -> 中心线 6 条（新增 6，清除旧 0，手描保留 1）
D/CbctMeasureAi(12764): runCanal -> ok=1 canalVoxels=522 thr=0.660 tracks=6 total=948ms (prep 591 / infer 353 / post 4)
```

三条口径检查：① 阈值 **0.66 来自外置 `canal_cnn.json`**（`threshold_chosen_by: "max Dice on val case dentvoxel_0074 full grid"`），
设备端与主机端读同一份 spec；② **`adoptCanalTracks` 只删自己生成的那批**（「清除旧 0，手描保留 1」），
构型 A 里第二次跑变成「清除旧 6，手描保留 1」（`:8715`，pid 9713），手描路径永不被 AI 覆写；
③ AI-02 复用 AI-01 的同一个 `runSegment` 引擎，所以 `alloc=21.9MB` 与牙齿通道逐项相同 —— 没有第二份运行时开销。

**AC-08-2 设备/主机奇偶校验（下颌管分支）** —— `round8/logs/canal_parity.txt`，用真机 `dumpParity` 拉回的 20,643,904 B
（sha256[:12] `961b7a577b45`）当场复算，五段全过：

```
[1] feat  主机复算: bitwise=no max|diff|=2.980e-08 differing=799/3538944
[2] prob  主机复算: bitwise=no max|diff|=4.172e-07 differing=18087/1179648
          正类概率 max 主机 0.999983 / 真机 0.999983
[3] label 阈值 0.66: mismatch=0 voxels（真机 522 / 主机 522），且 = 冻结夹具 canal_pred_0101.raw = logcat 的 522
[4] inst  真机 15 / 主机 15，尺寸序列一致 [93,70,32,30,18,15,12,11,11,11,9,9,8,8,8]
          inst 非零但 label 为零的体素: 0
[5] 中心线 条数=6 体素数=[93,70,32,30,18,15]
          半径 mm=[1.5139,1.6584,0.9575,1.3541,1.0651,0.9575] 点数=[18,13,18,8,6,8] 描记轴=[0,0,0,0,0,1]
          vs metrics_canal.json 半径/点数: 逐位一致
VERDICT: PASS - 下颌管通道的 feat/prob/label/inst/中心线与主机复算一致
```

第 (5) 段是关键：中心线的**追踪规则**（26 邻域、次键排序、bbox 最大轴当描记轴、逐层截面质心、半径取中位数，
见 `include/NerveAutoTracker.h` 顶部注释）在 C++ 与 Python 两侧独立实现，真机产物与主机 `canal.py::centerlines()` 逐值重合 ⇒
`NerveAutoTracker` 不是「设备上一套黑盒」。

**门禁与判级联动**（`:61350`、`:61761`，逐字）：

```
D/CbctMeasureAi(12764): canalFeedGrading = 1（判级实际使用的神经管 7 条）
D/CbctMeasureAi(12764): canalFeedGrading = 0（判级实际使用的神经管 1 条）
```

代码默认**关**，而且这个默认是**在界面上自证的**（不是只写在注释里）—— UI 状态节点 `tv_ai_status` 原文（`:58054`）：

> 标定阈值 0.66（max Dice on val case dentvoxel_0074 full grid）；留出集 Dice 0.146 / HD95 42.5mm → 准入门禁未过（门槛 Dice>=0.85、HD95<=1.2 mm），默认只预览不判级

以及同行下方（`:59162`）：`判级：未采纳（预览），实际参与判级的神经管 1 条`。
代码侧默认同样明确（`CanalEngine.kt:40` 注释「默认关」），UI 勾选框 `cbAiCanalGrading` 由 native 回执回显，不靠本地状态猜。
主机准入门禁实测 **Dice 0.146 / HD95 42.52 mm** vs 门槛 **Dice ≥ 0.85 / HD95 ≤ 1.2 mm** ⇒ **MISSED**，
所以「只预览不判级」是**由指标推出来的默认值**，不是随手写的保守开关。打开开关后 S-05 立刻变严：
每颗牙出现两行 axis —— 手描 `:74013` 起 `axis=38.87mm radius=1.50 -> 37.37mm`，
自动管 `:74014` `axis=39.09mm radius=1.50 -> 37.59mm`（`#177` 同对：`:74020`），最终 `eval` 行仍取 37.37 ——
即 **min-over-paths 生效且不会因加入更差的自动路径而变松**。

**P-01 的产物落地**（三处都有，且措辞一致）：
- `plan.json`：`nervePaths` 8 条 = 手描 `#128`(4 点, r=1.5) + AI `#178–#183`(18/13/18/8/6/8 点，半径 1.51/1.66/0.96/1.35/1.07/0.96 mm) + `#184`(0 点)；
- PDF（`r8_out/new.txt`）「四、…」之后逐条列出：`AI-02 自动下颌管 1：18 点，半径 1.5 mm` … `神经管 8：0 点，半径 1.5 mm`；
- SR（`new.dcm`）7 条 `NUM Distance` 带 Annotation `下颌神经管描记总长（18 点）`，最长 **28.8857 mm**，最短 9.1492 mm。
  注意 0 点路径**不进 SR**（`SrReport.cpp:704` 的 `points.size() < 2` 守卫），但**进 PDF**（如实列出 0 点）—— 两种呈现都合理，
  只是口径不同，写在这里以免下轮误读成「丢了 一条」。

### 2.3 P-09 植体型号库 + crown-down（场景 B）

资产与载入（`:81609`，逐字）：

```
D/CbctMeasureCore(12764): loadFromJson: 20 models loaded (builtin=false, first sku=GEN-3.4-7)
```

`cbctmeasure/src/main/assets/implant_models.json`：20 个型号，`sku` 形如 `GEN-<Ø>-<len>`
（GEN-3.4-7 … GEN-5.8-15），**`vendor` 全空、`platformMm/abutmentMm` 全 0**。诚实边界写在 `include/ImplantLibrary.h`：
平台/连接基台属厂商目录数据，凭记忆填等于编造，所以只给「名义直径 × 长度」这一层，
并由 `isGeneric()` **从数据本身派生**「通用 vs 商品目录」标记，一路带进 `plan.json` 与报告：

```json
"modelLibrary": {"count": 20, "generic": true, "builtin": false}
```

```
型号库：通用名义尺寸目录（非厂商商品数据，不可直接作为采购依据）   ← PDF 第四章首行
```

设备侧选型 → 重算（`:82488`、`:83348`、`:82647`，逐字）：

```
D/CbctMeasureFragment(12764): editImplant: 选中型号 GEN-5.8-15 Ø5.8×15.0
I/AccessibilityNodeInfoDumper(...): text: 【已更新种植体 #73】 型号 GEN-5.8-15 · Ø5.8×15.0mm
D/CbctMeasureCore(12764): implant #73 eval: height=19.50 width=13.20 nerve=37.37(measured=1) spacing=-5.40(measured=1) crown=0.00mm/0.00deg(measured=0) level=2
```

**型号改动只移动它该移动的数**：Ø 由 5.0 → 5.8 后，S-06 由 −5.00 → **−5.40**（同位置重叠：`0 − (5.8+5.0)/2 = −5.4`，逐位可算），
邻牙 `#127` 由 80.96 → **80.56**（`85.46 − (5.8+4.0)/2`），而骨高/骨宽/神经距离**一位不变**（19.50 / 13.20 / 37.37）。
这是「型号库不引入新阈值」这条设计约束的真机证据。

P-09 反过来喂给 AI-03 也算落地了 —— 候选对话框（`r8_rec.xml`，UI 原文）现在给的是**可采购的离散组合**而不是连续值：

```
上颌 18-6 间隙 42.8mm（已放置） · 评分 100 · GEN-5.8-15 · Ø5.8x15.0mm · 不安全
下颌 8-13 间隙 6.4mm · 评分 95 · GEN-5.8-11.5 · Ø5.8x11.5mm · 安全
上颌 10-18 间隙 6.9mm · 评分 87 · 无匹配型号 · Ø3.5x13.0mm · 不安全
```

即候选规格已从「Ø4.27×10.8」这种不存在的型号换成库内 SKU；**装不下就直说「无匹配型号」**，不硬凑。

**crown-down（S-11/S-12）第 8 轮当轮确实未取证** —— 这不是「没注意」而是查过的：`measure.json` 里 26 个 ROI 的 type 分布是 **`5`(R-06 掩膜)×24 + `2`(R-03 平面)×2**，
`ROI_SPHERE=3` 一个都没有（`MeasureTypes.h:51`），且 `anno.json` 的 `annotations: []`、报告日志 `annos=0`。
`showCrownReferencePicker()` 的前置守卫因此 toast 拦下（提示先建球面 ROI 或 A-* 标注），
三颗种植体一律 `crown=0.00mm/0.00deg(measured=0)`。落盘侧的处理是**如实写「未测量」而不是写 0**：
PDF 每行 `型号：GEN-5.8-15 · S-11/S-12 未设冠方参考`，SR 一条 TEXT
`S-11/S-12 未测量：未设置冠方参考（R-04 球面 ROI 或 A-* 标注）`。S-11/S-12 的算术正确性只有主机覆盖（组14 的 131 条里含 crown 相关 10 条、S-11 16 条、S-12 12 条）。

#### 2.3.1 第 9 轮补录：S-11/S-12 的真机数值取证（2026-10-11，主机 15:43–16:15 采集）

上面这条缺口已在**同一份代码、同一台设备、同一份归档末态**上补齐。日志换成 `-v threadtime`
（`/tmp/dcmtk_r9/logcat_r9.txt`，定稿 **32,548 行 / 5,990,678 B**，主机 16:15 停止采集；下面用时间戳定位），
界面原文取 `/tmp/dcmtk_r9/ui_*.xml`（33 份），截图 `16_r9_01_crown_applied.png`、`17_r9_02_invalid_ref_recompute.png`。
**本小节的时间约定**：日志里的时间戳是**设备时钟**，dump / 截图的 mtime 是**主机时钟**，本机两者差 71 s（设备慢）——
每条引用都写清是哪一种，换算表放在本节末尾。

**① 先造一个 R-04 球面 ROI** —— 不新增取点工具，借 M-08 骨密度的自动建球
（`MeasureToolController.submitDensityAt()`，`DENSITY_SPHERE_RADIUS_MM = 5.0`），在 MPR Axial 第 63 层点一下
（`:9266`–`:9271`，`15:46:01.987`–`.991`，逐字）：

```
15:46:01.987 D CbctMeasureCore: plane(0,63) at voxel(96,87,63)=134HU
15:46:01.987 D CbctMeasureCtrl: pick disp=(940.0,318.0) mode=3 plane=0/63 win=- -> hit=true plane(0,63) at voxel(96,87,63)=134HU t=5.0
15:46:01.989 D CbctMeasure: addRoi #359 type=3 name=骨密度球 r=5.0mm
15:46:01.991 D CbctMeasureCore: ROI #359 骨密度球 r=5.0mm analyze: scanned=6859 coverage=2440.0 vol=0.527cm3 mean=86.4HU in 1 ms
15:46:01.991 D CbctMeasure: addMeasure #360 type=7 value=86.4464 HU ok=1
```

**② 设参考**（选择器只列临床对象。下面三份快照的时间是**主机侧文件 mtime**，设备时钟慢 71 s，
换算成设备时刻见括注 —— 这条偏移就是本节末尾那三处「界面与日志对不上」的根因）：
`ui_dlg.xml`（主机 15:49:50 ≈ 设备 15:48:39）首帧写 `冠方参考：未设置` →
`ui_crownpicker.xml`（主机 15:50:00 ≈ 设备 15:48:49）→
`ui_dlg2.xml`（主机 15:50:18 ≈ 设备 15:49:07）写 `冠方参考：R-04 球面 ROI #359`，
**6 秒后**就是 ③ 那次 `measured=1` 的 eval：

```
标题 冠方参考 = 预定牙冠位置 / 球心/标注首点为冠中心；两点标注的两端为冠长轴
选项 不设置（S-11/S-12 不出数值） | R-04 球面 ROI #359 骨密度球 半径 5.0mm
```

**③ 设备读数**（`:16742`–`:16745`，`15:49:14.439`，同一批里 #73/#127 未受影响，逐字）：

```
D CbctMeasureCore: crown deviation #177: offset=41.11mm angle=48.37deg along=-36.54mm (axisFromRef=0)
D CbctMeasureCore: implant #177 eval: height=19.50 width=13.20 nerve=37.37(measured=1) spacing=-5.40(measured=1) crown=41.11mm/48.37deg(measured=1) level=2
D CbctMeasureCore: implant #73  eval: height=19.50 width=13.20 nerve=37.37(measured=1) spacing=-5.40(measured=1) crown=0.00mm/0.00deg(measured=0)  level=2
```

列表行原文（`ui_row177.xml`，主机 15:53:15 ≈ 设备 15:52:04，即候选删除点 15:52:57 **之前**）：

```
#177 AI推荐 上颌18-6（不安全）
神经距 37.4mm · 间距 -5.4mm
S-11 41.1mm / S-12 48.4°（成角按入口→冠中心） · 种植体表面重叠 -5.4mm（安全值 >= 3.0mm）;
S-11 冠轴水平偏差 41.1mm / S-12 成角偏差 48.4°（参考未给冠长轴，成角按入口→冠中心连线）（判级阈值待 PRD 定值，不参与 S-02）
```

三件事同时成立：`measured=1` 才出数字；**没给冠长轴的参考（球面）明说是「按入口→冠中心连线」**而不是假装测了冠轴；
尾注 `（判级阈值待 PRD 定值，不参与 S-02）` 且 `level` 仍是 2 —— 决策点 4「不自定冠偏差阈值」在真机上兑现。

**④ 主机侧独立复算**（`ImplantPlanner::crownDeviation()` 同一公式；输入 = 归档 `plan.json` 的 #177
`entry=(73.2739130435, 13.5014492754, 73.6)` / 单位 `axis=(0.824455510089, -0.563680140505, 0.0503766918644)`，
球心两种口径对比）：

| 球心口径 | offset | angle | along | 与设备一致？ |
|---|---|---|---|---|
| 体素中心 `(96,87,63)×0.6 = (57.6, 52.2, 37.8)` | 41.1066 | **48.3662** | **−36.5395** | **逐位吻合**（41.11 / 48.37 / −36.54） |
| 拾取射线未取整 `(57.30, 52.44, 37.80)` | 41.1068 | 48.0698 | −36.9222 | 不一致（48.07 ≠ 48.37） |

⇒ R-04 球心是**体素中心**（`VolumeRef.cpp:67-68` 的 `indexToWorld` 无 +0.5 偏移），与 ① 的 `voxel(96,87,63)` 一致。
`along=−36.54mm` 的负号符合约定：`axis` 指向根方，冠中心在冠方 ⇒ 投影为负。
**方法论提醒**：offset 两种口径都给 41.11（差异在第 4 位小数），**能区分口径的是 angle**（48.37 vs 48.07）——
以后做球相关复算必须挑对角敏感的检校量，否则「复算吻合」是假象。

**⑤ 参考失效的口径**（`:21456`–`:21458`，`15:54:23.271`，逐字）：

```
W CbctMeasure: resolveCrownReference: 植体 #177 的冠方参考 ROI #359 不存在或非球面类型 -1
D CbctMeasureCore: implant #177 eval: height=19.50 width=13.20 nerve=37.37(measured=1) spacing=-5.40(measured=1) crown=0.00mm/0.00deg(measured=0) level=2
```

`resolveCrownReference()`（`MeasurementManager.cpp:820-832`）在引用失效时**清缓存并让 S-11/S-12 显式变未测量**，
而不是继续用上一次解析出的球心 —— 这条真机成立。**但删除动作本身不触发重算，这是新缺陷 D-14**（见 §4）。

**原本有三处「界面和日志对不上」，根因是同一个：主机时钟比设备快 71 s**。
量法（报告定稿时补测）：同一瞬间 `adb shell date +%s` 减 `date +%s` = **−71 s**；
两张截图的状态栏时钟独立佐证 —— `16_r9_01`（主机 15:53:16）状态栏 `3:52`、
`17_r9_02`（主机 15:55:53）状态栏 `3:54`，两条分钟级区间的交集正好罩住 71 s。
logcat 的时间戳来自**设备**，`ui_*.xml` 的 mtime 来自**主机**，混在一起比就会造出假象；
全部换算成设备时刻后每一条都自洽：

| 界面快照 | 主机 mtime | ≈ 设备时刻 | 内容 | 与日志对齐后 |
|---|---|---|---|---|
| `ui_impl5.xml` | 15:49:38 | 15:48:27 | #177 行 `S-11/S-12 未设冠方参考` | 在 ③ 的 apply（15:49:14）**之前** ⇒ 正确 |
| `ui_dlg.xml` | 15:49:50 | 15:48:39 | 弹窗 `冠方参考：未设置` | 同上，开窗时确实没设 ⇒ 正确 |
| `ui_dlg2.xml` | 15:50:18 | 15:49:07 | 弹窗 `冠方参考：R-04 球面 ROI #359` | 7 s 后 eval `measured=1` ⇒ 正确 |
| `ui_scan.xml` | 15:53:57 | 15:52:46 | 列表仍有 `#359 球面 ROI 骨密度球` 行 | 在**候选**删除点（15:52:57 prims 69→66）之前 ⇒ 正确 |
| `ui_177_after_del.xml` | 15:54:43 | 15:53:32 | `#359` 行已消失，**#177 行仍 `S-11 41.1mm`** | 删除之后、重算之前 ⇒ **这一格就是 D-14 的画面** |
| `ui_dlg3.xml` | 15:55:26 | 15:54:15 | 弹窗 `S-11 41.1mm` + `… #359` | 在降级（15:54:23.271）**之前** 8 s ⇒ 正确（也不再需要「开窗快照」来解释） |
| `ui_after_apply.xml` | 15:57:12 | 15:56:01 | #177 行 `S-11/S-12 未设冠方参考` | 降级之后 ⇒ 正确 |

顺带钉住一条计数事实：整份日志里 `crown deviation` **只有 `:16742`、`:16749` 两行**（同一次 apply 的 2 pass），
`implant #177 eval` 只有 `15:49:14`（measured=1）与 `15:54:23`（measured=0）两批 ——
带有效参考的应用全程**只成功一次**，③ 就是那一次，中间没有任何重算。

**残留的不确定只剩一处，写清楚**：`#359` 的删除精确落在设备 `15:52:46`（列表还在）与 `15:53:32`（列表已无）之间，
两个候选是 `15:52:57.491 :20274` 的 `prims=69 → 66`（点击锚点 `:20255`，`15:52:57.138`）
和 `ui_delconfirm.xml`（主机 15:54:11 ≈ 设备 15:53:00）那份确认框之后的执行；`removeRoi` 不打日志，二者分不开。
**这不影响 D-14**：无论按哪个时刻，到 `15:54:23` 之间 core 一次重算都没有，而那批日志是 3 颗 × 2 pass 的
「应用并重算」签名（删除不可能产 eval）。

仍未取证（如实）：S-11/S-12 进 **PDF/SR** 的那两行 —— 第 9 轮刻意不点「导出报告」，因为 `exportReport() → ex.exportAll(...)`
会连归档一起写（`archive=3/3`），会覆写 §7 记录的第 8 轮末态。落盘文本仍是第 8 轮那句「未设冠方参考」，
带数值版本（`S-11 41.1mm / S-12 48.4°` 进第四章、SR 的 NUM 分流）只有主机覆盖。

### 2.4 P-10 第三磨牙 ↔ 下牙槽神经风险（场景 C）

一键评估（`:87504`、`:87508`、`:87513`、`:87803`，逐字）：

```
D/CbctMeasure(12764): assessAllNerveRisks: 实例 18 个 × 路径 8 条 -> 成功 126 条（archKnown=1）
D/CbctAiFragment(12764): P-10 风险度量：新增 126 条（下颌弓实例 × 8 条路径，耗时 41ms）
D/CbctAiFragment(12764):  · 其余 122 条见「导出风险取证」
D/CbctAiFragment(12764): 风险取证已写出：126 条 / 125487 字节
```

**126 = 18 × 7，少的 18 次不是静默丢的**：0 点路径 `#184`（就是 §2.5 撤销到空的那条）对每个实例都留一行原因
（`:87249`–`:87503`，18 行，逐字）：

```
W/CbctMeasure(12764): assessNerveRisk: #1 × 神经 #184 未测量：神经管折线少于 2 点：请先描记或运行 AI-02 自动下颌管
```

归档侧（`plan.json`）`nerveRisks` 126 条、`nervePaths` 8 条 —— 风险条目跟着方案一起归档，重开可见。

取证 JSON（`r8_out/risk.json`，`{count, gradingCaveat, risks[126]}`）的分布与口径：

| 项 | 本轮真机数字 |
|---|---|
| 成功测量 | `measured=true` **126/126**（跳过的 18 次根本不入库） |
| 路径来源 | `AI-02 自动下颌管（本轮准入门禁未过，默认只作预览）` **108 条**、`手工描记` **18 条** |
| minDistMm | 126 条，范围 **0.00 – 78.89 mm**；< 2 mm 的 6 条（0.00×2、0.94、1.65、1.70、1.80） |
| 进入管腔半径 | `withinCanalRadius=true` **3 条**；`contact`（折线穿掩膜）**0 条** |
| 皮质骨取样 | `cortexMeasured` 123 真 / **3 假**，`cortexPresent` 121 真 / 5 假；`cortexGapMm` 0.00 – 48.90 mm |
| 带 error 的行 | 3 条：`管壁区间不足一个取样步长：根面已进入管腔半径`（实例 #5/#12/#15 × 神经管 1） |
| 溯源字段 | 每条都带 `rootVoxels`（如 4927）、`canalRadiusMm`（1.5 / 1.51387…）、FOV `115.2×115.2×76.8`、spacing `0.6³`、`seriesUid`、`doseText` |

两点值得单独说：① **来源文字自带结论**——108 条的 `nerveSource` 直接写「准入门禁未过，默认只作预览」，
读者不需要回头查开关；② **`doseText` 没有留空**，逐字写
`曝光/剂量参数：本序列的 CbctVolume 未携带（DICOM 头未导出 KVP/mAs/Dose*），无法据此评估剂量对可重复性的影响` —— 按 P-10 的要求「剂量与 FOV 必须有来源」，缺就如实写缺。

**P-10 不参与判级**是硬约束（用户决策点 4：PRD 没给数字就不自定阈值）。`gradingCaveat` 原文：
`P-10 只给几何事实（距离/接触/管壁区间骨取样），判级阈值待 PRD 定值，不参与 S-02 安全色，也不改写 S-05。`
真机产物核对：PDF 文本 `风险` 出现 **0 次**，SR 内容树无风险容器 ⇒ **P-10 目前只在归档 + 取证 JSON 两条通道落地，报告侧未导出**（列入 §5）。

### 2.5 D-08 神经管撤销取点（修复复验，场景 D）

改前（第 7 轮）：`undoPendingPoint()` 只弹未提交的草稿点，而描记是逐点直接提交到 Native 的，所以「撤销」是结构性空操作。
本轮修复改成：**有草稿点弹草稿；没有则读实际点列表并调 `removeNervePath` 的最后一项**（`MeasureToolController.kt`，
并补 `Log.d` 便于取证）。真机连续 5 次点击（`:74435`→`:75273`，逐字）：

```
D/CbctMeasureCtrl(12764): undoPendingPoint: NERVE_TRACE 撤销第 4 个点 path=184 -> ok=true
D/CbctMeasureCtrl(12764): undoPendingPoint: NERVE_TRACE 撤销第 3 个点 path=184 -> ok=true
D/CbctMeasureCtrl(12764): undoPendingPoint: NERVE_TRACE 撤销第 2 个点 path=184 -> ok=true
D/CbctMeasureCtrl(12764): undoPendingPoint: NERVE_TRACE 撤销第 1 个点 path=184 -> ok=true
D/CbctMeasureCtrl(12764): undoPendingPoint: NERVE_TRACE 路径 184 无点可撤
```

三条取证要点：
1. **第 5 次不是空操作，是「到边界了」**：4→3→2→1→空，且空态有明确提示而不是静默；
2. **落盘自证**：`plan.json` 的 `path #184 pts=0`，PDF 也印 `神经管 8：0 点，半径 1.5 mm` —— UI 之外的两处独立确认点数真变了；
3. 本轮曾踩到一个真实的使用陷阱：**只选 `rb_nerve` 不新建路径**时 `nervePathId=0`，撤销与描记都不生效
   （提示「请先新建神经管路径再描记」）。改成先点 `btn_add_nerve` 建路径 #184 再描记，才有上面这串日志。
   这条交互守卫是正确的（避免把点画到不存在的路径上），但新手容易误判成「按钮坏了」，值得在 USER 文档里写明。

### 2.6 D-10 S-06 负间距 + 未测量哨兵分离（修复复验，场景 E）

同日志内的改前/改后（构型 A vs 构型 B，同设备同归档同病例）：

```
改前 :3616: implant #73  eval: height=19.50 width=13.20 nerve=37.37(measured=1) spacing=-1.00              level=0
改前 :3620: implant #177 eval: height=19.50 width=13.20 nerve=37.37(measured=1) spacing=80.96              level=0
改后 :54992: implant #73  eval: height=19.50 width=13.20 nerve=37.37(measured=1) spacing=-5.00(measured=1)  level=2
改后 :54996: implant #177 eval: height=19.50 width=13.20 nerve=37.37(measured=1) spacing=-5.00(measured=1)  level=2
```

第 7 轮记录的两个 bug 都消掉了：
- **顺序效应消失**：改前同几何的两颗一个报哨兵 −1.00、一个报 80.96（谁先被比较决定结果）；改后**两条都 −5.00**，逐位对称；
- **漏报变报红**：`level` 从 0（SAFE）→ **2（DANGER_RED）**，PDF/SR 都写明原因
  `种植体表面重叠 -5.4mm（安全值 >= 3.0mm）`（`new.txt` 第四章 + `new.dcm` Annotation）；
- **未测量与负值不再是同一个数**：日志新增 `(measured=1)` 字段，`ImplantPlanner` 用 `spacingMeasured` 布尔位而不是 −1 哨兵；
  SR 侧改成未测量写 TEXT、实测负值照写 NUM —— 第 7 轮那条 `SR NUM Distance=-1.0 mm` 的假读数不复存在，
  本轮 `new.dcm` 的 70 个 NUM 里负值只有 `Distance=-5.4`（2 条），是真实测量。

### 2.7 D-11 AI 位点重复采纳（修复复验，场景 F）

```
:61069: D/CbctAiFragment(12764): placeRecommended: 候选「AI推荐 上颌18-6」已存在为 id=73，拒绝重复放置
```

候选对话框里同一条候选的标题已带状态标记（`r8_rec.xml` 原文）：`上颌 18-6 间隙 42.8mm（已放置） · 评分 100 · GEN-5.8-15 …`，
即「已放置」在**点之前**就能看见，拒绝时回显**既有 id**而不是只说「已存在」。

**必须如实标注的残留**：第 7 轮归档里那两枚重复位点（`#73` 与 `#177`，入口 `[73.27, 13.50, 73.60]` 完全相同）
**仍在**（`plan.json` 与 PDF 第四章都能看到两行同坐标位点）。修复只**阻断新增**，不追溯清理历史数据 ——
这是刻意的：静默删医生已归档的位点比留着更危险。要清理得手动删。

### 2.8 数据链路 / AI-01 / 自动测量去重复验（场景 G）

构型 B 冷启动到归档回填（`:54960`–`:54999`，逐字）：

```
D/CbctMeasure(12764): bindVolume: 192 x 192 x 128, spacing 0.60/0.60/0.60
D/CbctMeasureJni(12764): createSession: handle=0x7673ce7e00 volume=0x767465a8c0
D/CBCT_MEASURE_STORE(12764): listArchives key=...900000.10101_...900000.20101 found=3
I/CBCT_MEASURE_STORE(12764): loadAll key=... ok=3/3
```

AI-01 牙齿分割与第 7 轮同数（`:57155`）：`inst=24 toothVox=9958 thr=0.4900 total=968.8ms alloc=21.9MB` —— 跨轮确定性再一次成立。

**#27（恢复归档后重跑自动测量产生双份记录）第 4 次复验**，并且这次拿到了「先坏后好」的完整闭环：

| 阶段 | 产物 | 48 条 AI 行的状态 |
|---|---|---|
| 重跑推理后、重跑自动测量前 | `r8_out/measure.json`（15:11） | **48 条 value=0**，每条 `detail.error = "AI 掩膜 ROI 需要推理结果（实例 #1 不存在）"` |
| 重跑逐牙自动测量后 | `r8_out/measure2.json`（15:12） | **0 条 value=0、0 条 error**，48 条恢复实值（#311 `L2(#1) 体积 8.4707 cm3`、#312 `骨密度 2193.2742 HU` …） |

日志侧对应（`:97697`–，逐字）：`aiAutoMeasure: 复用 ROI #1（aiLabel=1），清理旧自动测量 2 条` 一直到 `#34（aiLabel=12）`；
两份 JSON 的记录总数**都是 51**（`#74 距离 91.0934` + 2 条 `ROI 面积 32.8032` + 48 条 AI 行），新行 id 311–358 —— 旧的删干净、新的不翻倍。

掩膜失效时为什么是「待重算」而不是 0：**PDF 侧做得对**。`new.txt` 里 `待重算` 出现 **48 次**，与 `measure.json` 的 48 条 error 行逐项对应
（`MeasureModels.kt:71 isFailed()` → `ReportGenerator.kt:161` 与 `MeasureToolController.kt:760` 都用同一判据）。
SR 侧没跟上 —— 见 D-13。

### 2.9 归档、PDF、DICOM SR（场景 H）

```
:94458: D/CbctMeasureView(12764): captureEvidence 1880x600 frame=203.0ms overlay=19.0ms total=222.0ms
:94905: I/CBCT_MEASURE_REPORT(12764): report ok pages=3 measures=51 rois=26 implants=3 nerves=8 annos=0 evidence=true -> .../reports/..._1791702400747.pdf
:94983: D/CbctMeasureSR(12764): SR verified: .../sr/..._1791702402117.dcm -> ComprehensiveSR (valid) / num=70 text=82 bytes=13439 observers=1
:94984: I/CBCT_MEASURE_REPORT(12764): exportAll key=... archive=3/3 pdf=true pages=3 sr=true verify=true
:94985: I/CbctMeasureFragment(12764): exportAll cost=1700ms
```

拉回主机的产物离线可读：`new.pdf` 866,080 B（3 页）、`new.dcm` 36,920 B、`new.txt` 4,827 字符、
`plan.json` 140,450 B、`risk.json` 125,487 B、`anno.json` 196 B。SR 内容树用 pydicom 逐层展开核对过：
容器 6 / NUM 70 / TEXT 82 / CODE 3 / IMAGE 1，`Impression` 一行
`共 51 条测量、3 颗种植体方案。存在 3 处低于安全阈值的位置，需调整植入点或深度后复评。`（三颗 `level=2`，与日志逐项一致）。

---

## 3. 性能取证 PC-01 ~ PC-05

| 编号 | 预算 | 本轮真机数字（构型 B，pid 12764） | 判定 |
|---|---|---|---|
| PC-01 单次测量计算 | ≤ 50 ms | 单点拾取 `cost=0.0~2.0 ms`；小掩膜 ROI 0–3 ms；`#4`(22,916 覆盖) **30 ms**；`#1`(39,216 覆盖) **108 / 123 ms**；P-10 一键评估 126 条 **41 ms**（整批，非单次） | **不达标（D-12 同源，非新增回归）**：巨型掩膜包围盒 1,121,796 遍历 / 39,216 覆盖 = **28.6×**；本轮未重建组合 ROI，故没再出现第 7 轮的 382 ms |
| PC-02 ROI 渲染帧率 | ≥ 30 FPS | `overlay fps=5 avgDraw=1.82~2.27ms frames=16~17 window≈3.1s prims=64`；静置 `fps=0 frames=1` | **口径受限**（同前轮）：绘制 ~2 ms 等价 >400 FPS，`fps` 上报值受合成 `input swipe` 事件密度限制，不能当反证 |
| PC-03 证据截图 | ≤ 500 ms | `captureEvidence 1880x600 frame=203.0ms overlay=19.0ms total=222.0ms` | **达标**（余量 2.25×） |
| PC-04 模块内存增量 | ≤ 100 MB | 全量状态 `dumpsys meminfo 12764`：**TOTAL Pss 325,885 KB**（Native Heap 185,176 / GL mtrack 49,932 / EGL 15,718 / Java Heap 16,124），Heap Size 500,911 / Alloc 450,274 KB；已存档 `meminfo_pid12764_full_state.txt` | **达标**（第 7 轮同状态 302,189 KB，差值 +23.7 MB 来自 8 条神经路径 + 126 条风险 + 型号库，量级合理）。本轮**只有全量一档**：冷启动/解析后两档仍未取，属取证缺口 |
| PC-05 单次推理 | ≤ 5 s | AI-01 `total=968.8ms`、AI-02 `total=947.9ms`（各含 prep/infer/post），`alloc=21.9MB` | **达标**（余量 ~5×，且是当次取证，不再引用存档） |
| PC-05 模型 + 运行时 | ≤ 80 MB | `libonnxruntime.so` 16,033,712 B + `teeth_cnn.onnx` 14,477 B = **16.05 MB**；下颌管权重**不在 APK 内**（§2.2 的 `unzip -l` 证据），设备侧另计 14,477 B | **达标** |
| 崩溃 / 泄漏 | — | 全量 121,464 行定稿日志：`FATAL EXCEPTION` **0**、`ANR in` **0**、crash buffer 空；LeakCanary 仅 40 行且全为「ready to detect / Watching instance / Setting up flushing」，**无任何 leak 报告**。另有 423 行 `W/app_process: [MemLeak.JavaTrack]: failed to dlopen libmemleak_tracker.so`，来自 ROM 的内存追踪钩子在数百个短命 `app_process` 派生进程里加载失败；按 pid 过滤后本模块两个进程（9713 / 12764）**0 命中**，**不是泄漏证据** | **通过** |

新链路的延迟都进了可接受量级：`P-10 一键 126 条 41 ms`、`P-01 中心线转换即时`、`P-09 型号应用后整方案重算 <1 s`
—— 三个新需求没有把 PC-01 的既有超标点放大。

---

## 4. 本轮缺陷清单（D-14 属第 9 轮补录时发现，非第 8 轮）

### D-14（第 9 轮新增，未修）删除被方案引用的对象不触发方案重算，S-11/S-12 会继续显示旧数值

- **现象**：把正在被植体当「冠方参考」用的 R-04 球面 ROI 删掉后，结果列表仍在读 `S-11 41.1mm / S-12 48.4°`，
  之后再打开植体对话框时（开窗快照取自实时 `dumpPlan`）甚至继续回显 `冠方参考：R-04 球面 ROI #359` —— 该 id 已不存在。
  数值只有在用户之后**恰好**又点了一次「应用并重算」时才降级成「未测量」。安全相关读数在两次编辑之间是**过期的**。

- **真机证据链**（`/tmp/dcmtk_r9/logcat_r9.txt`，行号为该文件行号；**时刻一律设备时钟**，
  界面快照的主机 mtime 已按 −71 s 换算，换算依据见 §2.3.1 末尾）：
  - 删除动作**本身没有日志**（`removeRoi` 全量 grep = 0 命中），只能锚定区间：设备 `15:52:46` 的列表快照
    （`ui_scan.xml`，主机 15:53:57）里 `#359 球面 ROI` 行还在；设备 `15:53:32`（`ui_177_after_del.xml`，主机 15:54:43）
    该行已消失。区间内两个候选点：`15:52:57.138 :20255`（点击音）+ `15:52:57.491 :20274`（`overlay … prims=69 → 66`），
    以及设备 `15:53:00` 那份删除确认框之后的执行。**按哪个算结论都一样。**
  - **决定性一格**：设备 `15:53:32` 的快照里 `#359` 已不在列表，而 #177 行仍写 `S-11 41.1mm / S-12 48.4°` ——
    数据里引用已悬空，界面上还是旧数值。
  - 从删除点到 `15:54:23` 之间，`:20256`–`:21451` 过滤 `eval|recompute` **0 命中**。
  - `15:54:23.271 :21456`–`:21458`：显式「应用并重算」后，core 立刻 `LOGW … #359 不存在或非球面类型 -1` 并给出 `crown=0.00/0.00(measured=0)`。
    这一批是 `:21452`–`:21465` **3 颗 × 2 pass**（`updateImplant` 内部重算 + 显式 `recomputePlan`），
    即对话框「应用并重算」的签名 —— 删除永远只会有 1 pass，所以它**不能**被解释成删除触发的重算；
    且这批里**没有 `crown deviation` 行**（参考解析已失败，根本不算偏差），与 `measured=0` 自洽。
  - 设备 `15:56:01`（`ui_after_apply.xml`，主机 15:57:12）列表才变成 `S-11/S-12 未设冠方参考`。

- **根因（定位到行）**：`CbctMeasureFragment.onRowDelete()`（`app/src/main/java/com/example/dcmtkdemo/fragment/CbctMeasureFragment.kt:1310-1327`）
  的 `when (row.ownerKind)` 里**只有 IMPLANT 分支**挂了 `.also { SurgeryPlanJni.recomputePlan(h) }`，
  ROI / NERVE / ANNOTATION 分支删完就走。JNI 层也只 `invalidate()`（`measure-native-lib.cpp:454-460` `RoiJni_removeRoi`），
  而 core 的 `MeasurementManager::removeRoi()`（`cbctmeasure/src/main/cpp/core/MeasurementManager.cpp:545-563`）
  清了关联测量的 `roiId` 和组合 ROI 的子引用，**唯独不动 `Implant.crownRefId`、不重算方案**。

- **不是显示层缓存，也不是 core 算错**：`dumpPlan`（`measure-native-lib.cpp:231-237`）= `planJson()` 实时序列化、无缓存；
  `resolveCrownReference()`（`MeasurementManager.cpp:810-833`，球面失配在 `:820-827`，标注失配在 `:834-842`）
  一旦被调用就能正确发现悬空引用并清零。所以缺陷**纯粹是「没人触发重算」** —— 这是好消息：修它只需要接上触发点。

- **影响面（逐条核过代码，不靠推测）**：
  - **ROI → S-11/S-12**：本轮真机取证成立（上面那条链）。
  - **A-* 标注 → S-11/S-12**：同族，代码路径同样不重算（`AnnotationJni_removeAnnotation`，`measure-native-lib.cpp:873-880`），
    而标注可以当冠方参考（`CROWN_ANNOTATION`，`MeasurementManager.cpp:834+`）。**本轮未取证**，按同族推定风险列出。
  - **神经管路径 → S-05/06**：**不受影响**。`removeNervePath()`（`MeasurementManager.cpp:891-907`）内部已经
    `recomputePlan()`（`:905`）并连带作废引用该路径的 P-10 风险条目 —— 我原本按「UI 分支没重算」推测它也有问题，
    读代码后纠正：**core 侧自己补了触发点**。
  - 顺带说明为什么「删除植体」一直是对的：`updateImplant`（`:758-769`）与 `removeImplant`（`:771-779`）各自内部 `recomputePlan()`。

- **附带观察（同一段 UI 文案的口径问题，未修）**：`crownText()`（`cbctmeasure/src/main/java/com/wangyao/cbctmeasure/model/MeasureModels.kt:290-395`）
  在 `!crownMeasured` 时统一输出「S-11/S-12 未设冠方参考」，**无法区分「从未设置参考」与「参考已失效」**；
  后者是数据完整性事件，临床上应当可见（`（成角按入口→冠中心）` 这类限定语已能区分口径，唯独这条不行）。

- **口径边界（别把这条缺陷说大）**：D-14 成立的是**「删除不触发重算」**，由代码（`onRowDelete` 的 ROI 分支）
  + 删除区间到降级日志之间 0 条 eval + 设备 `15:53:32` 那格「引用已悬空、数值仍是旧的」三条同向证据支持。
  它**不是**显示层缓存问题：`refreshResults()` 每行现取 `dumpPlan`（`:938`，实时无缓存），
  而 `planJson()` 序列化的就是 `Implant` 上那份**没被重算过**的 `crownMeasured/crownOffsetMm` —— 列表刷得再勤也还是旧值。
  唯一没定死的是删除的精确时刻（±35 s 区间，见上面证据链第一条），它不改变结论。

- **建议修法**（两处都要，缺一处仍会复现）：
  1. `onRowDelete()` 的 ROI / ANNOTATION 分支也 `.also { SurgeryPlanJni.recomputePlan(h) }`
     （或统一在 `notifyDataChanged()` 前无条件重算一次；删除是低频动作，本轮日志里 3 颗植体的重算落在同一毫秒内，代价可忽略）。
  2. core 侧在 `removeRoi()` / `AnnotationStore::remove()` 的调用链上，把引用该 id 的 `crownRefKind` 置 `CROWN_NONE`
     并清 `crownRefId` —— 让「引用失效」在删除那一刻就成为**数据状态**，而不是等下一次重算才被发现。
     只在 UI 层加重算（1）能修掉过期数值，但归档里会留着一个悬空 id，恢复后依旧靠 `LOGW` 兜。

### D-13（新增，未修）DICOM SR 把「待重算」行写成 `NUM 0.0`，与 PDF 的「待重算」口径不一致

- **现象**：重跑推理后、重跑自动测量前导出，PDF 48 行显示 `待重算`，而 SR 同一批行写成 **`NUM 0.0`**：
  `:94913`– 连续 `SR NUM Volume of ROI=0.0 cm3` × 24 + `SR NUM Density=0.0 [hnsf]` × 24；
  `new.dcm` 解析复核：70 个 NUM 里 **49 个为 0**（24 Volume + 24 Density + 1 个真实的 `Bone width 0.0`）。
- **不是数据错**：归档 `measure.json` 里这 48 行都带 `detail.error = "AI 掩膜 ROI 需要推理结果（实例 #1 不存在）"`，
  即程序**明确知道**这些行无效，只是 SR 导出没用这个信息。
- **根因（定位到行）**：`SrReport.cpp:624-636` 的记录循环无条件 `w.addNum(concept, it.value, …)`，
  既不读 `detail.error`，也没有 PDF 侧的 `isFailed()` 等价判据。对照同一个文件里 D-10 已经修对的写法
  （`:660-667`：`if (im.nerveMeasured) addNum(...) else addText(...)`）—— 种植体侧按 measured 分流了，**测量记录侧漏了同样一步**。
- **影响**：下游 PACS/读片端会把「0 mm³ / 0 HU 的牙」当真实测量值。0 HU 是空气、0 cm³ 是不存在的牙，
  比第 7 轮那条 `Distance=-1.0` 更容易被误读成「测量正常但值恰好为 0」。
- **建议修法**（需你确认口径，与 D-10 同族）：`SrMeasureItem` 带一个 `failed` 标志，
  `failed` 时写 `TEXT("待重算：<error 原因>")` 而不是 NUM；同一处顺手把 P-10 风险条目导出（见 §5.2）或明确不导出。

### D-12（第 7 轮已记，本轮仍在，未修）巨型掩膜包围盒把 PC-01 顶穿

本轮数字 **108 / 123 ms**（`#1`，1,121,796 遍历 / 39,216 覆盖），根因与缓解选项同第 7 轮 §D-12，未动代码。
本轮新增一条观察：同一颗牙的 `#1` 在两次连续导出里分别是 108 与 123 ms，说明该路径耗时抖动 ~14%，
判性能时至少要跑两次。

### D-07 / D-09（状态更新，非新缺陷）

- **D-07 叠加层标签互压**：本轮未动 `MeasureOverlayView`，维持「部分改善」。
- **D-09 物理返回键销毁会话**：本轮继续用 `KEYCODE_ESCAPE` 收输入法、不碰 BACK，**未复现取证**，风险仍在。

---

## 5. 未覆盖 / 证据薄弱项（不粉饰）

1. **S-11/S-12 的真机数值取证：已补录，但 PDF/SR 那两行仍未取证**。原第 8 轮缺口（会话内 0 个 R-04 球面 ROI、
   0 条 A-* 标注，选择器按前置守卫拦下）已在 §2.3.1 用第 9 轮真机日志补齐：`measured=1` 出数值、与主机复算逐位吻合、
   失效引用正确降级。**剩下的两条**：
   ① S-11/S-12 进 **PDF 第四章与 DICOM SR** 的带数值版本只有主机覆盖（第 9 轮刻意不点「导出报告」，避免覆写 §7 末态）；
   ② **A-* 标注作冠方参考**（`CROWN_ANNOTATION`）这条分支真机没跑过 —— 本轮只用球面 ROI 建参考，
   标注参考还带着「两点标注给冠长轴 ⇒ `axisFromRef=1`」这个未验分支（本轮 ③ 是 `axisFromRef=0`）。
2. **P-10 风险条目不进 PDF / SR**：`new.txt` 中 `风险` 0 次、SR 无风险容器。当前只有
   `plan.json.nerveRisks` + 「导出风险取证」JSON 两条通道。要不要进正式报告属于**报告法定内容口径**，需你定；
   若进，建议只进「几何事实 + 来源开关状态」，不要带颜色判级。
3. **P-01 的模型本身不合格**：held-out 测试例 Dice 0.146 / HD95 42.52 mm（门槛 0.85 / 1.2 mm），
   真机叠加表现为 **6 条碎段、最长 28.9 mm**（完整下颌管应 ~60 mm 连续）。
   工程链路（装载/阈值/追踪/判级联动/归档）全部对，短板在 3,474 参数的玩具级 CNN。
   **在没有合格权重之前，P-01 不具备临床可用性，只有「演示 + 代码就绪」价值。**
4. **AI-01 的 HU 读数仍偏高**：本轮 48 条骨密度落在 **1,733 – 2,657 HU**（ enamel/牙本质 + 部分容积 + 掩膜含牙冠）。
   数值是掩膜真实均值（与第 7 轮 2193.3 同源），但**不能当「骨密度」解读**；界面禁忌文案仍在，未改。
5. **黄档（level=1）真机仍无样本**：本轮三颗种植体全是 level=2，黄档只有主机覆盖。
6. **PC-04 冷启动 / 解析后两档仍未落盘**（连续第三轮的同一条取证缺口）。
7. **两套时钟不能混用**（第 9 轮踩到并已定位）：第 8 轮日志是 `-v brief` 无时间戳，靠行序；第 9 轮改 `-v threadtime`
   后时间戳来自**设备**，而 `ui_*.xml` / 截图的 mtime 来自**主机**，本机两者差 **71 s**（设备慢）。
   混着比一度造出三处「界面与日志对不上」的假象，换算后才全部消解（§2.3.1 末尾那张表）。
   本轮没有把偏移量写进采集脚本，只是定稿时补测一次 —— 下轮起**每轮开头量一次并记进日志文件名/报告**。
8. **归档被本轮改写过**：第 7 轮的 81 项状态已被 §7 描述的本轮末态覆盖（这是主动点的「保存归档」，不是副作用）。

---

## 6. 需求实现总结（P-01 / P-09 / P-10 + D-08 / D-10 / D-11）

### 6.1 每项的实现边界（做了什么、没做什么）

| 需求 | 做了什么 | 明确没做 |
|---|---|---|
| **P-01** AI-02 下颌管自动分割/追踪 | 第二模型通道（`CanalEngine.kt`，外置 `ai_models/canal_cnn.onnx` + 同名 spec json，`importFromUri` 支持 SAF）；掩膜→中心线（`NerveAutoTracker.cpp`，规则与 `canal.py` 同源且已真机奇偶校验）；`adoptCanalTracks` 只回收自己生成的路径；门禁指标驱动默认「只预览不判级」，`cbAiCanalGrading` 可显式采纳；S-05 变 min-over-paths；中心线进 plan.json/PDF/SR | 不换更好的权重（主机侧只有 3,474 参数的原型）；不把 AI 路径当手描一样可编辑；不做自动「找到完整一条管」 |
| **P-09** 型号库 + crown-down | `ImplantLibrary.cpp`（内置通用阶梯 + `loadFromJson` 覆盖）；`assets/implant_models.json` 20 型号；植体编辑对话框选型；`sku` 落进 `Implant` → 结果行标题、对话框、plan.json、PDF、SR；AI-03 候选规格改落离散 SKU 并给「无匹配型号」；`isGeneric()` 一路带到报告首行；S-11/S-12（冠方偏距/角度，参考 = R-04 球面 ROI 或 A-* 标注）+ `crownMeasured` | **不编厂商数据**：`vendor/platformMm/abutmentMm` 一律留空；**不引入新阈值**：判级仍只有 `kMin*` 那一套 |
| **P-10** 第三磨牙 ↔ 神经风险 | `NerveRisk.cpp`（点到折线最小距离 / contact / withinCanalRadius / 管壁区间皮质骨取样）；`assessNerveRisk` + `assessAllNerveRisks`（按牙弓过滤，arch 未知时退化为全口并**在条目里写清口径**）；归档 `nerveRisks`；「导出风险取证」JSON 带 FOV/spacing/doseText/seriesUid/rootVoxels/canalRadiusMm | **不自定临床阈值**（决策点 4）：不判红黄绿、不参与 S-02、不改写 S-05；不导出到 PDF/SR |
| **D-08** | `undoPendingPoint()` 接上原生 `removeNervePoint`，草稿点优先、否则删已提交末点、空态给原因；补日志 | 不改描记的逐层提交语义（每点即提交是对的，撤销只是接上删除通道） |
| **D-10** | `minSpacing(…, &found)` 用出参布尔位取代 −1 哨兵；`spacingMeasured` 分离「未测量/负值」；负间距进判级并产 `种植体表面重叠` 告警；SR 侧未测量写 TEXT、实测负值写 NUM | 不动阈值数字（3.0 / 2.0 mm 等沿用 PRD 5.3.2） |
| **D-11** | `placeRecommended()` 按「候选名 + arch + before/after id」键查重，命中即拒并回显既有 id；候选列表标「（已放置）」 | **不清理归档里已有的重复位点**（`#73`/`#177` 仍在，需手动删） |

### 6.2 三条贯穿各轮的约束，本轮的遵守证据

1. **权重不进 APK（决策 2）**：APK 内无 canal 权重（`unzip -l` 为空）、设备侧走外部目录、md5 与主机一致（§2.2）。
2. **不编 PRD 数字**：P-10 的 `gradingCaveat`、P-09 的 `isGeneric()`、D-10 的阈值沿用，三处都是「把不确定的东西显式标注」而不是填数。
3. **真机只读观测 + 点击**：全程无 `pm clear`/卸载；唯一写操作是主动点的「保存归档」（§7）。
   第 9 轮补录（§2.3.1）**连导出与保存都没点**，只有点击、删除会话内对象与滑动 —— 磁盘末态仍与 §7 一致。

### 6.3 代码/文件清单（供提交时切分 pathspec 参考）

- 新增 C++：`core/NerveAutoTracker.cpp` + `include/NerveAutoTracker.h`（P-01）、`core/NerveRisk.cpp` + `include/NerveRisk.h`（P-10）、
  `core/ImplantLibrary.cpp` + `include/ImplantLibrary.h` + `src/main/assets/implant_models.json`（P-09）
- 新增 Kotlin：`ai/CanalEngine.kt`（P-01 外置通道 + 门禁 + 判级开关）、`store/ImplantModelAssets.kt`（P-09 资产装载）
- 改动核心：`core/ImplantPlanner.cpp`（D-10 + S-11/S-12）、`core/MeasurementManager.cpp`（P-01 采纳 / P-10 编排 / #184 归档）、
  `core/SrReport.cpp`（D-10 的 measured 分流）、`core/AiPlanner.cpp`（P-09 离散候选）、`core/Json.cpp`、
  `measure-native-lib.cpp` / `ai-native-lib.cpp`（新 JNI）、`include/*`（AiTypes / MeasureTypes / AiPlanner / ImplantPlanner / MeasurementManager / MeasureJniHelper）
- 改动 UI：`view/MeasureToolController.kt`（D-08）、`report/ReportGenerator.kt`（P-09 第四章）、
  `jni/AiJni.kt`、`jni/SurgeryPlanJni.kt`、`model/AiModels.kt`、`model/MeasureModels.kt`、
  `app/…/CbctMeasureFragment.kt`（型号选择 + 冠方参考选择器）、`app/…/CbctAiFragment.kt`（D-11 + P-01/P-10 按钮组）、`res/layout/fragment_cbct_ai.xml`
- 主机侧：`src/host/test_main.cpp`（组12–组15，+1530 行）、`src/host/build_and_run.sh`、`src/host/ai/{model/canal_cnn.json, scripts/canal.py, scripts/check_canal_dump.py, parity_ref/*}`
- **未提交状态**：全部 49 项都在工作区；建议按分类切 5–6 个提交
  （P-01 / P-09 / P-10 / D-08+D-10+D-11 / 主机测试与夹具 / 本轮报告）。

---

## 7. 设备与数据留存状态（本轮与第 7 轮不同，务必注意）

- **本轮点过「保存归档」**：`:94140`、`:94873` 两次 `saveAll ok=3/3`。磁盘归档已从第 7 轮的 81 项状态
  变为**本轮末态**：51 条测量（48 条实值 + 3 条手量）、26 个 ROI、3 枚种植体（`#73` 带 `sku=GEN-5.8-15`）、
  **8 条神经管路径（含 1 条 0 点的 #184）**、**126 条 P-10 风险**、0 条标注。
- 因此第 7 轮报告里那份 81 项归档**已不可回退**；下轮若要复现第 7 轮场景，得先确认末态差异，
  或删掉 `#184` 与风险条目再存一版。
- 设备侧权重文件仍在外部目录（12:48 push）；**杀掉进程不会丢**，但重装 app 会清（外部目录随包卸载）。
- 进程 `12764` 在报告定稿时仍在运行；`dumpsys meminfo` 即在该状态下取。
- 主机证据（下轮换设备前请先归档）：`/tmp/dcmtk_r8/`（全量日志 + `canal_device.parity.bin`）、
  `/tmp/dcmtk_verify/round8/logs/`（4 份过滤件）、`/tmp/dcmtk_verify/r8_out/`（8 份产物）、
  截图 `05_r8_01_measure_page.png` – `15_r8_11_export_state.png`、dump `r8_dump/rec/list/nerve/dlg/lib/lib2/arch.xml`。
- **第 9 轮补录之后（报告定稿时复核过）**：进程 `12764` 仍在运行；`run-as … ls -l files/plans files/measurements`
  显示本例的两份归档 mtime **仍是设备时间 `10-11 15:11`**（第 8 轮那两次 `saveAll`），
  第 9 轮（设备 15:43–15:57）**一个字节都没写盘** —— 上面那句「末态 = 本轮末态」依然成立。
  但**内存态已经不同**：会话里 `#359` 已被删除、`#177` 的 `crownMeasured` 已变 0。
  所以「杀掉进程再进来」会把磁盘末态（含 `#359`、含 `S-11 41.1`）带回来，**别把两者混为一谈**。
- 第 9 轮主机证据：`/tmp/dcmtk_r9/logcat_r9.txt`（32,548 行 / 5,990,678 B，`-v threadtime`）、
  `logcat_r9b.txt`（二次采集 7,028 行，可弃）、33 份 `ui_*.xml`（含 `ui_r10_*` 6 份纯滚动勘查）、
  截图 `/tmp/dcmtk_verify/16_r9_01_crown_applied.png`、`17_r9_02_invalid_ref_recompute.png`。

---

## 8. 建议的下一步（按风险/收益排序）

1. **先按分类细粒度提交本轮 49 项改动**（未提交的 C++/Kotlin 改动一旦被 IDE 串档，D-10/D-11 这类判级修复很难单独回滚）。
2. 就 **D-13** 定口径：SR 对 `detail.error` 行写 TEXT 还是整条跳过（与 D-10 同族，改 `SrReport.cpp:624-636` 一处即可），
   改完在同一状态下重导一次 SR + PDF，比对 `num=` 计数应减少 48。
3. **修 D-14**（第 9 轮新发现，成本最低、后果最直接）：`onRowDelete()` 的 ROI / ANNOTATION 分支补 `recomputePlan`，
   core 侧删除时把引用该 id 的 `crownRefKind` 置 `CROWN_NONE`（见 §4 的两步修法）。
   复验口径：删掉被引用的球面 ROI 后**不点任何对话框**，日志里应立刻出现 `resolveCrownReference … 不存在` + `measured=0`，
   列表同一时刻变「未设冠方参考」。顺带把 §5 项 1 剩下的两条补掉：S-11/S-12 进 PDF/SR 的带数值行（需要点一次导出，
   **导出前先另存当前归档**）、以及 `CROWN_ANNOTATION` 分支（两点标注给冠长轴 ⇒ 验 `axisFromRef=1` 与「成角按冠长轴」文案）。
4. **P-10 是否进正式报告**（PDF 章节 + SR 容器）需要你定报告法定内容；同时建议顺手明确
   「风险条目在重跑推理后是否自动作废」—— 本轮只验了「旧条目会显示、重跑会重算」，没验「推理作废后风险条目行为」。
5. **P-01 的临床可用性卡在权重**：要么换真实分割网络（DentVoxel 官方基线级别）再跑门禁，要么把 P-01 在 README 里
   明确标成「工程链路就绪，模型不合格，默认预览」。当前默认行为已经正确，**不要**为了让叠加层好看而把开关设成默认开。
6. 取证卫生：`logcat -v threadtime` 分档落盘（第 9 轮已做到）、PC-04 三档全取、黄档造数据，再加两条第 9 轮新踩出来的：
   - **每轮开头量一次主机/设备时钟差并写进报告**（本机实测设备慢 71 s）。界面快照与截图的 mtime 是主机时间、
     logcat 是设备时间，混用会造出「界面比日志旧/新」的假缺陷（§2.3.1 末尾那张换算表就是补救）；
     更省事的办法是**每点一次就 dump 一次**，让点击与快照一一对应，再统一按设备时间排。
   - **给「没有日志的动作」补日志**：`removeRoi` / `removeAnnotation` 这类删除当前只有 `prims` 计数能反推，
     第 9 轮因此把删除时刻留成 ±35 s 区间。修 D-14 时顺手在删除路径打一行 `LOGD`（含 id 与是否触发重算）。
   以及 D-12 的性能优化（①按掩膜非零体素遍历 ②组合 ROI 复用子集）改完用同一份 parity dump 证明 R-06 体积逐位不变。
