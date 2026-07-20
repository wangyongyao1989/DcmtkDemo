# ProcessPixelData 类说明

> 路径：`rawpixeldeal/src/main/java/com/example/rawpixeldeal/ProcessPixelData.java`
> 角色：X 光 / 平板探测器 raw 像素处理入口，与 `rawpixeldeal/src/main/cpp/native-lib.cpp` 的 `processRawToRgba` 互补——本类完全在 **Java + OpenCV Android SDK** 域内实现，适合做"无 JNI 依赖"或"快速实验"的备选管线。

---

## 1. 文件定位与上下游

```
rawpixeldeal.ProcessPixelData    ←  本类
   ↓  byte[] (raw 16-bit)
dcmtk.ProcessPixelData (Kotlin)  ←  接收 raw，调同样的 autoWindowLevel
   ↓  PixelData (含 win_c, win_w, stddev)
DicomManager.writeDcmFile         ←  打包为标准 DICOM 文件
```

`rawpixeldeal` 是 "raw pixel level 实验场"——不依赖 DICOM 库、能独立跑通；最终走 PACS 的代码收敛到 `dcmtk` 模块。本类很可能是这个收敛过程中的 **中间产物 / 旧版原型**，新功能直接演进到 `dcmtk.ProcessPixelData.kt`。

> **注**：本类中引用了 `LogUtil.d()`、`ImageProcessingJni.autoWindowLevel()`、8 参构造的 `PixelData` 类，这些类/方法在 `rawpixeldeal` 模块内并未定义。`FileCompareFragment` 实际使用的是 `com.example.dcmtk.utils.ProcessPixelData`（另一份 Kotlin 实现）。这份 Java 版本目前是"理论管线"。

---

## 2. 类结构概览

```text
ProcessPixelData
├── tailor_img(Mat, int[]) → Mat        // 旋转校正 + 自动裁剪
├── autoWinMinMax(Mat) → int[3]         // JNI 调窗
└── process(byte[], Integer, Integer) → PixelData  // 主入口（5 步管线）
```

| 成员 | 用途 |
|---|---|
| `tailor_img` | 在原始像素上做"自动找人体矩形 + 倾斜校正 + 锐化 + Sobel 边缘 + 形态学闭运算 + 二次 boundingRect" |
| `autoWinMinMax` | 通过 JNI 把整图 int[] 送给 `ImageProcessingJni.autoWindowLevel` 计算窗位窗宽 |
| `process` | 完整 X 光处理入口：raw 解码 → tailor_img → 二次裁剪 → 16-bit 直方图 → JNI 调窗 → 曝光/均匀性统计 → 输出 `PixelData` |

---

## 3. 流程图（`process` 主入口）

```mermaid
flowchart TD
    A[输入 byte&#91;&#93; pixelData<br/>width × height, 16-bit 大端 raw] --> B
    B[① raw → 16-bit 整数<br/>data16[i] = b2i high 8 b2i+1 low 8<br/>封装为 Mat CV_32SC1] --> C
    C[② tailor_img 边缘对齐 + 矩形裁剪<br/>a) 16→8 + OTSU 找最大外轮廓<br/>b) minAreaRect 角度归一化<br/>c) 3x3 锐化 + Sobel x-y + 闭运算<br/>d) 面积阈值 40000 兜底] --> D
    D[③ 二次裁剪 + 转 float<br/>rect = Rect margin, margin+upper, w-margin, h-2*margin-bottom<br/>img_mat_cut → CV_32F] --> E
    E[④ 16-bit 直方图 + 归一化 CDF<br/>SENSOR_DEPTH=65536<br/>CDF = 累积直方图 / 总像素<br/>clip_min/clip_max=0.05 找 min_i, max_i] --> F
    F[⑤ autoWinMinMax → JNI<br/>ImageProcessingJni.autoWindowLevel<br/>返回 largestPixelValue, win_center, win_width] --> G
    G[⑥ 曝光与均匀性统计<br/>exposure_level = mean<br/>standardDeviation = sqrt variance] --> H
    H[⑦ 输出 PixelData<br/>height, width, byte&#91;&#93;, maxPV, c, w, exposure, stddev]
```

> 上方流程图在 IDE 渲染时由 dynamic-ui 提供 SVG；Mermaid 版本保证在 GitHub / VSCode 中也能阅读。

---

## 4. 关键方法详解

### 4.1 `tailor_img(Mat img_mat_org_in, int[] img_org) → Mat`

