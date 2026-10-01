# CbctMeasure Module — CBCT 三维测量与手术规划

本模块是 [`:cbctdeal`](../cbctdeal/README.md) 的**上层扩展**：复用同一份 `CbctVolume` 体数据指针与同一个 VTK 渲染窗口，在其上实现 PRD（[`cbctmeasure_prd.pdf`](../cbctmeasure_prd.pdf)）Phase 1 的全套临床测量、ROI 分割、种植体规划、标注与报告导出能力，并实现 Phase 2 的 **AI 辅助分析**（§5.6：AI-01 牙齿自动分割经 **ONNX Runtime Android** 端上推理、AI-03 种植位点规则推荐；AI-02/AI-04 未交付，原因见 §1.5）。

设计原则与项目其它 Native 模块一致，**三层解耦**：

```
Kotlin（UI/编排/持久化/报告排版）  →  JNI 桥接（只做类型转换）  →  纯 C++ core（业务与几何计算）
```

core 层不 include 任何 Android/VTK/DCMTK 头文件，因此可以在主机侧直接编译跑单测（见 §5），这是本模块精度可证的前提。**AI 层同样守这条线**：`core/AiCore.cpp` / `AiEngine.cpp` / `AiPlanner.cpp` 是纯 C++（预处理、阈值、连通域、统计、推荐），`ai/OrtEngine.cpp` 是全工程**唯一** include ONNX Runtime 头并 `dlopen` 运行时 `libonnxruntime.so` 的翻译单元，所以主机单测可以用假 backend 顶替推理、把确定性那条路径全量跑通。

---

## 0. 文档索引

| 文档 | 面向 | 内容 |
|---|---|---|
| 本文件 `README.md` | 开发/维护 | 需求覆盖矩阵、三层结构与文件职责、构建与调试、JNI API、实现约定与踩坑、性能实测 |
| [`USER.md`](USER.md) | 使用/演示 | 每个工具的手势步骤、面板读数含义、归档与导出、常见问题 |
| [`doc/TEST_REPORT.md`](doc/TEST_REPORT.md) | 验收 | 一期真机回归逐条结果（AC-01~AC-10 / PC-01~PC-05）、缺陷与修复、证据（日志/产物/截图） |
| [`doc/AI_ONNX_TEST_REPORT.md`](doc/AI_ONNX_TEST_REPORT.md) | 验收（Phase 2） | **ONNX Runtime Android 模型推理**测试报告：运行时集成偏差记档（Prefab→dlopen）、牙科 CBCT 数据来源与划分纪律、AI-01 精度实测（F1 0.5828，**未达 PRD 0.85**）、AC-08 主机/真机奇偶校验 PASS、PC-05 延迟与体积取证、缺陷与修复 |
| [`doc/UI_SPLIT_LAYOUT_REGRESSION_REPORT.md`](doc/UI_SPLIT_LAYOUT_REGRESSION_REPORT.md) | 验收（UI 重构轮） | AI 面板剥离 + 结果折叠 + 按钮布局这一轮的两轮真机记录：round5 全量回归、D-01~D-07 缺陷与代码修复、round6 逐条回测（R1~R16）、性能与内存取证、主机侧 `verify_all` 56/56、AC-08 复跑（两次独立导出逐字节相同） |
| [`src/host/ai/parity_ref/METRICS.md`](src/host/ai/parity_ref/METRICS.md) | 复现 | 冻结模型 `teeth_cnn v2` 的训练/标定契约、设备轴序依据、parity 夹具清单与判定规则（机器可读版在 `device_parity.json`） |
| [`src/main/assets/dental_cbct/index.json`](src/main/assets/dental_cbct/index.json) | 数据来源 | 内置牙科 CBCT 病例的来源、体数据契约、train/val/test 角色、为什么有的病例不出包 |

---

## 1. 需求覆盖矩阵（PRD §5，Phase 1 + Phase 2 的 AI 层）

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

### 1.2 ROI 分割与裁剪（R-01 ~ R-06）

| 需求 | 名称 | 模型 | 实现要点 |
|---|---|---|---|
| R-01 | HU 阈值范围 | `[huMin, huMax]` | 体素级判定，`core/RoiExtractor.cpp`；面板「建 HU 阈值 ROI」 |
| R-02 | 空间裁剪盒 | box min/max | **角点权重快速路径**：把盒边界切割的体素按 8 角点落入比例给权重，与逐角点定义逐位一致（见 §5 的快速路径回归用例）；配合 `spatialBoundsAt` 只扫包围盒 |
| R-03 | 平面裁剪 | origin + normal + **勾画多边形** | 单层门（`slabHalfThickness`）+ 面内多边形角点加权；无多边形时退化为半空间切割（旧行为） |
| R-04 | 球面 ROI | center + radius | 角点权重球 |
| R-05 | 组合 ROI | A opB B (opAC) C | 递归求值 + **子 ROI 索引区间相交**裁剪；子 ROI 缺失时判失败并在 `error` 给出文案，不静默返回体积 |
| **R-06** | **AI 掩膜 ROI**（Phase 2 新增） | `aiLabel`（连通域实例号） | 几何 = 推理结果里该实例的体素集合，由 `core/AiCore.cpp` 的 26 邻接洪水填充给出；量化走与 R-01~R-05 同一条 `RoiExtractor::analyze`，因此 M-04/M-08 与手工 ROI **同一口径**。权重是 **0/1，不做角点比例加权**：app 体素（0.6 mm）的中心落在该实例的哪个掩膜单元（1.2 mm）里就归属它（`MeasurementManager::containsMm` 用 floor 映射，判据取 `inst` 而非 `label`，以免把已剔除的散点算进来）——掩膜单元比体素粗一个倍频，亚体素超采样没有意义。候选集仍按实例包围盒裁剪（`boundsMm` → `spatialBoundsAt`）。体积与 `AiInstance.volumeMm3` 数到的是同一批体素，只是计数单位不同（0.216 vs 1.728 mm³，1 个掩膜单元 = 8 个 app 体素），边界归属的取整会带来 ~0.5% 差（真机实测 8.471 cm³ vs 主机普查 8.514 cm³）。掩膜网格本身**不出档**（见 §6.9） |

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

