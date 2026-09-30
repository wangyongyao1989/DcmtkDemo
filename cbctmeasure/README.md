# CbctMeasure Module — CBCT 三维测量与手术规划

本模块是 [`:cbctdeal`](../cbctdeal/README.md) 的**上层扩展**：复用同一份 `CbctVolume` 体数据指针与同一个 VTK 渲染窗口，在其上实现 PRD（[`cbctmeasure_prd.pdf`](../cbctmeasure_prd.pdf)）Phase 1 的全套临床测量、ROI 分割、种植体规划、标注与报告导出能力。

设计原则与项目其它 Native 模块一致，**三层解耦**：

```
Kotlin（UI/编排/持久化/报告排版）  →  JNI 桥接（只做类型转换）  →  纯 C++ core（业务与几何计算）
```

core 层不 include 任何 Android/VTK/DCMTK 头文件，因此可以在主机侧直接编译跑单测（见 §5），这是本模块精度可证的前提。

---

## 0. 文档索引

| 文档 | 面向 | 内容 |
|---|---|---|
| 本文件 `README.md` | 开发/维护 | 需求覆盖矩阵、三层结构与文件职责、构建与调试、JNI API、实现约定与踩坑、性能实测 |
| [`USER.md`](USER.md) | 使用/演示 | 每个工具的手势步骤、面板读数含义、归档与导出、常见问题 |
| [`doc/TEST_REPORT.md`](doc/TEST_REPORT.md) | 验收 | 真机回归逐条结果（AC-01~AC-10 / PC-01~PC-05）、缺陷与修复、证据（日志/产物/截图） |

---

## 1. 需求覆盖矩阵（PRD §5，Phase 1）

实现位置以「core → JNI → Kotlin」三段标注；验证手段见 §5 与 [`doc/TEST_REPORT.md`](doc/TEST_REPORT.md)。

### 1.1 三维测量（M-01 ~ M-08）

| 需求 | 名称 | core | JNI / Kotlin | 手势（状态机） |
|---|---|---|---|---|
| M-01 | 两点距离 | `MeasureMath::distance` | `MeasureJni.addMeasure(DISTANCE)` | 测距 → 点 2 下 |
| M-02 | 三点角度 | `MeasureMath::angleAt`（顶点可切换） | 同上（`ANGLE`） | 测角 → 点 A/B(顶点)/C |
| M-03 | 点到线距离 | `MeasureMath::pointToSegment` + 垂足钳制 | 同上（`POINT_TO_LINE`） | 测距子类型「点到线」→ 点 3 下 |
| M-04 | ROI 体积 | `RoiExtractor::analyze`（HU 阈值 + 空间盒） | `RoiJni.statRoi` | 体积 → **按住拖动**画盒 |
| M-05 | ROI 面积 | `MeasureMath::polygonArea`（三维真实面积，非投影） | `MeasureJni.addMeasure(AREA)` | 面积 → 逐笔点 ≥3 点，点起点闭合 |
| M-06 | 弧线长度 | `MeasureMath::arcLength`（弦和 + 中值滤波采样） | 同上（`ARC_LENGTH`） | 测距子类型「弧线长度」→ 按住拖动描记 |
| M-07 | HU 值采样 | `VolumeRef::huAt`（体素中心原始 HU，**不插值**） | 同上（`HU_SAMPLE`） | 测距子类型「HU 采样」→ 点击 |
| M-08 | 骨密度评估 | `RoiExtractor` 的 mean/std/min/max | 同上（`BONE_DENSITY`） | 测距子类型「骨密度」→ 点击 ROI 内 |

PRD §5.3 的四种正畸量（S-07 牙弓弧线 / S-08 排列角度 / S-09 中线偏移 / S-10 覆合覆盖）走同一套测量通道，类型在 `MeasureType` 中独立存在，因此也能进结果列表、归档与报告。

### 1.2 ROI 分割与裁剪（R-01 ~ R-05）

