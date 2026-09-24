# DcmtkDemo - Android 医疗影像全流程解决方案 (PACS + DICOM + 3D CBCT)

🌐 **中文** | **[English](README.en.md)**

`DcmtkDemo` 是一个专为 Android 平台设计的医疗影像处理全栈示例项目。它集成了 **DCMTK (DICOM Toolkit)**、**VTK 9.1.0** 和 **OpenCV**，实现了从 PACS 网络通信、DICOM 文件解析到高性能像素预处理，以及 **CBCT（锥形束 CT）三维体渲染与 MPR 重建** 的全链路功能。

> [README.en.md](README.en.md) 是本文件的英文对照版，内容与中文版逐节同步；仓库内的模块文档、代码注释与技术文章以中文为主。

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

## 📚 技术文章与原理出处 (Technical Articles)

本项目的实现原理已整理成三篇系列文章，覆盖「工程架构 → 交叉编译 → 服务端联调」三条主线。下表标注了每篇文章对应本仓库的哪些模块与代码，便于对照阅读。

| # | 文章 | 讲什么 | 对应本仓库位置 |
| :-- | :--- | :----- | :------------- |
| 1 | [DcmtkDemo 技术解析：医学影像处理全栈方案](https://blog.csdn.net/wangyongyao1989/article/details/163419757) | 三模块架构与数据流、JNI 桥接层设计（`SafeNewStringUTF` / `JniString` RAII）、PACS 网络通信、DICOM 文件 I/O、Kotlin 协程编排、预处理算子与智能调窗、`Mat` 地址链式传递、FT46 探测器调参、MWL 同步落库 | `:dcmtk`（`native-lib.cpp` / `PacsClient.cpp` / `DicomFileIO.cpp` / `ProgressScu.cpp`）、`:rawpixeldeal`（`native-lib.cpp` / `MedicalCTPreprocess.cpp` / `CtSeriesProcessor.cpp` / `ImageProcessor.cpp`）、`:app` 各 Fragment |
| 2 | [Mac Pro 上的 DCMTK 服务搭建](https://blog.csdn.net/wangyongyao1989/article/details/162460525) | 主机侧 PACS 与 Worklist 服务端从零搭建：DCMTK 3.6.8 源码编译、`dcmqrscp.cfg` 配置、C-ECHO / C-STORE / C-FIND / C-MOVE 回环验证、`.wl` 待检查单模板与二进制转换、Android 端参数对接 | 本文的「环境搭建流程 → 第 5 步 主机侧 PACS 服务」；App 侧参数见 `dcmtk/src/main/java/com/example/dcmtk/utils/PacsPrefs.kt`，Worklist 模板见 `dcmtk/src/main/assets/wlistqry/` |
| 3 | [CBCT 三维可视化实战：DCMTK + VTK 交叉编译、集成与体渲染全流程](https://blog.csdn.net/wangyongyao1989/article/details/164620462) | VTK 9.1.0 面向 Android 的模块化裁剪交叉编译、DCMTK 交叉编译与**数据字典陷阱**、双 Pass 序列解析、零拷贝体数据导入、VR/MPR 双管线、GLES3 上的 4 个黑屏兼容性问题 | `:cbctdeal`（`CbctSeriesParser.cpp` / `CbctVtkRenderer.cpp` / `CbctJniHelper.cpp` / `cbct-native-lib.cpp`）、编译脚本 [`cbctdeal/doc/android_vtk.sh`](cbctdeal/doc/android_vtk.sh) |

**仓库内配套深度文档**（PDF / 长文，文章 3 的原始素材）：

*   [Android 平台 VTK 集成与 CBCT 三维重建技术实战](cbctdeal/Android平台VTK集成与CBCT三维重建技术实战.md)
*   [CBCT 原理、技术架构与临床应用深度研究报告](cbctdeal/doc/CBCT（锥形束计算机断层扫描）原理、技术架构与临床应用深度研究报告.pdf)
*   [Android 平台基于 DCMTK 的 CBCT DICOM 序列解析技术](cbctdeal/doc/深度研究：Android平台基于DCMTK的CBCT%20DICOM序列解析技术.pdf)
*   [自适应调节医学 CT 序列图像窗宽窗位算法](dcmtk/src/main/doc/自适应调节医学CT序列图像窗宽窗位算法.pdf) —— `Peak Area Auto` 调窗算法的需求出处

---

## 🛠️ 环境搭建流程 (Getting Started)

### 0. 版本与依赖

| 组件 | 版本 | 在本仓库中的位置 |
| :--- | :--- | :--------------- |
| AGP / Gradle | 9.2.1 / 9.4.1 | `gradle/libs.versions.toml`、`gradle/wrapper/gradle-wrapper.properties` |
| compileSdk / minSdk / targetSdk | 37 / 24 / 37 | `app/build.gradle.kts` |
| CMake | 3.22.1 | 各模块 `build.gradle.kts` 的 `externalNativeBuild` |
| NDK | r25 (25.1.8937393)，`android-24` 为 API 基线 | 见 `cbctdeal/doc/android_vtk.sh` 的 `ANDROID_NDK` |
| ABI | `:dcmtk`、`:cbctdeal` 仅 `arm64-v8a`；`:rawpixeldeal` 另含 `armeabi-v7a` | 各模块 `abiFilters` |
| C++ 标准 / STL | C++11，`-frtti -fexceptions`，`c++_shared` | 各模块 `CMakeLists.txt` |
| DCMTK | 3.6.9（静态库 + 头文件已入库） | `dcmtk/src/main/cpp/dcmtk/`（头文件）、`dcmtk/src/main/cpp/lib/arm64-v8/`（29 个 `.a`） |
| VTK | 9.1.0 裁剪版（静态库 + 头文件已入库） | `cbctdeal/src/main/cpp/include/vtk-9.1/`、`cbctdeal/src/main/cpp/lib/`（43 个 `libvtk*.a`） |
| OpenCV | 4.12.0（共享库） | `rawpixeldeal/src/main/cpp/include/`、`rawpixeldeal/src/main/cpp/libs/<ABI>/libopencv_java4.so` |

> 文章 2 的主机端服务用 DCMTK **3.6.8** 源码编译，Android 端交叉编译入库的产物是 **3.6.9**（见 `osconfig.h` 的 `PACKAGE_VERSION`）；两者在 C-ECHO / C-FIND / C-STORE / C-MOVE 这些上层协议上完全互通，联调无需对齐小版本。

> **好消息：三套第三方二进制都已随仓库提交，clone 后无需重新交叉编译即可直接 `assembleDebug`。** 下面的第 1~3 步只在你要换 NDK 版本、加模块或精简体积时才需要。

### 1. 交叉编译 DCMTK（出处：文章 3 第四章）

```bash
cmake .. \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 \
  -DBUILD_SHARED_LIBS=OFF \
  -DDCMTK_WITH_ICU=OFF -DDCMTK_WITH_XML=OFF -DDCMTK_WITH_PNG=OFF \
  -DDCMTK_WITH_TIFF=OFF -DDCMTK_WITH_ZLIB=ON \
  -DDCMTK_BUILD_SAMPLES=OFF -DDCMTK_BUILD_TESTING=OFF
```

关第三方依赖是为了裁体积，本仓库入库产物的取舍可以直接从 `dcmtk/src/main/cpp/dcmtk/config/osconfig.h` 读出来：`#define WITH_ZLIB` 生效，`WITH_LIBPNG` / `WITH_LIBTIFF` / `WITH_LIBXML` / `WITH_LIBICONV` / `WITH_OPENSSL` 全部 `#undef`。

两个易错点：出现 `__atomic_load_8` 未定义时追加 `-latomic`；静态库间循环依赖（`dcmjpeg` / `dcmjpls` / `dcmimage` / `dcmimgle` / `ijg*` 互相引用）用 `-Wl,--start-group ... --end-group` 解开——本工程在 `dcmtk/src/main/cpp/CMakeLists.txt` 与 `cbctdeal/src/main/cpp/CMakeLists.txt` 中都已这样处理。产物放入 `dcmtk/src/main/cpp/lib/arm64-v8/`（该目录名是历史遗留，CMake 里同时 glob 了 `lib/${ANDROID_ABI}/` 与 `lib/arm64-v8/` 两个路径）。

### 2. 交叉编译 VTK 9.1.0（出处：文章 3 第五章）

直接执行仓库脚本 [`cbctdeal/doc/android_vtk.sh`](cbctdeal/doc/android_vtk.sh)，把 `ANDROID_NDK` 与 `SRC_DIR` 改成你本机路径即可。裁剪要点：

*   **先关组、再开模块**：VTK 9.1 顶层 CMakeLists 默认把 `StandAlone` / `Rendering` 两组设为 `WANT`，逐个模块 `=NO` 无效，必须 `-DVTK_GROUP_ENABLE_StandAlone=DONT_WANT -DVTK_GROUP_ENABLE_Rendering=DONT_WANT`，再显式 `=YES` 需要的模块，否则退化成全量构建数百个库。
*   **9.1.0 没有 `VolumeRendering` 和 `MPR` 模块**（那是 9.2+ 的命名）。体渲染拆成 `RenderingVolume` + `RenderingVolumeOpenGL2`；MPR 由 `ImagingCore` 的 `vtkImageReslice` 配合 `RenderingImage` 的 `vtkImageActor` 实现。
*   产物：`include/vtk-9.1/` 平铺头文件 + `lib/libvtk*.a`，链接时同样用 `--start-group` 解模块间循环依赖。

### 3. 注入 DCMTK 数据字典（出处：文章 3 第 4.4 节，**必读的坑**）

交叉编译的 DCMTK 未把字典编进库。症状极具迷惑性：**未压缩序列解析一切正常，只有 JPEG / JPEG-LS 压缩序列报 `readPixels: all access methods failed`，根因是解压路径查标准 tag 的 VR 时字典为空。**

仓库里的 `dcmtk/src/main/cpp/dcmtk/config/osconfig.h` 可以直接印证这个状态：`#define DCM_DICT_DEFAULT 2`（启动时加载**外部文件字典**，而非内置字典）、`/* #undef ENABLE_PRIVATE_TAGS */`（私有字典未编入），且 `DCM_DICT_DEFAULT_PATH` 指向编译机上残留的 Windows 路径 `C:/Users/Fall/Desktop/dcmtk/install-android-arm64/share/dcmtk-3.6.9/dicom.dic`——在 Android 上必然不存在。所以字典必须由 App 自己给，本工程两处配合、缺一不可：

1.  把 `dicom.dic` 放进模块 assets，首次解析前释放到内部存储再注入 —— 见 `cbctdeal/src/main/java/com/wangyao/cbctdeal/engine/CbctParseEngine.kt` 的 `ensureDictionary()` 与 `cbctdeal/src/main/cpp/cbct-native-lib.cpp` 的 `initDictionary()`：

    ```kotlin
    CbctJni.initDictionary(File(context.filesDir, "dicom.dic").absolutePath)
    ```

2.  `CMakeLists.txt` 保留 `-ffunction-sections -fdata-sections`，但**不要开 `--gc-sections`** —— 字典是惰性初始化的静态数据表，链接期会被当作未引用段回收，注入了也白搭（注释见 `cbctdeal/src/main/cpp/CMakeLists.txt`）。

### 4. 编译安装 App

```bash
./gradlew :app:assembleDebug            # 依赖已缓存时可加 --offline
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.example.dcmtkdemo/.activity.MainActivity
```

三维可视化（VR / MPR）依赖 OpenGL ES 3.0+ 与 EGL，**必须真机验证**；模拟器通常拿不到可用的 GPU raycast 上下文。

### 5. 主机侧 PACS 与 Worklist 服务（出处：文章 2，联调网络功能时需要）

上传 / 查询 / 调阅 / Worklist 四类功能需要一个真实 SCP。在 Mac 上按文章 2 从零搭：

1.  源码编译 DCMTK 并安装：`dcmtk-3.6.8.tar.gz` → `mkdir build && cd build` → `cmake .. -DCMAKE_INSTALL_PREFIX=$HOME/dcmtk-3.6.8-install -DCMAKE_CXX_STANDARD=11 -DBUILD_SHARED_LIBS=ON -DDCMTK_WITH_WLSTORAGE=ON` → `make -j$(sysctl -n hw.ncpu) && make install` → 把 `bin` 追加进 `~/.zshrc` 的 `PATH`。
2.  建目录并写 `dcmqrscp.cfg`。**路径必须纯英文且提前全部建好**，目录不存在会导致服务静默失败（没有报错但收不到图）：

    ```
    DCMTK_PACS_TEST/
    ├─ SCP/  ├─ database/      # PACS 影像存储库（提前新建）
    │        └─ dcmqrscp.cfg
    └─ SCU/  ├─ test.dcm
             └─ recv_db/        # C-MOVE 落地目录（提前新建）
    ```

    关键三行：`NetworkTCPPort 11112`；`HostTable` 里把 **Android 真机的局域网 IP** 写进白名单（`android_client = (ANDROID_SCU, 192.168.1.11, 1234)`，1234 是 C-MOVE 的接收端口）；`AETable` 里 `ACME_STORE <database 绝对路径> RW (9, 1024mb) acmeCTcompany`。
3.  启动服务：在 `SCP/` 目录执行 `dcmqrscp -d --config dcmqrscp.cfg 11112`。
4.  先用命令行回环自测，再上 Android：`echoscu`（期望 `Received Echo Response (Success)`）→ `storescu ... +sd test.dcm`（期望 `DIMSE Status 0x0000: Success`）→ `findscu -k QueryRetrieveLevel=STUDY -k StudyInstanceUID=` → `movescu --port 1234 -od .../recv_db`，成功后 `recv_db/` 里应出现拉取到的 `.dcm`。
5.  App 侧参数与服务端一一对应，默认值在 `dcmtk/src/main/java/com/example/dcmtk/utils/PacsPrefs.kt`：Remote AE = `ACME_STORE`、Local AE = `ANDROID_SCU`、Port = `11112`；Worklist 的 Remote AE 默认 `OFFIS`。改 IP 后无需重编，界面上直接填。手机与服务端须在**同一局域网**。
6.  Worklist 数据源：编辑待检查单模板 → 转成二进制 `.wl` → `findscu` 查询验证，仓库自带样例 `dcmtk/src/main/assets/wlistqry/wlistqry1.wl`。App 查询结果会走 MWL 同步落库（`patient` / `study` 两张表，单事务 upsert），原理见文章 1 第十章。

### 6. 测试数据与一个易踩的坑

`app/src/main/assets/` 下已内置全部演示数据：CT Preprocess 用的 `.raw` / `.bin` 裸数据、若干 `.dcm`，以及 `neck_ct/`（265 层 CBCT 序列，供 `:cbctdeal` 的「加载 ASSETS/NECK_CT」使用）。

> ⚠️ **CT Preprocess 页选对字节序**：`.raw` 文件要用 **Little**，`.bin` 文件要用 **Big**。选错时像素值会散到 0~65535，直方图看起来近似均匀、自动调窗退化成全跨度——这是**读错字节序的症状，不是算法缺陷**。另外 W/H 输入框与所选文件不联动，例如 1112x1740 对 1112x1700 的 `CR*.raw` 会多要 40 行，Native 侧会按行截断并打一条 `RawPixelDealJni: ... truncate to 1700 rows` 的 WARN，属正常提示。

---

## 🏗️ 项目架构 (Architecture)

*   **`:app`**: UI 表现层，基于协程编排异步业务。
*   **`:cbctdeal`**: **三维核心模块**。包含 VTK 9.1.0 静态库与 C++ 渲染引擎。
*   **`:dcmtk`**: 通信模块。封装 DCMTK 静态库，处理网络与文件 IO。
*   **`:rawpixeldeal`**: 算法模块。基于 OpenCV 处理底层像素变换。

三层解耦是贯穿全项目的核心设计：**JNI 桥接层是唯一接触 `JNIEnv` 的地方，只做类型转换；业务逻辑层（`PacsClient` / `DicomFileIO` / `CtSeriesProcessor`）使用纯 C++ 类型，不依赖 JNI，可独立阅读和测试。** 原理见文章 1 第二、五章。

### 功能入口速查（左侧抽屉 8 项，定义在 `app/src/main/res/menu/bottom_nav_menu.xml`）

| 入口 | 对应 Fragment | 依赖模块 | 上手要点 | 原理出处 |
| :--- | :--- | :--- | :--- | :--- |
| Upload | `UploadFragment` | `:dcmtk` | 需先按第 5 步起好 `dcmqrscp`，C-STORE 带实时进度回调 | 文章 1 第三章；文章 2 第二节 3 |
| Query | `QueryFragment` | `:dcmtk` | 按姓名 / 接入号 / StudyUID 查询 | 文章 1 第三章；文章 2 第二节 4 |
| Worklist | `WorklistQueryFragment` | `:dcmtk` | Remote AE 默认 `OFFIS`，结果自动同步落库 | 文章 1 第十章；文章 2 第三节 |
| Retrieve | `RetrieveFragment` | `:dcmtk` | C-MOVE 需要 `HostTable` 里的接收端口 `1234` 通畅 | 文章 1 第三章；文章 2 第二节 5 |
| Show | `DcmShowFragment` | `:dcmtk` | 本地 `.dcm` 浏览，支持手势缩放 | 文章 1 第四章 |
| Compare | `FileCompareFragment` | `:dcmtk` + `:rawpixeldeal` | DICOM 文件与调窗结果对比 | 文章 1 第四、七章 |
| CT Preprocess | `CTPreprocessFragment` | `:rawpixeldeal` | 选资产 → 选字节序 → 执行最优调节 / 自定义流水线 / 写入 DICOM | 文章 1 第六、七、九章；`rawpixeldeal/README.md` |
| CBCT Parse | `CbctParseFragment` | `:cbctdeal` | 「加载 ASSETS/NECK_CT」一键体验解析 + Bitmap 2D + VTK VR / MPR | 文章 3 第七、八、九章；`cbctdeal/README.md` |

---

## 🤝 总结
本项目通过将重度图像算法与解析逻辑下沉至 Native 层，在 Android 移动端实现了接近桌面工作站级的医学影像处理能力，是开发**移动医生站、口腔影像阅片、远程会诊 App** 的理想参考方案。
