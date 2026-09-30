# ONNX Runtime Android 模型推理 — 测试报告（PRD §5.6 AI 辅助分析 / Phase 2）

| 项 | 内容 |
|---|---|
| 被测功能 | AI-01 牙齿自动分割（ONNX Runtime Android 推理）+ AI-03 种植位点推荐 |
| 模块 | `:cbctmeasure`（`com.wangyao.cbctmeasure`），入口页面「抽屉菜单 → CBCT Measure」的「6) AI 辅助分析 (Phase 2)」 |
| 运行时 | `com.microsoft.onnxruntime:onnxruntime-android:1.17.0`（AAR 内的 `libonnxruntime.so`，C++ 侧 `dlopen` + OrtApi 调用） |
| 模型 | `teeth_cnn.onnx` v2-device-order，14,477 B，opset 17，3,474 参数，sha256 `3092730fc286607447bbc8dadb678530535711aadd75429cf047e499ac3b767f` |
| 验证数据 | 公开 **DentVoxel** 牙科 CBCT（专家逐牙标注），转成本工程解析器直接消费的 192×192×128 @0.60 mm 符号 int16 DICOM 序列，随模块出包 |
| 真机 | Honor **AGM3-W09HN**（序列号 `AWTYCP2C27400666`，Android 10，arm64-v8a，density 320） |
| 主机侧单测 | `RESULT: PASSED 386 / FAILED 0 / FINDINGS 4`（含 AI 组 11 的 22 条新断言） |
| 报告日期 | 2026-09-30 |
| 相关文档 | 一期回归：[`TEST_REPORT.md`](TEST_REPORT.md)；使用手册：[`../USER.md`](../USER.md)；开发说明：[`../README.md`](../README.md) |

---

## 1. 结论摘要（先看这一张表）

| PRD 指标 | 门槛 | 实测 | 结论 |
|---|---|---|---|
| AI-01 分割精度（F1） | ≥ 0.85 | **0.58285**（未见过的测试例 dentvoxel_0101，阈值 0.49） | **未达标**，见 §6 |
| AI-01 推理链路可用 | 端上出掩膜并进既有测量链路 | 真机 13 实例 / 13,225 牙齿体素（0021）、24 实例 / 9,958 体素（0101），全部进 R-06 掩膜 ROI + M-04/M-08 | 功能达标，精度不达标 |
| AI-03 推荐的安全距离判定 | 与手工种植体同一套判据（S-02~S-06） | 推荐候选落库后由 `ImplantPlanner` 重算，同一口径；真机 8 候选、采纳 1 颗 → `#40 AI推荐 上颌6-9（安全）骨高 12.9 / 骨宽 14.1 mm` | 达标（判据复用，无第二套数值） |
| PC-05 单次推理延迟 | ≤ 5 s | `total=1002.0ms`（prep 658.7 + infer 334.6 + post 8.5）；另一次 `669+341+9=1019 ms` | **达标**（余量 ~5×） |
| PC-05 模型 + 运行时体积 | ≤ 80 MB | 16,033,712 B（so）+ 14,477 B（模型）= **16.05 MB** | **达标** |
| AC-08 主机/真机奇偶校验 | 同输入同输出 | `VERDICT: PASS`：label 逐位相同（0/589,824 不一致）、inst 100% 精确匹配、feat max\|diff\| 2.98e-08、prob 1.19e-07 | **达标**，见 §7 |
| PC-01 测量计算 | ≤ 50 ms | 逐牙自动测量 26 条：**24 条 0~1 ms**；被合并出的巨型实例 #1（100,696 app 体素）两条 **178 / 162 ms** | **2 条超标**（同一根因），见 §8.3 |
| AI-02 智能分割（颌骨/气道等其它结构） | — | 未交付 | 见 §2.2 |
| AI-04 自动报告文本生成 | — | 未交付 | 见 §2.2 |

**一句话结论**：ONNX Runtime Android 的**推理链路、精度取证口径、与既有测量/规划/报告链路的贯通**全部按 PRD 落地并在真机验证通过（含主机/真机逐元素奇偶校验 PASS）；**分割精度本身不达 PRD AI-01 的 F1 ≥ 0.85**，实测 0.5828，根因是可用训练数据只有 2 例、模型只有 3,474 参数（见 §6.3），不是链路缺陷。掩膜的**逐实例合并（over-merging）**使最大实例的体积不具备临床意义，这条限制必须与功能一起交付。

---

## 2. 交付范围

### 2.1 已交付

* **AI-01 牙齿自动分割**：`feat`(6 通道) → Conv3d×3 → `prob`(2 通道 Softmax) 的 ONNX 模型在端上 CPU 推理；阈值掩膜 → 26 邻接连通域（<8 体素丢弃）→ 每实例质心/长轴(PCA)/包围盒/轮廓/平均 HU/体积；每个实例自动落成一条 **R-06「AI 掩膜」ROI**，并挂 **M-04 体积** + **M-08 骨密度**，进既有的结果列表、叠加层、JSON 归档、PDF 报告与 DICOM SR 链路（同一份数，不存在"屏幕一个值、报告一个值"）。
* **AI-03 种植位点推荐**：ML 提供解剖先验（牙弓归属、每颗牙质心与长轴来自 AI-01 掩膜），规则提供判定（净间隙 ≥ `minGapMm`、邻牙长轴加权平均作植入轴向、牙槽嵴顶高度估计、按骨宽/骨高选直径与长度、0~100 打分排序）。候选点击即落一颗真正的种植体，走 S-02~S-06 同一套安全评估。
* **数据交付**：`cbctmeasure/src/main/assets/dental_cbct/` 内置 2 例牙科 CBCT（0101 测试例 / 0021 训练例）+ `index.json` 来源说明；页面「1) Series Source → 选择加载牙科 CBCT」弹窗选例、释放到私有目录并自动解析。
* **取证能力**：「导出取证数据」把真机 feat/prob/label/inst 落盘（20,643,904 B），主机脚本 `check_device_dump.py` 逐元素比对 → AC-08 有硬证据，不靠肉眼截图。