| 需求 | 名称 | 模型 | 实现要点 |
|---|---|---|---|
| R-01 | HU 阈值范围 | `[huMin, huMax]` | 体素级判定，`core/RoiExtractor.cpp`；面板「建 HU 阈值 ROI」 |
| R-02 | 空间裁剪盒 | box min/max | **角点权重快速路径**：把盒边界切割的体素按 8 角点落入比例给权重，与逐角点定义逐位一致（见 §5 的快速路径回归用例）；配合 `spatialBoundsAt` 只扫包围盒 |
| R-03 | 平面裁剪 | origin + normal + **勾画多边形** | 单层门（`slabHalfThickness`）+ 面内多边形角点加权；无多边形时退化为半空间切割（旧行为） |
| R-04 | 球面 ROI | center + radius | 角点权重球 |
| R-05 | 组合 ROI | A opB B (opAC) C | 递归求值 + **子 ROI 索引区间相交**裁剪；子 ROI 缺失时判失败并在 `error` 给出文案，不静默返回体积 |

### 1.3 手术规划（S-01 ~ S-10）

| 需求 | 名称 | 判据（PRD） | 实现 |
|---|---|---|---|
| S-01 | 种植体定位 | 位置 + 俯仰/偏转 + 深度 + 直径 | `ImplantPlanner`，入口点由「种植体」工具点击牙槽嵴顶拾取 |
| S-02 | 可视化与安全色 | 红=不安全 / 黄=临界 / 绿=安全 | 叠加层按 `level` 着色（`MeasureOverlayView`），阈值来自 `SurgeryPlanJni.safetyThresholds` |
| S-03 | 骨高度 | <10mm 警告 | 沿种植体轴线采样皮质骨 |
| S-04 | 骨宽度 | <6mm 警告 | 入口截面颊舌向宽度 |
| S-05 | 神经管距离 | <2mm 红色警告 | 轴线到神经管折线最短距离 − 管道半径 |
| S-06 | 多种植体方案 | 最小间距 ≥3mm | 表面间距 = 轴线间距 − 两半径；任一增删都触发 `recomputePlan()` 全量回填 |
| S-07~S-10 | 正畸评估 | 牙弓/排列/中线/覆合覆盖 | 测量类型 + `MeasureMath` 中的无向轴折叠与水平分量分解 |

### 1.4 标注（A-01 ~ A-07）与持久化、报告

| 需求 | 实现 |
|---|---|
| A-01 文字 / A-02 线段 / A-03 箭头 / A-04 自由曲线 / A-05 环形 | `core/AnnotationStore.cpp` + 「标注」工具条子类型；自由曲线与环形走拖动采样，环形为「点圆心 → 拖到边缘」 |
| A-06 MPR 截面标注 | 绑当前层平面坐标，随层位切换重投影 |
| A-07 截图圈注 | 记录**像素坐标**，与 `captureEvidence()` 抓到的帧同一坐标系 |
| JSON 持久化（PRD §5.6 / §8.4） | `store/MeasurementStore.kt`，三份文件 `{StudyUID}_{SeriesUID}_{measure,anno,plan}.json`，原子写 + `.bak` 备份（PRD 风险项 R-10） |
| PDF 报告（PRD §5.5.2，AC-08） | `report/ReportGenerator.kt`（章节排版）+ `report/PdfPager.kt`（A4 分页/表格/换行/页脚） |
| DICOM SR（AC-09） | `core/SrReport.cpp` + `dcmsr`，输出 Comprehensive SR（`1.2.840.10008.5.1.4.1.1.88.33`）；**导出后现场 `ReportJni.verifySr` 读回自校验**，不依赖主机 `dsr2xml` |
| 手势状态机（PRD §8.5，9 态） | `view/MeasureToolController.kt`：`VIEW / MEASURE_DISTANCE / MEASURE_ANGLE / MEASURE_VOLUME / MEASURE_AREA / ROI_EDIT / IMPLANT_PLACE / NERVE_TRACE / ANNOTATE` |

Phase 2（AI 辅助分析，ONNX/3D U-Net，PRD §5.6）**不在本模块范围内**，未实现。

---

## 2. 模块结构

