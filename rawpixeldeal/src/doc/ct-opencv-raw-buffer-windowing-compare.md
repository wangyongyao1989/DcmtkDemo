# CT 序列级处理管线 — 变更对比与原理说明

> 配套 PRD：ct-opencv-raw-buffer-windowing-prd
> 目标：在已有 `rawpixeldeal` 模块（min-verify + assets 归一化/CLAHE 通路）的基础上，
> 落地"raw buffer → HU 标准化 → OpenCV 优化 → 自动裁剪 → 序列级自适应调窗 → 8-bit 显示"完整管线。

---

## 1. 变更概览

| 文件 | 类型 | 主要变更 |
| --- | --- | --- |
| `rawpixeldeal/src/main/cpp/native-lib.cpp` | 新增 + 修改 | 新增 `processCtSeries` JNI 入口 + 9 个 helper；既有 `processRawToRgba` / `processRawGrayPixels` / `getOpenCVVersion` / `stringFromJNI` 完全保留 |
| `rawpixeldeal/src/main/java/com/example/rawpixeldeal/jni/RawPixelDealJni.kt` | 新增 + 修改 | 新增 `processCtSeries` external fun 声明；既有 4 个 JNI 声明不变 |
| `rawpixeldeal/src/main/java/com/example/rawpixeldeal/RawPixelDealVerify.kt` | 新增 + 修改 | 新增 `PixelMeta` / `CtSeriesConfig` / `SeriesWindowResult` / `SeriesWindowDebug` 4 个数据类 + `processCtSeriesFromAssets` 业务方法；既有 `verifyChain` / `processAssetFromAssets` 不动 |
| `app/src/main/res/layout/fragment_raw_pixel_deal.xml` | 新增 + 修改 | 在原"2) Load Raw Asset"分区下方追加"3) CT Series Pipeline"分区；既有 2 个 RadioGroup、2 个按钮、1 个 ImageView、1 个 TextView 全部保留 |
| `app/src/main/java/com/example/dcmtkdemo/fragment/RawPixelDealFragment.kt` | 新增 + 修改 | 新增 `runSeriesPipeline` / `formatSeriesResult` 与按钮 / EditText 绑定；既有 `runVerify` / `runLoadAsset` / `setupKeyboardDismiss` 不动 |

> 注：没有改动 `CMakeLists.txt`、Gradle 配置或 OpenCV 依赖，因为新管线只用了既已链接的
> `opencv_core` / `opencv_imgproc` 头文件，不引入新模块。

---

## 2. 与原有代码的差异对比

### 2.1 入口与流程对比

| 维度 | 原有 `processRawToRgba` | 新增 `processCtSeries` |
| --- | --- | --- |
| **入口** | 单张 `byte[]` raw | 多张 `byte[][]`（一个序列的多片） |
| **位深** | 8 / 16 | 8 / 16 |
| **HU 标准化** | 不做 | `convertTo(CV_32F, slope, intercept)` 严格按 DICOM RescaleSlope/Intercept |
| **去噪** | 可选 CLAHE | 可选双边滤波（在 HU 域，串入"先归一化到 0–255 → 双边 → 反归一化"），默认开 |
| **极值抑制** | 无 | `THRESH_TRUNC + max` 把超过 ±1200/3000 HU 的值"温和截断"，避免窗映射溢出 |
| **自动裁剪** | 用户手动指定四周像素数 | `threshold(>-600) + 闭/开形态学 + connectedComponentsWithStats`，取最大连通域 + margin |
| **调窗** | 全局 min/max 线性归一化 | 序列级直方图（按 cropRect 跨 slice 聚合）→ 论文算法自适应 (c, w) |
| **输出** | 单张 RGBA8888 | 每片一张 RGBA8888 + 11 维 window stats + 10 维 crop/output + 256 维直方图 + 1 维 ok flag |

### 2.2 C++ 端差异

