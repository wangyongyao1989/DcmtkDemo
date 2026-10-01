# UI 重构轮：AI 面板剥离 + 结果折叠 + 按钮布局 —— 真机测试/回测报告

本轮需求（用户原文编号 1–5）对应的实现与两轮真机验证记录。
**第一轮 = 全量功能回归（round5）**，在回归中发现的 UI/提示缺陷**改完代码后**再做
**第二轮 = 定向回测（round6）**，两轮都用同一台真机、同一份 APK 构型。
所有点击坐标取自**当次** `uiautomator dump` 的节点 bounds 中心（脚本
`.qoder/skills/android-adb-ui-regression-loop/scripts/nodes.py`），滚动/弹窗后重新 dump；
没有真机日志或截图支撑的行，本报告标注为「未取证」。

---

## 0. 结论摘要

| 需求 | 结果 | 关键证据 |
|---|---|---|
| 1 牙科 CBCT 数据入 assets，开箱可用 | **通过** | 真机日志 `病例 dentvoxel_0101 已释放 128 张 -> /storage/emulated/0/Android/data/com.example.dcmtkdemo/files/dental_dentvoxel_0101`；clone 后不联网、不手动放文件即可解析（`round6/logs/02_load_0101.txt`） |
| 2 AI 辅助分析剥离为独立 Fragment | **通过** | 新增 `CbctAiFragment.kt`(447 行) + `fragment_cbct_ai.xml`(159 行)；宿主 `CbctMeasureFragment.kt` 由 1509 行降到 1367 行，与 AI 的唯一耦合收敛为 `CbctAiHost` 四个方法 |
| 3 「结果 Results」可折叠 | **通过** | 78 项时默认折叠（`▸ 点击展开`），点标题展开后按钮文案变 `点击折叠`（`round6/04_04_results_expanded.png`） |
| 4 按钮布局紧凑不遮挡 | **通过**（叠加层标签只部分改善，见 D-07） | 测量页快捷操作压成 5 按钮一行；AI 页读数移到按钮之后 —— 跑完分割按钮不再位移（`btn_ai_*` 始终 y=854/938/1018） |
| 5 全量回归 + 缺陷修复 + 回测 + 报告 | **通过** | round5 记 7 项缺陷（D-01~D-04 UI/提示、D-05~D-06 主机侧脚本、D-07 叠加层标签），前 6 项改完并回测复验，D-07 只部分改善已如实标注；性能取证见 §6 |
| AC-08 主机/真机奇偶校验（顺带复跑） | **通过** | round6 重新导出真机 dump 并用 `check_device_dump.py` 判 PASS；且这份 dump 与 round5（01 02:55 拉取）的 dump **逐字节相同**（sha256 前 12 位都是 `ddc22d9e5bb3`，20,643,904 B） |
| AI-01 精度 | **仍未达 PRD 门槛** | held-out F1 0.5828 < 0.85；本轮定位是「粗筛」，界面固定展示读数禁忌文案 |

一句话：**功能与交互本轮闭环；精度是遗留问题，不是本轮引入的回归。**

---

## 1. 改动范围（文件与体量）

```
app/src/main/java/com/example/dcmtkdemo/fragment/
  CbctMeasureFragment.kt        1509 -> 1367 行   AI 逻辑外移 + 新按钮接线
  CbctAiFragment.kt              (新增)  447 行   AI 面板：装载/分割/逐牙测量/推荐/取证/清除
app/src/main/res/layout/
  fragment_cbct_measure.xml      756  -> 792 行   tab 条 + 结果折叠头 + 5 按钮快捷行（含新「闭合路径」）
  fragment_cbct_ai.xml           (新增)  159 行   读数在下、按钮在上 + 固定禁忌提示
cbctmeasure/src/main/java/com/wangyao/cbctmeasure/view/
  MeasureToolController.kt      1106 -> 1143 行   discardDraft() / closeAreaDraft() / stickyMissHint 清理 / 面积提示文案
  MeasureOverlayView.kt           451 -> 482 行    标签避让：同帧已画文字框求交后整行下移
app/src/main/res/{color,color-night,drawable,values}/  (新增)   tab 文字夜间对比度、bg_tab、CompactButton/TabButton 样式
cbctmeasure/src/host/ai/scripts/                     9 改 + 2 补提交   见 §7，主机侧可复现性修复
```

`:rawpixeldeal` 与 `:cbctdeal` 的图像/体数据算子源文件**本轮零改动**（`git status` 的工作区
清单里没有这两个模块的任何文件）；350 HU 窗宽下限等基线约定保持原样。

