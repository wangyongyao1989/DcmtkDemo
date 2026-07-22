# Color Invert LUTs 功能实现总结

本报告详细说明了在 `CTPreprocessFragment` 中新增的 “Color Invert LUTs” 功能的设计初衷、技术实现及应对策略。

---

## 1. 需求背景 (Requirements)

在医学图像处理中，颜色反转（Color Inversion）不仅是简单的视觉效果，更是临床诊断中的重要工具。通过反转像素值，医生可以：
*   **模拟 Monochrome1 与 Monochrome2 切换**：某些 DICOM 文件默认数值越大越暗，反转后可符合人眼“高密度 = 亮”的直觉。
*   **突出高对比度细节**：在反色背景下，原本微小的钙化点或伪影可能更易被捕捉。

---

## 2. 技术实现 (Implementation)

本功能遵循 **Kotlin -> JNI -> C++** 的分层架构实现：

### 2.1 C++ 核心算法 (Core Algorithm)
在 `MedicalCTPreprocess.cpp` 中实现了 `EnhanceInvertLut` 函数：
*   **16-bit 无符号 (CV_16U)**：执行 `dst = 65535 - src`。
*   **16-bit 有符号 (CV_16S)**：采用动态反转策略 `dst = (min + max) - src`。这保证了反转后的数值仍落在原始数据的动态范围内，避免了 HU 值的大规模溢出。
*   **8-bit (CV_8U)**：执行标准的 `255 - src`。

### 2.2 JNI 桥接 (JNI Bridge)
在 `RawPixelDealJni` 中新增 `invertLut` 接口：
*   负责将 Java 层传入的 `ByteArray` 封装为 `cv::Mat`。
*   调用 C++ 算法处理后，将结果重新写回 `ByteArray` 返回。
*   **零拷贝思想**：通过 `JniHelper::wrapRawMat` 尽量减少不必要的内存拷贝，提升大图处理性能。

### 2.3 Kotlin 业务逻辑 (Business Logic)
在 `CTPreprocessFragment` 中新增 `runInvertLutPipeline`：
1.  **裁剪 (Tailor)**：利用已有算法定位主体区域。
2.  **反转 (Invert)**：在 **16-bit 原始像素域** 进行反转，而不是在 8-bit 显示域。这保证了后续流水线（如 HU 校正、CLAHE）能处理反转后的物理值。
3.  **流水线 (Pipeline)**：对反转后的数据执行标准预处理流程（HU -> 降噪 -> 增强）。

---

## 3. 应对策略与原理 (Strategies & Principles)

### 3.1 为什么在 16-bit 原始域执行？
*   **原理**：如果只在最后 8-bit 显示时取反，只是单纯的视觉反色。
*   **策略**：在原始域反转，意味着图像的“物理极性”发生了改变。后续的直方图均衡（CLAHE）会针对反转后的分布进行优化，能够更好地拉开原本处于低对比度区域的细节。

### 3.2 针对有符号数的平移策略
*   **问题**：直接取反（`-val`）会导致正常的软组织（~0 HU）变到 -1000 HU 之外，破坏物理含义。
*   **策略**：使用 `(min + max) - val`。例如数据范围是 [-1000, 2000]，则 2000 会映射到 -1000，-1000 映射到 2000。这保留了数据的分布宽度，仅改变了指向。

### 3.3 交互优化
*   **UI 隔离**：将该功能作为一个独立的 Button 暴露，不影响原有的“标准流水线”逻辑，方便用户进行 A/B 对比观察。

---

## 4. 结论

通过引入 `Invert LUTs`，该模块现在支持更灵活的诊断视图。配合已有的自动裁剪和自适应调窗，能够为复杂 CT 序列提供更全方位的预处理支持。