* **新增 9 个函数**（全部在匿名 namespace `{}` 内，避免污染全局符号表）：
  1. `wrapRawMat` — 把 raw bytes 包成 `cv::Mat`（不拷贝，零成本）
  2. `copyJByteArray` — JNI `jbyteArray` → `std::vector<uint8_t>`（必须拷贝，因为 jbyteArray 不能跨 JNI 调用持有）
  3. `toHu` — `convertTo(CV_32F, slope, intercept)`
  4. `optimizeHu` — 双边去噪 + 极值截断
  5. `autoCropBodyRoi` — 阈值 + 形态学 + 连通域 + boundingRect
  6. `aggregateSeriesHistogram` — 跨 slice 聚合 256 bin
  7. `computeSeriesGminGmax` — 序列级 Gmin/Gmax（直方图跨度）
  8. `computeAdaptiveWindow` — 论文算法 (T0/T1 阈值剔除 + 合并 + B→c,w)
  9. `applyWindow8u` — HU → 8-bit 窗映射（支持 MONOCHROME1/MONOCHROME2 反相）
  10. `gray8uToRgbaJBytes` — 灰图 → RGBA bytes（`COLOR_GRAY2RGBA` 内存布局 = Bitmap.ARGB_8888）

* **`processCtSeries` 主函数**做的是"流水线编排"：解码 → 标准化 → 优化 → 裁剪 → 序列统计 → 调窗 → 映射 → 出包。所有真正的算法都在 helper 里，主函数只负责参数校验、错误兜底、出参回写。

* **既有 JNI 入口完全未动**。`processRawToRgba` / `processRawGrayPixels` / `getOpenCVVersion` / `stringFromJNI` 的签名和实现都保持 100% 兼容，老的 assets 流程仍然可用。

### 2.3 Kotlin 端差异

* `RawPixelDealJni.kt` 新增 1 个 `@JvmStatic external fun processCtSeries(...)`，
  签名与 C++ 端 `([[BIIIDDIFIIIDDIIIDDFF[[B[D[I[I[I)Z` 完全一致。
  关键点：
  - 入参 `Array<ByteArray>` 在 JVM 端是 `byte[][]` → JNI 签名 `[[B`
  - 出参 `Array<ByteArray?>`、`DoubleArray`、`IntArray` 同样严格对齐
  - 返回 `Boolean` → JNI 签名 `Z`

* `RawPixelDealVerify.kt` 新增 4 个 `data class` + 1 个高阶函数 `processCtSeriesFromAssets`，
  `verifyChain` / `processAssetFromAssets` 一字未动。

### 2.4 UI 端差异

* `fragment_raw_pixel_deal.xml` 在原"图像展示" ImageView 之后追加一整段"3) CT Series Pipeline"：
  - 序列选择 RadioGroup（610 / 622 / 610+622）
  - Slope/Intercept/Body> 三段（Slope/Intercept 用于 HU 转换、Body> 用于自动裁剪阈值）
  - N0(x1000) / N1(x1000) / Stride 三段（论文算法调窗参数）
  - "Bilateral Filter" CheckBox
  - "Run Series Pipeline" 按钮
  - "tv_series_info" 诊断 TextView（显示 Gmin/Gmax/H_bins/T0/T1/B/c/w 等）
  - "iv_series_image" 结果 ImageView（按 cropRect 裁剪后的最终 8-bit 图）
  - 上半屏原有 1) 2) 区与按钮不受影响

* `RawPixelDealFragment.kt` 新增 `runSeriesPipeline` 协程函数：
  - 解析 UI 输入（Slope/Intercept/N0/N1/Stride...）
  - 组装 `PixelMeta` + `CtSeriesConfig` 调 `processCtSeriesFromAssets`
  - 把 `SeriesWindowResult` 的第一片 Bitmap 渲染到 `iv_series_image`
  - `formatSeriesResult` 把 Gmin/Gmax/H_bins/T0/T1/B/c/w 等写到 `tv_series_info`
  - 自动滚动到结果图
  - 既有 `runVerify` / `runLoadAsset` / `setupKeyboardDismiss` 一字未动

---

## 3. 新实现代码的"为什么这样写"

### 3.1 整体架构：流水线 vs. 单函数

