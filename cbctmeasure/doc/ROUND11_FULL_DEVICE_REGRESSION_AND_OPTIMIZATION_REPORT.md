# 第 11 轮：全项目功能真机验证、优化实施与回归报告

> 取证时间：2026-10-26 23:20 ~ 2026-10-27 11:10（设备时钟；设备比主机慢约 185 s）
> 设备：AGM3-W09HN，Android 10 / API 29，物理 1200×1920（横屏 1920×1200），`arm64-v8a`，序列号 AWTYCP2C27400666
> 被测包：`com.example.dcmtkdemo` versionName 1.0，`lastUpdateTime=2026-10-27 10:26:53`（设备时钟）≈ 主机 10:29:53，与主机 APK 构建时刻 10:29 对齐
> APK：`app/build/outputs/apk/debug/app-debug.apk`，177,853,338 B
> 代码基线：`d419163`（已提交部分）+ 工作区 14 个未提交文件（本轮的优化改动，**尚未提交**）
> 主机测试：`sh cbctmeasure/src/host/build_and_run.sh` → `PASSED 790 / FAILED 0 / FINDINGS 4`
> 截图：`/tmp/dcmtk_verify/01_r11_*.png ~ 66_r11_*.png`、`r11_01~r11_05_*.png`、`/tmp/r11_06_measure_ai.png`
> PDF 取证：`/tmp/r11_07_report.pdf`（修复后）、`/tmp/r11_08_report_before.pdf`（修复前）

---

## 0. 结论摘要

| # | 项目 | 结论 | 关键证据 |
|---|---|---|---|
| 1 | 全项目页面/功能可达性 | **通过** | Upload / Query / Worklist / Retrieve / Show / Compare / CT Preprocess / CBCT Parse / CBCT Measure(AI) 逐页截图 01~66 |
| 2 | CBCT Parse（解析 + VR/MPR） | **通过** | 512×512×265，解析 1853 ms；VR 骨架、MPR 轴位切面截图 `r11_01/r11_02` |
| 3 | CT Preprocess（最优/自定义调窗） | **通过（并修掉默认字节序错误）** | 修复前 Big 端 `.raw`：`C=29113.1 / W=527366.0` 满屏不可用；修复后默认 Little：`C=1782.7 / W=3240.9` |
| 4 | CBCT Measure 测量工具链 | **通过** | 距离/角度/面积/体积/HU/组合 ROI/隔离显示/神经管/标注，截图 36~58 |
| 5 | AI-01 牙齿分割 + 逐牙自动测量 | **通过** | `inst=24 toothVox=9958 total=980.3ms`（prep 654.2 / infer 318.7 / post 7.3），48 条测量 |
| 6 | 归档 / PDF / DICOM SR | **通过（含 D-18 拦截与 D-17 报告口径修复）** | 无推理导出被拦（10:37）；重跑推理后 `archive=3/3 pdf=true pages=3 sr=true verify=true`（10:50） |
| 7 | 本轮缺陷修复 | **7 项已实施** | D-15 / D-16 / D-17 / D-18 / D-19 / D-20 / D-21（O-15） |
| 8 | 本轮优化修复 | **2 项已实施** | O-01（Java Int 颜色序列化饱和）、O-02（ORT 会话释放顺序） |
| 9 | 稳定性 | **无崩溃 / 无 ANR / 无 Native signal** | 本轮多次冷启动与重活；当前 logcat 无 `FATAL`/`tombstone` |
| 10 | 设备数据状态 | **已恢复基线并复核 md5** | measure `fae2fe73…`、plan `a2b5f411…`、anno `3f08971d…` |

未修复、需决策或建议排期的项目见 §5；证据缺口与操作污染见 §6。

---

## 1. 被测构型与取证方法

### 1.1 构型