### 2.2 明确未交付（以及原因）

| 需求 | 状态 | 原因 |
|---|---|---|
| **AI-02** 其它结构分割（颌骨 / 下牙槽神经管 / 气道） | **未交付** | 需要各自的专家标注与各自的模型。本机可得的公开标注只有 DentVoxel 的逐牙标注；神经管仍靠一期「神经管」工具手工描记（S-05 依赖它）。多结构分割不是链路问题，是数据问题。 |
| **AI-04** AI 生成报告文字 / 自动诊断结论 | **未交付** | 属于临床诊断输出，PRD 自身要求"任何诊断与手术决策必须由临床医师复核"。本轮没有可验证的临床语料，做成文本生成只会产出看似通顺但无依据的结论，因此刻意不做。报告里 AI 相关条目只呈现数值与"AI-01 自动分割"来源标记。 |
| 掩膜网格的持久化 | **未交付（设计取舍）** | 掩膜是 96×96×64 的体素场，归档会让"≤1MB/Study"的存储指标爆掉。恢复归档后 R-06 ROI 只保留 `aiLabel`，重跑一次推理即可原位复用同一批 ROI（见 §9.1 的修复）。 |

---

## 3. 运行时集成：与 PRD 字面写法的偏差（必须记档）

PRD §5.6 写的是"通过 **Prefab** 集成 ONNX Runtime"。实测该路线在本工程不可用，已按下面的方式落地，这是**有意偏差**，不是遗漏：

* `com.microsoft.onnxruntime:onnxruntime-android:1.17.0` 的 AAR 解压后只有 `AndroidManifest.xml` / `classes.jar` / `headers/` / `jni/<abi>/libonnxruntime.so`，**没有 `prefab/` 元数据**，因此 CMake 既不能 `find_package(onnxruntime CONFIG)`，也没有 imported target。
* 采用做法：**vendored 头文件 + 运行时 `dlopen`**。
  * `cbctmeasure/src/main/cpp/third_party/onnxruntime/include/` 放 AAR `headers/` 里的 4 个头（`onnxruntime_c_api.h` 等）；
  * `build.gradle.kts` 用 `implementation(libs.onnxruntime.android)` 只取它的**交付物**（`jni/<abi>/libonnxruntime.so` 由 AGP 打进 APK），Kotlin 侧**不使用** ORT 的 Java API；
  * C++ 侧 `ai/OrtEngine.cpp` 是**全工程唯一** include ORT 头、唯一 `dlopen` 的翻译单元，`core/AiCore.cpp` / `AiEngine.cpp` / `AiPlanner.cpp` 保持纯 C++（不依赖 Android/VTK/DCMTK/ORT），因此主机单测可以用一个假 backend 顶替推理、继续验证预处理/掩膜/连通域/统计/推荐这条确定性路径。
* **API 版本协商**：头文件是 `ORT_API_VERSION=17`，`OrtEngine::load` 从期望版本往下找"两端都支持的最高版本"（低版本 API 结构体是高版本前缀）。真机取到 `onnxruntime 1.17.0 / C API v17`，无降级。
* **不静态链接**：不把 16 MB 的 `.so` 链进本模块，避免每个 ABI 重复；同时 `:app` 加了 `ndk { abiFilters += "arm64-v8a" }`（见 §8.4）。

### 3.1 `libonnxruntime.so` 到底怎么被加载的（真机取证）

AGP 默认 `useLegacyPackaging=false`，`.so` **不落盘**，以不压缩形式留在 APK 内原位映射，linker 认的是 `<apk>!/lib/<abi>/libonnxruntime.so` 这种写法。因此：

* Kotlin 侧 `AiEngine.ortSoCandidates()` 生成候选链：`nativeLibraryDir/libonnxruntime.so`（磁盘形态，实测不存在）→ `sourceDir!/lib/arm64-v8a/libonnxruntime.so` → 各 split APK 的同形态；用 `;` 拼串交给 Native。
* C++ 侧 `OrtEngine::load` 逐条 `dlopen`，并额外补一条**裸 SONAME** 兜底；真正命中的路径经 `usedSo()` → JSON `runtimeSo` → `LoadResult.usedSoPath` → 状态栏「dlopen 命中：…」显示，报告引用同一份。
* 不在 Kotlin 侧提前"挑一条"：`!/` 形态不是 `File`，`exists()` 恒为 false，只有 linker 能给结论。

真机取证（截图 `08_08_dental0021_ai_overlay.png`，状态栏原文）：

```
运行时 onnxruntime 1.17.0 / C API v17 / in 'feat' [6,96,96,64] / out 'prob'；阈值 0.49
dlopen 命中：/data/app/com.example.dcmtkdemo-RaEwUA9nSc8-jZ4lIyETnQ==/base.apk!/lib/arm64-v8a/libonnxruntime.so
```

```
D/CbctMeasureAi( 9098): dlopen ok: /data/app/com.example.dcmtkdemo-.../base.apk!/lib/arm64-v8a/libonnxruntime.so
D/CbctMeasureAi( 9098): 模型 /data/user/0/com.example.dcmtkdemo/files/ai_models/teeth_cnn.onnx（14477 字节），运行时 so=...base.apk!/lib/arm64-v8a/libonnxruntime.so，ORT 1.17.0 api v17
D/CbctMeasureAi( 9098): ORT 就绪：in='feat' 原始shape[1,6,96,96,64] -> 逻辑 C6 × X96 × Y96 × Z64；out='prob' rank=5 通道 in/out=6/2
```