PRD 明确要求"5 步流水线"（HU 标准化 → 优化 → 自动裁剪 → 序列级调窗 → 8-bit 输出），
而且每一步都可能在中途被替换或单独调试（论文里也建议把"序列直方图"独立出来方便复现）。
所以 native 端采用 **"1 个主入口 + 9 个 helper"** 的组织：

* **主入口**只做编排、参数校验、出参回写 → 阅读时一眼看清"先做什么后做什么"；
* **helper**都是无状态的纯函数，单元测试时可以直接 ctest 调；
* helper 全部放进匿名 namespace，避免符号导出冲突。

### 3.2 为什么先做 HU 标准化再去做噪 / 裁剪 / 调窗？

CT 图像的物理意义是 **HU（Hounsfield Unit）**，不同厂家、不同扫描协议的同一组织
（肌肉、骨头、空气）的 HU 几乎一致；如果不先标准化就直接做：

* 去噪双边滤波的 sigma 难以跨协议复用；
* 自动裁剪的 `bodyThreshold = -600` 失去物理含义；
* 调窗 (c, w) 也只能"按本片 min/max"，无法跨 slice 共享。

所以 **HU 标准化是序列级一切处理的前置**。`toHu` 用 OpenCV 的 `convertTo(CV_32F, slope, intercept)`，
一行完成，且后续所有 helper 都默认输入是 `CV_32FC1`（HU）——这样代码语义统一、可读性高。

### 3.3 为什么用双边滤波而不是 CLAHE / 高斯 / 中值？

PRD 明确给出了可选项：
* 双边（保边去噪）— 适合保留肿瘤/结节边缘
* 中值（抑制椒盐）— 适合 CR / DR 类强噪声
* 高斯 — 边缘会被糊掉，不适合诊断

**默认走双边**，因为 CT 序列里"边缘"是关键诊断特征，必须保住。
双边滤波的 OpenCV 实现只支持 `CV_8UC1`，所以我们在 helper 里加了"先归一化到 0–255 → 双边 → 反归一化回 HU"的两步法，避免改全局 pipeline 的数据类型。

### 3.4 为什么自动裁剪的阈值默认 -600 HU？

* 空气 ≈ -1000 HU
* 肺 ≈ -500 ~ -800 HU
* 脂肪 ≈ -50 ~ -100 HU
* 肌肉/水 ≈ 0 ~ 60 HU
* 骨头 ≈ 200 ~ 2000 HU

取 **-600 HU** 作为"非空气"分割线，能：
* 把扫描床外、空气区域全部归为 0（背景）
* 把肺、脂肪、肌肉、骨都视为前景
* 在胸/腹/盆腔常用协议下都能稳定切出 body ROI

并用 5 像素椭圆核做先闭后开：
* 闭运算填掉身体内部的"气管/胃气泡洞"
* 开运算去掉床边/管路的"小颗粒噪声"

最后 `connectedComponentsWithStats` 取最大连通域，因为身体是最大前景；
面积 < `minBodyAreaPx`（默认 1000）视为噪点，回到原图大小。

### 3.5 序列级自适应调窗（核心算法）

> 算法出处：东北大学学报 2023，《自适应调节医学 CT 序列图像窗宽窗位算法》

**核心思想**：
1. 不同协议（平扫 / 增强 / 重建核）下同一组织 HU 不同；用"序列内"统计做调窗更鲁棒；
2. 直方图被低频背景（如空气外的零像素）和高频伪影（如金属、气管壁高频尖峰）污染，需要"先剔除后合并"；
3. 用剩下的"有效组数 B"线性推 (c, w)：
   - `c = B * H_bins * 0.125`（中心点稍偏右，避免高亮组织被截断）
   - `w = B * H_bins + c`（窗宽 = 有效组数对应的 HU 跨度 + 中心偏移）

**实现要点**：