- 设备侧权重仍走**外置通道**（决策 2 的遵守证据）：`files/ai_models/teeth_cnn.onnx`（14,477 B）与 `files/ai_models/canal_cnn.onnx`；运行时 so 来自 APK 的 `libonnxruntime.so`（ORT 1.17.0 / C API v17）。
- 本轮未新增任何内置权重资产，`assets` 中仍只有测试用 `.raw/.bin/.dcm` 与字典。

### 1.2 方法与铁律

- 全程**只读观测 + 点击**：未 `pm clear`、未卸载重装、未删除应用数据。
- 点击坐标只取自**当次** `uiautomator dump` 的 `clickable="true"` 节点 bounds；滚动、切页、弹窗后重新 dump。
- 内部产物用 `run-as com.example.dcmtkdemo` 读取；外部 PDF/SR 用 `adb pull` 拉回主机离线校验。
- 唯一一次 `am force-stop` 发生在基线文件回灌之后（10:59），目的是让进程重新从磁盘装载基线，**不是清理数据**；冷启动后 11:00 自动恢复出 61 条记录 / 8 条神经管 / 234 条风险。

### 1.3 时钟与坐标

- 设备比主机慢约 **185 s**（设备 11:08:15 vs 主机 11:11:20）。本文日志时间一律用设备时钟。
- 横屏坐标系：`uiautomator dump` 的 bounds 与 `screencap` 像素一致（1920×1200），无需缩放。

---

## 2. 全项目功能清单与代码地图

> 本轮“阅读整个项目”的落点：先按页面建立功能入口，再沿入口追到模块边界。第三方源码（VTK / DCMTK / OpenCV / ONNX Runtime）不在下表。

| 页面 / 模块 | 主要功能 | 代码入口 |
|---|---|---|
| Upload | DICOM 文件选择、上传列表、详情查看、删除 | `fragment/UploadFragment.kt`、`activity/DetailActivity.kt`、`adapter/DcmUploadAdapter.kt`、`utils/FileUtil.kt` |
| Query | C-FIND 患者/检查查询、结果列表 | `fragment/QueryFragment.kt`、`adapter/PatientAdapter.kt` |
| Worklist | MWL 查询（按日期/模态） | `fragment/WorklistQueryFragment.kt` |
| Retrieve | C-MOVE/C-GET 检索与落盘 | `fragment/RetrieveFragment.kt` |
| Show | DICOM 序列浏览、缩放/翻页、网格 | `fragment/DcmShowFragment.kt`、`adapter/DcmImageAdapter.kt`、`view/ZoomImageView.kt` |
| Compare | 两种 raw 处理结果对比 | `fragment/FileCompareFragment.kt` |
| CT Preprocess | `.raw/.bin` 载入、最优/自定义调窗、旋转、细粒度参数、结果展示 | `fragment/CTPreprocessFragment.kt`；`:rawpixeldeal` 的 `ImageProcessor.cpp`、`MedicalCTPreprocess.cpp`、`CtSeriesProcessor.cpp` |
| CBCT Parse | DICOM 序列解析、体数据上屏、VR/MPR、相机操作、SAF 目录 | `fragment/CbctParseFragment.kt`；`:cbctdeal` 的 `CbctParseEngine.kt`、`CbctVtkView.kt`、`CbctJni.kt`、`CbctVtkJni.kt`、`CbctFileTransfer.kt` |
| CBCT Measure | 数据源/视口/工具/叠加层/结果列表/归档/报告 | `fragment/CbctMeasureFragment.kt`；`:cbctmeasure` 的 `view/*`、`jni/*`、`store/*`、`report/*` |
| CBCT AI（测页子面板） | AI-01 分割、逐牙测量、AI-02 下颌管、AI-03 推荐、parity、P-10 风险 | `fragment/CbctAiFragment.kt`；`:cbctmeasure` 的 `ai/*`、`model/AiModels.kt` |
| 主机侧测试 | C++ 核心单元测试与 PRD 精度门禁 | `cbctmeasure/src/host/test_main.cpp`、`cbctmeasure/src/host/build_and_run.sh` |