结构上的所有权约定（这是能剥离的前提，也是本轮没有引入「两个 Fragment 各持一份 Native 会话」
这类事故的原因）：AI 层不改几何、不算数值，只把「推理回执 + 掩膜 ROI + 推荐候选」投影成状态文字，
所以它与宿主的耦合面是 `sessionHandle` + 一次刷新回调：

```kotlin
interface CbctAiHost {
    fun aiSessionHandle(): Long          // 0 = 尚未解析序列
    fun aiVolumeInfo(): VolumeInfo       // 取证文件命名要用
    fun aiInvalidate(overlay: Boolean, results: Boolean)
    fun aiPickDentalCase()               // 让宿主走完整的「选择加载牙科 CBCT」流程
}
```

体数据、渲染窗口、结果列表仍归宿主 `fl_ai_panel` 之外的主页所有。

---

## 2. 环境与构型

| 项 | 值 |
|---|---|
| 设备 | Honor AGM3-W09HN，serial `AWTYCP2C27400666`，Android 10，arm64-v8a |
| 分辨率 | physical 1200x1920，uiautomator 窗口 1920x1200（横屏），density 320，夜间模式 ON |
| 构建 | `./gradlew :app:assembleDebug --offline`；APK `app-debug.apk` 177,581,194 B，构建于 10-01 03:46:46 |
| 安装 | `adb install -r`，`lastUpdateTime=2026-10-01 03:47:21`；回测前确认无源文件比 APK 更新（`find app/src/main cbctmeasure/src/main -newer <apk>` 为空） |
| 推理运行时 | ONNX Runtime 1.17.0 / C API v17，从 `base.apk!/lib/arm64-v8a/libonnxruntime.so` dlopen |
| 证据目录 | 第一轮 `/tmp/dcmtk_verify/round5/`（28 张编号截图 + `device_0101.parity.bin`，02:50–03:55）；第二轮 `/tmp/dcmtk_verify/round6/`（7 张截图 + 14 份日志 + `device_0101_round6.parity.bin`，04:10–04:22） |

---

## 3. 第一轮：全量功能回归（round5）

覆盖测量页每一节与 AI 页每一个按钮。判定依据 = 模块 logcat TAG
（`CbctMeasure` / `CbctMeasureCore` / `CbctMeasureAi` / `CbctMeasureCtrl` / `CbctMeasureJni` /
`CbctMeasureOverlay` / `CbctVtk` / `CbctNative`）+ 拉回主机的产物离线复核。

| 功能项 | 期望 | 实测 | 判定 |
|---|---|---|---|
| 1) 数据源：解析 neck_ct | 出像素与 C/W | `解析完成 (225 ms)` / `(229 ms)`，`tv_info` 同步刷新 | 通过 |
| 1) 数据源：加载牙科 CBCT（0101/0021） | 释放 assets → 填路径 → 自动解析 | 128 张释放成功；`bindVolume: 192 x 192 x 128, spacing 0.60/0.60/0.60` | 通过 |
| 换例（0101 → 0021） | 旧会话必须销毁，AI 需重新装载 | `destroySession(handle, 50 measures)` → `createSession` → 0021 归档 `29/16` | 通过（设计如此，见 §8） |
| 3) 三维画面：VR/MPR 切换 | 切面工具进 MPR、VR 工具退回 | `ensureMprForSliceTools` / `exitSliceToolForVr` 各自 toast 生效 | 通过 |
| 4) 工具：距离/角度/弧/密度/面积 | 数值与单位正确 | 面积多边形闭合后 `addMeasure ROI 面积 cost=0.0ms`，值 `32.8032 cm2` 与主机几何公式一致 | 通过 |
| 5) AI：装载模型 | dlopen + session 建成 | `loadModel ok: onnxruntime 1.17.0 / C API v17 / in 'feat' [6,96,96,64] / out 'prob'`，模型 14,477 B | 通过 |
| 5) AI：牙齿自动分割 | 阈值 0.49、26 邻域、丢 <8 体素 | 0101 `inst=24 toothVox=9958 thr=0.4900`；0021 `inst=13` | 通过 |
| 逐牙自动测量 | 每实例体积 + 骨密度，且**不重复堆积** | `24 实例 -> 48 条测量（R-06 掩膜 ROI 复用 M-04/M-08）`；再点一次走「清理旧自动测量 2 条」分支 | 通过（#27 的回归保持） |
| 种植位点推荐 | 候选弹窗、安全判级 | 9 个候选，弹窗含间隙/骨密度/神经管距离 | 通过 |
| 导出取证数据 | AC-08 夹具落盘 | round6 复跑：`dumpParity -> .../files/ai_parity/device_DENTAL_CBCT_0.6MM_192X192X128.parity.bin (20643904 bytes)`，`run-as` 拉回主机后 `VERDICT: PASS`（详见 §5 R16 与 D-06） | 通过 |
| 清除 AI 结果 | 叠加图元减少、记录不脏 | `clearAi` 后 prims `36 -> 17`，R-06 ROI 保留 | 通过 |
| 6) 结果列表 | 自动折叠阈值生效 | 76 项时折叠为一行标题 | 通过 |
| 7) 归档：保存/清空/恢复 | 往返无损 | 保存 3/3 → 清空（二次确认）→ 0 → 恢复 `51 records, 26 ROIs, 78 行` | 通过 |
| 报告导出 | PDF + SR 可用 | `exportAll cost=1550 ms`；PDF 3 页 765,225 B（拉回主机数页确认）；SR 50 measures / 54 num / 59 text，10,123 B，设备侧 `verifySr` 自校验通过 | 通过 |
| 夜间模式 tab 对比度 | tab 文字在深色背景可读 | 补 `res/color-night/tab_text.xml` 前后截图对比（`round5/07*.png` vs `round5/08*.png`） | 通过 |