### 1.5 AI 辅助分析（AI-01 / AI-03，PRD §5.6 Phase 2）

| 需求 | 名称 | core | JNI / Kotlin | 界面入口 |
|---|---|---|---|---|
| AI-01 | 牙齿自动分割（ONNX Runtime Android 推理） | `core/AiCore.cpp`（6 通道特征 → 阈值 → 26 邻接连通域 → 质心/PCA 长轴/包围盒/轮廓/逐实例 HU 与体积）+ `core/AiEngine.cpp`（会话内的推理编排）+ `ai/OrtEngine.cpp`（唯一 dlopen + OrtApi 跳转表） | `AiJni.nativeLoadModel / nativeRunSegment / nativeAiStatus / nativeAiAutoMeasure / nativeAiDumpParity`；`ai/AiEngine.kt` 只做"找 so、释放 assets、切线程、落盘"四件事 | tab「5) AI 辅助分析 (Phase 2)」→ 装载模型 → 牙齿自动分割 |
| AI-01→测量贯通 | 逐牙自动测量 | `MeasurementManager::aiAutoMeasure`：每实例建/复用一条 **R-06 掩膜 ROI**，挂 M-04 体积 + M-08 骨密度，`note = AiConst::AUTO_NOTE` | `nativeAiAutoMeasure(handle, withBoneDensity)` 返回条数 | 「逐牙自动测量」；结果进既有列表/归档/PDF/SR |
| AI-03 | 种植位点推荐（规则 + ML 混合） | `core/AiPlanner.cpp`：ML 出解剖先验（牙弓归属、每牙质心与长轴来自 AI-01 掩膜），规则出判定（净间隙 ≥ `minGapMm`、邻牙长轴加权平均作轴向、嵴顶高度估计、按骨宽/骨高选 Ø/长度、0~100 打分降序） | `AiJni.nativeAiRecommend` → `AiCandidateInfo`；采纳即 `SurgeryPlanJni.addImplant` + `recomputePlan`，**安全判级完全复用 S-02~S-06**，不存在第二套数值 | 「种植位点推荐」→ 候选对话框 → 点一条即落一颗 |
| AC-08 | 主机/真机奇偶校验 | `AiEngine::dumpParity`（64 字节头 + feat/prob/label/inst） | `nativeAiDumpParity` 落 `filesDir/ai_parity/` | 「导出取证数据」→ `adb run-as` 拉回 → `src/host/ai/scripts/check_device_dump.py` |
| 数据 | 内置牙科 CBCT（公开 DentVoxel，192×192×128 @0.6 mm 符号 int16 序列） | — | `ai/DentalDataAssets.kt`（列病例 / 释放到私有目录，带 `.release_ok` 标记防半批） | 「1) Series Source → 选择加载牙科 CBCT (assets/dental_cbct)」 |

**精度结论（不粉饰）**：AI-01 在**未参与训练**的测试例 dentvoxel_0101 上 **F1 = 0.58285**（阈值 0.49），**未达 PRD 的 ≥0.85**；根因是可用标注数据只有 2 例 + 模型只有 3,474 参数（无多尺度分支），因此存在**实例过合并**（最大实例含多颗牙，0101 前两名占全部预测牙齿体素的 78%），这类实例的体积/骨密度/间隙不具备单牙意义。推理链路、契约与取证口径按 PRD 落地并通过（parity `VERDICT: PASS`）。数字、成因与改进路径见 [`doc/AI_ONNX_TEST_REPORT.md`](doc/AI_ONNX_TEST_REPORT.md) §6、§13。

**明确未交付**：AI-02（颌骨/神经管/气道等其它结构分割，缺各自专家标注；S-05 仍靠手工描记）与 AI-04（AI 生成报告文字，属临床诊断输出，无可验证语料，刻意不做）。掩膜网格也不随归档持久化（会让"≤1MB/Study"爆预算），恢复归档后需重跑一次推理，R-06 ROI 会**原位复用**而不产生双份（§6.9）。

---

## 2. 模块结构