```
cbctmeasure/
├── build.gradle.kts                        # library 模块 + externalNativeBuild（仅 arm64-v8a）
├── src/main/cpp/
│   ├── CMakeLists.txt                      # 只链 DCMTK 的 dcmsr 等所需库；Debug 构型 -O2（见 §6.1）
│   ├── measure-native-lib.cpp              # JNI 注册与类型转换（Measure/Roi/SurgeryPlan/Annotation/Report 五组）
│   ├── MeasureJniHelper.cpp                # SafeNewStringUTF / JniString，与 :cbctdeal 同风格
│   ├── include/                            # MeasureTypes / MeasureMath / VolumeRef / MeasurePicker /
│   │                                       # MeasurementManager / RoiExtractor / ImplantPlanner /
│   │                                       # AnnotationStore / SrReport / Json
│   └── core/                               # 上述头的纯 C++ 实现（不依赖 Android/VTK/DCMTK，SrReport 除外）
├── src/main/java/com/wangyao/cbctmeasure/
│   ├── jni/                                # MeasureJni / RoiJni / SurgeryPlanJni / AnnotationJni / ReportJni / MeasureNative
│   ├── model/                              # 数据类与枚举（MeasureType、RoiType、AnnotationType、SafetyLevel…）
│   ├── view/                               # CbctMeasureView（容器）/ MeasureOverlayView（2D 叠加层）/ MeasureToolController（9 态状态机）
│   ├── store/                              # StorePaths + MeasurementStore（JSON 归档、截图落盘）
│   └── report/                             # ReportGenerator / PdfPager / ReportExporter
├── src/host/                               # 主机侧单测：build_and_run.sh + stub/android/log.h + test_main.cpp
└── doc/                                    # TEST_REPORT.md（真机回归）
```

宿主接入在 `:app`：`fragment/CbctMeasureFragment.kt` + `res/layout/fragment_cbct_measure.xml`（沿用既有 Material 卡片分节 + RadioButton 工具条风格），抽屉菜单项 `nav_cbct_measure`。

### 2.1 与 `:cbctdeal` 的关系

`:cbctmeasure` 依赖 `:cbctdeal` 的三个**只读内省**能力（为叠加层新增，不改变渲染行为）：

| API | 用途 |
|---|---|
| `CbctVtkJni.projectPoints(handle, world[])` | 世界坐标 → 屏幕坐标，与体绘制共用同一相机矩阵，保证叠加像素级对齐 |
| `CbctVtkJni.renderSnapshot(handle)` | 取 surface 尺寸/相机状态，供投影换算与拾取射线构造 |
| `CbctVtkJni.captureFrame(handle)` | 抓当前渲染帧（GPU 读回），供 `captureEvidence()` 与 A-07 截图圈注 |

体数据指针由 `:cbctdeal` 的 `CbctVolumeHandle` 传入，`MeasureJni.bindVolume` 在 core 侧建 `VolumeRef`（**零拷贝引用**，不复制 265MB 体数据）。

---

## 3. JNI API 速查

| Kotlin 类 | 方法 | 说明 |
|---|---|---|
| `MeasureJni` | `createSession / destroySession / bindVolume` | 会话生命周期；句柄是 `Long` 指针 |
| | `pick(screenX, screenY, mode)` | 射线-体素求交：`SURFACE`（第一个骨面）/ `MAX_HU` 等模式 |
| | `addMeasure / recalcMeasure / removeMeasure / renameMeasure / setMeasureNote / setMeasureVisible` | 测量条目 CRUD |
| | `dumpRecords / records()` / `dumpPlan` / `restoreRecords` / `restorePlan` | JSON 进出（归档与叠加层数据源） |
| | `overlayPoints / overlayJson / overlayVersion` | 叠加层几何：Native 一次返回所有图元，Kotlin 只投影不重算 |
| | `summaryText / tissueName / snapToVoxel` | 会话摘要、HU→组织名、吸附体素中心 |
| `RoiJni` | `addRoi / updateRoi / removeRoi / setRoiVisible / dumpRois / statRoi / roiPatch / dominantHuRange` | ROI CRUD 与量化统计（体积/面积/HU 分布/权重体素/遍历量/耗时） |
| `SurgeryPlanJni` | `addImplant / updateImplant / removeImplant / addNervePath / appendNervePoint / removeNervePoint / removeNervePath / setNerveVisible / setImplantVisible / recomputePlan / clearPlan / safetyThresholds` | 种植体与神经管方案管理 |
| `AnnotationJni` | `addAnnotation / updateAnnotation / removeAnnotation / setAnnotationText / setAnnotationColor / setAnnotationVisible / dumpAnnotations / loadAnnotations / clearAnnotations` | 标注 CRUD 与 JSON 序列化 |
| `ReportJni` | `initSrDictionary / exportSr / verifySr / newSopUid` + `request(...)` 构造器 | DCMTK SR 导出与读回校验 |