---

## 4. 回归中发现的缺陷与代码修复

前四个是「界面看起来没反应」或「按钮会跳走」这一类只会在真机上暴露的问题，
每一个都先复现、再改代码、再在 round6 逐条复验（§5 R6/R10–R13）。
D-05/D-06 是主机侧流水线缺陷：不改 APK，但会让下一个人重跑取证链时得到假结论，
它们的复验证据是 round6 拉回的那份 dump 与 §7 的 `verify_all.py` 全绿。
D-07 是需求 4「不遮挡」范围内的显示层缺陷：本轮只做到「多数标签错行」，
密集场景仍会压字，已用截图取证并列为遗留项，不当成已修复。

### D-01 点「丢弃草稿」后提示文案停在旧点数

* **现象**：草稿已清空（再点切面重新计数从 0 开始），但提示行仍显示 `（6 点）`。
* **根因**：按钮走的是 `abortDraft()`，它只清 `pending` 数据，不刷新提示；提示是
  `updateHint()` 渲染的，于是文案与状态脱节。
* **修复**：新增 `MeasureToolController.discardDraft()` = `abortDraft()` + `updateHint()` +
  `pushDraft()`，宿主按钮改调它。
* **回测**：`round6/logs/09_discard_draft_hint.txt` —— 点「丢弃草稿」后提示立刻变成
  `在切面上逐笔点出封闭路径（0 点），点击起点闭合，或按「闭合路径」直接提交`。

### D-02 「丢弃草稿」后提示变成「当前切面未拾取到点」且一直挂着

* **现象**：D-01 修完的第一版构建上，点完丢弃草稿提示反而显示 `当前切面未拾取到点`。
* **根因**：`stickyMissHint`（没点中切面时留下的粘性提示）只在指针 DOWN 时被冲掉；
  按钮点击不是 measure view 上的 DOWN，于是粘性提示覆盖了新提示。
* **修复**：`discardDraft()` 与 `undoPendingPoint()` 里显式 `stickyMissHint = null`。
* **回测**：`round6/logs/11_undo_point_hint.txt` —— 撤销/丢弃后提示都是正常的 `（0 点）` 行。

### D-03 放大视图下面积多边形「闭不上」

* **现象**：起点闭合同容差是 2.0 mm 世界坐标，放大后不到一个屏幕像素，用户反复点起点
  都点不中，只能靠 120 点自动闭合或丢弃重来。
* **根因**：闭合只有「点起点」一条路径；不是缺陷但属于不可用的交互。
  （上一轮误判为「闭合逻辑坏了」，实际是残留 `pending` 让 `pending.first()` 不是刚点的那个点。）
* **修复**：新增 `closeAreaDraft()` + 布局里的 `btn_close_area`「闭合路径」按钮直接提交；
  点数不足时给出可执行的提示（`闭合无效：还需 N 个点（当前 M 点）`）而不是静默失败；
  面积提示文案里写明按钮这条备用路径。
* **回测**：`round6/logs/08_close_area_by_button.txt` —— 4 点草稿点「闭合路径」即
  `addRoi #175 type=2 name=截面 4 点` + `addMeasure #176 type=4 value=2.6010 cm2`，
  提示行同步显示 `ROI 面积 2.601 cm2`；1 点时点闭合显示
  `闭合无效：还需 2 个点（当前 1 点）` 且草稿保留（`round6/logs/10_close_invalid_hint.txt`）。