模块边界上的关键事实：

- `:cbctmeasure` 的 C++ 核心负责几何、ROI、规划、风险、JSON；Kotlin 负责 UI、归档、PDF、SR 与 AI 编排。
- `:cbctdeal` 只负责解析与渲染，测量叠加层通过 `CbctMeasureView` 叠在 VTK 视图上。
- 归档是“三层信封”：`{envelope, kind, savedAt, checksum, payload}`，`payload` 本身是 JSON **字符串**，读取时要二次 `json.loads`（历史坑，本轮未再踩）。
- AI 面板是测页的 child fragment，会话句柄由宿主页借出，AI 侧不另建 Native 会话。

---

## 3. 逐项真机验证结果

### 3.1 Upload / Query / Worklist / Retrieve / Show / Compare

- Upload：列表、详情、上传模式、无选择提示均可达；截图 `17~21_r11_*`。
- Query：页面与“查询”动作可达（`24/25_r11_*`）。**未连接外部 PACS**，因此只证明 UI 与本地校验链路，不证明外部 C-FIND 成功；本轮未做任何外部写入。
- Worklist / Retrieve：页面与表单可达（`26/27_r11_*`），同样未连外部服务。
- Show：网格与单帧浏览可用（`28/29_r11_*`）。
- Compare：默认参数与自定义参数两种对照结果均可渲染（`22/23_r11_*`）。

### 3.2 CT Preprocess（含 D-21/O-15 修复）

修复前（默认 Big 端读 `.raw`）：

```
method=6 -> C=29113.1, W=527366.0
```

整幅图处于极端窗宽下，肉眼不可用。手动切 Little 端后：

```
C=1782.7, W=3240.9
```

修复方式是“资产扩展名 → 字节序默认值”的提示（`.raw` → Little，`.bin` → Big），**不改 rawpixeldeal 的算子与调窗算法**，用户仍可手动改单选。修复后冷启动默认 Little，切到 `.bin` 自动改 Big；截图 `r11_03_ct_optimal.png`、`r11_04_ct_little.png`、`r11_05_ct_result.png`。

> 这项修复遵守了既有铁律：rawpixeldeal 的显示算法零改动，只纠正“输入解码参数”的默认值，因此不触发 A/B 回滚条款。

### 3.3 CBCT Parse

- 512×512×265 序列解析完成，耗时 **1853 ms**；VR 骨架与 MPR 轴位切面正常，截图 `r11_01_cbct_parse_vr.png`、`r11_02_cbct_parse_mpr.png`。
- 设备旋转、相机重置、MPR 层位切换、SAF 目录授权与解析均可达（`01~16_r11_*`）。

### 3.4 CBCT Measure 测量工具链

覆盖：距离、角度、面积、体积、HU 阈值 ROI、组合 ROI（并/差）、隔离显示、神经管描记、文字/线标注、行选中与删除确认。

- 隔离显示：开 / 关 / MPR 下忽略三种分支均有日志与截图（`47~51_r11_*`）。
- 删除结果：先弹确认框，再按 `ownerKind` 调对应删除通道；不再“点一下就没”。
- 本轮未改动任何测量算法；叠加层与 VTK 渲染的显示行为保持不变。

### 3.5 AI 链路（本轮 10:48~10:50 的完整复跑）

```
10:48:19  loadModel ok: onnxruntime 1.17.0 / in 'feat' [6,96,96,64] / out 'prob'
10:48:28  grid: app 192x192x128@0.600 -> model 96x96x64 factor 2x2x2
10:48:29  runSegment ok: inst=24 toothVox=9958 thr=0.4900
          prep=654.2ms infer=318.7ms post=7.3ms total=980.3ms alloc=21.9MB
10:49:15  aiAutoMeasure: 24 实例 -> 48 条测量
10:50:22  exportAll archive=3/3 pdf=true pages=3 sr=true verify=true
```