**字典注入**：本模块自带一份独立静态 DCMTK 副本，字典不与 `:cbctdeal` 的 `libcbct_native.so` 共享，所以 `ReportExporter.ensureDictionary()` 会按本 `.so` 再注入一次（复用 `:cbctdeal` 释放到 `filesDir/dicom.dic` 的文件）。导出 SR 前不注入会得到空 SOP —— 这是最容易漏的一步。

---

## 4. 数据与文件布局

| 内容 | 位置 | 命名 |
|---|---|---|
| 测量归档 | `filesDir/measurements/` | `{StudyUID}_{SeriesUID}_measure.json`（+ `.bak`） |
| 标注归档 | `filesDir/annotations/` | `…_anno.json`（+ `.bak`） |
| 规划方案 | `filesDir/plans/` | `…_plan.json`（+ `.bak`） |
| 证据截图 | `filesDir/screenshots/` | `{StudyUID}_{ts}.png` |
| PDF 报告 | 外部应用专属目录 `reports/` | `{StudyUID}_{ts}.pdf` |
| DICOM SR | 外部应用专属目录 `sr/` | `{StudyUID}_{ts}.dcm` |

`getExternalFilesDir()` 在个别机型返回 null，`StorePaths.external()` 会退回内部存储并打日志，避免「点导出没反应」的静默失败。

---

## 5. 构建、主机单测与真机安装

```bash
# 1) 构建宿主 APK（依赖已在本机缓存时加 --offline 更快）
./gradlew :app:assembleDebug --offline

# 2) 真机安装（AGM3-W09HN，arm64-v8a）
ADB=$HOME/Library/Android/sdk/platform-tools/adb
$ADB install -r app/build/outputs/apk/debug/app-debug.apk
$ADB shell am start -n com.example.dcmtkdemo/.activity.MainActivity

# 3) 主机侧纯 C++ 单测（不需要设备，不需要 VTK/DCMTK）
cd cbctmeasure/src/host && ./build_and_run.sh
```

主机单测覆盖：M-01~M-08 解析值对照、R-01~R-05 体积/面积/权重逐位一致、S-03~S-06 与安全判级、S-07~S-10 正畸量、JSON 往返（值与单位不变）、`detailJson` 字段完备性、异常路径（点数不足 / ROI 不存在 / 组合 ROI 环引用）。最近一次运行：

```
RESULT: PASSED 364 / FAILED 0 / FINDINGS 4
AC-01 最坏绝对误差 8.88e-16 mm · AC-02 7.11e-15 度 · AC-03 最坏相对误差 0.1244% · AC-04 0.000 HU
```

`FINDINGS` 是「实现与 PRD 字面表述不一致但已按更合理定义实现并写入文档」的提示项，不是失败。

界面回归操作手册见仓库技能 `.qoder/skills/android-adb-ui-regression-loop/`（坐标必须取自当次 `uiautomator dump`；判定看 logcat TAG，不靠肉眼截图）。

### 5.1 日志 TAG 对照

| TAG | 层 | 看什么 |
|---|---|---|
| `CbctMeasureCtrl` | Kotlin view | 状态机切换、拾取失败原因 |
| `CbctMeasure` | JNI | 方法注册、字符串转换 |
| `CbctMeasureCore` | C++ core | `analyze` 的 `scanned/coverage/vol/mean/in N ms`、种植体 `eval` 行 |
| `CbctMeasureOverlay` | Kotlin view | PC-02：`overlay fps=… avgDraw=…ms prims=…` |
| `CbctMeasureView` | Kotlin view | PC-03：`captureEvidence WxH frame=… overlay=… total=…` |
| `CBCT_MEASURE_STORE` | Kotlin store | `saveAll/loadAll ok=x/3` |
| `CBCT_MEASURE_REPORT` | Kotlin report | `report ok pages=…`、`exportAll …` |