### D-04 跑完 AI，下面一排按钮整体往下跳

* **现象**：点「牙齿自动分割」后 `btn_ai_measure` 从 y=572 跳到 y=700（第二轮量到最狠是
  状态文本 7 行），用户按上一个位置的手势会点空。
* **根因**：状态读数 `tv_ai_status` 排在按钮**上方**，`minLines=3` 只预留了 3 行，
  而分割后合法地需要 7 行。
* **修复**：把 `tv_ai_status` 移到 `progress_ai` 之后（即按钮与禁忌提示之下），
  面板顺序改为「操作在前、读数在后」；`minLines=3` 保留。
* **回测**：装载模型前后、分割之后三次 dump，`btn_ai_load/segment/measure` 恒为 y=854，
  `btn_ai_recommend/parity/clear` 恒为 y=938，`btn_ai_dental` 恒为 1018
  （`round6/03_03_after_segment.png`）。

### D-05（主机侧）`run_all.sh` 会把随包模型换成 1×1×1 旧模型

* **现象**：按脚本跑一遍主机流水线，`model/teeth_cnn.onnx` 变成 3,851 B 的 1×1×1 版本，
  与随包 14,477 B 的设备轴序模型不是同一个网络。
* **根因**：step 03 指向 `train_model.py`（旧实验），而当前交付模型由 `train2.py` 产出。
* **修复**：step 03 改 `train2.py`（`SKIP_TRAIN=1` 可跳过），1×1×1 备份留在
  `model/teeth_cnn_pointwise_old.onnx`；必需脚本清单补 `dump_template.py`、
  `make_parity_device.py`；`verify_all.py` 现在断言随包资产 sha256 == 重建模型 sha256
  == `device_parity.json` 记录的 sha256。详见 §7。

### D-06（主机侧）`check_device_dump.py` 的正常设备默认判 FAIL

* **现象**：round6 把真机 dump 拉回主机跑 `check_device_dump.py`，输出
  `FAIL: feat max|diff| 2.980e-08 > 0.000e+00` → `VERDICT: FAIL`，
  而 label/inst 都是 `bitwise=YES`、逐实例体素数完全一致。
* **根因**：`--feat-max-abs` 的默认值是 `0.0`（要求逐位相同）。真机是 ORT 1.17 + arm64 kernel，
  主机夹具是 ORT 1.23 + x86，float32 累加顺序就注定了 1e-8 量级差；
  上一轮（09-30）判 PASS 靠的是命令行上手工加了容差 —— 默认值会把每台正常设备都判成故障。
* **修复**：默认改为 `1e-6`，并把实测数字与理由写进代码注释；
  `--fixtures-dir` 的默认值同时从绝对路径 `/tmp/ai_build/parity_ref` 改成
  随仓库定位的 `parity_ref/`（尊重 `AI_BUILD`），clean clone 不用再记参数。
* **回测**：同一份 round6 dump 重跑 → `EXIT=0`，`VERDICT: PASS`，
  label 0/589824 体素不一致、inst 100% 精确匹配（`round6/logs/14_check_device_dump.txt`）。

### D-07 叠加层标签互相压字（部分改善，未完全消除）

* **现象**：24 颗 AI 实例 + ROI + 推荐位点同时显示时，多条文字标签投影到同一行，
  肉眼读不出任何一条 —— 属需求 4「不遮挡」范围内。
* **根因**：`drawLabel()` 把每条标签钉在锚点上方固定偏移处，与同帧其它标签无关。
* **修复**：`onDraw` 开头清空 `labelRects`，每条标签落笔前与已画框求交，压字就整行下移
  （上限 `LABEL_AVOID_TRIES = 6`）；求交用成员 `scratch RectF`，不在绘制路径里分配（PC-02 预算）。
* **回测**：round5/round6 都跑在这份代码上（改动 02:43，首个含它的 APK 02:47 构建）。
  多数标签已错行，但**没有完全解决**：`round6/03_03_after_segment.png` 放大后仍能看到
  `ROI 面积 32.80 cm2` 与 `间距: 9.9mm` 压在同一行 —— 6 次下移上限在密集场景不够，
  且只对「已画过」的框避让，绘制顺序影响结果。列为遗留项（§8 第 8 条）。

---

## 5. 第二轮：回测（round6）逐条复验

回测只跑「本轮改过的东西 + 它们直接影响的链路」，全部在同一份 APK（03:46 构建）上完成。