> 失败路径也验证过：把候选链改成只给 `nativeLibraryDir` 的磁盘路径时，`loadModel` 返回 `ok=false` + `error`，页面禁用 AI 按钮，**一期的测量/ROI/规划/报告功能不受影响**（AI 是可选层，不抛异常）。

---

## 4. 验证数据（用户要求"全网找标准牙科 CBCT"的落地）

### 4.1 来源与转换

* 数据集：公开 **DentVoxel**（牙科 CBCT + 专家逐牙标注）。选它的理由：它是**真实临床采集**的体数据、带**逐牙**标注（能算 Dice），且体量小到一个笔记本能跑完整流水线。
* 转换：主机脚本 `src/host/ai/scripts/build_app_volumes.py` 把原始数据写成 **192×192×128 @0.60 mm 符号 int16** 的 DICOM 序列（Explicit VR LE，`PixelRepresentation=1`，`RescaleIntercept=0`，`Slope=1`），`:cbctdeal` 的解析器直接消费，不需要任何特殊分支。
* 出包位置：`cbctmeasure/src/main/assets/dental_cbct/<case>/0000NN.dcm`，每例 128 张 / 约 9.5 MB。

### 4.2 划分纪律（避免"在见过的数据上吹精度"）

| 划分 | 病例 | 用途 | 是否随 APK 出包 |
|---|---|---|---|
| train | dentvoxel_0021, dentvoxel_0047 | 只用于 patch 训练 | 0021 **出包**（演示观感最好），0047 不出包 |
| val | dentvoxel_0074 | 早停 + **阈值标定** | **不出包**（把选阈值用的数据打进 APK 会让演示看起来像在见过的数据上测精度） |
| test | dentvoxel_0101 | **唯一一次最终评估** + 奇偶校验夹具 | 出包（AC-08 基准就是它） |

测试例没有被训练、标定或模型选择触碰过。`index.json` 里对每个病例写明 `role`、精度、以及"为什么出/不出包"，页面上选例弹窗直接把这段说明显示给操作者（0021 那一条明确写着"参与过训练…不能拿它的分数当泛化指标"）。

### 4.3 为什么把数据内置而不是让用户自己挑 DICOM

AI-01 的验收（AC-08）要求**输入完全相同**：病例固定 ⇒ 特征张量固定 ⇒ 概率图固定 ⇒ 连通域编号固定，真机 dump 的 feat/prob 才能与主机夹具逐元素比对。让用户临时找数据就没有比对基线。

---

## 5. 模型与输入契约

```
Conv3d(6->8,k3,p1)+Relu+Conv3d(8->8,k3,p1)+Relu+Conv3d(8->2,k3,p1)+Softmax(axis=1)
输入 feat  float32 [1,6,96,96,64]     输出 prob float32 [1,2,96,96,64]     opset 17     3,474 参数
```

* **特征通道**：c0 = 抽稀归一化 HU；c1..c4 = 盒均值 r=1,2,4,8；c5 = 盒(r=4) − 盒(r=8) 带通。HU 归一：`clip[-1024,4095] -> (h+1024)/5120 - 0.5`。
* **体数据契约**：app 网格 192×192×128 @0.6 mm（体素 0.216 mm³）→ factor 2×2×2 抽稀 → 模型/掩膜网格 96×96×64 @1.2 mm（体素 **1.728 mm³**）。
* **轴序（关键）**：设备轴序 axis0=DICOM 列、axis1=DICOM 行、axis2=z（依据 `cbctdeal/CbctSeriesParser.cpp` 的 `vol->width=Columns` 与内存布局 `[depth][height][width]`）。3×3×3 卷积对该平面转置**不等价**，所以模型按设备轴序训练**并**按设备轴序导出；主机 canonical 数组（rows,cols,z）要 `transpose(0,2,1,3)` 才是设备张量。
* **阈值**：`0.49`，来自随模型交付的 `assets/models/teeth_cnn.json`（在验证集 0074 上扫 0.05→0.95 步长 0.01 取 Dice 最大点，不是拍脑袋的 0.5）。Kotlin 的 `DEFAULT_THRESHOLD=0.5` 只是规格文件缺失时的保守回退，且与 `AiConst::TOOTH_THRESHOLD` 同步。
* **掩膜判定**：`prob[1] > 0.49` 严格大于、float32 提升为 double 比较（`AiCore::thresholdMask`），**不是 argmax**（两者在 0101 上差 133 体素）。
* **连通域**：26 邻接、C 顺序线性扫描、LIFO 洪水填充、原始计数 <8 的连通域丢弃、按裁剪后体素数降序重编号 1..N；邻居表顺序固定，因此编号可跨端复现（但 C++ `std::sort` 非稳定，**尺寸相同的实例之间只保证多重集一致**，0101 上有 21/11/8 各两例并列）。

### 5.1 为什么不是 nnU-Net / 不是公开预训练权重

用户选的取数方式是"公开牙科 CBCT + **公开预训练权重**"。公开权重这一条**在本机未能落地**，如实记录：