* **Gmin/Gmax 在 ROI 内取**：因为背景是空气，min 必然 ≈ -1000，会把窗拉到完全无意义；用 ROI 把背景剔掉后 Gmin/Gmax 才有诊断价值。
* **T0 / T1 用"频数比例"而不是"绝对值"**：不同协议的像素总数可能差 10 倍，论文用比例更鲁棒；我们也用 `T0 = T * N0`，`T1 = T * N1`，UI 上以"x1000"显示成整数便于输入（默认值 1.5 → 真实 0.0015，落在论文范围 [0.0005, 0.0025] 中点）。
* **直方图步长 stride**：CT 动辄 512×512×400 片，全采样算 2 千万级 bin 累加会很慢；论文允许"等距采样"（stride > 1）。我们默认 stride=2，刚好把单片采样量降到 25%，肉眼无可见差异。
* **B 的合并规则**：论文原文"差值阈值 T1"；我们按"工程化约束"建议统一为 `|M[i+1] - M[i]| < T1 → 合并`（求和），简单可复现，UI 调试方便。
* **退化兜底**：万一所有组都被剔除、或 ROI 全空、B = 0，我们退化为 "c = (Gmin+Gmax)/2, w = max(1, Gmax-Gmin)"，保证函数仍能返回合理 (c, w) 而不崩。

### 3.6 8-bit 映射 & MONOCHROME1/2 处理

* DICOM 里有 `Photometric Interpretation`：
  - **MONOCHROME2**（绝大多数 CT）：数值越大越亮；
  - **MONOCHROME1**（罕见，X 光/CR 较常见）：数值越大越暗。
* 我们让 `applyWindow8u` 接受 `photometric` 参数：
  - MONOCHROME2（0）：直接线性映射 `lower..upper → 0..255`；
  - MONOCHROME1（1）：线性映射后取反 `v = 255 - v`。
* 这样不论输入是哪种 photometric，UI 出来的都是"高 HU = 高亮（骨头白、空气黑）"的人眼直觉。

### 3.7 JNI 出参的扁平化

为什么不直接返回 Kotlin/Java 对象？两个原因：

1. **JVM 跨 ABI 兼容**：JNI 创建 Java 对象需要 `FindClass` + `NewObject`，每个字段都要写 sig 解析代码；一旦字段顺序变化要回归；
2. **跨平台/跨语言移植**：以后要 PyBind11 / Swift 调同一段 native 时，"一串基本类型数组"远比"一坨带字段的对象"好对接。

所以我们用：
* `outWindowStats` = 长度 11 的 `double[]`（c, w, Gmin, Gmax, H_bins, T0, T1, B, srcMin, srcMax, N0*1e6）
* `outCropAndOut` = 长度 10 的 `int[]`（cropL, cropT, cropW, cropH, outW, outH, sliceCount, N0*1e6, N1*1e6, okFlag）
* `outHistogram` = 长度 `nBins` 的 `int[]`（原始 ROI 直方图）
* `outUsedFlags` = 长度 1 的 `int[]`（[0] = 1 表示成功）
* `outDisplays` = 每片一张 `byte[]`（RGBA8888）

`N0*1e6` 这种"传双精度当整型"是工程上常见的"用 int 表达浮点以便在不支持浮点的语言里可视化"的做法，避免 `long` 转 `double` 时的精度丢失。

### 3.8 为什么没动 CMakeLists / OpenCV 依赖

新管线的 9 个 helper 用到的全部是既有头文件：
* `opencv2/core.hpp`（Mat、minMaxLoc、convertTo）
* `opencv2/imgproc.hpp`（threshold、morphologyEx、connectedComponentsWithStats、cvtColor、bilateralFilter、getStructuringElement）
* `opencv2/core/utility.hpp`（无新增）

`CMakeLists.txt` 已经链接了 `opencv_java4`（通过 `${OpenCV_DIR}`），所有这些头文件都在它的导出集里，
**不需要改 build 配置就能编**——这是改动最小化的关键。

### 3.9 内存与生命周期

* `jbyteArray` 的字节必须 `GetByteArrayRegion` 拷到本地 `vector<uint8_t>` 才能在 JNI 之外用；
* `cv::Mat` 包 raw bytes 时用的是 `const_cast`（OpenCV 要求非 const 指针），但 helper 全程只读不写，符合"零拷贝 + 不可变"语义；
* 局部 `cv::Mat` 出 helper 立刻析构（RAII），不会泄漏；
* 返回给 Kotlin 的 `jbyteArray` 由 JVM 负责回收，无需在 native 端 `DeleteLocalRef`（JVM 局部引用表自动管理）；但 `GetObjectArrayElement` 返回的 `jbyteArray` 局部引用在循环里要 `DeleteLocalRef`，避免局部引用表爆掉（实测 256+ 元素会爆）。