| # | 动作（坐标取自当次 dump） | 实测日志/读数 | 判定 |
|---|---|---|---|
| R1 | 冷启动 | `am start -W`：`LaunchState: COLD`，`TotalTime: 1626 ms` | 通过 |
| R2 | 进入 CBCT Measure，加载牙科 0101 | `tv_info = 解析完成 (222 ms)`；`病例 dentvoxel_0101 已释放 128 张 -> .../files/dental_dentvoxel_0101`；`bindVolume: 192 x 192 x 128, spacing 0.60/0.60/0.60`；归档自动恢复 `measures loaded: 51 records, 26 rois` | 通过（需求 1） |
| R3 | 切到 tab「5) AI 辅助分析 (Phase 2)」 | 面板节点：`tab_ai` / `fl_ai_panel` / `ai_root`；6 个 AI 按钮 + `cb_ai_overlay` + `btn_ai_dental` | 通过（需求 2：AI 已在独立 Fragment 内） |
| R4 | 「装载模型」 | `ORT so 候选（2 条，磁盘形态无）：.../lib/arm64/libonnxruntime.so \| .../base.apk!/lib/arm64-v8a/libonnxruntime.so`；`dlopen ok: base.apk!/lib/arm64-v8a/...`；`模型 .../files/ai_models/teeth_cnn.onnx（14477 字节）... ORT 1.17.0 api v17`；`ORT 就绪：in='feat' 原始shape[1,6,96,96,64] -> 逻辑 C6 × X96 × Y96 × Z64；out='prob' rank=5 通道 in/out=6/2`；状态读数出现在按钮**下方**（y=1114） | 通过（D-04） |
| R5 | 「牙齿自动分割」 | `grid: app 192x192x128@0.600 -> model 96x96x64 factor 2x2x2`；`thresholdMask: thr=0.4900 tooth voxels=9958 / 589824`；`connectedComponents: 24 instances`；`archAssign: occlusal z=61.17mm gap=15.95mm instances=24`；`runSegment ok: inst=24 prep=676.8ms infer=329.6ms post=7.5ms total=1014.1ms alloc=21.9MB` | 通过 |
| R6 | 分割后再 dump 按钮 | `btn_ai_load/segment/measure` = y 854；`btn_ai_recommend/parity/clear` = 938；`btn_ai_dental` = 1018（与分割前逐位相同） | 通过（D-04 复验） |
| R7 | 「逐牙自动测量」 | `aiAutoMeasure: 复用 ROI #67/#70（aiLabel=23/24），清理旧自动测量 2 条`；`addMeasure #171 type=3 value=0.0138 cm3`、`#172 type=7 value=2274.5312 HU`；汇总 `24 实例 -> 48 条测量（R-06 掩膜 ROI 复用 M-04/M-08）` | 通过（无重复堆积） |
| R8 | 结果列表 | 标题 `6) 结果 Results（78 项）`，折叠态 `tv_results_arrow = ▸` + `点击展开`；点标题后 `点击折叠` 且行渲染出来（`#127 AI 牙 L2(#1) 体积 8.471 cm3`、`遍历 1121796 体素 / 114 ms`、`#128 ... 骨密度 2193 HU`） | 通过（需求 3） |
| R9 | 「清除 AI 结果」 | `clearAi: 推理结果已丢弃（R-06 ROI 保留，统计将为空并给出原因）`；叠加层 `prims=32 -> prims=25` | 通过 |
| R10 | 面积工具 + 「闭合路径」 | 3 次点空后 `当前切面未拾取到点`（粘性提示，见 D-02 的机制说明），改点在切面内 → `pick ... hit=true plane(0,63) at voxel(159,119,63)=253HU`，提示涨到 `（4 点）`；点「闭合路径」→ `addRoi #175 type=2 截面 4 点` + `addMeasure #176 type=4 value=2.6010 cm2`，提示 `ROI 面积 2.601 cm2` | 通过（D-03） |
| R11 | 「丢弃草稿」 | 提示立刻回到 `在切面上逐笔点出封闭路径（0 点）...`，无残留 `未拾取到点` | 通过（D-01 + D-02） |
| R12 | 1 点时点「闭合路径」 | `闭合无效：还需 2 个点（当前 1 点）`，草稿未被吞 | 通过（D-03 边界） |
| R13 | 「撤销取点」 | 提示回 `（0 点）` | 通过 |
| R14 | 内存采样 | 装载 AI 并分割后 `TOTAL PSS 268,869 KB`（Native Heap 116,088 / GL 73,300 / EGL 13,856 / Java 14,900）；清除 AI 结果后 `TOTAL PSS 257,488 KB` | 见 §6 |
| R15 | 「牙齿自动分割」二次运行（为导出取证数据重跑） | `runSegment -> ok=1 inst=24 total=997ms (prep 659 / infer 331 / post 7)`，实例数/体素数与 R5 完全一致 | 通过（结果可重复，耗时与首次 1014 ms 同量级） |
| R16 | 「导出取证数据」→ 主机奇偶校验 | 设备：`dumpParity -> /data/user/0/com.example.dcmtkdemo/files/ai_parity/device_DENTAL_CBCT_0.6MM_192X192X128.parity.bin (20643904 bytes)`，状态区直接把 `run-as cat` 拉取命令打出来；主机：header 解出 `featCount=3538944 probCount=1179648 labelBytes=589824 modelDim=(96,96,64)`，`check_device_dump.py` → `VERDICT: PASS`、`EXIT=0`，且这份 dump 的 sha256 前 12 位 `ddc22d9e5bb3` 与 round5 拉到的 dump **逐字节相同** | 通过（AC-08，见 D-06） |