同轮早段还验证了 AI-02 下颌管（采纳 6 条自动中心线，清除旧 6 条、手描 2 条保留）与 P-10 风险批量评估（结果 234 条 = 13 组 × 18；0 点路径不参与）。这两个场景的日志未单独落盘为文件，但截图与归档数值可交叉核对：恢复基线后 `plan loaded: 3 implants, 8 nerve paths, 234 nerve risks`。

### 3.6 归档 / PDF / SR 与 D-18、D-17

**D-18 现场（10:37，未重跑推理）**：恢复归档后直接导出，被护栏拦下：

```
saveAll 拒绝覆盖测量档：未写入：48 条 AI 掩膜测量会因缺少推理结果被清零
（#359、#360、#361、#362、#363、#364 等）。请依次执行「装载模型 → 牙齿自动分割 →
逐牙自动测量」重算这些行（仅分割不会刷新读数），或先在结果列表中删除这些行再保存。
exportAll blocked: ...
archive=2/3 blocked=1
```

界面不再谎报“已保存/已导出”，而是把拦截原因原样展示。

**重跑推理后（10:50）**：

```
saveAll ... ok=3/3 blocked=0
SR written: ... 61 measures, 3 implants
SR verified: ComprehensiveSR (valid) / num=80 text=92 bytes=15057 observers=1
exportAll archive=3/3 pdf=true pages=3 sr=true verify=true
exportAll cost=1898ms
```

**D-17 的改前/改后对照**（同一基线、同一计划，PDF 文本抽取）：

| 版本 | 页数 | “神经管”文本出现次数 | 0 点路径 |
|---|---|---|---|
| 修复前 `00:49` 导出 | 3 | 10（8 条路径；手描条目会同时出现“神经管”标签与“神经管 N”名称） | `神经管 8：0 点，半径 1.5 mm` 存在 |
| 修复后 `10:50` 导出 | 3 | 8（7 条路径） | **已消失** |

修复后 7 条有效路径 = 手描 `神经管 1`（4 点）+ 6 条 `AI-02 自动下颌管`（18/13/18/8/6/8 点）；与 SR 侧“<2 点跳过”的口径一致。

### 3.7 基线恢复

本轮两次回灌基线并逐文件 md5 复核，最终值：

```
fae2fe732fbb6f7da58f60cbddcd8a0f  ..._measure.json
a2b5f4115d55b962191c4aa656be844a  ..._plan.json
3f08971d98374599ce716054e3e883d8  ..._anno.json
```

冷启动后自动装载基线：`measures loaded: 61 records, 36 rois`、`plan loaded: 3 implants, 8 nerve paths, 234 nerve risks`。最终页面截图 `67_r11_67_baseline_after_restart.png`（VR + 基线叠加层）。

---

## 4. 本轮优化清单（已实施）

### 4.1 缺陷修复

#### D-18（新增，高）恢复归档未重跑推理时，保存会把 48 条 AI 掩膜读数清零

- 现场：恢复归档后 Native 侧没有推理结果，AI 掩膜 ROI 的统计退化为 0 并带 `detail.error`；旧逻辑直接覆盖归档，真实读数永久丢失。
- 改法（`MeasurementStore.kt`）：新增 `aiMaskDegradedIds()` 三项同时成立才拦截——新值 0 + error 以「AI 掩膜」开头 + 旧归档同 id 非 0；命中则 `Result.blocked=true` 并拒绝写入测量档。
- 配套：`ReportExporter.exportAll()` 发现任一 `blocked` 即中止整条导出链，PDF/SR 都给“未生成：归档未通过数据完整性检查”；`CbctMeasureFragment.saveArchive()` 以 `results.all { it.ok }` 决定 toast，失败时展示首条真实原因；保存改到 IO 线程并加 `archiveSaving` 重入保护。
- 验证：真机 10:37 拦截、10:50 重跑后 3/3 通过（§3.6）。