* 本机装不上 torch / tensorflow / nnU-Net（无网络与包源），也没有可下载的 Dataset112_DentalSegmentator 权重导出件；
* 因此模型是**纯 numpy 手写前向/反向**训练的 3,474 参数小 CNN（有限差分梯度校验最大相对误差 8.48e-6 < 1e-4 通过；numpy vs ORT 在 0101 设备张量上 max|diff| 1.25e-6；DICOM 重读 feat max|diff| 0.0），导出为标准 ONNX opset 17，**推理链路仍是真正的 ONNX Runtime**；
* 结论：§6 的精度数字刻画的是**这个小模型**，不是公开 SOTA（nnU-Net 级架构 + 数百例数据可到 ~0.96 Dice）。把链路、契约、取证口径按 PRD 做对是本轮的交付物；精度缺口需要更多标注数据 + 更大架构来补。

---

## 6. 精度实测（PRD AI-01）

### 6.1 整块网格指标（测试例 dentvoxel_0101，未见过）

| 指标 | @0.5 | **@标定阈值 0.49** | PRD 目标 | 达标 |
|---|---|---|---|---|
| Dice | 0.58145 | **0.58285** | — | — |
| Precision | 0.64845 | **0.64521** | — | — |
| Recall | 0.52701 | **0.53147** | — | — |
| **F1** | 0.58145 | **0.58285** | **≥ 0.85** | **未达标** |
| F1（β=0.5，偏精确率） | — | 0.61873 | — | — |
| IoU | — | 0.41128 | — | — |

计数：tp 6,425 / fp 3,533 / fn 5,664；预测 9,958 体素，GT 12,089 体素，总 589,824 体素。
这些数字由**真机 dump 的夹具**反推（tp/fp/fn 逐位计数），与 `train2_full.json` 完全一致（`device_parity.json` 的门 `dice_agrees_with_train2_full_json`）。

参考：验证集 0074 @0.49 的 Dice 0.62246（precision 0.54091 / recall 0.73296），训练例 0021 的 Dice 0.6789 —— 三者同量级，说明**没有过拟合到某一例**，是模型容量本身的天花板。

### 6.2 逐实例普查（0101，24 个实例，9,935 体素）

| 实例 | 红体素 | 体积 | 实例 | 红体素 | 体积 |
|---|---|---|---|---|---|
| #1 | 4,927 | **8.514 cm³** | #5 | 407 | 0.703 cm³ |
| #2 | 2,855 | **4.933 cm³** | #6 | 207 | 0.358 cm³ |
| #3 | 586 | 1.013 cm³ | … | … | … |
| #4 | 479 | 0.828 cm³ | #24 | 8 | 0.014 cm³ |

一颗磨牙的解剖体积量级在 ~0.4–1.0 cm³。**#1 = 8.514 cm³、#2 = 4.933 cm³ 明显不是单颗牙**：前两名合计占全部预测牙齿体素的 **78%**，是把相邻多颗牙连成了一整块（over-merging）。0021 更极端：最大实例 12,587 红体素 = 该例全部牙齿体素的 **95%**（真机归档里这条记录写着 `coverageVoxels=100696`（app 网格）/ `scannedVoxels=1686672`，见 §8.3）。

> 数字口径：§6.1 的 9,958 是**阈值掩膜**的体素数，本表的 9,935 是**剔除 <8 体素碎块后**24 个实例的体素数之和，差的 23 个体素就是被丢弃的散点。两者都对，用途不同。

截图 `03_ai_mpr_overlay.png` 是这一现象的直接证据：VR 画面上 `AI#1 8.51cm3 4927vox` 的轮廓沿半个牙弓连成一条带，而不是包住一颗牙。

### 6.3 精度不达标的根因（不粉饰）

1. **训练数据只有 2 例**（0021 + 0047），2,048 次 Adam 更新、16³ patch、正:负损失质量 1:4。牙齿分割的类间差异（牙釉质/牙本质/骨小梁 HU 重叠）在 2 例上学不到不变量。
2. **模型容量 3,474 参数**、感受野只有 7³ 体素，没有下采样/多尺度分支 ⇒ 无法区分"紧贴的两颗牙"，于是连通域把它们连成一块。这直接解释了 §6.2 的 over-merging，也解释了为什么 Dice 卡在 0.58 而不是更低（大块的召回其实不差，错在边界与分牙）。
3. **阈值只救得了工作点，救不了分牙**：0.05→0.95 全扫的最大 Dice 就是 0.49 处的 0.5828。

### 6.4 对临床使用的直接影响（必须写进使用手册）

* 大实例（#1/#2）的 **M-04 体积、M-08 骨密度是"多颗牙的混合值"，不可用于单牙判断**；小实例（<~600 红体素）观感上接近单颗牙，但仍需肉眼核对叠加轮廓。
* AI-03 的"净间隙"取的是**相邻实例质心/长轴**之间的距离：实例被合并时间隙会虚高。真机 8 个候选里出现的 `上颌 5-6 间隙 88.1mm`、`上颌 6-9 间隙 48.8mm` 在解剖上不可能，就是这个成因（截图 `06_06_dental0021_recommend_dialog.png`）。**这类候选必须丢弃**，打分与判级本身仍按 S-02~S-06 正确执行（例：`上颌 5-6 … 骨高 3.9 / 骨宽 0.0 … 不安全；骨高不足，需植骨或改短桩`）。
* 掩膜轮廓是**逐层轮廓线叠加**，不做深度剔除，密集区会前后叠在一起（一期叠加层的既有限制）。

---

## 7. AC-08 主机/真机奇偶校验（本轮最硬的一条证据）

### 7.1 取证数据格式

真机点「导出取证数据」（内部走 `keepParity=true` 的推理）→ `AiEngine::dumpParity` 写 `filesDir/ai_parity/device_<case>.parity.bin`：