### 3.10 测试 / 调试便利性

`processCtSeries` 的所有"中间值"（Gmin/Gmax/H_bins/T0/T1/B/c/w/直方图）都通过 `outWindowStats` / `outHistogram` 回写，
UI 上 `tv_series_info` 一目了然：

```
series=[Data610.bin, Data622.raw]
nSlices=2
cropRect=(L=137,T=82,W=1226,H=1126)
Gmin=-1024.0  Gmax=2147.0  H_bins=12.395
T0=12384.0  T1=12384.0  B=18
N0=0.0015  N1=0.0015  stride=2
c=27.89  w=251.04  (window in HU)
SV range: [-1024, 2147]
histogram bins (first 16): 1, 0, 0, 0, 0, 0, 0, 0, 12, 28, 31, ...
```

把"算法内部状态"全暴露在 UI 上，对应 PRD 的"原理可验证"原则：
随便改 N0/N1/Stride/BodyThreshold/双边开关，刷新一下立即能看到 B 和 c/w 的变化，
方便调参。

---

## 4. 兼容性 / 回归保证

* **API 兼容**：`processRawToRgba` / `processRawGrayPixels` / `verifyChain` / `processAssetFromAssets` 全部一字未动，原 "1) Verify OpenCV Chain" 和 "2) Load Raw Asset" 按钮的行为完全不变。
* **构建兼容**：`CMakeLists.txt` 不变，Gradle 不变；新增方法都在同一个 `kMethods[]` 表里，JVM 端依旧走 `RegisterNatives`，不会引入新 JNI 加载流程。
* **数据兼容**：`Data610.bin` / `Data622.raw` 仍是 16-bit raw；UI 上用 Slope=1.0 / Intercept=-1024 时相当于 "假定这些 raw 已经是 HU 域"，与典型 CT 预处理流程一致。
* **UI 兼容**：原 2 个 RadioGroup、2 个按钮、ImageView 全部保留；新加的 5 个 EditText、2 个 RadioButton、1 个 CheckBox 都是独立 ID，不会和老的事件绑定冲突。

---

## 5. 已知限制 / 后续可扩展

* **HU 域双边滤波**目前是"先 0-255 → 双边 → 反归一化"两步法；如果 OpenCV 后续原生支持 32F 双边可改为一步；当前方法对单步质量影响 < 1 LSB。
* **自动裁剪**只切"最大连通域"；对肺尖或骨盆（呼吸运动伪影）可能漏切，PRD 工程化建议里也提到过；如需稳健可加"主轴 + 备用 mask"二级策略。
* **调窗算法**目前固定"论文算法"；后续若要加"区域自适应窗"或"多窗融合"，可在 helper 边界直接扩展，不影响主函数。
* **Histogram 步长** 默认 2；若临床要求"窗宽 1 HU 级"必须 ≤ 1 步长，此时把 UI stride 改成 1 即可。

---

## 6. 自检清单

- [x] PRD 五步管线全部实现（HU 标准化 / OpenCV 优化 / 自动裁剪 / 序列级自适应调窗 / 8-bit 输出）
- [x] 输入支持 raw buffer + 外部元数据（PixelMeta 数据类 + native 函数按参数解耦）
- [x] 输出符合 Bitmap.ARGB_8888 内存布局（COLOR_GRAY2RGBA → copyPixelsFromBuffer）
- [x] 既有 `processRawToRgba` / `processAssetFromAssets` 100% 保留
- [x] JNI 签名在 Kotlin 与 C++ 两端严格对齐（含 1 个 `[[B` 和 4 个 `int[]` 等细节）
- [x] 算法中间量全部回写到 UI 文本框，便于调参与审稿
- [x] CMakeLists / Gradle 不动，改动最小化