#### D-19（新增，中）AI-03 推荐完成后状态行停在“正在评估候选位点…”

- 改法（`CbctAiFragment.kt`）：`setBusy(false, null)` 之后补 `refreshStatus()`。
- 验证：代码级；本轮未单独重跑推荐场景（见 §6.2）。

#### D-20（新增，低）D-18 提示语没有给出可执行步骤

- 改法：提示明确写成「装载模型 → 牙齿自动分割 → 逐牙自动测量」，并点明“仅分割不会刷新读数”。
- 验证：真机拦截文案（§3.6）即为最终文案。

#### D-15（第 10 轮新增，高）`clearCanalTracks()` 删管道但留下悬空风险行

- 改法（`MeasurementManager.cpp`）：删除自动管道时同步收集 id，并在同一批次 `erase-remove_if` 清理 `nervePathId` 命中的风险行；保留手描管道。
- 验证：真机日志 `clearCanalTracks: 清除 6 条自动管道，连带清理 108 条风险行`；结果条数 108 → 102，保存后的 plan 为 2 条神经管 / 126 条风险。
- 边界：**不追溯**历史悬空行。基线里引用 178~183 的 108 条仍在（这些路径已不存在且不是当前自动管道），处置建议见 §5.2。

#### D-16（第 10 轮新增，中）`assessNerveRisk()` 覆盖分支丢 `createdAt`

- 改法：覆盖同一组合时保留原 `createdAt`，只更新数值与时间戳；新增主机断言 `D-16 覆盖保留 createdAt（首次评估时间不漂移）`。
- 验证：主机 790 条断言通过；真机未单独构造覆盖场景（见 §6.2）。

#### D-17（第 10 轮新增，低/口径）0 点神经管路径仍进 PDF

- 改法（`ReportGenerator.kt`）：PDF 神经管小节只列 `points.size >= 2` 的路径，与 SR 侧口径统一。
- 验证：真机导出的 PDF 文本对照（§3.6）。

#### D-21 / O-15（新增，中）CT Preprocess 默认 Big 端读 `.raw`，首屏调窗不可用

- 改法（`CTPreprocessFragment.kt`）：资产下拉选中时按扩展名给默认字节序（`.raw` → Little，`.bin` → Big），只改默认值，不改算法。
- 验证：真机默认 Little 后 `C=1782.7 / W=3240.9`；切 `.bin` 自动 Big（§3.2）。

### 4.2 优化项

#### O-01（高）Java Int 颜色写入 JSON 被当成无符号长整型 → 读回饱和

- 现场：历史 ROI 颜色写成 `4294951936`，读侧 `optLong().toInt()` 饱和成 `2147483647`；36 个 ROI 中 12 个已损坏。
- 改法：`MeasureModels.kt` 的 `toJson()` 直接写有符号 `Int`；`Json.cpp` 的 `asInt()` 按 Java 窄化语义先 clamp 到 int64，再取低 32 位并按有符号解释。
- 验证：真机新建 ROI `#548`（HU 200~3000）落盘颜色 **-15360**（0xFFFFC400），读回正常；12 个历史饱和值不可逆，见 §5.3。

#### O-02（中）ORT 会话释放顺序不安全，重复装载有泄漏/崩溃风险

- 改法（`OrtEngine.cpp`）：先经 `s->api` 释放 session/runOptions/options/memInfo/env，置空后再 `dlclose`，最后清空 `base_`/`session_`/`soLoaded_` 与名称、维度、通道缓存。
- 验证：真机连续两次「装载模型」均 `loadModel ok`，pid 不变、无 tombstone。

#### 其他本轮优化（交互/可观测性）

