# DcmtkDemo - Android DICOM 医疗影像全流程解决方案 / Android DICOM Medical Imaging Solution

`DcmtkDemo` 是一个专为 Android 平台设计的医疗影像处理示例项目。它集成了强大的 **DCMTK (DICOM Toolkit)** 和 **OpenCV**，实现了从 PACS 网络通信、DICOM 文件解析到高性能底层像素预处理的全链路功能。

`DcmtkDemo` is a comprehensive medical imaging solution for Android. By integrating **DCMTK** and **OpenCV**, it provides a full-stack workflow including PACS communication, DICOM parsing, and high-performance low-level pixel preprocessing.

---

## 1. 项目功能概览 (Project Features)

### A. PACS 网络服务 (PACS Network Services)
依托 `dcmtk` 模块，支持标准 DICOM 通信协议：
*   **C-ECHO**: 测试与远程 PACS 服务器的连通性。
*   **C-FIND**: 支持按姓名、接入号或工作列表（MWL）进行多维查询。
*   **C-STORE**: 支持单/多文件异步上传，具备实时进度回调。
*   **C-GET / C-MOVE**: 影像调阅与下载。

Leveraging the `dcmtk` module, it supports standard DICOM protocols: C-ECHO (connectivity test), C-FIND (multi-dimensional query), C-STORE (async upload with progress), and C-GET/C-MOVE (image retrieval).