---

## 6. 关键实现约定（改动前请先读）

### 6.1 Debug 构型为什么补 `-O2`

AGP 传的是 `CMAKE_BUILD_TYPE=Debug`，NDK 工具链把 `CMAKE_CXX_FLAGS_DEBUG` 置空串 —— 等于**不带任何 `-O`**（`-O0`）。R-02 体积盒即使已有角点权重快速路径，145 万体素在 `-O0` 下 `analyze` 实测 73ms，达不到 PC-01 的 ≤50ms。

因此在 `CMakeLists.txt` 里用 **target 级** `target_compile_options(... $<$<CONFIG:Debug>:-O2>)`（target 参数排在 `CMAKE_CXX_FLAGS_DEBUG` 之后才生效，`cppFlags("-O2")` 会被覆盖），并额外加 `-ffp-contract=off` 关掉乘加收缩为 FMA —— 否则 `-O2` 下真机与主机 `-O0` 的均值/标准差不是逐位可比，AC-02 的浮点断言会莫名漂移。

**作用域仅 `:cbctmeasure`**：`:cbctdeal` / `:rawpixeldeal` / `:dcmtk` 的编译参数一字未动，渲染与像素算子行为零变化（真机对照：解析耗时 `1899/1908/1920/2064 ms` 同分布）。改后 R-02 大盒 21~23ms，R-03 单层截面 12ms。

### 6.2 PDF 分页器：canvas 绝对不能跨页缓存

`PdfPager` 里 `val cc = canvas` 之后再调 `ensure()/newPage()`，一旦换页就会对**已 `finishPage()` 的页**继续 `drawText` —— 其 native `SkCanvas` 已销毁，Java 侧持有野指针，直接 SIGSEGV（崩在 `libhwui android::Canvas::drawText+300`，fault addr 0x8），且崩在协程线程上，Kotlin `try/catch` 拦不住。数据少时永远不换页，所以小报告导出一切正常；条目一多、某段文本正好压到页底就必崩。

约定：所有绘制统一走 `surface(need)`（先保证空间、再返回当前页 canvas），**取到就立刻落笔，不跨下一次 `ensure` 持有**。真机证据与判定见 `doc/TEST_REPORT.md` §缺陷 3。

### 6.3 叠加层只在事件流里跟着重投影

`CbctMeasureView.dispatchTouchEvent` 在 `cameraActive` 时每帧调 `controller.reprojectOnly()`，**不在 `onDraw` 里拉 Native**：VTK 在自己的渲染线程出帧，重投影与相机最多差一帧，又不会把 `projectPoints` 变成每帧必调的开销。空闲时不 invalidate，所以 `overlay fps` 反映的是「用户实际操作时的刷新率」。

### 6.4 拖动类工具必须按下即播种

`seedDragPoint()`：`appendDragPoint()` 以 `pending.last()` 为间距基准，如果按下时只 `clear()` 不写入首个采样点，后续每个 MOVE 都因 `lastOrNull() == null` 直接 return —— 真机上表现为「拖半天一个点都没有」。**体积工具同理是拖动**，且 ACTION_DOWN 的拾取必须命中骨面，否则 `commitVolumeBox` 静默放弃。

### 6.5 拾取的深度锁层与中值滤波

`SURFACE` 取射线上第一个骨面，而真实数据的骨面本身断续（沿脊柱拖动时相邻采样的相机距离在 1302/1430/1516/1869mm 间横跳）。因此弧线/自由曲线走「最近几个采样深度中位数 ± 窗口」的锁层 + 中值滤波，连续漏检达到阈值才放弃，避免跳层造成的毛刺。

### 6.6 提交文案不能被通用引导覆盖

`onUp()` 里记录 `hintBefore`：提交路径给出了读数或失败原因（如「未拾取到点」）时保留它，否则会被下一次 `updateHint()` 的「已取 0/2 点」盖掉，真机上表现为「点了没任何反应」。

