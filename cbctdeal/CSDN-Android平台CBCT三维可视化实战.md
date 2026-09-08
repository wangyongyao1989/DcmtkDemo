# Android 平台 CBCT 三维可视化实战：DCMTK + VTK 交叉编译、集成与体渲染全流程

> 医疗影像 App 开发系列。本文完整复盘在 Android 真机上实现 **CBCT（锥形束 CT）DICOM 序列解析 + VTK 三维体渲染（VR）+ 多平面重建（MPR）** 的全流程：从 DCMTK / VTK 9.1.0 的 NDK 交叉编译、Android 工程集成，到序列解析引擎、体渲染管线的实现原理，以及真机调试中踩过的每一个"黑屏深坑"。全文代码均来自可运行的工程实现（arm64-v8a 真机验证通过）。

**阅读收获**：

- 掌握 VTK 9.1.0 面向 Android 的**模块化裁剪交叉编译**方法（44 个静态库裁剪版，vs 全量构建数百个）；
- 掌握 DCMTK 交叉编译的关键配置，以及一个隐蔽的**数据字典陷阱**；
- 理解 CBCT DICOM 序列的**双 Pass 解析、Z 坐标排序、断层补全、HU 换算**的完整实现；
- 理解 VTK **零拷贝体数据导入、VR/MPR 双管线、专用渲染线程**的架构设计；
- 收录 GLES3 平台上 VTK 体渲染的 **4 个致命兼容性问题**及修复方案（ES3 上下文、R32F 纹理、深度纹理格式、状态缓存同步）。

---

## 目录