```
cbctmeasure/
├── build.gradle.kts                        # library 模块 + externalNativeBuild（仅 arm64-v8a）
│                                           # + implementation(libs.onnxruntime.android)：只取 AAR 的 .so 交付物
├── src/main/cpp/
│   ├── CMakeLists.txt                      # 只链 DCMTK 的 dcmsr 等所需库；Debug 构型 -O2（见 §6.1）；
│   │                                       # ORT 只 include 头不链接（运行时 dlopen，见 §6.7）
│   ├── measure-native-lib.cpp              # JNI 注册与类型转换（Measure/Roi/SurgeryPlan/Annotation/Report 五组）
│   ├── ai-native-lib.cpp                   # AI 组 JNI 桥接（loadModel/runSegment/status/autoMeasure/recommend/dumpParity）
│   ├── MeasureJniHelper.cpp                # SafeNewStringUTF / JniString，与 :cbctdeal 同风格
│   ├── include/                            # MeasureTypes / MeasureMath / VolumeRef / MeasurePicker /
│   │                                       # MeasurementManager / RoiExtractor / ImplantPlanner /
│   │                                       # AnnotationStore / SrReport / Json
│   │                                       # + AiTypes / AiCore / AiEngine / AiPlanner（AI 层头）
│   ├── core/                               # 上述头的纯 C++ 实现（不依赖 Android/VTK/DCMTK/ORT，SrReport 除外）
│   │   ├── AiCore.cpp                      # 特征 6 通道、阈值、26 邻接连通域、质心/PCA/包围盒/轮廓、逐实例统计
│   │   ├── AiEngine.cpp                    # 会话内 AI 编排：抽稀、推理调用（经 backend 接口）、adopt、dumpParity
│   │   └── AiPlanner.cpp                   # AI-03 候选：间隙检测 + 轴向/规格选择 + 打分（安全判级复用 ImplantPlanner）
│   ├── ai/
│   │   └── OrtEngine.{h,cpp}               # 全工程唯一 include ONNX Runtime 头 + 唯一 dlopen libonnxruntime.so 的 TU
│   └── third_party/onnxruntime/include/     # 从 AAR headers/ vendor 的 4 个头（onnxruntime_c_api.h 等）
├── src/main/java/com/wangyao/cbctmeasure/
│   ├── jni/                                # MeasureJni / RoiJni / SurgeryPlanJni / AnnotationJni / ReportJni / AiJni / MeasureNative
│   ├── model/                              # 数据类与枚举（MeasureType、RoiType(含 AI_MASK=5)、AnnotationType、SafetyLevel、AiModels…）
│   ├── ai/                                 # AiEngine（so 候选链 / assets 释放 / 线程编排 / 取证落盘）
│   │   └── DentalDataAssets.kt             # 内置牙科 CBCT 病例列举与释放（.release_ok 标记防半批）
│   ├── view/                               # CbctMeasureView（容器）/ MeasureOverlayView（2D 叠加层）/ MeasureToolController（9 态状态机）
│   ├── store/                              # StorePaths + MeasurementStore（JSON 归档、截图落盘）
│   └── report/                             # ReportGenerator / PdfPager / ReportExporter
├── src/main/assets/
│   ├── dental_cbct/                        # 公开 DentVoxel 转成的 192x192x128@0.6mm int16 DICOM 序列
│   │   ├── index.json                      # 来源 / 体数据契约 / train-val-test 角色 / 为何某例不出包
│   │   ├── dentvoxel_0101/                 # 测试例（未参与训练）—— AC-08 比对基准，128 张 ≈9.5MB
│   │   └── dentvoxel_0021/                 # 训练例 —— 掩膜观感最好，128 张 ≈9.5MB
│   └── models/                             # teeth_cnn.onnx（14,477 B）+ teeth_cnn.json（阈值 0.49 与契约）
├── src/host/                               # 主机侧单测：build_and_run.sh + stub/android/log.h + test_main.cpp
│   └── ai/                                 # AI 流水线与证据（.gitignore 写明「输入入库 / 派生物忽略」）
│       ├── scripts/                        # dump_template / build_app_volumes / prep / variants /
│       │                                   # train2（交付模型）/ train_model（1x1x1 历史实验）/ export2 /
│       │                                   # make_parity[_device] / check_device_dump / parity /
│       │                                   # make_report / probe / verify_all / run_all.sh
│       ├── raw/                            # 公开 DentVoxel NIfTI 输入（4 例 + GT + cases.json，来源见 raw/README.md）
│       ├── model/                          # 重建出的 teeth_cnn.onnx（与随包资产同 sha256）+ 1x1x1 历史备份
│       ├── gt/                             # 专家 GT（.nii.gz，仅主机侧算 Dice，不进 APK；等价 .raw 属派生物）
│       └── parity_ref/                     # 设备链夹具 device_*（随包证据）+ METRICS.md + device_parity.json
└── doc/                                    # TEST_REPORT.md（一期真机回归）、AI_ONNX_TEST_REPORT.md（Phase 2 推理）、
                                            # UI_SPLIT_LAYOUT_REGRESSION_REPORT.md（AI 剥离 + 布局重构轮）
```

宿主接入在 `:app`，两个 Fragment 共用同一个 Native 会话：

| 文件 | 职责 |
|---|---|
| `fragment/CbctMeasureFragment.kt` + `res/layout/fragment_cbct_measure.xml` | 主页：1) 数据源 / 2) 视口 / 3) 三维画面（含 5 按钮快捷行）/ tab「4) 测量工具与 ROI」「5) AI 辅助分析 (Phase 2)」/ 6) 结果 Results（可折叠）/ 7) 归档与报告。实现 `CbctAiHost`，是 `MeasureSession` 与渲染窗口的唯一所有者 |
| `fragment/CbctAiFragment.kt` + `res/layout/fragment_cbct_ai.xml` | AI 面板：装载模型 / 自动分割 / 逐牙测量 / 位点推荐 / 导出取证 / 清除，外加掩膜开关与换病例。经 `childFragmentManager` add 到 `R.id.fl_ai_panel`，切换只改 visibility —— 抽屉切页用 `replace()`，会把会话和渲染窗口一起销毁 |
| `res/values/styles.xml` + `res/{color,color-night}/tab_text.xml` + `res/drawable/bg_tab.xml` | `CompactButton`/`TabButton` 等紧凑样式（默认 Button 的 48dp 最小高度 + 6dp inset 是页面变高的主因）与 tab 夜间对比度 |