---

## 6. 性能与内存取证

| 指标 | 实测 | 备注 |
|---|---|---|
| 冷启动 `am start -W TotalTime` | round6 **1626 ms**；round5 2648 / 1737 ms（COLD） | 同一 APK，抖动主要来自系统侧；热启动 150 / 143 ms |
| 序列解析 | **222 ms**（本轮 `tv_info`），round5 225 / 229 ms | 192×192×128×int16，9.4 MB 量级 |
| AI 分割（全链路） | round6 首次 **1014 ms**（prep 676.8 + infer **329.6** + post 7.5）、二次 **997 ms**（prep 659 / infer 331 / post 7） | 0101；两次都是 `inst=24 toothVox=9958`，结果完全一致；round5 两次 1006 ms；抽稀 + 特征构建占 2/3，纯推理 1/3 |
| 单实例体积遍历 | `1121796 体素 / 114 ms` | 结果行里直接可读，属宿主测量而非 AI |
| 逐牙自动测量 | 24 实例 → 48 条记录 | ROI 复用，不重复建 ROI |
| 叠加层绘制成本 | `avgDraw = 0.46 / 0.72 / 0.82 / 1.49 ms`，prims 33 | 折叠长列表后叠加层不是瓶颈；fps 行只是低频采样 |
| 报告导出 | `exportAll cost=1550 ms`（round5） | PDF 3 页 765,225 B + SR 10,123 B |
| 内存 | AI 装载+分割后 TOTAL PSS **268.9 MB**；清除 AI 后 **257.5 MB**；round5 对照：AI 在 253.9 MB → 清后 184.1 MB | 差值主要来自推理期间的特征/概率缓冲（日志 `alloc=21.9MB`）与 native 堆；**注意**：AI 会话是跟着 `MeasureSession` 活的，清除结果不等于释放 ORT 会话，只有换例/退出才释放，这解释了本轮「清除后仍 257 MB」而 round5 归档态量到 184 MB |

---

## 7. 主机侧可复现性（本轮顺带把 story 修实）

需求 1 说「开箱可用」，随包的是 assets 里的 DICOM 与 `teeth_cnn.onnx`；
主机侧 AI 流水线（`cbctmeasure/src/host/ai/`）不随 APK，但要能被下一个人重跑。
本轮把这条链修到「跑完不崩、结论诚实」：

```
$ cd cbctmeasure/src/host/ai/scripts && python3 verify_all.py
...
PASS model sha256 == the record the device fixtures were built from  |  built 3092730fc2866074 vs device_parity 3092730fc2866074
PASS model/teeth_cnn.onnx == shipped asset assets/models/teeth_cnn.onnx
PASS every Conv has kernel_shape [3,3,3]  |  [(3, 3, 3)]
PASS ORT(device_feat_0101.raw) == device_prob_0101.raw to 1e-6  |  max abs diff 0.000e+00
PASS device label == (prob[ch1] > 0.49)  |  tooth voxels 9958
PASS 26-CC census reproduced (count + per-instance sizes, after the 8-voxel gate)
     |  scipy 34 comps / 9958 voxels -> gated 24, fixture 24 comps / 9935 voxels
PASS build_features(canonical).transpose(0,2,1,3) == device_feat_0101.raw to 1e-6  |  max abs diff 0.000e+00
HEADLINE (held-out dentvoxel_0101, 阈值 0.49):
  dice 0.5828  precision 0.6452  recall 0.5315  f1 0.5828  tp 6425 fp 3533 fn 5664
  PRD AI-01 门槛 F1>=0.85: 未达标（本模型定位是粗筛）
56 checks run, 56 passed, 0 failed
```

逐条对应本轮的修复：

