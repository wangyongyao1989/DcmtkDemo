# DcmtkDemo - Android 医疗影像全流程解决方案 (PACS + DICOM + 3D CBCT)

`DcmtkDemo` 是一个专为 Android 平台设计的医疗影像处理全栈示例项目。它集成了 **DCMTK (DICOM Toolkit)**、**VTK 9.1.0** 和 **OpenCV**，实现了从 PACS 网络通信、DICOM 文件解析到高性能像素预处理，以及 **CBCT（锥形束 CT）三维体渲染与 MPR 重建** 的全链路功能。

---

## 🚀 核心功能 (Core Features)

### 1. PACS 网络服务 (`:dcmtk`)
支持标准 DICOM 3.0 通信协议，可直接对接商用 PACS 系统或医院工作站：
*   **C-ECHO**: 连通性测试。
*   **C-FIND**: 按姓名、接入号或 Worklist 进行多维查询。
*   **C-STORE**: 影像异步上传，支持实时进度回调。
*   **C-GET / C-MOVE**: 影像调阅与下载。

### 2. CBCT 三维可视化与序列解析 (`:cbctdeal`)
基于 **VTK 9.1.0** (Android arm64 裁剪版) 与 **DCMTK** 实现：
*   **VR (Volume Rendering)**: GPU 射线投射体绘制，骨骼三维重建，支持明暗着色。
*   **MPR (Multi-Planar Reconstruction)**: 横断/冠状/矢状三平面切面浏览，实时窗宽窗位调节。
*   **序列解析**: 双 Pass 多线程解析（支持 JPEG/JPEG-LS 压缩），Z 坐标自动排序，断层空隙补全。
*   **手势交互**: 单指旋转/平移、双指缩放/平移，极致流畅的 Native 渲染体验。

### 3. 高性能像素预处理 (`:rawpixeldeal`)
针对 CT/X-Ray 原始数据（16bit Raw/HU）进行 Native 级优化：
*   **物理校正**: Log 变换与 HU 值标准化校正。
*   **智能调窗 (Smart Windowing)**: 自研 `Peak Area Auto` 算法，自动识别组织波峰。
*   **图像增强**: 双边滤波去噪、CLAHE 对比度增强、USM 锐化。

---

## 📺 演示视频 (Video Demos)

| 视频演示 | 功能描述 |
| :--- | :--- |
| [**vtk3D.mp4**](cbctdeal/doc/vtk3D.mp4) | CBCT 序列解析 + 骨骼三维重建 + 手势交互全过程 |
| [**vtk3D-1.mp4**](cbctdeal/doc/vtk3D-1.mp4) | VR/MPR 模式切换、切面浏览、窗宽窗位动态调节细节 |

---

## 📚 技术文档 (Technical Documentation)

*   **[Android 平台 VTK 集成与 CBCT 三维重建技术实战](cbctdeal/Android平台VTK集成与CBCT三维重建技术实战.md)** —— **深度技术长文**，涵盖 VTK/DCMTK 交叉编译、GLES3 兼容性补丁、序列解析算法及渲染管线实现原理。
*   **[VTK Android 交叉编译脚本 (android_vtk.sh)](cbctdeal/doc/android_vtk.sh)** —— 基于 NDK r25 的模块化裁剪编译配置。

---

## 🏗️ 项目架构 (Architecture)

*   **`:app`**: UI 表现层，基于协程编排异步业务。
*   **`:cbctdeal`**: **三维核心模块**。包含 VTK 9.1.0 静态库与 C++ 渲染引擎。
*   **`:dcmtk`**: 通信模块。封装 DCMTK 静态库，处理网络与文件 IO。
*   **`:rawpixeldeal`**: 算法模块。基于 OpenCV 处理底层像素变换。

---

## 🛠️ 快速开始 (Getting Started)

### 环境要求
*   Android SDK / NDK (r25+)
*   OpenGL ES 3.0+ (三维可视化需真机验证)
*   CMake 3.22+

### 初始化
在使用前需注入 DICOM 数据字典：
```kotlin
val dictPath = FileUtil.getDictPath(context)
CbctJni.initDictionary(dictPath)
```

---

## 🤝 总结
本项目通过将重度图像算法与解析逻辑下沉至 Native 层，在 Android 移动端实现了接近桌面工作站级的医学影像处理能力，是开发**移动医生站、口腔影像阅片、远程会诊 App** 的理想参考方案。