| 项 | 文件 | 说明 |
|---|---|---|
| 归档写入异步化 + 重入保护 | `CbctMeasureFragment.kt` | 几百 KB JSON 不再卡主线程；重复点击不叠加写 |
| 删除结果二次确认 | 同上 | 弹窗说明不可撤销、可用「恢复归档」找回 |
| 隔离显示日志 | 同上 | 开/关/MPR 忽略三分支分别记录，便于回归定位 |
| VTK 触摸日志降噪 | `CbctVtkView.kt` | 删除每事件一行的 `VTK_TOUCH` Log.e，拦截行为不变 |
| AI 模型复用日志修正 | `AiEngine.kt` | 复用长度改用 `target.length()`，不再出现“0 字节模型”的假象 |
| 闭合路径仅限面积工具 | `MeasureToolController.kt` | 非面积状态给出对应提示，避免神经管描记凭空多出面积测量 |
| 归档去易变字段 | `MeasurementStore.kt` | 测量档去 `elapsedMs`、计划档去 process state，减少无意义 checksum 漂移 |
| 报告空括号 | `ReportExporter.kt` | 空字段不再输出 `（）` |
| 导出提示真实化 | `CbctMeasureFragment.kt` | 以 `summary.allOk()` 决定“导出完成/未全部成功” |

### 4.3 主机侧测试增量

- 组 16：O-01 窄化 8 条断言。
- D-15：3 条（先有 1 条风险行 → 清除自动管道 → 风险行归零）。
- D-16：1 条（覆盖保留 `createdAt`）。
- 全量结果：`PASSED 790 / FAILED 0 / FINDINGS 4`；PRD 精度汇总（AC-01 8.882e-16 mm、AC-02 7.105e-15°、AC-03 0.1244%、M-06 5.037e-02 mm、AC-04 0 HU）全部达标。

---

## 5. 未修复 / 需决策 / 建议排期

### 5.1 D-13：SR 对“待重算”记录的口径（需产品/临床决策）

D-18 已把“推理缺失导致清零”的整批保存拦住，但“记录被显式标记为待重算时，SR 应该怎么写”仍是临床口径问题：

- 写成 `NUM 0.0`：会被下游当成真实测量值，风险最高（第 10 轮已取证）。
- 写测量值 + 状态标志：需要 SR 模板扩展与下游约定。
- 跳过该记录：报告数值变少，但不会误读。

在用户给定口径前不擅自实现；建议优先选“跳过 + 报告注明 N 条未纳入”。

### 5.2 历史悬空风险行（基线里 108 条）

D-15 只对“本次清除动作涉及的自动管道”生效；基线中引用已不存在的 178~183 的 108 条风险行仍会随归档恢复。建议二选一：

1. 装载时做一致性裁剪：`nervePathId` 不在当前 `nerves` 集合中的风险行直接丢弃，并记日志（推荐，行为可解释）。
2. 保留数据但在 UI 标灰并计数，由用户点「清除风险条目」显式清理。

不要在未告知用户的情况下批量删除历史归档内容。

### 5.3 12 个历史 ROI 颜色饱和（2147483647）

O-01 只能保证“新写入正确”；已饱和的 12 个 ROI 无法还原原始颜色。建议装载时检测 `color == Int.MAX_VALUE`，一次性分配调色板颜色并标记 `colorRepaired=true`，或仅在 UI 提示“颜色异常”。

### 5.4 其他

- **P-07 多期对比 MVP**：仍未排期。
- **决策 2/3**：权重外置通道缺少版本/sha256 校验与许可台账；PC-05 内存预算未落表。
- **AI-03 / P-10 的本轮复跑**：本轮完整复跑的是 AI-01 + 逐牙测量 + 导出；AI-03 与 P-10 沿用同构建早段结果，建议下一轮补一次连续取证。
- **UI 叠加层标签密度**：测量结果多时标签互相遮挡，建议按深度/选中态做优先级抽稀（纯显示，不改数值，需另做 A/B）。
- **Kotlin 侧测试空白**：`MeasurementStore` 的 D-18 判据目前只有设备端验收，建议补 JVM 单测（构造新旧 payload 即可，不需要真机）。