---

## 7. 性能与验收实测摘要（真机 Honor AGM3-W09HN）

| 指标（PRD） | 门槛 | 实测 | 结论 |
|---|---|---|---|
| PC-01 测量计算 | ≤50ms | 距离/角度/HU 0~1ms；面积/截面量化 12ms；体积盒（145 万体素）21~23ms | 达标（交互态）；批量归档重算会出现 ~70ms，属装载耗时 |
| PC-02 ROI 渲染帧率 | ≥30FPS | `avgDraw 0.57~2.04ms @ prims=89`；adb 注入下 `fps=2~12` | **未充分验证**：合成 swipe 只产生 ~5Hz 的 MOVE，事件密度是瓶颈；见 §7.1 |
| PC-03 截图生成 | ≤500ms | 四次采样 `total=319 / 795 / 825 / 934 ms`，其中 `frame=313 / 779 / 807 / 903 ms` | **未达标**（抖动大）：`frame` 段是 `:cbctdeal` GPU 读回；本模块叠加层只占 6~31ms |
| PC-04 模块内存增量 | ≤100MB | Pss 759,169 → 755,562（恢复归档）→ 748,425 KB（导出后） | 达标：测量层增量为 0（<±10MB 噪声）；700MB 量级基线是体数据 + GL，与测量层无关 |
| 归档存储 | ≤1MB/Study | measure 6,021 B + anno 375 B + plan 1,051 B ≈ **7.4KB**/Study | 达标 |

### 7.1 PC-02 的测量手段局限（不要误读）

`overlay fps` 统计的是**叠加层自己每秒真正 onDraw 了几次**。手指拖动时 MOVE 事件是 60~120Hz，而 `adb shell input swipe` 即便把 duration 拉到 6~9s，也只产生约 5Hz 的采样点，于是实测帧率只有 2~12fps。**瓶颈在注入事件的密度，不在绘制成本**（单帧 0.57~2.04ms，换算 30FPS 预算富余 16 倍以上）。本轮没有可信手段在真机上复现连续高密度触控流，因此 PC-02 记录为「组件成本达标、整体验证不足」，不宣称通过。

### 7.2 已知限制

1. **截图耗时受 GPU 读回支配**（PC-03）：要达标得改 `:cbctdeal` 的抓帧路径（离屏 FBO 同步读 or `PixelPack` 异步），属于渲染模块范畴，本模块不改渲染行为。
2. **AI 辅助（Phase 2）未实现**：PRD §5.6 的 ONNX/3D U-Net 与 PC-05 不在本模块范围。
3. **叠加层是 2D Canvas**：图元按世界坐标重投影，不随深度遮挡做剔除（种植体圆柱用轮廓 + 填充近似），与 PRD S-02 提到的「螺纹纹理 Actor」相比是简化实现。
4. **双指手势无法用 adb 真实注入**：AC-10（描记过程中双指缩放/平移）只验证到状态机与代码路径，未做真机双指取证。
5. **画面滚出可见区时证据图会全黑**：`SurfaceView` 不可见后渲染线程不出帧，`captureFrame()` 读到空 surface，而 `captureEvidence()` 仍返回一张合法（全黑）位图、日志照打 `evidence=true`。真机复现：可见 282,849 B / 有效像素 7.13%，不可见 7,760 B / 0%。当前只在 `USER.md` 给了操作规避（导出前把画面滚回来），**代码侧的全黑判空尚未实现**——要补的话改 `CbctMeasureView.captureEvidence()`：对合成位图做网格采样，全黑则返回 null 并提示「画面不可见，未取到证据图」。详见 `doc/TEST_REPORT.md` §4.4。

---

## 8. 提交与验证纪律

- 改动 core C++ 之后**必须**先跑 `src/host/build_and_run.sh`，再谈真机；
- 真机判定以 logcat 行 + 拉回主机的产物（PDF / SR / JSON）为准，**没有真机证据不宣称验证通过**；
- 涉及显示效果的本模块改动只影响叠加层，不动 `:rawpixeldeal` / `:cbctdeal` 的像素算子与调窗算法（该两模块有强制回滚基线约定）。