```
64 字节头 = 8 × int64 LE：featCount, probCount, labelBytes, modelDim0..2, redDim0..1
随后（按顺序）：
  float32 feat   3,538,944 个元素 = 14,155,776 B   （6×96×96×64）
  float32 prob   1,179,648 个元素 =  4,718,592 B   （2×96×96×64）
  uint8  label     589,824 个元素 =    589,824 B   （96×96×64）
  int16  inst      589,824 个元素 =  1,179,648 B   （96×96×64）
合计 20,643,904 B
```

注意单位：头里三个计数都是**元素个数**（`feat.size()` / `prob.size()` / `label.size()`），只有 label 因为是 uint8 才恰好等于字节数；写盘时 feat/prob 各 ×4、inst ×2。读文件时按元素数换算，别把三者当同一单位。

主机侧 `src/host/ai/scripts/check_device_dump.py`（numpy-only）按同一格式读，与 `parity_ref/device_*_0101.raw` 夹具比对。容差：`--feat-max-abs`（默认 0）、`--prob-max-abs` 1e-5、`--label-max-rate` 0.001、`--inst-require-exact`。脚本自检：用夹具造的假 dump 判 PASS(exit 0)，篡改后的判 FAIL(exit 2)。

### 7.2 真机 dump 实测结果（case 0101，`/tmp/dcmtk_verify/parity_verdict.txt`）

```
header: featCount=3538944 probCount=1179648 labelBytes=589824 modelDim=(96,96,64) redDim0/1=(96,96)
dump sha256[:12]=ddc22d9e5bb3 bytes=20643904
feat : bitwise=no  max|diff|=2.980e-08 differing=799/3538944
prob : bitwise=no  max|diff|=1.192e-07 differing=2887/1179648
label: bitwise=YES mismatch=0 voxels (0.000000% of 589824) ; device tooth voxels=9958
inst : bitwise=YES exact-match-rate=100.0000% mismatch=0
per-instance voxel counts (host -> device): 4927→4927, 2855→2855, 586→586, 479→479, 407→407,
  207→207, 123→123, 47→47, 44→44, 41→41, 35→35, 23→23, 21→21, 21→21, 19→19, 18→18,
  13→13, 12→12, 11→11, 11→11, 10→10, 9→9, 8→8, 8→8
host size multiset (top8 nonbg): [579889, 4927, 2855, 586, 479, 407, 207, 123]
device size multiset          : [579889, 4927, 2855, 586, 479, 407, 207, 123]
VERDICT: PASS - device dump matches host device-order fixtures within tolerances
```

**判读**：
* **label 逐位一致、inst 编号 100% 精确匹配** ⇒ 端上分割结果与主机**完全同一个掩膜**，24 个实例个体素不差。这是 AC-08 的实质结论。
* feat/prob 不是 memcmp 级一致，但差异在 **1 个 float32 ulp 量级**（feat 2.98e-08、799/3,538,944 个元素；prob 1.19e-07），且只出现在盒均值通道 —— 成因是浮点**加法顺序**不同（`AiCore.cpp` 里明确注释了这一点），不是算子实现差异。判定口径因此定为"max|diff| + label 一致率"，而不是逐字节相等。
* 环境差异如实记录：主机 onnxruntime **1.23.2**（python 3.13.0 / numpy 2.5.1 / scipy 1.18.1）vs 真机 **1.17.0**，同一 op set；主机侧两次运行逐位相同（CPUExecutionProvider 确定性）。

---

## 8. 性能与打包取证

### 8.1 PC-05 延迟（≤5 s）

两次独立采样都在 **dentvoxel_0021**（13 实例 / 13,225 牙齿体素）上取，一次来自 Native 日志、一次来自界面状态栏：

| 阶段 | 采样 A（`runSegment ok` 日志） | 采样 B（状态栏，截图 `08_*`） | 说明 |
|---|---|---|---|
| 预处理 prep | 658.7 ms | 669 ms | 抽稀 + 6 通道特征（盒均值 r=1/2/4/8 + 带通），是主要成本 |
| 推理 infer | 334.6 ms | 341 ms | ORT CPU EP，`SetIntraOpNumThreads=4`、inter=1（平板 8 核留一半给渲染/手势，护 PC-02） |
| 后处理 post | 8.5 ms | 9 ms | 阈值 + 26 邻接连通域 + 逐实例统计 |
| **total** | **1002.0 ms** | **1019 ms** | **达标**，余量约 5×；推理占 33.4% |

两例牙科数据的体数据契约完全相同（192×192×128 @0.6 mm → 96×96×64），所以 0101 的整例耗时与上表同量级；但**只有 0021 有逐次计时取证**，0101 那轮取的是掩膜/实例的比对证据（§7），没有单独记录分段耗时，这里不为它编数字。

```
D/CbctMeasureCore: runSegment ok: inst=13 toothVox=13225 thr=0.4900
                   prep=658.7ms infer=334.6ms post=8.5ms total=1002.0ms alloc=21.9MB
D/CbctMeasureCore: grid: app 192x192x128@0.600/0.600/0.600 -> model 96x96x64 factor 2x2x2 red 96x96x64
```

状态栏同步显示同一份数字（`新增 Native 内存约 21.9 MB；PC-05<=5s：达标`），报告与屏幕同源。

### 8.2 PC-05 体积（模型 + 运行时 ≤80 MB）

| 项 | 实测 |
|---|---|
| `libonnxruntime.so`（arm64-v8a，APK 内） | 16,033,712 B |
| `teeth_cnn.onnx` | 14,477 B |
| **合计** | **16.05 MB ≤ 80 MB** |
| 推理期新增 Native 内存（feat+prob+掩膜缓冲） | ≈21.9 MB（`alloc=21.9MB`） |
| 内置牙科 CBCT 资产 | 2 例 × 约 9.5 MB（数据，不计入"模型+运行时"） |

### 8.3 PC-01 测量计算（≤50 ms）