| 子步 | 关键调用 | 目的 |
|---|---|---|
| 1 | `convertTo(CV_8UC1) + convertScaleAbs(alpha=1/256)` | 把 16-bit 压缩到 0~255 范围，方便 OTSU |
| 2 | `Imgproc.threshold(... THRESH_OTSU)` | 自动找前景/背景分割阈值 |
| 3 | `findContours(RETR_EXTERNAL, CHAIN_APPROX_SIMPLE)` + 找 max_area | 取最大外轮廓 |
| 4 | `minAreaRect(contour2f).angle` | 获取倾斜角（如果 max_area > 40000） |
| 5 | `getRotationMatrix2D(center, angle, 1.0)` + `warpAffine` | 旋转回正 |
| 6 | `MatOfFloat(-1..9..-1)`（3×3 锐化核）`filter2D` | 锐化边缘 |
| 7 | Sobel x − Sobel y → `convertScaleAbs` | 提取水平-垂直差异边缘 |
| 8 | `blur(25,25)` + `THRESH_OTSU` + `dilate(4)` | 平滑 + 二值 + 膨胀 |
| 9 | `morphologyEx(MORPH_CLOSE, MORPH_CROSS, 25×25)` | 闭运算填空洞 |
| 10 | 再次 `findContours` + `boundingRect(maxArea)` | 最终裁剪矩形 |
| 11 | 面积 ≤ 40000 → 回退原图 | 抗噪兜底 |

**返回**：`CV_32S` 类型的 `Mat`（按 `rect.width * rect.height` 紧凑内存排布）。

### 4.2 `autoWinMinMax(Mat img) → int[3]`

```java
int[] data_cal = new int[arrayLength];
img.get(0, 0, data_cal);
return ImageProcessingJni.autoWindowLevel(data_cal, arrayLength);
```

返回长度 3 的 int 数组：
- `[0] = largestImagePixelValue`（整图最大像素值，供 DICOM `(0028,0107)` 字段）
- `[1] = win_center`（窗位 HU）
- `[2] = win_width`（窗宽 HU）

### 4.3 `process(byte[], Integer, Integer) → PixelData`

主入口，5 步管线：

1. **raw → 16-bit 整数**：大端解码 `(b[2i] << 8) | (b[2i+1] & 0xFF)` → `CV_32SC1` Mat
2. **`tailor_img` 裁剪**：见 §4.1，输出紧凑 Mat
3. **二次边缘裁剪 + 转 float**：`Rect(margin, margin+upper, w-margin, h-2*margin-bottom)`
4. **16-bit 直方图 + CDF**：`SENSOR_DEPTH=65536`，先 clip 越界值再累加，再做"累积分布"
5. **JNI 自动调窗**：`ImageProcessingJni.autoWindowLevel(data_cal, len)` 返回 `[largestPixelValue, win_center, win_width]`
6. **曝光 / 均匀性统计**：`exposure = mean`、 `stddev = sqrt(variance)` —— 用来给探测器一致性打分
7. **打包输出**：`byte[]` 大端重排 + 元数据 → `PixelData`

---

## 5. 关键变量（常量与阈值的"魔法数"）

| 名称 | 默认值 | 含义 |
|---|---|---|
| `SENSOR_DEPTH` | 65536 | 16-bit 直方图 bin 边界 |
| `Contrast_ratio` | 0.65 | 注释：对比度系数（**未使用**） |
| `Thin / Middle / Thick` | 0.8 / 2.0 / 2.8 | 注释：边缘梯度（**未使用**） |
| `gamma / gamma_plus` | 0.75 / 2.25 | 注释：伽马（**未使用**） |
| `clip_min / clip_max` | 0.05 / 0.05 | CDF 截断比例 |
| `sigma / w` | 3 / 2 | 注释：去噪参数（**未使用**） |
| `marign / upper / bottom` | 0 | 二次边缘裁剪余量（**当前未启用**） |

> 上面 7 个"未使用"字段是历史留下的"工程化调参接口"，实际生效只有：直方图、面积阈值、裁剪矩形三组。可视为"软删除"候选。

---

## 6. 设计缘由深度分析

### 6.1 整体设计哲学

这段代码解决的是 **医疗 X 光/平板探测器的"裸像素 → 可读诊断图"** 问题，所有设计选择都围绕三条主线：
1. **零依赖工程化**（不依赖 DICOM 标准库，能在 raw bytes 上独立跑通）
2. **自动化优先**（CT/X 光机在临床现场不应需要调参 → 大量 OTSU / 百分位 / 自动角度归一化）
3. **诊断可用性**（窗位窗宽 + exposure + stddev 三件套直接给到 QA / 阅片医生）