**本地 PACS 服务器搭建参考 (Local PACS Server Setup Guide)**:
[Android 医学影像开发：本地 PACS 服务器搭建指南](https://blog.csdn.net/wangyongyao1989/article/details/162460525?spm=1001.2014.3001.5502)

### B. 影像文件处理 (DICOM File I/O)
*   **元数据解析**: 将 DICOM 标签（Tags）解析为易用的 Map 结构。
*   **DICOM 写入**: 支持将处理后的原始像素数据封装并保存为标准 `.dcm` 文件。
*   **格式转换**: 提供高性能的 `dcmToJpg` 序列帧预览生成。

Supports metadata parsing (tags to Map), DICOM writing (saving processed raw pixels to `.dcm`), and high-performance frame conversion (DICOM to JPEG).

### C. 高性能像素预处理 (Advanced Image Processing)
依托 `rawpixeldeal` 模块，针对 CT/X-Ray 原始数据进行 C++ 级优化：
*   **物理校正**: Log 变换（针对 Raw 数据）与 HU 值（亨氏单位）标准化校正。
*   **智能调窗 (Smart Windowing)**: 自研 `Peak Area Auto` 算法，自动识别人体组织波峰并计算最优窗宽窗位。
*   **增强与去噪**: 集成双边滤波（保边去噪）、CLAHE（局部对比度增强）及 USM 锐化。
*   **几何变换**: 支持高性能 Native 旋转、裁剪及色度反转（Invert LUTs）。

Powered by the `rawpixeldeal` module, it offers C++ level optimizations for CT/X-Ray raw data: Log/HU correction, Smart Windowing (Peak Area Auto), denoising/enhancement (Bilateral, CLAHE, USM), and geometric transforms (rotation, cropping, inversion).

### D. CBCT 三维可视化 (CBCT 3D Visualization)
依托 `cbctdeal` 模块，基于 **DCMTK + VTK 9.1.0**（Android arm64 交叉编译）实现 CBCT（锥形束 CT）全链路三维能力：
*   **序列解析**: 双 Pass 多线程解析 DICOM 序列（支持 JPEG / JPEG-LS / RLE 压缩），按 `ImagePositionPatient` Z 坐标排序、断层间隙补全，在 Native 堆组装连续 3D 体数据。
*   **VR 体绘制**: `vtkSmartVolumeMapper` GPU RayCast（CPU FixedPoint 自动回退），骨骼窗传递函数，明暗着色渲染骨骼三维结构。
*   **MPR 多平面重建**: `vtkImageReslice` 实现横断/冠状/矢状三平面任意层浏览，灰阶窗宽窗位映射，平行投影正交显示。
*   **渲染交互**: OpenGL ES 渲染，专用渲染线程 + 按需渲染（闲置 0 GPU 占用），单指旋转/双指缩放平移手势，跨序列浏览参数保持。

Leveraging the `cbctdeal` module, built on **DCMTK + VTK 9.1.0** (cross-compiled for Android arm64), it delivers full-pipeline CBCT 3D capabilities: dual-pass parallel DICOM series parsing (JPEG/JPEG-LS/RLE), GPU volume rendering (VR) with bone-window transfer functions, and MPR (axial/coronal/sagittal) with real-time window/level, all rendered via OpenGL ES with gesture interaction on a dedicated render thread.

**演示视频 (Video Demos)**:
- [vtk3D.mp4](cbctdeal/doc/vtk3D.mp4) —— 序列解析 + 骨骼三维重建 + 手势交互
- [vtk3D-1.mp4](cbctdeal/doc/vtk3D-1.mp4) —— VR/MPR 模式切换、切面浏览、窗宽窗位调节

**技术文章 (Technical Article)**:
- [Android 平台 CBCT 三维可视化实战：DCMTK + VTK 交叉编译、集成与体渲染全流程](cbctdeal/CSDN-Android平台CBCT三维可视化实战.md)

---

## 2. 模块架构 (Module Architecture)

*   **`:app`**: 业务表现层。包含 Fragment UI、权限管理及基于协程的异步调用逻辑。
*   **`:dcmtk`**: 核心协议模块。通过 JNI 封装了 DCMTK 静态库，处理网络与文件 IO。
*   **`:rawpixeldeal`**: 算法增强模块。基于 OpenCV 4.x 构建，负责毫秒级的底层图像算子。
*   **`:cbctdeal`**: CBCT 三维模块。DICOM 序列解析（DCMTK）+ VR 体绘制/MPR 重建（VTK 9.1.0 arm64 交叉编译静态库），详见 [cbctdeal/README.md](cbctdeal/README.md)。

*   **`:app`**: UI layer with Fragment-based interfaces and Coroutine-based async logic.
*   **`:dcmtk`**: Core protocol module. Wraps DCMTK static libs via JNI for network and IO.
*   **`:rawpixeldeal`**: Algorithm module. Built on OpenCV 4.x for millisecond-level image operators.
*   **`:cbctdeal`**: CBCT 3D module. DICOM series parsing (DCMTK) + volume rendering & MPR (VTK 9.1.0 cross-compiled static libs). See [cbctdeal/README.md](cbctdeal/README.md).

---

## 3. 使用指南 (Usage Guide)

### 环境要求 (Requirements)
*   Android SDK / NDK (r25+)
*   CMake 3.22+
*   已编译的 DCMTK 静态库 (libdcmdata, liboflog 等)
*   已交叉编译的 VTK 9.1.0 静态库 (arm64-v8a，编译脚本见 [cbctdeal/doc/android_vtk.sh](cbctdeal/doc/android_vtk.sh))
*   CBCT 三维可视化需真机验证（依赖 OpenGL ES 3.0+）

### 初始化 (Initialization)
在使用任何 DICOM 功能前，需初始化数据字典：
Before using DICOM features, initialize the data dictionary:

```java
// 获取字典路径并加载
String dictPath = FileUtil.getDictPath(context);
DcmtkJni.initDcmtk(dictPath);
```

### 影像预处理流程 (Preprocessing Workflow)
1.  **加载数据**: 从 Assets 或文件系统读取 Raw/DICOM 数据。
2.  **配置流水线**: 在 `CT Preprocess` 界面选择算子（如：裁剪 -> Log 变换 -> HU 校正 -> 去噪）。
3.  **智能调窗**: 点击 `RUN WINDOWING` 应用自适应算法，获取最佳视觉效果。
4.  **保存**: 处理满意后，点击“写入 DICOM”持久化处理结果。

1. **Load**: Read Raw/DICOM data. 2. **Pipeline**: Select operators (Crop -> Log -> HU -> Denoise). 3. **Windowing**: Run adaptive algorithms for best visualization. 4. **Save**: Export as a standard `.dcm` file.

---

## 4. CBCT 三维可视化使用流程 (CBCT 3D Workflow)

入口：主界面「CBCT Parse」页（`CbctParseFragment`）。详细操作说明见 [cbctdeal/README.md](cbctdeal/README.md)。

1.  **加载序列**: 「选择目录 (SAF)」选取 CBCT 序列目录，或直接填写绝对路径 / 加载内置测试数据；
2.  **解析**: 点击「Parse Series」，双 Pass 进度展示，完成后输出体数据维度、层间距、耗时等摘要；
3.  **三维浏览**: 查看器切换「VTK 3D」载体——VR 体绘制（默认 45° 俯视）或 MPR 三平面切面浏览；
4.  **窗宽窗位**: SeekBar 实时调节，VR 传递函数与 MPR 灰阶区间同步生效；
5.  **手势**: 单指滑动旋转（VR）/平移（MPR）、双指捏合缩放、双指拖动平移。

Entry: "CBCT Parse" page (`CbctParseFragment`). Load a CBCT series (SAF / path / assets), parse with dual-pass progress, then explore via VTK 3D (VR volume rendering / MPR planes) with gesture interaction and real-time window/level. See [cbctdeal/README.md](cbctdeal/README.md) for details.

---

## 5. 技术亮点与优化 (Technical Highlights)

*   **链式 Native 调用**: 引入 Native 地址（Mat Address）传递机制，极大地减少了 JNI 通信开销和内存拷贝频率。
*   **针对探测器数据的专项优化**: 针对 `FT46` 等探测器产生的 Raw 数据，优化了高斯平滑（sigma 8.0->3.0）与边缘检测参数，有效解决了图像虚化和骨骼细节丢失问题。
*   **内存安全**: 利用 C++ 辅助类管理 JNI 资源生命周期，防止在大数据量影像处理时出现内存泄漏。
*   **CBCT 零拷贝渲染**: 数百 MB 体数据经 `vtkFloatArray::SetArray` 只读借用导入 VTK 管线，Native 堆零重复占用；专用渲染线程 + 按需渲染（闲置 0 GPU 占用）。
*   **VTK 裁剪交叉编译**: VTK 9.1 模块化裁剪至 44 个静态库（vs 全量数百个），并修复 GLES3 上下文、深度纹理格式等 Android 兼容性问题（详见技术文章）。

*   **Chained Native Calls**: Uses Mat Address passing to minimize JNI overhead and memory copying.
*   **Detector-Specific Optimization**: Optimized for Raw data (e.g., FT46) to fix blurring and detail loss by tuning Gaussian sigma and edge detection.
*   **Memory Safety**: Managed JNI resource lifecycles via C++ helpers to prevent leaks during large-scale image processing.
*   **CBCT Zero-Copy Rendering**: Hundreds of MB of volume data are borrowed read-only into the VTK pipeline via `vtkFloatArray::SetArray`, with zero duplication on the native heap; dedicated render thread with on-demand rendering (0% GPU when idle).
*   **Trimmed VTK Cross-Compilation**: VTK 9.1 trimmed to 44 static libs via module-level cropping, with GLES3 context / depth-texture compatibility fixes for Android (see the technical article).

---

## 6. 总结 (Summary)
本项目通过将复杂的 DICOM 协议和重度图像算法下沉到 Native 层，在 Android 移动端实现了接近桌面级的医学影像处理能力——从 PACS 通信、DICOM 解析、像素预处理，到 CBCT 序列解析与 GPU 体渲染三维可视化，是开发移动医生站、影像阅片 APP 的理想参考方案。

By pushing complex protocols and heavy algorithms to the Native layer (DCMTK / OpenCV / VTK), this project achieves desktop-grade medical imaging capabilities on Android — from PACS communication, DICOM parsing, and pixel preprocessing, to CBCT series parsing and GPU volume-rendered 3D visualization — serving as an ideal reference for mobile RIS/PACS or diagnostic apps.