抽屉菜单项 `nav_cbct_measure`。`:app` 另加了 `ndk { abiFilters += "arm64-v8a" }`，原因见 §6.8。

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
| `AiJni` | `nativeLoadModel(soPath, modelPath, modelKey, threads)` | `soPath` 是 **`;` 分隔的候选链**（Native 逐条 `dlopen` 并补裸 SONAME 兜底）；返回 JSON 含 `ok/error/runtimeInfo/runtimeSo/inChannels/outChannels/modelDim` |
| | `nativeReleaseModel / nativeAiReady` | 会话销毁与可用性查询 |
| | `nativeRunSegment(threshold, keepParity)` | 一次完整推理 + 后处理；`keepParity=true` 才保留 feat/prob 缓冲供取证导出（否则省 ~20MB）。返回 `AiResultInfo`（实例数、牙齿体素、prep/infer/post/total ms、allocBytes、redDim…） |
| | `nativeAiStatus / nativeAiClear / nativeAiOverlayVisible` | 只查回执不重算；清推理结果（**不动**已生成的测量行）；掩膜叠加开关 |
| | `nativeAiAutoMeasure(withBoneDensity)` | 逐实例建/复用 R-06 掩膜 ROI + M-04/M-08，返回条数（见 §6.9 的复用规则） |
| | `nativeAiRecommend(minGapMm, maxOut)` | AI-03 候选列表 + 摘要文案 |
| | `nativeAiPickInstance(x,y,z)` | 世界坐标 → 实例号（叠加层点选用） |
| | `nativeAiDumpParity(path)` | AC-08 取证落盘（格式见 §6.10） |

**AI 层的失败面**：`nativeLoadModel` 不抛异常，只回 `ok=false + error`；Kotlin 据此禁用 AI 按钮，一期的测量/ROI/规划/报告继续可用。

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
| ONNX 模型释放 | `filesDir/ai_models/` | `teeth_cnn.onnx` + `teeth_cnn.json`（阈值来源；缺 json 回退 0.5） |
| AC-08 取证数据 | `filesDir/ai_parity/` | `device_{case}.parity.bin`（0101 为 20,643,904 B，`adb run-as` 可拉） |
| 牙科 CBCT 释放目录 | `getExternalFilesDir(null)/dental_{caseId}/` | 128 张 `.dcm` + `.release_ok` 标记（记病例 id 与张数，任何一项不符整批重拷） |

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

# 3) 主机侧纯 C++ 单测（不需要设备，不需要 VTK/DCMTK/ONNX Runtime）
cd cbctmeasure/src/host && ./build_and_run.sh
```

主机单测覆盖：M-01~M-08 解析值对照、R-01~R-06 体积/面积/权重逐位一致、S-03~S-06 与安全判级、S-07~S-10 正畸量、**AI 组 11（掩膜 ROI 复用与归档往返）**、JSON 往返（值与单位不变）、`detailJson` 字段完备性、异常路径（点数不足 / ROI 不存在 / 组合 ROI 环引用）。最近一次运行：

```
RESULT: PASSED 386 / FAILED 0 / FINDINGS 4
AC-01 最坏绝对误差 8.88e-16 mm · AC-02 7.11e-15 度 · AC-03 最坏相对误差 0.1244% · AC-04 0.000 HU
```

`build_and_run.sh` 编译 `core/AiCore.cpp + AiEngine.cpp + AiPlanner.cpp`（预处理/阈值/连通域/轮廓/统计/推荐这条确定性路径），**刻意排除 `ai/OrtEngine.cpp`**（依赖 `libonnxruntime.so` 与 NDK `dlfcn`），用例里用假 `AiBackend` 注入已知 prob，因此"掩膜→实例→体积"整条链在主机侧可断言。

AI 的**端到端精度与主机/真机一致性**不在单测里，而在两套离线脚本里：

```bash
# 主机侧重建夹具/指标（step 00 模板 -> 01 体积+GT -> 02 特征研究 -> 03 train2 -> 04/04b parity -> 05 verify -> 06 报告）
cd cbctmeasure/src/host/ai/scripts && ./run_all.sh
#   输入默认取 ../raw（换目录：DENTAL_SRC=/path），产物默认写回 host/ai（换目录：AI_BUILD=/tmp/out）
#   只重建夹具/报告、不动模型：SKIP_TRAIN=1 ./run_all.sh

# AC-08：把真机 dump 拉回来逐元素比对（PASS 才允许宣称主机/真机一致）
ADB=$HOME/Library/Android/sdk/platform-tools/adb
$ADB shell run-as com.example.dcmtkdemo cat \
  files/ai_parity/device_DENTAL_CBCT_0.6MM_192X192X128.parity.bin > /tmp/dcmtk_verify/device_0101.parity.bin
python3 cbctmeasure/src/host/ai/scripts/check_device_dump.py /tmp/dcmtk_verify/device_0101.parity.bin
```

`check_device_dump.py` 的默认容差是 `--feat-max-abs 1e-6`，**不要改回 0.0**：真机跑的是
ORT 1.17 + arm64 kernel，主机夹具是 ORT 1.23 + x86，float32 累加顺序就注定 1e-8 量级差
（实测 feat 2.980e-08 / 799 个元素不同，prob 1.192e-07，label 与 inst 逐位相同）。
默认值要求逐位相同会把每台正常设备判成 FAIL。夹具目录默认随仓库定位，clean clone 不用带参数。

`FINDINGS` 是「实现与 PRD 字面表述不一致但已按更合理定义实现并写入文档」的提示项，不是失败。

界面回归操作手册见仓库技能 `.qoder/skills/android-adb-ui-regression-loop/`（坐标必须取自当次 `uiautomator dump`；判定看 logcat TAG，不靠肉眼截图）。

### 5.1 日志 TAG 对照

| TAG | 层 | 看什么 |
|---|---|---|
| `CbctMeasureCtrl` | Kotlin view | 状态机切换、拾取失败原因 |
| `CbctMeasure` | JNI | 方法注册、字符串转换 |
| `CbctMeasureCore` | C++ core | `analyze` 的 `scanned/coverage/vol/mean/in N ms`、种植体 `eval` 行、AI 的 `runSegment ok: inst=… prep/infer/post/total ms alloc=…`、`grid: app … -> model … red …`、`aiAutoMeasure: 复用 ROI #N（aiLabel=…）` |
| `CbctMeasureAi` | Kotlin ai + C++ `OrtEngine` | `ORT so 候选（…）`、`dlopen ok: <命中路径>`、`ORT 就绪：in='feat' 原始shape[…] -> 逻辑 C… × X… × Y… × Z…`、`模型 …（14477 字节），运行时 so=…，ORT 1.17.0 api v17`、病例释放与阈值规格回退 |
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