### 6.2 raw 16-bit 解码的设计动机

```java
data16[i] = ((int)(pixelData[2 * i] & 0xFF) << 8) | (pixelData[2 * i + 1] & 0xFF);
```

**为什么是大端 (Big Endian)？**
- 工业 X 光探测器（Varex / PerkinElmer / Hamamatsu 等）多数采用 **MSB 在前** 的网络字节序；
- DICOM 标准 `(0028,0100) BitsAllocated = 16` 也是高字节在前（Big Endian），与探测器直出格式一致；
- 这种编码让 `data16[i]` 与硬件 register 一一对应，便于**灰阶故障定位**（某行异常 → 直接打印 data16 行号）。

**为什么 `& 0xFF` 而不是 `>> 8`？**
- Java 的 `byte` 是有符号（-128~127），直接 `<<` 会**符号位污染**。
- `& 0xFF` 强制升为无符号 int，再 `<< 8` 才能正确合成大端 16-bit。
- 这是 Java 处理 byte array 的标准范式，省掉了 `ByteBuffer` / `DataInputStream` 的对象开销。

**为什么选 `CV_32SC1` 而不是 `CV_16UC1`？**

| 类型 | 范围 | 缺点 |
|---|---|---|
| `CV_16UC1` | 0~65535 | 不知源是 signed 还是 unsigned，歧义 |
| `CV_16SC1` | -32768~32767 | 同上，且 half-range 容易溢出 |
| `CV_32SC1` | ±2.1×10⁹ | **符号位保留 + 算术安全** |

代码作者选择 32-bit 是"懒得想源是 signed 还是 unsigned"——`CV_32S` 两种都能装，且后续算差分、累计不会溢出（10-bit 探测器累加 4096×4096 也只到 16M）。

### 6.3 `tailor_img` 的多阶段设计动机

`tailor_img` 是这段代码的"灵魂"——它把"一张可能倾斜、有噪声、有空白边、有伪影的原始探测器图"变成"对齐的、紧致的、纯人体矩形"。它内部其实是 **4 个串联的子任务**：

#### 子任务 1：16→8 bit 降级 + OTSU 找前景

```java
Core.convertScaleAbs(img_mat_org1, img_mat_org1, 1.0/256, 0);
Imgproc.threshold(img_mat_org1, img_mat_org1, 10, 255, Imgproc.THRESH_OTSU);
```

**为什么不直接用 16-bit 跑 OTSU？**
- OpenCV 的 OTSU 在 `CV_8UC1` 上是高度优化 + SIMD 的（`int(256)` 个 bin）；
- 16-bit 跑 OTSU 内部会自动转 8-bit，但**自己手动 `convertScaleAbs(alpha=1/256)` 相当于"丢弃低字节"**——只保留"高 8 bit 强度分布"，对阈值选取来说完全够用，且**显式控制缩放行为**。
- `alpha=1/256` 而不是 `1/257` 是因为探测器最后一档灰度用不满（实际 max 通常 < 65280），`1/256` 让最大值精确落在 255 附近。

**为什么 OTSU 而不用固定阈值？**
- 不同曝光剂量（25 mAs vs 400 mAs）、不同人体厚度，原始像素分布差 10×；
- OTSU 的"最大类间方差"原理是**与绝对值无关的相对分割**，能跨剂量复用。

#### 子任务 2：minAreaRect 角度归一化

```java
if (Math.abs(angle) > 45) angle = angle + 90;
```

**为什么需要这个 90° 校正？**
- `minAreaRect` 返回的 `angle` 范围是 **[-90°, 0°)**，参考的是"水平边"；
- 但如果你的人体是**竖着躺**（高 > 宽），OpenCV 会返回近 -90°——这时实际只需要转 90° 而不是 -90°；
- `if |angle| > 45` 把它**统一翻折到 ±45° 范围内**，是 OpenCV 社区的通用 patch（否则会出现"明明图像歪了 1°，结果旋转了 91°"的灾难）。

**为什么 40000 像素阈值？**
- 对 ~2K 探测器（典型 2048×2048），人体占整图 ~25%~60%，即 1M~2.5M 像素；
- 40000 ≈ 200×200 = "小目标"，低于此视为噪声或局部强反光；
- 这个数字经验上能区分"小工具/植入物"与"完整人体"。