1. [背景：为什么要做 CBCT 三维可视化](#一背景为什么要做-cbct-三维可视化)
2. [CBCT 概念速览](#二cbct-概念速览)
3. [总体架构与数据流水线](#三总体架构与数据流水线)
4. [DCMTK 交叉编译](#四dcmtk-交叉编译)
5. [VTK 9.1 交叉编译（android_vtk.sh 全解析）](#五vtk-91-交叉编译android_vtk_sh-全解析)
6. [交叉编译产物集成到 Android 工程](#六交叉编译产物集成到-android-工程)
7. [CBCT DICOM 序列解析实现](#七cbct-dicom-序列解析实现)
8. [VTK 三维渲染实现](#八vtk-三维渲染实现)
9. [踩坑实录：GLES3 上的黑屏连环坑](#九踩坑实录gles3-上的黑屏连环坑)
10. [性能与稳定性设计](#十性能与稳定性设计)
11. [总结](#十一总结)

---

## 一、背景：为什么要做 CBCT 三维可视化

移动医疗影像 App 的典型场景：医生在平板/手机上阅片。二维阅片（单张 DICOM 切片 + 窗宽窗位调节）相对成熟，但**三维阅片**——容积渲染（Volume Rendering，VR）看骨骼空间结构、MPR（Multi-Planar Reconstruction）任意切面——长期被当作桌面工作站（如 Mimics、Simplant、3D Slicer）的专属能力。

把这套能力搬到 Android，核心矛盾有三：

| 矛盾 | 说明 |
|---|---|
| **库体积** | VTK/DCMTK 均为重量级 C++ 库，全量构建后 so 体积、编译时间难以接受，必须交叉编译 + 模块裁剪 |
| **内存** | 一例 CBCT 序列 500×500×500 体素，16bit 就要 250MB，Java 堆必然 OOM，必须 Native 堆管理 + 零拷贝 |
| **图形 API** | Android 只有 OpenGL ES（GLES3），VTK 默认面向桌面 OpenGL，体渲染管线存在多处不兼容，需要打补丁 |

本文对应的工程 `cbctdeal` 模块已解决上述全部问题，在 arm64-v8a 真机上实现：

- **序列解析**：JPEG / JPEG-LS / RLE 压缩序列，双 Pass 多线程解析；
- **VR 体绘制**：`vtkSmartVolumeMapper` GPU RayCast，骨骼窗传递函数，触摸旋转/缩放/平移；
- **MPR**：横断/冠状/矢状三平面任意层浏览，灰阶窗宽窗位。

---

## 二、CBCT 概念速览

> 本章知识点提炼自模块 doc 目录下的两份深度研究报告：《CBCT（锥形束计算机断层扫描）原理、技术架构与临床应用深度研究报告》《深度研究：Android平台基于DCMTK的CBCT DICOM序列解析技术》，只讲和"解析与渲染"相关的部分。

### 2.1 什么是 CBCT

**CBCT（Cone Beam Computed Tomography，锥形束计算机断层扫描）** 是 CT 技术的一个分支，因采用**锥形 X 线束**作为扫描介质而得名，主要应用于口腔及头颈部成像。

其成像数学基础与传统 CT 一致——**Radon 变换的逆运算**：三维物体内部结构信息被编码在不同角度的 X 线投影中，通过"滤波反投影"叠加即可还原三维衰减系数分布。差异在采集架构：

- **CBCT**：锥形 X 线束 + 二维平板探测器同步旋转，**单次旋转（200°~360°，10 秒内）即可采集整个容积**；
- **螺旋 CT**：薄层扇形束 + 多排探测器，需螺旋轨迹连续扫描覆盖容积。

重建算法上，CBCT 单圈采集在数学上不满足精确重建前提，商用设备普遍采用 **FDK 算法（Feldkamp-Davis-Kress）近似重建**：先对每张二维投影做斜坡滤波补偿边缘衰减，再沿锥形束方向反投影叠加还原三维体素分布。

### 2.2 CBCT vs 螺旋 CT：决定解析策略的差异

| 特性维度 | CBCT | 螺旋 CT | 对解析/渲染的影响 |
|---|---|---|---|
| 射线与采集 | 锥形束 + 平板探测器，单次旋转 | 扇形束 + 多排探测器螺旋扫描 | CBCT 体素数据连续，不可拆帧乱序 |
| 体素特征 | **各向同性**（PixelSpacing ≈ SliceThickness，常见 0.125/0.25mm） | 各向异性 | 解析后可直接用原始间距渲染，无需拉伸 |
| 空间分辨率 | 各向同性体素 0.125~0.4mm，骨细节优 | 受层厚限制 | 16bit 精度必须全程保留，禁止降 8bit |
| 序列存储 | 单帧密集（100~500 张）或多帧封装，**JPEG-LS 压缩常见**（设备默认省空间） | 单帧不压缩 / Deflate | 必须支持 JPEG-LS 解压 + 多帧封装 |
| 灰度特征 | HU 范围窄，骨骼灰度集中，RescaleSlope/Intercept 常为 1.0 / -1000 | HU 范围广 | 缺失窗宽窗位时回退 **CBCT 骨骼窗 WW4000 / WC600** |
| 软组织对比度 | 差 | 好 | VR 渲染以骨骼（高 HU）为主目标 |
| 辐射剂量 | 单例 < 100μSv | 单例 > 500μSv | —— |

### 2.3 临床应用与我们的功能映射

CBCT 当前最核心的临床场景是**口腔种植**（术前骨量评估、种植导板规划）、**牙体牙髓**（隐匿性根管如 MB2、牙根纵裂诊断）与**正畸**（埋伏牙定位、气道评估）。这些场景的软件功能映射到工程上就是三件事：

1. **DICOM 序列 → 3D 体数据**（解析，第七章）；
2. **MPR 多平面重建**——在三维容积上任意截取冠状/矢状/横断面，测量线性精度需达 0.1mm 级（第八章）；
3. **VR 体绘制**——骨骼三维立体渲染，支撑术前三维规划（第八章）。

另外一条与 Android 直接相关的约束：CBCT 的 DICOM 数据必须支持 **DICOM 3.0 标准接口**（PACS 对接），这正是选择 DCMTK 作为解析层的行业理由。

---

## 三、总体架构与数据流水线

### 3.1 分层架构

```
┌─────────────────────────────────────────────────────────────┐
│ Kotlin 层（UI / 业务）                                        │
│   CbctParseFragment   —— 解析入口、进度展示                   │
│   CbctParseEngine     —— 协程编排（IO 线程 + 字典注入）        │
│   CbctVtkView         —— SurfaceView（生命周期转递 + 手势）   │
│   CbctJni / CbctVtkJni —— JNI 接口声明                       │
├─────────────────────────────────────────────────────────────┤
│ JNI 桥层（cbct-native-lib.cpp）                              │
│   仅做类型转换：jstring/jobject ↔ C++ 类型；进度回调经         │
│   AttachCurrentThread 从任意 Native 线程回传                 │
├─────────────────────────────────────────────────────────────┤
│ Native 业务层                                                │
│   CbctSeriesParser    —— DCMTK 序列解析（双 Pass）           │
│   CbctVtkRenderer     —— VTK 渲染（VR/MPR + 专用渲染线程）   │
├─────────────────────────────────────────────────────────────┤
│ Native 基础层（交叉编译静态库）                               │
│   DCMTK: libdcmdata / dcmimgle / dcmjpeg / dcmjpls / ofstd…  │
│   VTK 9.1.0: 44 个 libvtk*.a（裁剪版）                       │
└─────────────────────────────────────────────────────────────┘
```

设计原则：**解析与渲染全部内聚在单一 `cbct_native.so` 内**，对外只暴露 Kotlin API；与宿主 App 的 `dcmtk`、`rawpixeldeal` 模块在 Kotlin 层零依赖，仅在构建层共享 DCMTK 静态库（各自链进自己的 so，运行时互不影响）。

### 3.2 数据流水线

```
DICOM 序列目录
     │ CbctParseEngine（IO 线程）
     ▼
CbctSeriesParser（Native, DCMTK）
     │  Pass A 并行读元数据 → 按 Z 坐标排序
     │  Pass B 并行逐片解码 → 填充 Volume（含断层补全）
     ▼
CbctVolume（Native 堆连续 float HU [z][y][x]）
     │
     ├────────────────────────────────┐
     ▼                                ▼
Bitmap 2D 路径                  CbctVtkRenderer 路径
  extractAxial/extractMpr         零拷贝导入 vtkImageData
  CPU 窗宽窗位 → Bitmap           ├─ VR：vtkSmartVolumeMapper
  （ImageView 显示）              │    GPU RayCast 体绘制
                                  └─ MPR：vtkImageReslice 三轴切面
                                       + vtkImageMapToColors 灰阶
                                  （SurfaceView + GLES 上屏）
```

---

## 四、DCMTK 交叉编译

### 4.1 版本与环境

- **DCMTK 3.6.8**：医疗行业事实标准的开源 DICOM 工具集，是唯一稳定支持 JPEG-LS、16bit 体素、LPS 坐标系的跨平台解析库；
- **NDK r25（25.1.8937393）**：LTS 版本，clang 工具链，arm64-v8a；
- **android-24**（Android 7.0+）；
- CMake + NDK toolchain 文件方式构建。

### 4.2 编译要点

与常规 NDK 交叉编译一致的骨架：

```bash
export ANDROID_NDK=/path/to/ndk/25.1.8937393
export TOOLCHAIN=$ANDROID_NDK/build/cmake/android.toolchain.cmake

cmake .. \
  -DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN} \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-24 \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF \
  # 关闭无关第三方依赖（裁体积）
  -DDCMTK_WITH_ICU=OFF \
  -DDCMTK_WITH_XML=OFF \
  -DDCMTK_WITH_PNG=OFF \
  -DDCMTK_WITH_TIFF=OFF \
  -DDCMTK_WITH_ZLIB=ON \
  # ARM 平台适配：SSE 是 x86 指令集，必须关闭
  -DDCMTK_WITH_SSE=OFF \
  -DDCMTK_WITH_NEON=ON \
  -DDCMTK_BUILD_SAMPLES=OFF \
  -DDCMTK_BUILD_TESTING=OFF \
  -DCMAKE_INSTALL_PREFIX=$(pwd)/install
make -j8 && make install
```

三个易错点：

1. **`-DDCMTK_WITH_SSE=OFF` 必须显式设置**——SSE 是 x86 专属指令集，ARM 上会直接编译报错；
2. **NEON 随 arm64-v8a 基线自动生效**（arm64 指令集基线含 NEON），像素拷贝/解压路径自动加速；
3. **链接顺序与原子库**：出现 `__atomic_load_8` 未定义时追加 `-latomic`；静态库间循环依赖用 `-Wl,--start-group ... -Wl,--end-group` 解决（见第六章 CMakeLists）。

### 4.3 产物与模块选择

产物为一组 `libdcm*.a` 静态库（dcmdata、dcmimgle、dcmjpeg、dcmjpls、dcmimage、dcmtkcharls、ofstd、oflog 等），放入 Android 工程的 `dcmtk/src/main/cpp/lib/arm64-v8/` 目录。CBCT 解析链路用到的核心模块：

| 模块 | 作用 |
|---|---|
| `libdcmdata.a` | DICOM 文件元数据读取、标签解析、数据字典 |
| `libdcmimgle.a` | 像素格式转换、窗宽窗位映射 |
| `libdcmjpeg.a` / `libdcmjpls.a` | JPEG / **JPEG-LS**（CBCT 常见压缩）解压 |
| `libofstd.a` / `liboflog.a` | 基础工具、日志 |

### 4.4 一个隐蔽的陷阱：数据字典被裁掉了

**现象**：解析未压缩序列一切正常；解析 JPEG Lossless 压缩序列时，DCMTK 报错 `readPixels: all access methods failed`，进一步定位到 `Tag not found in data dictionary`——标准 tag 的 **VR（Value Representation）查询失败**。

**根因**：DCMTK 交叉编译默认 `--without-private-dictionary`，甚至标准字典也可能未编译进库；而 JPEG/JPEG-LS 解压路径在构造标准 tag 时依赖字典查询 VR。字典数据是**惰性初始化的静态数据表**，两个连环坑：

1. 字典根本没进库 → 必须从**外部注入**；
2. 若在链接时开了 `-Wl,--gc-sections`，字典静态数据会被当作"未引用段"回收 → 注入了也白搭。

**解决方案**（两步）：

- 把 `dicom.dic`（DCMTK 源码自带的字典文件）放进模块 `assets`，首次解析前释放到内部存储，调用 `DcmDataDictionary::loadDictionary()` 注入：

```cpp
// cbct-native-lib.cpp
extern "C" JNIEXPORT void JNICALL
Java_com_wangyao_cbctdeal_jni_CbctJni_initDictionary(JNIEnv *env, jclass clazz,
                                                     jstring dict_path) {
    JniStr p(env, dict_path);
    DcmDataDictionary &dict = dcmDataDict.wrlock();
    OFBool ok = dict.loadDictionary(p.c());
    dcmDataDict.wrunlock();
}
```

```kotlin
// CbctParseEngine.kt：进程级一次
private fun ensureDictionary(context: Context) {
    if (dictReady) return
    val dictFile = File(context.filesDir, "dicom.dic")
    if (!dictFile.exists() || dictFile.length() == 0L) {
        context.assets.open("dicom.dic").use { input ->
            dictFile.outputStream().use { output -> input.copyTo(output) }
        }
    }
    CbctJni.initDictionary(dictFile.absolutePath)
    dictReady = true
}
```

- CMakeLists 中**保留 `-ffunction-sections -fdata-sections`，但暂不启用 `--gc-sections`**（见 6.1），等字典问题确认无虞后再评估开启范围。

---

## 五、VTK 9.1 交叉编译（android_vtk.sh 全解析）

VTK 是三维医学可视化的标准库，但它的官方发布面向桌面平台（Qt/X11/桌面 OpenGL），交叉编译到 Android 需要**理解 VTK 9.1 的模块化构建系统**并做精准裁剪。完整脚本见工程 `cbctdeal/doc/android_vtk.sh`，本章逐段解析。

### 5.1 先搞懂 VTK 9.1 的模块开关机制

VTK 9 采用 **module + group** 两级构建控制（`CMake/vtkModule.cmake`）：

- `VTK_MODULE_ENABLE_VTK_<Module>=YES/WANT/NO/DONT_WANT`——控制单个模块；
- `VTK_GROUP_ENABLE_<Group>=...`——控制一组模块；
- **关键陷阱**：VTK 9.1 顶层 CMakeLists **默认把 `StandAlone` 和 `Rendering` 两组设为 `WANT`**，即使你逐个 `=NO`，组开关仍会把整组拉进来 → 全量构建。

所以裁剪的正确姿势是**先关组、再开模块**：

```bash
-DVTK_GROUP_ENABLE_StandAlone=DONT_WANT \
-DVTK_GROUP_ENABLE_Rendering=DONT_WANT \
# 之后逐个显式启用需要的模块
-DVTK_MODULE_ENABLE_VTK_CommonCore=YES \
-DVTK_MODULE_ENABLE_VTK_RenderingOpenGL2=YES \
...
```

这样只会编译显式 `=YES` 的模块及其依赖闭包。

### 5.2 CBCT 场景的模块裁剪清单

体渲染（VR）+ 多平面重建（MPR）需要的最小模块集：

| 模块 | 用途 |
|---|---|
| `CommonCore` | 基础数据结构/智能指针 |
| `CommonDataModel` / `CommonExecutionModel` / `CommonMath` 等 | 依赖闭包自动带入 |
| `ImagingCore` | **`vtkImageReslice`（MPR 核心）** |
| `ImagingGeneral` | 通用图像算法 |
| `RenderingCore` | 渲染抽象（Renderer/Camera/Volume） |
| `RenderingOpenGL2` | **`vtkEGLRenderWindow`（对接 Android Surface 的 EGL 窗口）** |
| `RenderingImage` | `vtkImageActor`（切面显示） |
| `RenderingVolume` | `vtkGPUVolumeRayCastMapper`、`vtkFixedPointVolumeRayCastMapper`（CPU 回退） |
| `RenderingVolumeOpenGL2` | **`vtkSmartVolumeMapper`、`vtkOpenGLGPUVolumeRayCastMapper`（体绘制）** |
| `InteractionStyle` | 相机交互（可选） |

两个"想当然会踩"的模块名问题（脚本注释中也有记录）：

1. **9.1.0 没有 `VTK::VolumeRendering` 模块**——那是 9.2+ 的命名。9.1 中体渲染拆为 `RenderingVolume` + `RenderingVolumeOpenGL2` 两个模块；
2. **9.1.0 没有 `VTK::MPR` 模块**——MPR 由 `ImagingCore` 的 `vtkImageReslice` 实现，配合 `RenderingImage` 的 `vtkImageActor` 显示。

### 5.3 完整编译脚本

```bash
#!/bin/bash
# Android 平台 VTK 9.1.0 交叉编译脚本（CBCT 三维可视化专用裁剪版）
set -e

# ---------- NDK 环境 ----------
export ANDROID_NDK=/path/to/ndk/25.1.8937393
export TOOLCHAIN=$ANDROID_NDK/build/cmake/android.toolchain.cmake
export ANDROID_ABI=arm64-v8a        # 舍弃 32 位，精简体积
export ANDROID_PLATFORM=android-24  # Android 7.0 (API 24) 及以上

SRC_DIR="$(cd "$(dirname "$0")" && pwd)/vtk"
BUILD_DIR="$SRC_DIR/build-android-vtk"
INSTALL_PREFIX="$BUILD_DIR/install"
mkdir -p "$BUILD_DIR" && cd "$BUILD_DIR"

cmake "$SRC_DIR" \
  -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}" \
  -DANDROID_ABI="${ANDROID_ABI}" \
  -DANDROID_PLATFORM="${ANDROID_PLATFORM}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF \
  -DVTK_BUILD_TESTING=OFF \
  -DVTK_BUILD_EXAMPLES=OFF \
  -DVTK_BUILD_DOCUMENTATION=OFF \
  -DVTK_ENABLE_REMOTE_MODULES=OFF \
  -DVTK_BUILD_ALL_MODULES=OFF \
  -DVTK_USE_X=OFF \
  -DVTK_ENABLE_WRAPPING=OFF \
  -DVTK_GROUP_ENABLE_StandAlone=DONT_WANT \
  -DVTK_GROUP_ENABLE_Rendering=DONT_WANT \
  -DCMAKE_C_FLAGS_RELEASE="-Os -DNDEBUG -g0" \
  -DCMAKE_CXX_FLAGS_RELEASE="-Os -DNDEBUG -g0" \
  -DCMAKE_CXX_STANDARD=11 \
  -DCMAKE_INSTALL_PREFIX="${INSTALL_PREFIX}" \
  -DVTK_MODULE_ENABLE_VTK_CommonCore=YES \
  -DVTK_MODULE_ENABLE_VTK_ImagingCore=YES \
  -DVTK_MODULE_ENABLE_VTK_ImagingGeneral=YES \
  -DVTK_MODULE_ENABLE_VTK_RenderingCore=YES \
  -DVTK_MODULE_ENABLE_VTK_RenderingOpenGL2=YES \
  -DVTK_MODULE_ENABLE_VTK_RenderingImage=YES \
  -DVTK_MODULE_ENABLE_VTK_RenderingVolume=YES \
  -DVTK_MODULE_ENABLE_VTK_RenderingVolumeOpenGL2=YES \
  -DVTK_MODULE_ENABLE_VTK_InteractionStyle=YES \
  -DVTK_MODULE_ENABLE_VTK_FiltersGeneric=NO \
  -DVTK_MODULE_ENABLE_VTK_IOImage=NO

make -j4 && make install
```

逐项说明容易困惑的参数：

| 参数 | 说明 |
|---|---|
| `BUILD_SHARED_LIBS=OFF` | 静态库形态，最终全部链进 `cbct_native.so`，避免 so 数量爆炸 |
| `VTK_USE_X=OFF` | 真实存在的开关，Android 下默认也 OFF，显式声明保险 |
| `VTK_ENABLE_WRAPPING=OFF` | **交叉编译必关**：wrapping（Java/Python 封装）需要宿主机 VTKCompileTools，交叉环境下无法生成 |
| ~~`VTK_USE_OPENGL_ES=ON`~~ | 9.1.0 **无此开关**：Android 下 `VTK_OPENGL_USE_GLES` 由 `vtkOpenGLOptions.cmake` 自动强制开启，`vtkEGLRenderWindow` 默认注册 |
| `VTK_MODULE_ENABLE_VTK_IOImage=NO` | DCMTK 负责解析，VTK 无需图像 IO（该模块仅出现在测试依赖中，关闭安全） |
| `-Os -DNDEBUG -g0` | NDK Release 下仍强制 `-g`，用 `-g0` 去调试信息，显著缩小产物 |
| `make -j4` | 小内存开发机建议 -j4（-j8 会因内存压力偶发 `Abort trap: 6`） |

产物：`install/include/vtk-9.1`（头文件）+ `install/lib/libvtk*.a`（44 个静态库，含依赖的第三方库 fmt/loguru/lz4/lzma/pugixml/zlib 等，均带 `libvtk` 前缀）。

### 5.4 必须打的 3 个 Android/GLES 兼容补丁

VTK 9.1 的 OpenGL ES 后端有几处与真实 Android 设备不兼容的地方，直接交叉编译会**编译通过但运行黑屏**。以下补丁直接改 VTK 源码（`/Users/wangyao/cross-compilation/vtk/vtk/` 下），再执行 5.3 脚本。

#### 补丁 1：EGL 上下文必须是 ES3（否则 shader 全部编译失败）

`Rendering/OpenGL2/vtkEGLRenderWindow.cxx`：

```cpp
// VTK9 的 glew 以 GLES3 模式构建（vtkglew_GLES3=1），全部 shader 均为
// #version 300 es，必须创建 ES3 上下文；ES2 上下文下 shader 编译失败、
// 3D 纹理与 FBO READ/DRAW 绑定不可用，渲染静默黑屏。
clientAPI = EGL_OPENGL_ES3_BIT;
const EGLint contextES3[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
contextAttribs = contextES3;
```

#### 补丁 2：深度纹理格式 Fixed24 + Nearest 过滤（GPU 体绘制黑屏元凶）

`Rendering/VolumeOpenGL2/vtkOpenGLGPUVolumeRayCastMapper.cxx`：

```cpp
// OpenGL ES: glBlitFramebuffer requires the FBO depth attachment to
// match the window's depth buffer format exactly, otherwise the blit
// fails with GL_INVALID_OPERATION, the depth texture stays empty,
// and the ray termination test discards every fragment (black volume).
// EGL windows on Android use a 24-bit depth buffer, so use Fixed24
// (GL_DEPTH_COMPONENT24) instead of Fixed32 (GL_DEPTH_COMPONENT32).
this->DepthTextureObject->AllocateDepth(
  this->WindowSize[0], this->WindowSize[1], vtkTextureObject::Fixed24);
```

```cpp
#ifdef GL_ES_VERSION_3_0
// OpenGL ES: DEPTH_COMPONENT24 is not texture-filterable (ES 3.x spec
// Table 3.13). With LINEAR filters the depth texture is incomplete and
// the sampler silently returns 0, so the ray termination test discards
// every fragment (black volume). NEAREST is exact here anyway: the depth
// blit below is a 1:1 copy.
this->DepthTextureObject->SetMagnificationFilter(vtkTextureObject::Nearest);
this->DepthTextureObject->SetMinificationFilter(vtkTextureObject::Nearest);
#else
this->DepthTextureObject->SetMagnificationFilter(vtkTextureObject::Linear);
this->DepthTextureObject->SetMinificationFilter(vtkTextureObject::Linear);
#endif
```

#### 补丁 3：Shader 源码 `#version` 必须在第一行

`Rendering/OpenGL2/vtkOpenGLRenderWindow.cxx` 中 `ResolveShader` / `DepthBlitShader` 的原始字符串字面量**首字符不能有换行**，否则 GLES 驱动报 `#version must be on the first line`，FBO 深度拷贝（体渲染前置步骤）失败。

这三个补丁的完整排障过程见第九章。

---

## 六、交叉编译产物集成到 Android 工程

### 6.1 CMakeLists.txt 集成

```cmake
# ========== DCMTK 头文件与静态库（复用 dcmtk 模块预编译产物） ==========
set(DCMTK_CPP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../dcmtk/src/main/cpp)
include_directories(${DCMTK_CPP_ROOT})
file(GLOB DCMTK_STATIC_LIBS
        "${DCMTK_CPP_ROOT}/lib/${ANDROID_ABI}/*.a"
        "${DCMTK_CPP_ROOT}/lib/arm64-v8/*.a")

# ========== VTK 9.1.0 头文件与静态库（android_vtk.sh 产物） ==========
set(VTK_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${VTK_ROOT}/include/vtk-9.1)
# 全量收集 VTK 模块库与第三方库（均带 libvtk 前缀）
file(GLOB VTK_STATIC_LIBS "${VTK_ROOT}/lib/libvtk*.a")

add_library(${CMAKE_PROJECT_NAME} SHARED
        cbct-native-lib.cpp
        CbctSeriesParser.cpp
        CbctVtkRenderer.cpp)

# 裁包体积：函数/数据分节。
# 注意：暂不启用 --gc-sections —— DCMTK 内置数据字典等静态数据表
# 仅被惰性初始化路径引用，链接期回收会导致 JPEG 解压时
# "Tag not found in data dictionary"（见 4.4）。
target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE
        -ffunction-sections -fdata-sections)

target_link_libraries(${CMAKE_PROJECT_NAME}
        # DCMTK：dcmjpeg/dcmjpls/dcmimage 之间循环依赖，分组链接
        -Wl,--start-group
        ${DCMTK_STATIC_LIBS}
        -Wl,--end-group
        # VTK：模块间同样存在循环引用
        -Wl,--start-group
        ${VTK_STATIC_LIBS}
        -Wl,--end-group
        # Android 系统依赖（EGL/GLES：VTK ES 渲染窗口所需）
        log android jnigraphics EGL GLESv2 GLESv3 z m atomic)
```

要点：

- **`-Wl,--start-group/--end-group`**：静态库链接器按命令行顺序单向解析符号，VTK 模块间、DCMTK 模块间均存在循环依赖，分组让链接器反复扫描直至稳定；
- **系统库**：`EGL` + `GLESv2/GLESv3` 是 `vtkEGLRenderWindow` 的运行时依赖；
- **`ANDROID_STL c++_shared`**：VTK 9.1 需要 RTTI/异常（`-frtti -fexceptions`）。

### 6.2 Gradle 与 so 加载

```kotlin
// build.gradle.kts
android {
    defaultConfig {
        ndk { abiFilters += "arm64-v8a" }   // VTK 静态库仅此 ABI
    }
    externalNativeBuild {
        cmake { path = file("src/main/cpp/CMakeLists.txt") }
    }
}
```

```kotlin
object CbctVtkJni {
    init { System.loadLibrary("cbct_native") }  // 单 so 含解析+渲染双能力
}
```

### 6.3 静态库初始化：VTK_MODULE_INIT

VTK 静态构建时，各模块的**对象工厂（vtkObjectFactory）注册不会自动执行**，必须显式初始化，否则 `vtkSmartPointer<vtkSmartVolumeMapper>::New()` 找不到 OpenGL 实现类：

```cpp
#include "vtkAutoInit.h"
VTK_MODULE_INIT(vtkRenderingOpenGL2)       // -> vtkEGLRenderWindow
VTK_MODULE_INIT(vtkRenderingVolumeOpenGL2) // -> vtkSmartVolumeMapper 工厂
```

漏掉这一步的现象：`vtkSmartVolumeMapper` 创建的是抽象基类实例或直接空指针，渲染管线装配失败。

---

## 七、CBCT DICOM 序列解析实现

解析层全部在 `CbctSeriesParser.cpp`（纯 C++，可独立复用/测试），JNI 层只做类型转换。

### 7.1 双 Pass 设计：先读元数据排序，再读像素填充

一例 CBCT 序列动辄数百张切片，若"逐文件全量读入再组装"，峰值内存会同时持有数百个 `DcmFileFormat`（含完整像素数据）。本实现拆成两遍：

**Pass A（并行，多线程）**：只读元数据——`Rows/Columns`、`PixelSpacing`、`SliceThickness`、`ImagePositionPatient[2]`（Z 坐标）、`RescaleSlope/Intercept`、`NumberOfFrames/GridFrameOffsetVector`（多帧封装）、患者/检查信息、窗宽窗位。单文件级内存开销极小（读完全即析构）；

**Pass B（并行，多线程）**：按排序结果逐文件解码像素，写入 Volume 的对应层。

```cpp
// Pass A：多线程读元数据（原子计数取任务，天然负载均衡）
parallelFor(files.size(), [&](size_t i) {
    if (readMeta(files[i], metas[i])) valid[i] = 1;
    size_t done = metaDone.fetch_add(1) + 1;
    if (progress) progress(done, files.size() * 2);   // 两阶段共 2N 步
});
```

`parallelFor` 是自实现的动态分片线程池：线程数 `min(CPU 核数, 8)`，原子计数取任务，避免慢文件拖垮整体。

### 7.2 切片排序：Z 坐标是唯一可靠依据

**按文件名排序是被禁止的**——部分口腔 CBCT 设备导出的文件名是随机命名，不代表扫描顺序，会导致体数据上下颠倒、切片错乱。DICOM 标准定义的切片真实位置是 LPS 坐标系中的 `ImagePositionPatient`：

```cpp
// 排序优先级：ImagePositionPatient[2] > SliceLocation > InstanceNumber
if (withZ == slices.size()) {
    std::stable_sort(slices.begin(), slices.end(),
                     [](const SliceMeta &a, const SliceMeta &b) { return a.z < b.z; });
} else if (...) { /* SliceLocation / InstanceNumber 兜底 */ }
else {
    err = "inconsistent positioning tags (mixed ImagePositionPatient)";
    return nullptr;   // 定位标签不一致 = 序列可疑，拒绝解析
}
```

多帧封装（整个序列存成单个多帧 DICOM 文件的高端设备）同样适配：`NumberOfFrames` + `GridFrameOffsetVector` 逐帧换算 Z 坐标（缺失时按层厚估算）。

### 7.3 断层补全与 Volume 组装

排序后计算 Z 间距（**取相邻 Z 差的中位数**，抗离群值），再把每个切片（及其多帧的每一帧）映射到 Volume 层索引：

```cpp
long idx = lround((zf - z0) / spacingZ);   // Z 坐标 -> 层索引
```

间隙处索引跳空——Volume 预先填充为 `HU(intercept)`（即空气值），**断层间隙自动补全为空白切片**，保证体数据几何连续（MPR/VR 正确成像的前提，否则切面出现黑线）。

内存守卫：`500×500×500 float` 体数据上限 600MB，超限拒绝解析并报错；`new float[]` 落在 Native 堆（绕开 Java 堆上限）。

### 7.4 为什么体素存 float HU：GLES3 的纹理格式约束

这是本工程最"反直觉"的设计决策，链路如下：

1. DICOM 像素是 16bit 整数，`HU = pixel × slope + intercept`；
2. VTK 体渲染把体数据作为 **3D 纹理**上传 GPU；
3. **GLES3 没有 `GL_R16` / `GL_R16_SNORM` 归一化 16bit 整数 3D 纹理格式**（桌面 GL 专属），`vtkVolumeTexture` 对 Uint16 输入在 ES 端解析出空 internalFormat → 体绘制静默黑屏；
4. `GL_R32F` 是 ES3 核心保证可用的全精度路径。

因此解析侧直接把体素换算为 **float HU** 存储，一举三得：规避纹理格式问题、传递函数/窗宽窗位全程直接工作在 HU 域、无需维护 raw↔HU 双域换算。代价是内存 ×2（uint16→float），由 600MB 守卫约束。

```cpp
// Pass B：raw -> HU，逐片 Rescale 参数换算（slope/intercept 可能逐片不同）
if (sm.pixelRepresentation != 0) {   // 有符号
    for (size_t i = 0; i < sliceSize; ++i)
        dst[i] = (float) ((double) (Sint16) src[i] * slope + intercept);
} else {
    for (size_t i = 0; i < sliceSize; ++i)
        dst[i] = (float) ((double) src[i] * slope + intercept);
}
```

### 7.5 压缩序列解压

Pass B 中统一经 `chooseRepresentation` 转显式小端，JPEG / JPEG-LS / RLE 解码器进程级注册一次：

```cpp
void ensureCodecsRegistered() {
    static bool registered = false;
    if (!registered) {
        DJDecoderRegistration::registerCodecs();      // JPEG
        DJLSDecoderRegistration::registerCodecs();    // JPEG-LS（CBCT 常见）
        DcmRLEDecoderRegistration::registerCodecs();  // RLE
        registered = true;
    }
}

// 读像素时：压缩/大端/隐式格式统一归一
OFCondition repCond = ds->chooseRepresentation(EXS_LittleEndianExplicit, nullptr);
```

JPEG-LS 解压依赖数据字典（4.4 节的陷阱），`loadSeries` 入口处有诊断日志 `dataDict loaded=1/0`。

### 7.6 窗宽窗位容错

部分 CBCT 设备导出缺失 `WindowWidth/WindowCenter`，解析侧自动回退 **CBCT 骨骼窗 WW4000 / WC600**，避免后续渲染灰度异常。

### 7.7 Android 分区存储适配

Android 10+ 分区存储下，SAF 返回的是 `content://Uri`，而 DCMTK 只支持文件路径。模块 `CbctFileTransfer` 的做法：SAF 授权目录 → 遍历 `DocumentFile` 拷贝到应用私有目录 → DCMTK 从私有目录读取。测试路径可直填绝对路径（如 `/storage/emulated/0/...` 或 Assets 内置测试序列释放后的私有路径）。

---

## 八、VTK 三维渲染实现

渲染核心 `CbctVtkRenderer.cpp`，实现 VR/MPR 双管线 + 专用渲染线程。

### 8.1 零拷贝体数据导入

数百 MB 体数据不能拷贝进 VTK（内存直接翻倍），用 `SetArray(ptr, n, save=1)` 让 `vtkFloatArray` **只读借用** Native 内存：

```cpp
vtkNew<vtkImageData> img;
img->SetDimensions(vol->width, vol->height, vol->depth);
img->SetSpacing(vol->spacingX, vol->spacingY, vol->spacingZ);
img->SetOrigin(0.0, 0.0, 0.0);   // index 域与世界域对齐，简化 MPR 轴矩阵
const vtkIdType n = (vtkIdType) vol->width * vol->height * vol->depth;
vtkNew<vtkFloatArray> arr;
arr->SetArray(vol->data, n, 1);  // save=1：VTK 不接管释放
img->GetPointData()->SetScalars(arr);
```

**生命周期强约束**：`CbctVolume` 的释放权仍在解析侧，因此 **`destroyRenderer()` 必须先于 `releaseVolume()` 调用**（`CbctVtkView.release()` 内部已保证该顺序）。

### 8.2 VR 体绘制管线

```
vtkImageData ──> vtkSmartVolumeMapper ──> vtkVolume
                      │                       │
                      └── vtkVolumeProperty ──┘
                          ├─ vtkColorTransferFunction（颜色传递）
                          └─ vtkPiecewiseFunction（不透明度传递）
```

```cpp
volMapper_ = vtkSmartPointer<vtkSmartVolumeMapper>::New();
volMapper_->SetInputData(imageData_);
volMapper_->SetBlendModeToComposite();     // 组合成像

volProperty_->SetColor(colorTF_);
volProperty_->SetScalarOpacity(opacityTF_);
volProperty_->ShadeOn();                   // 明暗着色，增强骨骼立体感
volProperty_->SetInterpolationTypeToLinear();
```

两个关键选型说明：

- **为什么是 `vtkSmartVolumeMapper`**：它按设备能力自动选择 GPU RayCast / CPU FixedPoint 路径（`GetLastUsedRenderMode()` 可查询）。网上大量 VTK 8.x 教程用的 `vtkVolumeRayCastMapper` 在 9.x 已移除；
- **传递函数即窗宽窗位**：`vtkVolumeProperty` 没有 `SetColorWindow/SetColorLevel`，颜色/不透明度控制点由 WW/WC 换算到 HU 域动态生成——**色彩跨度跟随窗宽窗位实时变化**：

```cpp
void CbctVtkRenderer::applyWindowLevel() {
    const double lo = wc_ - ww_ / 2.0;
    const double hi = wc_ + ww_ / 2.0;
    const double mid = 0.5 * (lo + hi);

    // VR：暖灰阶骨窗，低密度暗色 -> 高密度亮白
    colorTF_->RemoveAllPoints();
    colorTF_->AddRGBPoint(lo,  0.02, 0.02, 0.04);
    colorTF_->AddRGBPoint(mid, 0.68, 0.64, 0.58);
    colorTF_->AddRGBPoint(hi,  1.00, 0.99, 0.96);

    // 不透明度：HU<200 全透明（骨骼阈值过滤软组织），
    // 200->1300 HU 渐升，高位饱和
    opacityTF_->RemoveAllPoints();
    opacityTF_->AddPoint(-1024.0, 0.00);
    opacityTF_->AddPoint(200.0,   0.00);
    opacityTF_->AddPoint(1300.0,  0.85);
    opacityTF_->AddPoint(4000.0,  0.90);
    volume_->Modified();   // 触发 Mapper 重建纹理查找表
}
```

**R32F 线性过滤的设备差异**：`GL_R32F` 的 LINEAR 过滤依赖 `GL_OES_texture_float_linear` 扩展，渲染窗口初始化后检测，不支持则自动降级 Nearest（否则不完整纹理采样全黑）：

```cpp
const char *exts = (const char *) glGetString(GL_EXTENSIONS);
if (exts && strstr(exts, "GL_OES_texture_float_linear")) {
    floatLinear_ = true;                     // Linear 可用
} else {
    volProperty_->SetInterpolationTypeToNearest();   // 降级
}
```

### 8.3 MPR 切面管线

```
vtkImageData ──> vtkImageReslice ──> vtkImageMapToColors ──> vtkImageActor
                （三轴切面矩阵）      （vtkLookupTable 灰阶）
```

`vtkImageReslice` 用 `SetResliceAxesDirectionCosines + SetResliceAxesOrigin` 定义切面（axial 固定 Z / coronal 固定 Y / sagittal 固定 X），`SetOutputDimensionality(2)` 输出单张 2D 切面，线性插值采样。以冠状面为例：

```cpp
case PLANE_CORONAL:
    // 输出 x 轴取输入 x，输出 y 轴取输入 z（横向 x 纵向 z）
    reslice_->SetResliceAxesDirectionCosines(1, 0, 0, 0, 0, 1, 0, 1, 0);
    reslice_->SetResliceAxesOrigin(0.0, pos * vol_->spacingY, 0.0);
    reslice_->SetOutputSpacing(vol_->spacingX, vol_->spacingZ, vol_->spacingY);
    reslice_->SetOutputExtent(0, w - 1, 0, d - 1, 0, 0);
    break;
```

灰阶映射用饱和度为 0 的 `vtkLookupTable`，其 **Range 即窗宽窗位区间**——MPR 切面与 VR 色彩跨度共用同一 WW/WC 状态：

```cpp
lut_->SetHueRange(0.0, 0.0);        // 灰阶：色相/饱和度置零
lut_->SetSaturationRange(0.0, 0.0);
lut_->SetValueRange(0.0, 1.0);      // 亮度 0..1（窗宽窗位映射区间）
lut_->SetNumberOfTableValues(256);
// applyWindowLevel() 中：lut_->SetRange(lo, hi)
```

相机用**平行投影**（`ParallelProjectionOn`）保证切面无透视畸变；VR 则透视投影 + 45° 俯视初始视角。

### 8.4 渲染线程模型（关键设计）

EGL/OpenGL ES 上下文与线程绑定，因此**所有 GL 操作（含 VTK Render）固定在模块内部专用渲染线程**执行。外部调用（JNI/UI 线程）通过任务队列投递，`post(task, synchronous)` 双模式：

- **异步任务**（手势、参数调节等高频事件）：入队即返回，渲染线程排空队列后统一上屏——连续滑动 N 帧自动合并，不丢事件也不重复渲染；
- **同步任务**（Surface 创建/销毁等时序点）：`doneCv_.wait()` 阻塞等待渲染线程执行完毕，保证与 Android Surface 状态严格一致；
- **按需渲染**：队列排空且 `dirty_` 才调 `Render()`，闲置状态 **0 GPU 占用**。

```cpp
void CbctVtkRenderer::renderLoop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        cv_.wait(lock, [this] { return quit_ || !tasks_.empty(); });
        while (!tasks_.empty()) {          // 排空任务队列
            auto task = std::move(tasks_.front());
            tasks_.pop_front();
            lock.unlock();
            try { task(); } catch (...) { LOGE("task exception"); }
            lock.lock();
        }
        if (quit_) break;
        if (dirty_ && renderWindow_ && window_) {   // 按需渲染
            dirty_ = false;
            lock.unlock();
            renderWindow_->Render();
            lock.lock();
        }
    }
}
```

### 8.5 Surface 对接：vtkEGLRenderWindow + ANativeWindow

`vtkEGLRenderWindow::SetWindowId(ANativeWindow*)` 直接挂接 Android 原生窗口。SurfaceView 三个回调与 Native 动作的映射：

| SurfaceView 回调 | Native 动作（同步执行） |
|---|---|
| `surfaceCreated` | `ANativeWindow_fromSurface` 取窗口（+1 引用）→ 创建 EGL RenderWindow/Renderer → 挂载当前模式 Props → 首帧自动复位相机 |
| `surfaceChanged` | `SetSize` 适配旋转/分屏 |
| `surfaceDestroyed` | `Finalize()` 释放 EGL Surface/Context → `ANativeWindow_release` 归还引用。**必须同步**：回调返回后 Surface 即被系统回收，禁止再有任何 GL 操作 |

EGL 窗口配置要点（`ensureWindow()`）：

```cpp
renderWindow_->SetWindowId(window_);
renderWindow_->SetSize(surfW_, surfH_);
renderWindow_->SetMultiSamples(0);   // ES 端体渲染避免 MSAA 配置兼容问题
renderWindow_->SetSwapControl(1);    // 垂直同步，避免撕裂
renderWindow_->Initialize();         // 创建 EGL Context/Surface 并 MakeCurrent
```

### 8.6 手势交互（Kotlin → JNI → VTK 相机）

`CbctVtkView` 继承 SurfaceView，职责严格内聚：**生命周期转递 + 手势捕获**，渲染逻辑全在 Native。手势映射：

| 手势 | VR 模式 | MPR 模式 |
|---|---|---|
| 单指滑动 | 旋转相机（Azimuth/Elevation，trackball 语义约 180°/视口高） | 平移切面 |
| 双指捏合 | 相机沿视线远近（钳制初始距离 [0.1x, 10x]） | 平行缩放 |
| 双指拖动 | 模型位置偏移（捏合焦点位移即平移增量） | 同左 |
| 复位按钮 | 恢复 45° 俯视 + `ResetCamera` | 切面重新铺满视口 |

相机旋转的实现（trackball 灵敏度）：

```cpp
void CbctVtkRenderer::applyRotate(double dx, double dy) {
    vtkCamera *cam = renderer_->GetActiveCamera();
    const double k = 180.0 / (double) (surfH_ > 0 ? surfH_ : 720);
    cam->Azimuth(-dx * k);
    cam->Elevation(dy * k);
    cam->OrthogonalizeViewUp();
}
```

平移（`applyPan`）按平行/透视投影分别换算世界坐标/像素比（`ParallelScale` 或视场角×距离），沿视平面基向量（`dir × up` 右手系）偏移相机位置与焦点，保证拖拽方向与内容移动方向一致。

**状态重放机制**：`CbctVtkView` 缓存当前 UI 状态（模式/平面/层位置/窗宽窗位），`setVolume()` 注入新序列后自动重放——跨序列切换保持浏览参数。

### 8.7 JNI 接口一览

| 方法 | 说明 | 执行方式 |
|---|---|---|
| `createRenderer(volumePtr)` | 创建渲染器（零拷贝导入） | 调用线程 |
| `onSurfaceCreated/Changed/Destroyed` | Surface 生命周期转递 | 同步 |
| `setRenderMode(MODE_VR/MODE_MPR)` | VR/MPR 互切（Prop 增删 + 相机适配） | 异步 |
| `setPlane(plane, position)` | MPR 平面与层位置 | 异步 |
| `setWindowLevel(ww, wc)` | 窗宽窗位（双管线共用状态） | 异步 |
| `rotate/pan/zoom` | 手势驱动相机 | 异步 |
| `resetCamera()` | 复位相机 | 异步 |
| `destroyRenderer(ptr)` | 同步停渲染线程并销毁 | 同步 |

---

## 九、踩坑实录：GLES3 上的黑屏连环坑

VTK 在 Android GLES3 上跑体渲染，几乎每一步都有"编译通过、运行黑屏"的陷阱。以下按排查顺序记录（这部分没有原理图，全是真机 logcat 一行行抠出来的）。

### 坑 1：`#version must be on the first line`

**现象**：VTK 初始化即报 shader 编译失败。
**根因**：`vtkOpenGLRenderWindow.cxx` 中 `ResolveShader`/`DepthBlitShader` 原始字符串字面量首字符是换行，桌面 GL 驱动宽容、GLES 驱动严格。
**修复**：删掉字面量开头的换行（补丁 3，见 5.4）。

### 坑 2：ES2 上下文下一切静默失败

**现象**：shader 全部编译失败、3D 纹理与 FBO READ/DRAW 绑定不可用，渲染静默黑屏。
**根因**：VTK9 的 glew 以 GLES3 模式构建，全部 shader 为 `#version 300 es`，但 `vtkEGLRenderWindow` 默认创建 ES2 上下文。
**修复**：EGL 上下文升级 ES3（补丁 1，见 5.4）。

### 坑 3：3D 纹理格式——`GL_R16` / `GL_R16_SNORM` 是桌面 GL 专属

**现象**：`Texture 3D allocation failed! Failed to determine texture parameters`。
**根因**：GLES3 没有归一化 16bit 整数 3D 纹理格式，`vtkVolumeTexture` 对 Uint16 输入解析出空 internalFormat。
**修复**：解析侧体素直接存 **float HU**（对应 `GL_R32F`，ES3 核心格式），见 7.4。

### 坑 4：GPU RayCast 黑屏但帧耗时正常（14ms）

这是最隐蔽的一个。**现象**：VR 模式 14ms/帧（说明 GPU 确实在画），画面全黑；切 CPU RayCast 模式则正常显示。

**排查手段——诊断模式二分法**：复位按钮循环切换 4 种假设（GPU 基线 / 全值域不透明红 / CPU RayCast / GPU+Linear）：

- 全值域不透明红 → 黑：3D 纹理采样无输出 or 所有片元被丢弃；
- CPU 正常 → GPU 管线专属问题。

进一步在 Render 后回读 DisplayFBO：背景值正常、体数据区域全 0 → 片元被**光线终止测试（ray termination test）全部丢弃**。光线终止测试依赖**深度纹理**：体渲染先把场景深度 blit 到深度纹理，RayCast 中判断射线是否已命中前景物体。

**根因链（两连击）**：

1. `glBlitFramebuffer` 要求 FBO 深度附件与窗口深度缓冲**格式完全一致**，Android EGL 窗口深度缓冲是 24bit，VTK 默认 `Fixed32` → blit 报 `GL_INVALID_OPERATION`，深度纹理为空；
2. 改成 `Fixed24` 后仍黑——ES 3.x 规范（Table 3.13）规定 `DEPTH_COMPONENT24` **不可纹理过滤**，LINEAR 过滤下深度纹理不完整，采样器静默返回 0。

**修复**：补丁 2（见 5.4）——ES 下用 `Fixed24` + Nearest 过滤。

### 坑 5：诊断代码引发的 framebuffer 绑定漂移

**现象**：加了坑 4 的诊断代码（裸 `glBindFramebuffer` 遍历枚举 FBO）之后，MPR 渲染开始进错 FBO。
**根因**：VTK 的 `vtkOpenGLState` 维护 GL 状态缓存，裸 `glBindFramebuffer` 绕过缓存 → 缓存值 ≠ 真实绑定 → 后续 `Start()` 里 `RenderFramebuffer` 的 Bind 被缓存跳过（缓存认为已绑定）→ 场景渲染进错误 FBO。
**修复**：每帧渲染前把**真实绑定值经 `vtkglBindFramebuffer` 重新过一遍缓存**（不一致时触发真实 Bind 并更新缓存，一致时 no-op）：

```cpp
if (renderWindow_->GetState()) {
    GLint realDraw = 0, realRead = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &realDraw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &realRead);
    renderWindow_->GetState()->vtkglBindFramebuffer(
            GL_DRAW_FRAMEBUFFER, (unsigned) realDraw);
    renderWindow_->GetState()->vtkglBindFramebuffer(
            GL_READ_FRAMEBUFFER, (unsigned) realRead);
    while (glGetError() != GL_NO_ERROR) {}
}
```

### 坑 6：`--gc-sections` 裁掉数据字典

见 4.4 节。症状是 JPEG-LS 序列解压失败、未压缩序列正常，极难第一时间联想到链接期优化。

### 排障方法论小结

- **分层二分**：先验证世界空间（挂纯色不透明立方体 Actor）→ 再验证纹理类 Prop → 再验证 GPU/CPU 路径差异；
- **错误队列要手动排空**：Release 构建下 VTK 错误宏是 no-op，GL 错误会滞留队列，必须 `while (glGetError() != GL_NO_ERROR) {}` 隔离每帧错误；
- **Release 下 VTK 错误输出重定向 logcat**：`vtkAndroidOutputWindow::SetInstance()`，tag `CbctVtk`。

---

## 十、性能与稳定性设计

| 项 | 措施 |
|---|---|
| 内存 | 零拷贝导入（数百 MB 体数据无重复占用）；Native 堆连续存储 + 600MB 守卫；双 Pass 避免整序持有全部文件对象 |
| CPU | Pass A/B 双阶段并行（动态分片负载均衡）；R32F 纹理使传递函数直接工作在 HU 域（无逐帧换算） |
| GPU | 按需渲染（闲置 0 占用）；垂直同步防撕裂；GPU RayCast 失败自动回退 CPU FixedPoint |
| 流畅度 | 高频手势事件队列合并渲染；单帧 14ms 量级（arm64 真机，GPU RayCast） |
| 线程安全 | 所有 GL/VTK 操作固定渲染线程；Surface 时序点同步执行；DCMTK 逐线程独立 `DcmFileFormat` |
| 健壮性 | 渲染器/Volume 释放顺序强约束；任务异常捕获不崩线程；`onDetachedFromWindow` 兜底释放防泄漏；多级容错（文件级过滤损坏切片、元数据级补默认值、内存级守卫） |

---

## 十一、总结

本文完整呈现了 Android 平台 CBCT 三维可视化的技术闭环：

1. **概念层**：CBCT 的锥形束采集/FDK 重建原理，及其 DICOM 数据"各向同性体素 + JPEG-LS 压缩 + 骨骼窗"的解析侧特征；
2. **构建层**：DCMTK 3.6.8 与 VTK 9.1.0 的 NDK 交叉编译——VTK 的 module/group 两级裁剪机制、静态库分组链接、数据字典外部注入；
3. **解析层**：双 Pass 并行解析、Z 坐标排序、断层补全、float HU 体素策略；
4. **渲染层**：零拷贝导入、VR/MPR 双管线、专用渲染线程 + 按需渲染、Surface/手势适配；
5. **排障层**：GLES3 上从 shader、上下文、纹理格式到深度纹理连环坑的系统排查方法。

核心经验浓缩成一句话：**在移动端做医学三维渲染，一半工作在"让桌面图形库活着跑起来"**——理解 VTK 状态缓存机制、GLES 与桌面 GL 的格式差异、静态库链接的符号回收策略，与理解体渲染算法本身同样重要。

> 工程源码：`DcmtkDemo` 项目 `cbctdeal` 模块（含 VTK 交叉编译脚本 `doc/android_vtk.sh`、演示视频、参考研究报告）。演示视频见模块 README。