### 6.7 ONNX Runtime 用「vendor 头 + dlopen」，不是 Prefab（PRD 字面偏差）

`onnxruntime-android:1.17.0` 的 AAR 里**没有 `prefab/` 元数据**（只有 `AndroidManifest.xml` / `classes.jar` / `headers/` / `jni/`），CMake 无法 `find_package(onnxruntime CONFIG)`，也没有 imported target。因此：

* 头文件 vendor 到 `cpp/third_party/onnxruntime/include/`（4 个），`include_directories` 只给头；
* Gradle 依赖 `implementation(libs.onnxruntime.android)` **只为了把 `jni/<abi>/libonnxruntime.so` 打进 APK**，Kotlin 不用 ORT 的 Java API；
* `ai/OrtEngine.cpp` 用 `dlopen` + `dlsym(OrtGetApiBase)` 拿 `OrtApi` 跳转表，`CMakeLists.txt` 里链 `dl`；
* **只有这一个 TU 碰 ORT**，`core/Ai*.cpp` 保持纯 C++，主机单测才能跑（见 §5）。

`dlopen` 的路径必须由 **Native 逐条试**：AGP 默认 `useLegacyPackaging=false`，`.so` 不落盘，linker 认的是 `<apk>!/lib/arm64-v8a/libonnxruntime.so`，而 `!/` 形态不是 `File`（`exists()` 恒 false），Kotlin 侧无法提前挑一条。真机实测命中的就是 `base.apk!/lib/arm64-v8a/libonnxruntime.so`，`nativeLibraryDir` 那条不存在 —— 早期只试磁盘路径时 AI 永远"不可用"。命中路径经 `usedSo()` → `runtimeSo` → 状态栏，报告与屏幕同源。

API 版本：头是 `ORT_API_VERSION=17`，`OrtEngine::load` 从期望版本往下找两端支持的最高版本（低版本结构体是高版本前缀），设备取到 `onnxruntime 1.17.0 / C API v17`。

### 6.8 `:app` 的 `abiFilters += "arm64-v8a"` 是 PC-05 预算要求

onnxruntime AAR 自带 4 个 ABI（约 57 MB 冗余），而本工程自研 `.so` 只编 arm64-v8a。不收窄的话「模型 + 运行时 ≤80MB」会被**永远用不到的库**顶满。收窄后运行时 16,033,712 B + 模型 14,477 B = **16.05 MB**。换设备 ABI 时必须同步改这里，否则 `dlopen` 全链失败。

### 6.9 逐牙自动测量：ROI 按 `aiLabel` 原位复用，归属靠 `AUTO_NOTE`

`MeasurementManager::aiAutoMeasure()` 对每个实例先查 `ROI_AI_MASK && aiLabel == 实例号` 的既有 ROI：命中就 `updateRoi`（**id 不变**，只刷新名称/配色），并删掉该 ROI 上 `note == AiConst::AUTO_NOTE` 的旧测量行；未命中才 `addRoi`。

* 为什么必须复用：掩膜网格**不进归档**（96×96×64 体素场会让"≤1MB/Study"爆预算），所以「恢复归档 → 重跑自动测量」是常规路径；旧实现每次新建 ROI，结果列表翻倍且新行全部「待重算」，**这些失败行还会被写进临床 PDF**（真机复现：PDF 里 26 条「待重算」）。修后同一份 PDF 是 0 条，ROI 仍 13 个。
* 删除顺序：**先收集 id 再删**，否则 `erase` 时迭代器失效。
* `AiConst::AUTO_NOTE` 字面量被主机用例钉住（组 11）：改文案会让已归档的自动行认不出来变成孤儿；手工挂在同一掩膜 ROI 上的测量（`note` 不同）不会被误删，这条也有断言。
* `adoptAiResult` 是**按值拷贝**：`roiId` 只回填到 manager 内部那份副本，调用方的 `AiResult` 里恒为 0 —— 读回填值必须走 `mgr.ai()`。

### 6.10 设备轴序与 AC-08 取证格式（改模型/改抽稀前必读）

* **轴序**：`feat[c][i][j][k]` 的 i=DICOM **列**、j=DICOM **行**、k=z（依据 `cbctdeal/CbctSeriesParser.cpp` 的 `vol->width=Columns` 与 `[depth][height][width]` 布局）。3×3×3 卷积对该平面转置**不等价**，所以模型按设备轴序训练**并**按设备轴序导出；主机 canonical 数组（rows,cols,z）要 `transpose(0,2,1,3)`。
* **掩膜判定是 `prob[1] > 阈值` 严格大于**（float32 提升 double，`AiCore::thresholdMask`），**不是 argmax**：0101 上两者差 133 体素。
* **连通域可复现性**：邻居表顺序、扫描顺序、LIFO 栈、`<8` 丢弃、按裁剪后体素数降序重编号都固定，因此跨端逐实例一致；但 C++ `std::sort` 非稳定，**尺寸并列的实例之间只保证多重集一致**。
* **阈值来自数据不来自代码**：`assets/models/teeth_cnn.json` 的 `threshold=0.49`（在验证集 0074 上扫出来的 Dice 最大点）。`AiConst::TOOTH_THRESHOLD` / Kotlin `DEFAULT_THRESHOLD` 只是 json 缺失时的回退，改阈值应改随包 json。
* **dump 格式**：64 字节头 = 8×int64 LE（`featCount, probCount, labelBytes, modelDim0..2, redDim0..1`），随后 float32 feat、float32 prob、uint8 label、int16 inst。0101 合计 20,643,904 B。主机 checker 的默认容差：`--feat-max-abs 1e-6`、`--prob-max-abs 1e-5`、`--label-max-rate 0.001`、`--inst-require-exact`（`feat` 那项曾经是 `0`，会把每台正常设备判 FAIL，见 §5）。
* **feat/prob 不是 memcmp 级一致**：真机与主机差 1 个 float32 ulp 量级（2.98e-08 / 1.19e-07），成因是浮点**加法顺序**（`AiCore.cpp` 盒均值注释处），因此判定口径是"max|diff| + label 一致率"，不是逐字节相等。**改任何一处求和顺序都可能让 AC-08 的夹具失效**，改完必须重跑 §5 的 checker。