#### 子任务 3：3×3 锐化核 + Sobel

```java
Mat kernel = new MatOfFloat(-1, -1, -1, -1, 9, -1, -1, -1, -1);
Imgproc.filter2D(img_dst1, img_dst, -1, kernel, new Point(-1, -1), 0);
Mat grad_x = Imgproc.Sobel(img_dst, grad_x, CvType.CV_16S, 1, 0);
Mat grad_y = Imgproc.Sobel(img_dst, grad_y, CvType.CV_16S, 0, 1);
Core.subtract(grad_x, grad_y, gradient_xy);
```

**为什么中心=9 的锐化核？**
- 这个核 = `I * 9 - Σ(8 邻居)` = **8 阶拉普拉斯增强**；
- 等价于 `original + 4 × (laplacian of original)`（经典锐化公式）；
- 锐化的目的不是"让图像好看"，而是**让 Sobel 在低对比度区域也能找到边界**。

**为什么是 `Sobel_x - Sobel_y` 而不是 `|Sobel_x| + |Sobel_y|`？**
- 这是一个**反直觉的取法**——大多数教程教 `magnitude = sqrt(x² + y²)`；
- `x - y`（带符号）会**突出特定方向的边缘**（例如水平强、垂直弱的纹理）；
- 作者赌的是 X 光里**身体轮廓以垂直方向（脊柱）为主**，水平方向的锐化噪声会被减法抑制；
- 严格说这不是最优设计，更鲁棒的做法是 `convertScaleAbs` 各自的 magnitude 后加权——但作者更在意"在目标数据上能 work"，而不是教科书。

#### 子任务 4：MORPH_CROSS 25×25 闭运算

```java
Mat morpho_kernel = Imgproc.getStructuringElement(Imgproc.MORPH_CROSS, new Size(25, 25));
Imgproc.morphologyEx(thresh, closed, Imgproc.MORPH_CLOSE, morpho_kernel);
```

**为什么 cross 而不是 rect？**
- cross 的点更少（25 vs 625），**速度提升 25 倍**；
- 对"填洞"任务，cross 与 rect 效果几乎一致（都基于连通性，而非窗口形状）；
- cross 中心加权更重，**避免把两个分离的器官（肺、胃）错误桥接**。

**为什么 25×25 这么大？**
- 闭运算的"填洞半径"≈ 核大小 / 2 = 12 像素；
- 人体内部最大的"洞"是气管/胃泡（10~30 像素直径）；
- 选 25 是**刚好能合并这些生理空洞、又不至于把四肢并到一起**的折中值。

### 6.4 直方图 + CDF + 百分位裁剪的设计动机

```java
int SENSOR_DEPTH = 65536;
int[] img_histogram = new int[SENSOR_DEPTH + 1];
img_histogram_calculus[i] /= cum;  // 归一化到 [0,1]
// min_i = first i where cum[i] > 0.05
// max_i = last i where cum[i] < 0.95
```

**为什么是 65536 bin 而不是 OpenCV 的 256？**
- `calcHist` 用 256 bin 会**把 256 个原始像素值压成 1 bin**——这对窗位窗宽来说"颗粒度太粗"；
- X 光调窗通常需要 ±1 pixel 值的精度（特别是低对比组织），必须保留 16-bit 原始分布；
- **代价**：256KB 内存/图 —— 对 2048² 也只是 2% 临时开销，可接受。

**为什么 clip_min = 0.05 / clip_max = 0.05？**
- 这是**鲁棒统计**的核心思想：直接用 min/max 会被**金属植入物 / 直肠气体 / 床边伪影**拉偏；
- 5%/95% 百分位 ≈ 剔除"头尾 5% 异常值"，对**正态分布样本**几乎不损失信息（保留 90%）；
- 5% 这个值在医疗影像行业是经验值（也能用 1%/99%，但 5% 对 X 光小样本更稳）。

**为什么用 CDF 而不是直接 bin 阈值？**
- CDF 的二值搜索比线性扫描快（O(log N) vs O(N)）；
- CDF 在**统计上有更明确的物理意义**——"被低于某值的像素占多少"。

### 6.5 调窗走 JNI 的设计动机

```java
retArr = ImageProcessingJni.autoWindowLevel(data_cal, arrayLength);
```