* **随包模型 == 重建模型**：`run_all.sh` step 03 原来指向 `train_model.py`（1×1×1 旧实验），
  会把 14,477 B 的设备轴序模型覆盖成 3,851 B；现在指向 `train2.py`，并把旧模型留在
  `model/teeth_cnn_pointwise_old.onnx`，`verify_all.py` 断言三者
  （重建 / `device_parity.json` 记录 / app assets 随包）sha256 一致。
* **派生产物缺失不再假装失败**：`app_volume/`、`work/`、`*.log`、`__pycache__/`、`gt/*.raw`、
  旧主机链 `parity_ref/{feat,prob,lab,inst,label,gt}_*.raw` 全部写进新增的
  `cbctmeasure/src/host/ai/.gitignore`（跑一次 `run_all.sh` 就重新生成）；
  随包入库的证据只有输入 `raw/`、`gt/*.nii.gz`、`model/` 与设备链 `parity_ref/device_*`。
  没跑过 step 01/02 的克隆会打 `SKIP`，而不是 `FileNotFoundError` 或误判 FAIL。
* **26 邻域计数按设备规则比**：设备侧丢弃 <8 体素的连通域，检查也先过同一道闸，
  否则 scipy 裸跑 34 域 vs 夹具 24 域会被当成不一致。
* **`make_parity.py` 可单独重放**：不再硬依赖 step 02/03 的 `work/cache` 与 `metrics.json`，
  缺了打印 NOTE 并跳过那一项；`feat_match=None` 在 JSON 里写成显式字符串而不是 null。
* **轴序差异写成 WARN 而不是 FAIL**：旧主机链的 `prob/lab_0101.raw` 是主机轴序下的另一份预测
  （实测 prob 通道 1 逐元素最大差 9.97e-01，lab 在 (1,0,2) 置换下 Dice 0.81）。
  这不是 parity 失败，而是「两条链算的是两个不同的预测」——随包证据只认 `device_*_0101.raw`。
  这一条正是 AC-08 里「设备轴序」纪律的来源（README §6.10）。
* **`make_report.py` 的崩溃修成明确结论**：`TypeError: must be real number, not str` 源于
  模板里 `~%.1fx` 吃了 `f()` 返回的字符串；同时它读的是旧实验 `metrics.json` 的字段。
  现在（1）格式符改 `%s`；（2）`metrics.json` 缺失或 schema 不符时直接说明
  「本报告只描述旧 1×1×1 实验，当前交付链路的数字在 `parity_ref/metrics_0101.json`、
  `parity_ref/METRICS.md` 与 `cbctmeasure/doc/`」；（3）模型 sha 改为**现场计算**，
  并去掉那句已经反了的「另一个主机进程写了不合规的 3×3×3 模型」。
* **设备/主机数字对得上**：真机 `thresholdMask tooth voxels=9958` + `connectedComponents 24 instances`
  == 主机夹具复算值，这条等价是本轮最有价值的交叉验证。

---

## 8. 未覆盖 / 已知限制

1. **AI 精度仍是粗筛级**：held-out F1 0.5828（PRD AI-01 门槛 0.85）。掩膜会把相邻牙并成一个实例，
   最大实例「体积」要按牙组读（本轮结果行就显示 `8.471 cm3` 这种量级）；推荐候选采纳前必须自行核对。
   页面固定展示这段禁忌文案，不是可选说明。
2. **换例后需重新装载模型**：ORT 会话挂在 `MeasureSession` 里，属设计；AI 页首行读数已提示。
3. **本轮未重跑完整训练**：`train2.py` 的 150 epoch 全量重训（含 `variants.py` 特征设计研究）
   没在回测里执行，因此「重新训练能逐字节复现随包模型」这一步只做到了
   **随包模型与夹具/资产三方 sha256 一致**，不是「重训一遍再比 sha256」。
   要补做：`SKIP_VARIANTS=0 bash scripts/run_all.sh`（先备份 `model/teeth_cnn.onnx`）。
4. **UI 只在一台设备取证**：AGM3-W09HN 1200x1920 横屏 + 夜间模式。5 按钮一行按 360 dp 最窄
   屏算过宽度，但未在更小屏/竖屏真机上跑；宽屏未测。
5. **`AC-06 <2 mm` 近距离场景**沿用一期结论（判级链路已验证，近距离截图未取证）。
6. **PC-03 截图耗时**（瓶颈在 `:cbctdeal` 的 `captureFrame()` GPU 读回）本轮未触碰，
   与基线一致，属渲染模块遗留。