判定直接取自拉回主机的归档 JSON（`0021_measure.json`，每条记录的 `detail.elapsedMs` / `scannedVoxels`），不靠肉眼估计：

| 记录 | 遍历体素 | 覆盖体素 | `elapsedMs` |
|---|---|---|---|
| `AI 牙 L4(#1) 体积` | 1,686,672 | 100,696 | **178 ms** |
| `AI 牙 L4(#1) 骨密度` | 1,686,672 | 100,696 | **162 ms** |
| 其余 24 条（#2~#13 的体积/骨密度） | 480 ~ 9,180 | 64 ~ 2,172 | **0 ~ 1 ms** |

26 条里 **24 条 ≤50 ms（中位数 0 ms）**，超标的 2 条都属**同一个被合并出来的巨型实例 #1**（100,696 个 app 网格体素 = 12,587 个红网格体素，遍历量 168 万）。

* 定性：这是**大实例的必然代价**，不是回归。一期 R-02 的 145 万体素大盒在补 `-O2` 后是 21~23 ms，靠的是空间包围盒快速路径；R-06 掩膜 ROI 是稀疏体素集合，量化要按 app 网格逐体素累计角点权重（遍历量是覆盖量的 ~17 倍），目前没有等价的裁剪。
* 缓解手段（按实例包围盒分块 + 只扫非零体素）留待后续，本轮**如实记为超标点**。另注意 §6.4：这两条 178/162 ms 的行本身就是"多颗牙混合值"，临床上不该被采用 —— 超标与不可信是同一个根因（over-merging）的两个表现。

### 8.4 打包与 ABI

`:app` 新增 `ndk { abiFilters += "arm64-v8a" }`：本工程自研 `.so` 只编 arm64-v8a，而 onnxruntime AAR 自带 4 个 ABI，不收窄会在 APK 里多带约 **57 MB** 真机永远用不到的库，直接顶到 PC-05 的 80 MB 预算上。收窄后产物 `lib/arm64-v8a/` 单目录，APK 177,579,559 B（含 VTK/DCMTK/体数据资产），真机 `ro.product.cpu.abi` = arm64-v8a 一致。

### 8.5 内存（PC-04，一期口径）

AI 层不复制体数据（`bindVolume` 零拷贝引用），推理缓冲用完即释放；本轮 0021 全程 Pss 变化与一期基线同量级（700 MB 量级由体数据 + GL 决定，与测量/AI 层无关）。**AI 相关的可信数字是 `alloc=21.9MB` 这一次性推理峰值**，Pss 差异在噪声内，未宣称新的量化结论。

---

## 9. 缺陷与修复（本轮真机发现并修掉的）

### 9.1 缺陷 1：恢复归档后重跑「逐牙自动测量」产生双份记录 + 26 条「待重算」进临床报告

* **现象**：推理 → 逐牙自动测量（26 条）→ 保存归档 → 杀进程 → 恢复归档 → 再点一次「逐牙自动测量」。结果列表变成 **52 条**，其中 26 条显示「待重算」，并且这些行**被写进导出的 PDF**。
* **根因**（两处叠加）：
  1. `aiAutoMeasure()` 每次都 `addRoi()` 新建一条 R-06 掩膜 ROI，不复用同 `aiLabel` 的既有 ROI ⇒ ROI 也翻倍；
  2. 恢复归档时 `measuresFromJson()` 会"按当前体数据一律重算"（这条规则本身是对的：旧文件的数值可能来自不同 spacing 的序列），而掩膜网格**不在归档里**，`RoiExtractor` 对 AI 掩膜 ROI 返回 `"AI 掩膜 ROI 需要推理结果（实例 #N 不存在）"` ⇒ 新行数值失败 ⇒ 显示「待重算」。
* **修复**（`core/MeasurementManager.cpp::aiAutoMeasure`）：按 `ROI_AI_MASK + aiLabel` 查找既有 ROI，**命中则原位 `updateRoi`（id 不变，只刷新名称/配色），并先收集后删除**该 ROI 上 `note == "AI-01 自动分割"` 的旧测量行；手工挂在同一 ROI 上的测量不会被误删。新增常量 `AiConst::AUTO_NOTE` 作为归属标记。
* **真机证据（修复后）**：

```
D/CbctMeasureCore: aiAutoMeasure: 复用 ROI #25（aiLabel=9），清理旧自动测量 2 条
   ...（13 个实例各一条）
D/CbctMeasureCore: 13 实例 -> 26 条测量（R-06 掩膜 ROI 复用 M-04/M-08）
D/CBCT_MEASURE_STORE: loadAll key=...10021_...20021 ok=3/3
D/CbctMeasureCore: measures loaded: 26 records, 13 rois
D/CBCT_MEASURE_REPORT: report ok pages=2 measures=26 rois=13 implants=1 nerves=0 annos=0 evidence=true
                       -> .../reports/1.2.826.0.1.3680043.10.474.900000.10021_1790780403050.pdf
D/CBCT_MEASURE_REPORT: exportAll ... archive=3/3 pdf=true pages=2 sr=true verify=true cost=1712ms
```

* **产物级判定**（拉回主机用 pypdf 抽文本数「待重算」出现次数）：
  * 修复前 `...10021_1790778686728.pdf` → `待重算 count: 26`
  * 修复后 `...10021_1790780403050.pdf` → `待重算 count: 0`，`掩膜` 行 26 条，页数仍为 2