**为什么不在 Java 里直接算窗位窗宽？**
- **历史包袱**：这段 Java 早于 `processCtSeries` JNI 出现，最早 native 端就有 `autoWindowLevel`；
- **算法差异**：native 版本可能用了**直方图峰值检测** / **拉普拉斯锐化** / **局部最小可觉差（JND）** 等更复杂方法，Java 这层只是"瘦客户端"；
- **跨平台复用**：同一份 native 代码可以给 iOS、WebAssembly、桌面端用，避免算法二次实现。

**为什么 `largestImagePixelValue` 也要传出？**
- 这是给**阅片工作站**显示的参考值——"该片最亮像素是多少"能告诉医生"是否过曝"；
- 阅片软件通常会把这个值写在 DICOM header 的 `(0028,0107) Largest Image Pixel Value` 字段里。

### 6.6 Exposure / StdDev 的设计动机

```java
long sum = 0;
for (int j : img_data_16b) sum += j;
int exposure_level = (int)(sum / img_data_16b.length);
double varianceSum = 0;
for (int j : img_data_16b) varianceSum += Math.pow(j - exposure_level, 2);
double stddev = Math.sqrt(varianceSum / img_data_16b.length);
```

**为什么同时输出 mean 和 stddev？**

| 指标 | 临床含义 | 异常值可能原因 |
|---|---|---|
| `exposure_level` (mean) | 整体亮度 / 曝光剂量 | 偏低→欠曝；偏高→过曝 |
| `standardDeviation` | 对比度 / 动态范围 | 偏低→"全灰"、散射过多；偏高→"硬边"、丢失层次 |

**为什么是 `long` 累加？**
- 1500×1290 ≈ 2M 像素 × 最大 65535 = **14 亿**，`int` 会溢出（Integer.MAX_VALUE = 21 亿，但还要给方差预留）；
- `long` 累加 + 最后转 `int` 是工程化安全写法。

**为什么 stddev 用 `Math.pow` 而不是 Welford 在线算法？**
- Welford 数值更稳但代码复杂；
- 医疗图 N ≈ 2M，`double` 累加误差相对值 < 1e-10，可忽略；
- 简单循环 = 容易复现 / 容易审计 / 容易单测。

### 6.7 输出 byte[] 大端重排的设计动机

```java
for (int i = 0; i < img_data_16b.length; i++) {
    out_pixelData[2 * i] = (byte)((img_data_16b[i] >> 8) & 0xFF);
    out_pixelData[2 * i + 1] = (byte)(img_data_16b[i] & 0xFF);
}
```

**为什么输出也是大端？**
- 与输入同格式 → **流水线可串接**（"处理完的图"可以再喂进"处理中"）；
- 医院 PACS / DICOM 网关统一按大端解析，避免在网络层 byte-swap；
- 输出 byte[] 不是用于显示的（显示走 Bitmap.ARGB_8888），而是用于**回写 DICOM / 推送到工作站 / 写入中间缓存**——所以必须保留"原始探测器视角"。

### 6.8 整体管线的"看似冗余"实则精巧之处

| "冗余" | 真实用意 |
|---|---|
| 第 34 行 `convertTo(CV_8UC1)` + 第 37 行再 `convertTo` | 第一次是中间结果，第二次才是 OTSU 输入——**作者可能中途改过设计，残留了旧代码** |
| `img_mat_org.convertTo(img_mat_org, CV_16UC1)` 又被 warpAffine 写回 | warpAffine 要求 dst 与 src 同样深度，**保留 16-bit 是为了让旋转不损失精度** |
| 先 16-bit 直方图再算窗位窗宽 | 16-bit 直方图是"原始数据快照"，调窗可重复 / 可复算，**不依赖中间 Mat 状态** |
| `tailor_img` 与"二次边缘裁剪" 重复做裁剪 | tailor_img 给的是"倾斜对齐后的紧致矩形"，二次裁剪给的是"业务侧的留白 margin"（虽然代码里 marign=0）——**两层职责分离** |

---

## 7. 潜在问题（与截图场景相关）