### 6.11 AI 是可选层：失败面收敛在按钮可用性

`loadModel` / `runSegment` 失败都不抛异常，只回 `ok=false + error`；Fragment 据此禁用「牙齿自动分割/逐牙自动测量/种植位点推荐/导出取证数据」。一期功能（测量、ROI、规划、标注、归档、报告）不依赖 AI 可用。新增 AI 入口时保持这个契约：**不要让 AI 失败把整页拖崩**。

### 6.12 AI 面板是宿主的 child fragment，会话所有权必须唯一

AI 的输入是宿主 `MeasureSession` 里那份**零拷贝**体数据（`VolumeRef` 指向 `:cbctdeal` 的 `CbctVolume`）。
抽屉切页走的是 `FragmentTransaction.replace()`，把 AI 做成第二个页面 = 离开测量页就销毁会话与渲染窗口，
回来还得重新解析 9.5MB 序列。所以 `CbctAiFragment` 用 `childFragmentManager` add 到
`R.id.fl_ai_panel`，tab 切换只改 visibility；它不持有 `sessionHandle`，四件事（借句柄、要体数据概况、
请宿主重画/刷列表、走选病例流程）全部经 `CbctAiHost` 回宿主。

* 新增跨面板能力时**先加宿主接口，再在面板里调**，不要在 `CbctAiFragment` 里另建一份 Native 会话。
* ORT 会话跟着 `MeasureSession` 活：换例/退出才释放，「清除 AI 结果」只丢结果不释会话
  （所以清完 PSS 仍 ~257MB，不是泄漏，见 §7）。
* 本类不做任何数值计算，屏上每个数都是 JNI 回执 —— 与 PDF、DICOM SR 同源（PRD §6 精度项的前提）。

### 6.13 提示文案是状态的一部分：按钮路径必须显式刷新

`MeasureToolController` 的提示由 `updateHint()` 渲染，而内部工具切换（`switchState` /
`setMeasureType` / `setAnnotationType`）都在 `abortDraft()` 之后自己刷一次，于是**只有按钮**会露出
「图形没了、提示还停在（6 点）」这种数据与文案脱节。规则：

* 按钮要清草稿就调 `discardDraft()`（= `abortDraft()` + 清 `stickyMissHint` + `updateHint()` + `pushDraft()`），
  不要直接调 `abortDraft()`。
* `stickyMissHint`（"上一次没点中切面"的粘性提示）只在手指 DOWN 时冲掉，而**按钮点击不是 view 上的 DOWN**。
  任何"会改变提示语义"的按钮（丢弃草稿、撤销取点）都要显式 `stickyMissHint = null`，否则页面继续挂着
  「当前切面未拾取到点」。
* 点数不足的操作要给可执行文案而不是静默失败：`闭合无效：还需 N 个点（当前 M 点）`。
* 面积多边形除了「点中起点」（容差 2.0mm 世界距离，放大视图里不到一个屏幕像素）还必须有
  `closeAreaDraft()` 这条不依赖命中起点的提交路径，且失败时草稿不能丢。

### 6.14 读数排在操作之后；夜间 selector 只能放 `res/color-night/`

* 状态读数（`tv_ai_status`、面积提示）放在按钮**之后**。它原来在按钮上方且 `minLines=3`，
  而分割后合法需要 7 行 —— 结果整排按钮往下跳 128px，用户按旧位置的手势点空（点空没有报错，
  只表现为"界面没反应"）。本轮真机复验：装载前后与分割之后三次 dump，`btn_ai_*` 恒为 y=854/938/1018。
* 折叠面板只隐藏列表本体，标题行常驻 —— 它同时是「共几项」的读数；行数超阈值自动收起一次，
  用户手动开关过之后不再自动改（`resultsUserToggled`）。
* `selector` 属 **color 类型**资源，夜间变体必须放 `res/color-night/`；放 `res/values-night/color/`
  不参与合并，`merged_res` 里只会剩一个文件，症状是"夜间文字仍是浅色主题的近黑色"。

### 6.15 主机流水线：派生物缺失只 SKIP/WARN，两条链的夹具不可互比

`verify_all.py` 是定义完成的门，它必须区分三种情况，否则结论会骗人：

* **派生物没生成**（`app_volume/`、`work/`、`gt/*.raw`、旧链 `parity_ref/{feat,prob,lab,inst}_*.raw`）
  → 打 `SKIP`，不计入 checks 数。把"没跑"报成"跑过且对"和报成 FAIL 一样糟。