* **主机回归**：新增组 11（22 条断言）覆盖：首跑 4 条测量 / 2 ROI / 0 失败；M-04 体积 = 1.0 cm³；JSON 往返到新 manager 后仍 4 records + 2 ROIs 且 `aiLabel` 为 1/2、4 行全为待重算（这是掩膜网格不出档的既定行为）；重新接管 + 重测后仍 **2 ROI / 4 行 / 0 失败 / ROI id 不变**，且 `mgr2.ai().instances[0].roiId == oldRoiId`；手工行（`note "手工备注"`）存活（共 5 行）；`AiConst::AUTO_NOTE` 字面量被钉住（防止改文案把已归档的行变成孤儿）。总数 364 → **386，FAILED 0**。
* **写断言时踩到的坑（记档）**：`adoptAiResult` 是**按值拷贝**，回填 `roiId` 只发生在 manager 内部那份副本上，读调用方传进来的 `AiResult` 会恒为 0 —— 第一版断言因此 FAIL，改成读 `mgr2.ai().instances[0].roiId` 才是正确契约。

### 9.2 缺陷 2：两条日志会误导排查（已修 + 真机复验）

| 日志 | 问题 | 修法 |
|---|---|---|
| `ORT 就绪：in 'feat' [1,96,96,64,64]` 这类自相矛盾的假 shape | 只把原始 shape 的两个下标拼进一行，读的人以为模型是 5 维但第 3、4 维相同 | 分开打印「原始shape[1,6,96,96,64] -> 逻辑 C6 × X96 × Y96 × Z64」，并补 out rank 与通道 in/out |
| `grid: app 192192128 ...` | 三个维度之间没有分隔符，无法判断是哪三个数 | 改为 `app 192x192x128@0.600/0.600/0.600 -> model 96x96x64 factor 2x2x2 red 96x96x64` |

两条都在重新构建 + 安装后于真机复验（见 §3.1 与 §8.1 的日志原文）。

### 9.3 缺陷 3：`dlopen` 只试磁盘路径 ⇒ AI 永远"不可用"

AGP 默认打包下 `nativeLibraryDir/libonnxruntime.so` **不存在**，早期版本直接返回"找不到 libonnxruntime.so"。改为候选链 + 裸 SONAME 兜底 + 把真正命中的路径显示到状态栏（§3.1）。这是本轮 AI 层能跑起来的第一个必要条件。

---

## 10. 真机全局功能回归步骤（逐条留档）

设备连接：`ADB=$HOME/Library/Android/sdk/platform-tools/adb`（仓库内 app 未导 PATH）；坐标一律取自**当次** `uiautomator dump` 的 clickable 节点 bounds（见仓库技能 `android-adb-ui-regression-loop`）。

| # | 动作 | 判定依据（日志 / 产物） | 截图 |
|---|---|---|---|
| 1 | 「选择加载牙科 CBCT」→ 弹窗选 0101 → 自动解析 | `病例 dentvoxel_0101 已释放 128 张`、解析完成、三维画面出图 | `01_measure_dental_loaded.png` |
| 2 | 「装载模型」 | `dlopen ok: ...base.apk!/lib/arm64-v8a/libonnxruntime.so`、`ORT 就绪：in='feat' 原始shape[1,6,96,96,64]` | 同 `08_*` 状态栏 |
| 3 | 「牙齿自动分割」 | `runSegment ok: inst=… toothVox=… total=1002.0ms` | `02_ai_segment_done.png` |
| 4 | MPR/VR 下看掩膜叠加、勾/去「显示 AI 掩膜」 | `CbctMeasureOverlay` 的 `overlay fps/avgDraw/prims`；轮廓标注 `AI#1 8.51cm3 4927vox` | `03_ai_mpr_overlay.png`、`05_ai_mpr_vtk_mode.png` |
| 5 | 「逐牙自动测量」 | `13 实例 -> 26 条测量（R-06 掩膜 ROI 复用 M-04/M-08）`；列表出现 `#NN AI 掩膜 …` / 体积 / 骨密度 | `04_ai_auto_measure_list.png` |
| 6 | 「种植位点推荐」→ 点一条候选落一颗种植体 | `recommend: 8 candidates (minGap=5.0, arch0=9 arch1=4)`；`addImplant #40 entry=(28.9,75.7,64.0) pitch=4.9 yaw=-174.7 depth=10.9 dia=5.0`；行显示 `#40 AI推荐 上颌6-9（安全）骨高 12.9 / 骨宽 14.1 mm` | `06_06_dental0021_recommend_dialog.png` |
| 7 | 「保存归档」→ 杀进程 → 「恢复归档」 | `saveAll ok=3/3`、`loadAll key=…10021_…20021 ok=3/3`、`measures loaded: 26 records, 13 rois` | `07_07_dental0021_archive_restored.png` |
| 8 | 恢复后再点「逐牙自动测量」（§9.1 场景） | `复用 ROI #25（aiLabel=9），清理旧自动测量 2 条` ×13；仍 26 条 / 13 ROI | `09_09_after_fix_top_view.png` |
| 9 | 「导出取证数据」→ `adb run-as` 拉回 → 主机 checker | `VERDICT: PASS`（§7.2） | `/tmp/dcmtk_verify/device_0101.parity.bin`、`parity_verdict.txt` |
| 10 | 「导出报告 (PDF + DICOM SR)」 | `report ok pages=2 measures=26 rois=13 implants=1 … evidence=true`；`SR verified … ComprehensiveSR (valid) num=30 text=35 bytes=6309`；`exportAll … cost=1712ms`；拉回 PDF 抽文本 `待重算 count: 0` | `reports_out/*.pdf` |

**病例归属（避免误读为"一轮线性跑完"）**：步骤 1~5、9 在 **0101**（测试例，AC-08 基准）上做，`device_0101.parity.bin` 与 `...900000.10101_1790773091028.pdf` 是那一轮的产物；步骤 6~8、10 在 **0021** 上做（`recommend: 8 candidates`、`13 实例 -> 26 条测量`、`...10021_*.pdf` 都来自它）。§8.1 的两次分段计时均为 0021。

