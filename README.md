# DcmtkDemo - Android DICOM 医疗影像处理方案

`DcmtkDemo` 是一个基于 Android 平台，集成 **DCMTK (DICOM Toolkit)** 开源库的示例项目。项目展示了如何通过 NDK 技术在 Android 应用中实现 DICOM 协议的网络通信（PACS）及影像文件处理。

## 1. 项目架构概览

项目采用 **分层解耦** 的设计思路，确保 UI 层与底层复杂的 C++ 业务逻辑分离：

*   **App Module (`:app`)**: 表现层。负责 UI 展示（Fragment/Adapter）、权限管理及文件系统交互。
*   **Dcmtk Module (`:dcmtk`)**: 核心库模块。
    *   **Java/Kotlin API**: 提供 `DcmtkJni` 接口供上层调用。
    *   **JNI Bridge (`native-lib.cpp`)**: 负责 Java 对象与 C++ 类型的封送（Marshaling）。
    *   **Native Business Layer**: `PacsClient`（网络）与 `DicomFileIO`（IO/转换），完全脱离 JNI 环境，纯 C++ 编写。
    *   **DCMTK 静态库**: 编译后的核心库支持。

## 2. 核心功能模块

### A. PACS 网络服务 (DICOM Network)
通过 `PacsClient` 类实现，支持标准的 DICOM 服务类：
*   **C-ECHO**: 测试与远程 PACS 服务器的连通性。
*   **C-FIND**: 提供多维查询（按病人姓名、接入号 Accession Number 或 MWL 工作列表）。
*   **C-STORE**: 支持单文件及多文件（Multi-Store）异步上传，具备进度回调。
*   **C-GET / C-MOVE**: 影像调阅与下载。

### B. 影像文件处理 (DICOM File IO)
通过 `DicomFileIO` 类实现，屏蔽了底层数据字典的复杂操作：
*   **元数据解析**: 将 DICOM 标签（Tags）解析并映射为 Java 的 `HashMap<String, String>`。
*   **DICOM 写入**: 支持将原始数据（Raw Data）封装并保存为标准 DICOM 格式。
*   **影像转换**: 实现 `dcmToJpg` 功能，将 DICOM 序列帧转换为 Android 可直接展示的压缩格式。

### C. 数据字典初始化
DCMTK 需要加载私有或标准的 `dicom.dic` 字典才能正确识别标签。本项目通过 `native_initDcmtk` 接口实现运行时动态加载。

## 3. 技术实现要点

### JNI 桥接设计
*   **动态注册**: 在 `JNI_OnLoad` 中使用 `RegisterNatives` 进行方法绑定，提升运行效率并增加代码安全性。
*   **自动资源管理**: 利用辅助类（如 `JniString`）管理 `GetStringUTFChars` 的生命周期，防止内存泄漏。
*   **异步回调**: 通过自定义的 `ProgressCallback` 接口，将底层网络传输进度实时推送到 Java 端的 UI。

### 线程模型
*   原生层操作通常为同步阻塞式（如网络请求），建议在 Java 层通过协程或线程池封装 `DcmtkJni` 的调用，避免 UI 卡顿。

## 4. 快速接入指南

### 环境要求
*   Android SDK / NDK (r21+)
*   CMake 3.10+
*   DCMTK 已编译的静态库 (libdcmdata, liboflog, libofstd, 等)

### 初始化
在应用启动或使用 DICOM 功能前，必须先拷贝数据字典到私有目录并初始化：
```java
String dictPath = FileUtil.getDictPath(context);
DcmtkJni.initDcmtk(dictPath);
```

### 示例：查询 PACS 影像
```java
String[] results = DcmtkJni.cFind(host, port, localAet, remoteAet, "PatientName^*");
```

## 5. 模块文件说明
*   `native-lib.cpp`: JNI 入口，负责数据类型转换。
*   `PacsClient.h/cpp`: 封装 C-STORE/C-FIND 等网络逻辑。
*   `DicomFileIO.h/cpp`: 封装文件读写与转换逻辑。
*   `FileUtil.java`: 辅助处理 Android 系统路径与文件拷贝。

## 6. 医学图像预处理模块 (rawpixeldeal)

`rawpixeldeal` 模块专门用于处理 CT/X-Ray 等医学影像的原始像素数据，提供高性能的 Native 图像增强与调窗算法。

### A. 核心功能
*   **预处理流水线**: 集成了 HU 校正、自动裁剪、双边去噪、CLAHE 增强及特征锐化 (USM)。
*   **智能调窗 (Smart Windowing)**: 支持 Peak Area Auto 等多种高级调窗算法，实现高动态范围像素到 8-bit 可视化空间的映射。
*   **图像后处理 (Native ImageProcessor)**: 
    *   **实时调节**: 对比度、亮度、锐化程度的精细化调整。
    *   **视觉增强**: 反色 (Invert)、伪彩 (False Color)、浮雕 (Relief) 效果。
    *   **几何变换**: 高性能图像旋转。

### B. 细粒度控制 (Fine-Grained API)
支持基于 Native 地址 (Mat Address) 的链式调用，极大减少了 JNI 通信开销和内存拷贝频率。开发者可以手动控制处理流：
1.  `convertToGrayScale(Bitmap) -> MatAddr`
2.  `applyXXX(MatAddr, params...)`
3.  `convertMatToBitmap(MatAddr) -> Bitmap`

### C. 操作指南
1.  **执行预处理**: 在 `CT Preprocess` 界面选择 Asset 源，点击“执行最优调节”或自定义流水线。
2.  **后处理测试**: 
    *   拖动 **Contrast/Brightness/Sharpen** 滑块调整参数。
    *   勾选 **Invert/FalseColor/Relief** 开启特效。
    *   点击 **Test ImageProcessor** 应用所有后处理设置。
    *   点击 **Rotate 90°** 测试图像旋转。
    *   点击 **Fine-grained Test** 体验 Native 层链式处理流程。
3.  **持久化**: 处理满意后点击“写入DICOM”，系统将提取处理后的 16-bit 像素数据并生成标准 `.dcm` 文件。

## 7. 总结
本项目通过将复杂的 DICOM 协议和图像算法下沉到 Native (C++/OpenCV) 层，在 Android 移动端实现了接近桌面级的医学影像处理能力。`dcmtk` 模块解决了通信与格式兼容性问题，而 `rawpixeldeal` 模块则通过高性能算子保障了临床诊断所需的图像质量。