* **两条链本来就不同**：旧主机链（`make_parity.py`，主机轴序 + 通道 argmax）与随包设备链
  （`make_parity_device.py`，设备轴序 + `prob[1] > 0.49`）是**两份不同的预测**（实测 prob 通道 1
  逐元素最大差 9.97e-01，lab 在 (1,0,2) 置换下 Dice 0.81）→ 打 `WARN` 写明原因。
  随包证据只认 `parity_ref/device_*`（见 §6.10 的轴序纪律）。
* **本地副本陈旧**：`feat_0101.raw` 与随包夹具不一致时，先重跑 `make_parity.py` 再判；
  新鲜则 PASS，陈旧只 WARN。
* 26 邻域普查的比较必须**过同一道 `<8` 体素丢弃闸**，否则 scipy 裸跑 34 域 vs 夹具 24 域会被当成不一致。
* `run_all.sh` 的 step 03 必须是 `train2.py`（产出交付模型的那个）。指回 `train_model.py` 会把
  14,477B 随包模型覆盖成 3,851B 的 1×1×1 旧结构，然后 step 05 以 "kernel_shape [1,1,1]" 失败收尾 ——
  看起来像模型坏了，其实是流水线接错（本轮 D-05）。

---

## 7. 性能与验收实测摘要（真机 Honor AGM3-W09HN）

| 指标（PRD） | 门槛 | 实测 | 结论 |
|---|---|---|---|
| PC-01 测量计算 | ≤50ms | 距离/角度/HU 0~1ms；面积/截面量化 12ms；体积盒（145 万体素）21~23ms；**R-06 掩膜量化：24/26 条 0~1ms，被合并出的巨型实例 #1 两条 178/162ms** | 达标（交互态）；批量归档重算会出现 ~70ms，属装载耗时；**大掩膜超标 2 条**，见 §7.3 |
| PC-02 ROI 渲染帧率 | ≥30FPS | `avgDraw 0.57~2.04ms @ prims=89`；adb 注入下 `fps=2~12` | **未充分验证**：合成 swipe 只产生 ~5Hz 的 MOVE，事件密度是瓶颈；见 §7.1 |
| PC-03 截图生成 | ≤500ms | 四次采样 `total=319 / 795 / 825 / 934 ms`，其中 `frame=313 / 779 / 807 / 903 ms` | **未达标**（抖动大）：`frame` 段是 `:cbctdeal` GPU 读回；本模块叠加层只占 6~31ms |
| PC-04 模块内存增量 | ≤100MB | Pss 759,169 → 755,562（恢复归档）→ 748,425 KB（导出后） | 达标：测量层增量为 0（<±10MB 噪声）；700MB 量级基线是体数据 + GL，与测量层无关。AI 推理一次性峰值另计（下一行） |
| 归档存储 | ≤1MB/Study | measure 6,021 B + anno 375 B + plan 1,051 B ≈ **7.4KB**/Study | 达标（掩膜网格刻意不出档，见 §6.9） |
| **PC-05 AI 推理延迟** | ≤5s | `prep 658.7 + infer 334.6 + post 8.5 = total 1002.0 ms`（0101）；另一次 `669+341+9=1019 ms`（0021） | **达标**（余量 ~5×；预处理占大头，推理 33.4%） |
| **PC-05 模型 + 运行时体积** | ≤80MB | `libonnxruntime.so` 16,033,712 B + `teeth_cnn.onnx` 14,477 B = **16.05 MB**；推理期新增 Native 内存 ≈21.9 MB | **达标**（前提是 §6.8 的 ABI 收窄） |
| **AC-08 主机/真机奇偶校验** | 同输入同输出 | label **逐位一致**（0/589,824 不一致）、inst **100% 精确匹配**（24 实例个体素不差）、feat max\|diff\| 2.98e-08、prob 1.19e-07 | **PASS**（见 §6.10 判定口径） |
| **AI-01 分割精度** | F1 ≥0.85 | **0.58285**（未见过的 0101，阈值 0.49；precision 0.64521 / recall 0.53147 / IoU 0.41128） | **未达标**，见 §7.3 与 `doc/AI_ONNX_TEST_REPORT.md` §6 |
| UI 重构轮复测（AI 剥离 + 结果折叠 + 按钮布局） | — | 冷启动 1,626ms；序列解析 222ms；分割两次 `1014ms`（prep 676.8 + infer 329.6 + post 7.5）与 `997ms`（659/331/7），两次都是 `inst=24 toothVox=9958`；单实例体积遍历 1,121,796 体素 / 114ms；`avgDraw 0.46~1.49ms @ prims=33` | 剥离**没有引入性能回归**：AI 编排的耗时分项与一期同量级。逐条判定见 `doc/UI_SPLIT_LAYOUT_REGRESSION_REPORT.md` |
| 内存（AI 装载态） | — | AI 装载 + 分割后 TOTAL PSS **268.9 MB**；「清除 AI 结果」后 **257.5 MB** | 正常：ORT 会话跟着 `MeasureSession` 活，清除只丢结果（见 §6.12）；差值主要来自推理期特征/概率缓冲（日志 `alloc=21.9MB`） |

### 7.1 PC-02 的测量手段局限（不要误读）

`overlay fps` 统计的是**叠加层自己每秒真正 onDraw 了几次**。手指拖动时 MOVE 事件是 60~120Hz，而 `adb shell input swipe` 即便把 duration 拉到 6~9s，也只产生约 5Hz 的采样点，于是实测帧率只有 2~12fps。**瓶颈在注入事件的密度，不在绘制成本**（单帧 0.57~2.04ms，换算 30FPS 预算富余 16 倍以上）。本轮没有可信手段在真机上复现连续高密度触控流，因此 PC-02 记录为「组件成本达标、整体验证不足」，不宣称通过。

### 7.2 已知限制