---

## 6. 取证边界与证据缺口（不粉饰）

### 6.1 本轮做过的非只读操作

- 10:59 一次 `am force-stop` + 冷启动，用于让进程重新从磁盘装载基线；未 `pm clear`、未卸载、未删除数据。
- 回灌基线文件两次，均 md5 复核；第一次因路径拼错产生了 3 个多余文件（`files/<dir>/measure.json` 等），已在同一次操作中删除，目录列表已复核无残留。

### 6.2 证据强度说明

| 项 | 强度 | 说明 |
|---|---|---|
| D-18 / D-17 / D-15 / D-21 / O-01 / O-02 | **真机** | 有日志或导出文件佐证 |
| D-16 | **主机** | 790 条断言通过；真机未构造覆盖场景 |
| D-19 | **代码级** | 未在本轮重跑 AI-03 推荐 |
| Query / Worklist / Retrieve | **界面可达** | 未连接外部 PACS（无授权、也无外部写入需求） |
| 早段 AI-02 / P-10 | **日志未落盘** | 当时 logcat 被多次清理；数值与截图仍可复核，但无独立日志文件 |
| CT Preprocess 显示 A/B | **未做逐像素** | 本轮只修正输入字节序默认值，rawpixeldeal 算法零改动，故未触发 A/B 条款；如需“修复前后同参数逐像素对照”，需另跑一轮 |

---

## 7. 建议的下一步（按风险/收益排序）

1. **定 D-13 口径**并一次性把 PDF/SR/UI 三处对齐（高风险误读）。
2. **装载时清理/标记历史悬空风险行与饱和颜色**（数据可解释性；需先确认是否允许改写历史归档）。
3. **给 D-18 增加“保留旧读数”选项**：当前策略是整档拒绝写入，安全但会挡住“只想保存新测量”的医生；可提供“仅保存非 AI 行 / 跳过退化 AI 行”的显式选择。
4. **补 Kotlin 单测**（D-18 判据、颜色窄化、报告行过滤），把设备验收前移到 CI。
5. **AI 权重外置通道加固**（sha256 + 版本 + 许可台账 + PC-05 预算）。
6. **下一轮真机**：连续复跑 AI-03 + P-10 + D-19 状态刷新，补齐本轮缺口。
7. **P-07 多期对比 MVP** 与 UI 标签抽稀排期。

---

## 8. 本轮改动文件清单（工作区，未提交）

```
app/src/main/java/com/example/dcmtkdemo/fragment/CTPreprocessFragment.kt
app/src/main/java/com/example/dcmtkdemo/fragment/CbctAiFragment.kt
app/src/main/java/com/example/dcmtkdemo/fragment/CbctMeasureFragment.kt
cbctdeal/src/main/java/com/wangyao/cbctdeal/render/CbctVtkView.kt
cbctmeasure/src/host/test_main.cpp
cbctmeasure/src/main/cpp/ai/OrtEngine.cpp
cbctmeasure/src/main/cpp/core/Json.cpp
cbctmeasure/src/main/cpp/core/MeasurementManager.cpp
cbctmeasure/src/main/java/com/wangyao/cbctmeasure/ai/AiEngine.kt
cbctmeasure/src/main/java/com/wangyao/cbctmeasure/model/MeasureModels.kt
cbctmeasure/src/main/java/com/wangyao/cbctmeasure/report/ReportExporter.kt
cbctmeasure/src/main/java/com/wangyao/cbctmeasure/report/ReportGenerator.kt
cbctmeasure/src/main/java/com/wangyao/cbctmeasure/store/MeasurementStore.kt
cbctmeasure/src/main/java/com/wangyao/cbctmeasure/view/MeasureToolController.kt
```

（另有 `cbctmeasure/doc/ROUND10_FULL_DEVICE_REGRESSION_REPORT.md` 处于已暂存未提交状态；本报告未自动提交任何改动。）