1. **`autoWinMinMax` 走的是 JNI**——但 `rawpixeldeal` 模块下没有 `ImageProcessingJni` 绑定。若要在 `rawpixeldeal` 跑通，需要新建 `ImageProcessingJni.autoWindowLevel(int[] data, int length) → int[3]`（推荐直接复用 `dcmtk` 模块的 C++ 实现）。
2. **`LogUtil.d` 缺失**——建议在 `rawpixeldeal` 下加一个 `LogUtil` 包装 `android.util.Log`，并统一 TAG。
3. **`PixelData` 8 参构造**——`dcmtk/model/PixelDataNew` 当前是 7 参（少 stddev），与本类签名不一致；要么改本类少传 stddev，要么把 stddev 字段加到 `PixelDataNew`。
4. **`tailor_img` 第 31~37 行有冗余 `convertTo` 和中间 `img_mat_org1`**——第 34 行转换到 `CV_8UC1` 后立即 `convertScaleAbs`，第 37 行又把原图 `convertTo` 回 `CV_8UC1` 覆盖。这两处都建议删除一处。
5. **`img_mat_org` 既是输入又当 `convertTo(warpAffine)` 目标**——可能踩到 OpenCV 引用 aliasing；建议 `Mat warped = new Mat(); warpAffine(..., warped, ...)`。
6. **`data_cal[i] > 65535` 才 clip**——但 `data_cal` 来自 `CV_32SC1`，可能为负；当前没有处理负值 case。
7. **`sigma`、`w` 这种"高斯模糊/锐化"参数最终没接到 Imgproc**——很容易让维护者误以为有二次降噪。
8. **直方图 bin 数组是 `SENSOR_DEPTH + 1 = 65537`**——每片 256KB，对 1500×1290 来说是 ~5MB 临时内存，可优化为"按需装箱"或用 4096 bin 抽样。

---

## 8. 与新管线（`processCtSeries`）的差异

| 维度 | `ProcessPixelData.java` | `processCtSeries`（v2） |
|---|---|---|
| 域 | 16-bit raw（X 光/平板探测器） | CT HU（带 Slope/Intercept） |
| 裁剪 | 旋转 + Sobel + 闭运算 + boundingRect | HU 阈值 + 形态学 + 最大连通域 + margin |
| 直方图 | 16-bit 65536 bin | 256 bin（百分位 Gmin/Gmax） |
| 调窗 | JNI `autoWindowLevel`（min/max 法） | 论文算法 T0/T1 + 退化兜底 |
| 输出 | `PixelData`（含 stddev） | RGBA8888 字节数组 + 11+10+256+1 维 debug |
| 序列支持 | 单片 | 多片共享 ROI / 直方图 |

**总结**：Java 版面向"X 光单片自动处理"，新版面向"CT 序列自适应调窗"，**两者定位不冲突，可并存**。如果未来要让 `rawpixeldeal` 模块也支持 X 光，建议在 `tailor_img` 之外再补一个 8-bit 直方图（CLAHE 友好）分支。

---

## 9. 若让你重写，会改什么？

1. **把 OTSU 阈值从 8-bit 改回 16-bit 直方图**——避免 256 级精度损失；
2. **`Sobel_x - Sobel_y` 改为 `|grad_x| + |grad_y|`**——避免方向偏差；
3. **`marign / Thin / Middle / Thick / gamma / sigma / w` 等死参数**直接删除，**别留暗示**；
4. **`autoWindowLevel` 改为 Kotlin/Java 端实现**——避免不必要的 JNI 跳转（除非算法有真实复杂度）；
5. **`PixelData` 8 参构造 + stddev 字段**保留——这是医疗 QA 真正会用到的数据；
6. **加 `mat.useCount()` 异常处理**——OpenCV Mat 在 Android 上偶有回收时序问题，wrap 进 try/catch 更稳；
7. **直方图改用 `IntBuffer` / 直接数组**——避免 `int[65537]` 在 ART 虚拟机上的 large object 分配。

总体来说，**这段代码体现了"工程师主导的医疗图像处理"**——选择都是**经验驱动**（OTSU、40000 阈值、5% clip、cross 25×25）而非教科书式的"最优化设计"。它的优点是**可调可读可审计**，缺点是**对分布外数据鲁棒性一般**——这也是为什么要演进到 `processCtSeries` 论文算法 + 退化兜底的原因。

---

## 10. 自检清单

- [x] 主入口 5 步管线（解码 / 裁剪 / 直方图 / 调窗 / 输出）
- [x] 自动裁剪（`tailor_img` 旋转 + 闭运算 + boundingRect）
- [x] 16-bit 直方图 + 百分位 CDF 调窗
- [x] JNI 调窗接口
- [x] Exposure / Stddev 统计用于 QA
- [x] 输出 byte[] 大端，可直接回写 DICOM
- [x] 流程图（Mermaid + SVG 双版本）
- [x] 设计缘由逐条解释
- [x] 潜在问题清单
- [x] 与新管线对比表
- [x] 重写建议