1. **截图耗时受 GPU 读回支配**（PC-03）：要达标得改 `:cbctdeal` 的抓帧路径（离屏 FBO 同步读 or `PixelPack` 异步），属于渲染模块范畴，本模块不改渲染行为。
2. **AI-02 / AI-04 未交付**：AI-02（颌骨/神经管/气道等其它结构分割）缺各自的专家标注，S-05 仍依赖手工描记神经管；AI-04（AI 生成报告文字）属临床诊断输出，无可验证语料，刻意不做。已交付的是 AI-01（ONNX Runtime 端上推理的牙齿分割）与 AI-03（规则 + ML 混合的种植位点推荐），见 §1.5。
3. **叠加层是 2D Canvas**：图元按世界坐标重投影，不随深度遮挡做剔除（种植体圆柱用轮廓 + 填充近似），与 PRD S-02 提到的「螺纹纹理 Actor」相比是简化实现。
4. **双指手势无法用 adb 真实注入**：AC-10（描记过程中双指缩放/平移）只验证到状态机与代码路径，未做真机双指取证。
5. **画面滚出可见区时证据图会全黑**：`SurfaceView` 不可见后渲染线程不出帧，`captureFrame()` 读到空 surface，而 `captureEvidence()` 仍返回一张合法（全黑）位图、日志照打 `evidence=true`。真机复现：可见 282,849 B / 有效像素 7.13%，不可见 7,760 B / 0%。当前只在 `USER.md` 给了操作规避（导出前把画面滚回来），**代码侧的全黑判空尚未实现**——要补的话改 `CbctMeasureView.captureEvidence()`：对合成位图做网格采样，全黑则返回 null 并提示「画面不可见，未取到证据图」。详见 `doc/TEST_REPORT.md` §4.4。

### 7.3 AI-01 的精度局限与它对下游读数的污染（必读）

F1 0.5828 的根因是**数据量与模型容量**，不是链路：可用公开逐牙标注只训得起一个 3,474 参数、感受野 7³、无下采样/多尺度分支的小 CNN（本机装不上 torch/tf/nnU-Net，也没有可导出的公开预训练权重，模型是纯 numpy 手写前向反向训练后导出的标准 ONNX opset 17）。后果是**实例过合并**：

| 病例 | 最大实例 | 占该例预测牙齿体素 | 后果 |
|---|---|---|---|
| 0101（测试例） | #1 = 4,927 红体素 = **8.514 cm³**，#2 = 2,855 = 4.933 cm³ | 前两名合计 **78%** | 一颗磨牙量级 0.4~1.0 cm³ ⇒ 这两条是"半排牙连成一块" |
| 0021（训练例） | #1 = 12,587 红体素 | **95%** | 同上，且它参与过训练，分数也不能当泛化指标 |

因此：**大实例的 M-04 体积 / M-08 骨密度不可用于单牙判断**；AI-03 的"净间隙"取相邻实例质心距离，实例被合并时间隙会虚高（真机候选里出现过解剖上不可能的 `间隙 88.1mm`、`48.8mm`，这类候选必须丢弃，而打分与 S-02~S-06 判级本身仍正确执行）。小实例（<~600 红体素）观感接近单颗牙，仍需肉眼核对叠加轮廓。

另外两个已知的量化点：R-06 掩膜 ROI 的候选集**有**外层包围盒快速路径（`boundsMm` → `spatialBoundsAt`，见 §1.2），但被合并出的巨型实例其包围盒本身几乎横跨半个牙弓，裁剪收益有限——0021 的实例 #1 单次量化 178 / 162 ms（遍历 1,686,672 / 覆盖 100,696 app 体素，遍历量是覆盖量的 ~17 倍；其余 24 条遍历量只有 480~9,180，0~1 ms），是 PC-01 仅有的超标点（还缺的那一层是"按实例包围盒分块 + 只扫非零体素"）；主机 ORT 1.23.2 vs 真机 1.17.0，parity 在跨版本下 PASS，但升级设备端运行时应重跑 §5 的取证。完整数字、成因与改进路径见 `doc/AI_ONNX_TEST_REPORT.md` §6、§13。

---

## 8. 提交与验证纪律

- 改动 core C++ 之后**必须**先跑 `src/host/build_and_run.sh`，再谈真机；
- 真机判定以 logcat 行 + 拉回主机的产物（PDF / SR / JSON）为准，**没有真机证据不宣称验证通过**；
- 改动 AI 层（`core/AiCore.cpp` 的特征/阈值/连通域、抽稀倍率、求和顺序）之后，除单测外**必须**重跑 AC-08 取证：真机「导出取证数据」→ `run-as` 拉回 → `check_device_dump.py` 出 `VERDICT: PASS`；只有"肉眼看到掩膜"不构成 AI 精度证据；
- AI 的精度指标（Dice/F1）**必须**在未参与训练与阈值标定的测试例上报告，且同时给出实例普查（防止大块合并把体积读数伪装成合理结果）；
- 改动页面结构（面板增删、编号、tab 归属、按钮文案与提示语）**必须**三处同步：`fragment_cbct_measure.xml` 的区块注释、本文件 §2 的宿主接入表、`USER.md` 的小节标题与提示语对照表。本轮出现过布局注释仍写 `5) 结果`、README 仍写「6) AI」的情况——编号不一致会让人按旧位置抬手点空；
- 提示语（`setHint` / toast）是状态的一部分：新增按钮时确认它清掉了 `stickyMissHint` 并刷新文案，失败路径给的是**可执行的原因**而不是「操作失败」；文案改动同样要进 `USER.md` §9 的对照表。
- 涉及显示效果的本模块改动只影响叠加层，不动 `:rawpixeldeal` / `:cbctdeal` 的像素算子与调窗算法（该两模块有强制回滚基线约定）。
