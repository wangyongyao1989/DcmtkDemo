# Android 平台 VTK 集成与 CBCT 三维重建技术实战

> **摘要**：本文详细记录了在 Android 平台上实现 **CBCT（锥形束 CT）三维可视化** 的完整流程。涵盖 DCMTK 与 VTK 9.1.0 的 NDK 交叉编译方案、OpenGL ES 兼容性补丁、双 Pass 序列解析引擎实现，以及 VR（体绘制）与 MPR（多平面重建）的渲染管线架构。

---

## 一、 CBCT 原理与解析基础

> 本章知识点参考自《CBCT（锥形束计算机断层扫描）原理、技术架构与临床应用深度研究报告》。

### 1.1 什么是 CBCT
**CBCT (Cone Beam Computed Tomography)** 采用锥形 X 线束与二维平板探测器。与传统螺旋 CT 的扇形束扫描不同，CBCT 仅需单次旋转即可获取整个容积的投影数据。
*   **各向同性 (Isotropic)**：CBCT 的空间分辨率极高，且体素在三个轴向上通常是等距的（如 0.25mm），这意味着在进行 MPR 重建时无需进行复杂的重采样拉伸。
*   **HU 值特征**：CBCT 的灰度值与亨氏单位 (HU) 相关，但由于散射线影响，其定量精度略低于螺旋 CT。在解析时，必须正确处理 `Rescale Slope` 与 `Intercept`。

### 1.2 DICOM 序列解析挑战
医疗影像 App 面临的核心矛盾是**大数据量与移动端有限资源**的冲突。一例 CBCT 序列通常包含 300~600 张切片，16bit 原始数据量达 200MB+。
*   **压缩格式**：为了节省存储，CBCT 序列常采用 **JPEG-LS** 或 **JPEG Lossless** 压缩。
*   **坐标系**：必须依据 `ImagePositionPatient` 的 Z 坐标进行排序，严禁依赖文件名排序。

---

## 二、 VTK 9.1.0 Android 交叉编译实战

VTK 是医学三维可视化的标准库，但官方包体积庞大且默认面向桌面端。我们需要利用 NDK 进行**模块化裁剪编译**。

### 2.1 交叉编译脚本解析 (`android_vtk.sh`)
关键配置提取自项目脚本：
*   **NDK 版本**：NDK r25 (25.1.8937393)。
*   **ABI 选择**：仅保留 `arm64-v8a` 以精简体积并利用 NEON 指令集。
*   **模块裁剪策略**：
    ```bash
    -DVTK_GROUP_ENABLE_StandAlone=DONT_WANT \
    -DVTK_GROUP_ENABLE_Rendering=DONT_WANT \
    -DVTK_MODULE_ENABLE_VTK_RenderingVolumeOpenGL2=YES \
    -DVTK_MODULE_ENABLE_VTK_ImagingCore=YES
    ```
    通过先禁用组（`DONT_WANT`）再按需启用特定模块，最终将静态库数量控制在 44 个，so 总体积缩小了 70%。

### 2.2 GLES3 兼容性补丁 (黑屏修复)
VTK 在 Android 环境下存在几个致命的 GLES 兼容性坑位，必须手动修改源码：
1.  **上下文版本**：修改 `vtkEGLRenderWindow.cxx`，强制请求 `EGL_OPENGL_ES3_BIT`。
2.  **深度纹理格式**：GLES3 下 `glBlitFramebuffer` 要求格式严格一致。将深度纹理从 `Fixed32` 改为 `Fixed24`，并设置过滤方式为 `Nearest`。
3.  **Shader 首行**：确保 `#version 300 es` 前面没有任何换行符。

---

## 三、 CBCT 序列解析引擎实现

解析逻辑位于 `CbctSeriesParser.cpp`，采用双 Pass 并行架构：

### 3.1 双 Pass 设计
*   **Pass A (Meta Pass)**：多线程读取所有 DICOM 文件的元数据，提取 Z 坐标并进行全局排序。
*   **Pass B (Pixel Pass)**：按排序后的顺序，多线程解码像素数据（支持 JPEG-LS），并将 HU 值填充到 Native 堆上的连续内存块（`CbctVolume`）。

### 3.2 内存优化
为了规避 Java 堆的 OOM，体数据以 `float` (HU 值) 形式存储在 Native 堆中。VR 渲染时通过 `vtkFloatArray::SetArray` 实现**零拷贝导入**，避免了数百 MB 数据的内存二次拷贝。

---

## 四、 VTK 三维渲染管线架构

渲染核心 `CbctVtkRenderer.cpp` 实现了两套渲染管线：

### 4.1 VR (Volume Rendering) 管线
使用 `vtkSmartVolumeMapper` 进行 GPU 射线投射成像。
*   **传递函数**：根据窗宽 (WW) 和窗位 (WC) 动态计算颜色与不透明度控制点。
*   **骨骼增强**：通过 `ShadeOn()` 开启明暗着色，利用法向量计算提升骨骼的空间立体感。

### 4.2 MPR (Multi-Planar Reconstruction) 管线
使用 `vtkImageReslice` 对体数据进行切片提取。
*   **三轴切换**：通过变换 `DirectionCosines` 矩阵实现横断、冠状、矢状面的切换。
*   **灰阶映射**：使用 `vtkLookupTable` 将 HU 值区间映射为 0-255 的灰阶图像。

---

## 五、 Android 端交互适配

### 5.1 专用渲染线程
为了保证 UI 流畅，渲染逻辑运行在专用线程（绑定的 EGL 上下文）。
*   **按需渲染**：仅在手势交互或参数改变时触发 `Render()`，闲置时 0 GPU 占用。
*   **手势映射**：
    *   单指：VR 模式下旋转相机；MPR 模式下平移切面。
    *   双指：缩放与模型平移。

---

## 六、 总结与演示

本项目成功在 Android 真机上实现了接近桌面工作站级的 CBCT 阅片能力。

**演示视频**：
*   `vtk3D.mp4`：展示了从序列解析到骨骼三维重建的完整流畅体验。
*   `vtk3D-1.mp4`：展示了 VR/MPR 模式无缝切换及精准的窗宽窗位调节。

> 本项目代码已在 `cbctdeal` 模块中完整开源，包含交叉编译脚本与核心解析引擎。