一期全量回归（AC-01~AC-10 / PC-01~PC-05）另见 [`TEST_REPORT.md`](TEST_REPORT.md)，本轮未回归的项沿用该文件的结论与"未充分验证"标注。

---

## 11. 已知限制与未验证项（如实清单）

1. **AI-01 精度不达标**：F1 0.5828 < PRD 0.85（§6）。
2. **实例过合并**：最大实例含多颗牙 ⇒ 其体积/骨密度/间隙不可用于单牙判断（§6.2、§6.4）。
3. **AI-02 / AI-04 未交付**（§2.2）。
4. **主机 ORT 1.23.2 vs 真机 1.17.0**：奇偶校验在跨版本下 PASS，但没有同版本对照；若未来升级设备端运行时，需重跑 §7 的取证。
5. **PC-01 超标 2 条**：巨型实例 #1 的掩膜量化 178 / 162 ms，其余 24 条 0~1 ms（§8.3）。
6. **掩膜网格不出档**：恢复归档后 AI 行需重跑一次推理/自动测量才有数值（现为原位复用，不产生双份）。
7. **S-05 神经距离仍依赖手工描记**：未描记神经管时推荐行显示 `神经距 -1.0`（= 未测量），不会伪造距离；AI-02 未交付 ⇒ 没有自动神经管分割。
8. **AI 按钮的可用性依赖解析成功**：`sessionHandle==0` 时点「装载模型」只 toast「请先解析一个 CBCT 序列」。
9. **一期遗留未验证项照旧**：PC-02 帧率（合成 swipe 事件密度不足）、PC-03 抓帧 GPU 读回、AC-10 双指注入、画面滚出可见区时证据图全黑无自动判空。
10. **日志缓冲已滚过**：§10 引用的 logcat 行是当轮实时抓取并记录的原文；本报告撰写时设备缓冲已无这些行（`logcat -d` 返回空），复现需按 §12 重跑。

---

## 12. 复现步骤

```bash
# 0) 主机侧纯 C++ 单测（不需要设备/不链 ORT/DCMTK/VTK）
cd cbctmeasure/src/host && ./build_and_run.sh
#   期望：RESULT: PASSED 386 / FAILED 0 / FINDINGS 4

# 1) 构建 + 安装 + 启动
./gradlew :app:assembleDebug --offline
ADB=$HOME/Library/Android/sdk/platform-tools/adb
$ADB install -r app/build/outputs/apk/debug/app-debug.apk
$ADB shell am start -n com.example.dcmtkdemo/.activity.MainActivity
$ADB logcat -c

# 2) 驱动页面（坐标必须取自当次 dump）
python3 .qoder/skills/android-adb-ui-regression-loop/scripts/nodes.py 选择加载牙科 CBCT --tap
python3 .qoder/skills/android-adb-ui-regression-loop/scripts/nodes.py 装载模型 --tap
python3 .qoder/skills/android-adb-ui-regression-loop/scripts/nodes.py 牙齿自动分割 --tap
python3 .qoder/skills/android-adb-ui-regression-loop/scripts/nodes.py 逐牙自动测量 --tap
python3 .qoder/skills/android-adb-ui-regression-loop/scripts/nodes.py 种植位点推荐 --tap
python3 .qoder/skills/android-adb-ui-regression-loop/scripts/nodes.py 导出取证数据 --tap
$ADB logcat -d -v brief | grep -E 'CbctMeasureAi|CbctMeasureCore|CBCT_MEASURE_STORE|CBCT_MEASURE_REPORT'

# 3) AC-08 取证：拉回真机 dump 并与主机夹具逐元素比对
$ADB shell run-as com.example.dcmtkdemo cat files/ai_parity/device_dentvoxel_0101.parity.bin \
  > /tmp/dcmtk_verify/device_0101.parity.bin
python3 cbctmeasure/src/host/ai/scripts/check_device_dump.py /tmp/dcmtk_verify/device_0101.parity.bin
#   期望末行：VERDICT: PASS - device dump matches host device-order fixtures within tolerances

# 4) 模型/数据重建（可选；需要重训时）
cd cbctmeasure/src/host/ai/scripts && ./run_all.sh   # 训练 -> 导出 -> 夹具 -> 指标
```

产物与证据目录：`/tmp/dcmtk_verify/`（截图 `01_*`~`09_*`、`device_0101.parity.bin`、`parity_verdict.txt`、`0021_measure.json`、`reports_out/*.pdf`）。

---

## 13. 建议的下一步（按性价比排序）

1. **补标注数据到 ≥30 例并换多尺度架构**（下采样 + 跳连），这是把 F1 从 0.58 拉到 PRD 门槛的唯一路径；链路、契约、取证脚本都不用改，只换 `teeth_cnn.onnx` 与 `teeth_cnn.json` 的阈值。
2. **分牙后处理**：以牙冠/牙根 HU 分层或按 PCA 长轴做种子分水岭，先把 over-merging 压下去 —— 即使模型不变，也能立刻让 #1/#2 的体积变得可读。
3. **R-06 掩膜量化按实例包围盒分块**，消掉 §8.3 的 178/162 ms 超标点。
4. **AI-02 的下牙槽神经管分割**：一旦有标注，S-05 就能从"必须手工描记"变成自动，安全判级才完整。
5. 归档里加**掩膜摘要**（每实例体素数 + 包围盒 + 一阶矩），使恢复归档后不必重跑推理也能重算体积（体素数不依赖体数据，可校验一致性）。