7. **主机侧只提交输入与冻结产物**：`raw/`（46 MB 公开 DentVoxel NIfTI，含来源说明
   `raw/README.md`）、`gt/*.nii.gz`、`model/`、`metrics.json`、`toothseg_report.md`、
   `scripts/{dump_template,train_model}.py` 已入库；`app_volume/`、`work/`、`gt/*.raw`、
   旧主机链 `parity_ref/{feat,prob,lab,inst,label,gt}_*.raw` 属跑一次就重生的派生物，
   写进 `.gitignore` 不入库（随包证据只有 `parity_ref/device_*`）。
   真机取证 dump 本体（20 MB × 2）留在 `/tmp/dcmtk_verify/`，只把 sha256 与判定写进本报告。
8. **叠加层标签避让只做到「多数错行」**（D-07）：6 次下移上限 + 只跟已画框求交，
   24 实例 + ROI + 推荐同屏时仍会压字。要彻底解决得改成两遍布局（先量所有标签框再全局分配行位），
   本轮未做。

---

## 9. 证据清单

| 轮次 | 路径 | 内容 |
|---|---|---|
| round5 | `/tmp/dcmtk_verify/round5/01..22_*.png`（28 张，02:50–03:51） | 全量回归、夜间 tab 前后对比、丢弃草稿前后、闭合路径、AI 面板按钮位移 |
| round6 | `/tmp/dcmtk_verify/round6/01..07_*.png`（7 张，04:10–04:17） | 进入测量页、AI tab、分割后按钮位置、结果展开、闭合成功、丢弃草稿后提示、撤销后提示 |
| round6 | `logs/01_coldstart.txt` | `am start -W` 冷启动 1626 ms |
| round6 | `logs/02_load_0101.txt` | assets 释放 + `createSession` + 归档恢复 51/26 |
| round6 | `logs/03_ai_load.txt` | so 候选链、dlopen、模型 14477 B、ORT 1.17.0/api v17、输入输出契约 |
| round6 | `logs/04_ai_segment.txt` | 抽稀、`thresholdMask`、`connectedComponents`、`runSegment` 分段耗时 |
| round6 | `logs/05_ai_auto_measure.txt` | 24 实例 → 48 条、ROI 复用与旧记录清理 |
| round6 | `logs/07_clear_ai.txt` | `clearAi` + prims 32→25 |
| round6 | `logs/08_close_area_by_button.txt`、`09_discard_draft_hint.txt`、`10_close_invalid_hint.txt`、`11_undo_point_hint.txt` | D-01/D-02/D-03 的逐条复验 |
| round6 | `logs/06_meminfo_ai_loaded.txt`、`12_meminfo_after_clear.txt` | 268,869 KB / 257,488 KB 与叠加层 avgDraw 采样 |
| round6 | `logs/13_parity_dump.txt` | `dumpParity -> .../files/ai_parity/device_DENTAL_CBCT_0.6MM_192X192X128.parity.bin (20643904 bytes)` + 桩上打出的 `run-as cat` 拉取命令 |
| round6 | `logs/14_check_device_dump.txt` | 主机 `check_device_dump.py` 全文输出：header/`feat 2.980e-08`、`prob 1.192e-07`、`label bitwise=YES`、`inst 100.0000%`、逐实例体素数对照、`VERDICT: PASS`（D-06 复验） |
| round6 | `device_0101_round6.parity.bin`（20,643,904 B，sha256[:12] `ddc22d9e5bb3`） | 真机 dump 原件；与 round5 的 `device_0101.parity.bin` 大小与 sha 前缀一致 —— 同模型同数据的两次独立导出可复现 |
| 主机 | `cbctmeasure/src/host/ai/work/05_verify_log.txt`、`work/verify_summary.json` | 56 checks / 56 PASS / 0 FAIL |
| 主机 | `cbctmeasure/src/host/ai/work/{00,01,04}.log` | 模板导出、4 例体积 + GT + DICOM 往返、夹具重放 |

复跑方式（逐字可重放）：界面驱动见 `.qoder/skills/android-adb-ui-regression-loop/`
（`scripts/nodes.py` 取坐标、`scripts/shot.sh` 存档、`run-as` 拉私有目录），
主机侧见 `cbctmeasure/src/host/ai/scripts/run_all.sh`。AC-08 单独复跑：

```
ADB=$HOME/Library/Android/sdk/platform-tools/adb
$ADB shell run-as com.example.dcmtkdemo cat files/ai_parity/<name>.parity.bin > /tmp/dcmtk_verify/device.parity.bin
python3 cbctmeasure/src/host/ai/scripts/check_device_dump.py /tmp/dcmtk_verify/device.parity.bin   # 默认容差已改 1e-6，无需再带参数
```
