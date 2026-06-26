# 项目架构优化方案：Java 包细粒度划分 + native-lib.cpp 业务抽取

## Context（背景）

当前项目 `com.example.dcmtkdemo` 根包下平铺了 14 个 Java 类（Activity、Fragment、Adapter、ViewModel、工具类、JNI 桥接、数据模型、回调接口混在一起），缺乏分层，不利于阅读与维护。同时 `app/src/main/cpp/native-lib.cpp` 单文件达 870 行，集中了：JNI 字符串辅助类、带进度的 SCU 子类、PACS 网络操作（connect/cEcho/cStore/cFind/cMove/cGet 共 6 个）、DICOM 文件操作（loadDicomFileInfo/writeDicomFile/dcmToJpg 共 3 个）、NDK 符号 stub、JNI 注册表等，业务高度耦合。

本次优化的目标：
1. 按 Android 惯例将 Java 类拆分到细粒度子包，根包保持干净；
2. 将 `native-lib.cpp` 按业务域拆分为多个解耦的 C++ 类，使业务逻辑脱离 JNI 依赖、可独立阅读与测试，`native-lib.cpp` 仅保留薄 JNI 桥接层。

执行约定：
- 适配器包使用标准拼写 `adapter`（用户原文 "adpter" 视为笔误）。
- 在用户列出的 6 个子包之外，新增 `jni` 与 `model` 两个包，分别承载 `DcmtkJni` 与数据模型类，使根包完全清空。

---

## Part 1：Java 包细粒度划分

### 目标包结构（`app/src/main/java/com/example/dcmtkdemo/`）

| 子包 | 迁入的类 |
|------|----------|
| `activity` | `MainActivity`、`DetailActivity` |
| `adapter` | `DcmImageAdapter`、`PatientAdapter` |
| `fragment` | `UploadFragment`、`QueryFragment`、`RetrieveFragment`、`DcmShowFragment` |
| `callback` | `ProgressCallback` |
| `utils` | `FileUtil` |
| `viewmodel` | `PacsViewModel` |
| `jni`（新增） | `DcmtkJni` |
| `model`（新增） | `DicomImageRecord`、`PatientRecord` |

迁移后根包 `com.example.dcmtkdemo` 不再保留任何类。

### 对每个迁移文件的改动模式
1. 修改 `package` 声明为新子包；
2. 补充跨包 `import`（原先同包无需 import，迁移后需互相 import）。

跨包依赖（需补 import）的关键引用关系：
- 几乎所有 Fragment / Activity 引用 `DcmtkJni` → `import com.example.dcmtkdemo.jni.DcmtkJni;`
- `RetrieveFragment`、`UploadFragment` 引用 `ProgressCallback` → `import com.example.dcmtkdemo.callback.ProgressCallback;`
- `MainActivity`、各 Fragment 引用 `PacsViewModel` → `import com.example.dcmtkdemo.viewmodel.PacsViewModel;`
- `MainActivity` 引用 `FileUtil` → `import com.example.dcmtkdemo.utils.FileUtil;`
- `MainActivity` 引用 4 个 Fragment → `import com.example.dcmtkdemo.fragment.*;`
- `MainActivity`、`DcmShowFragment` 引用 `DetailActivity` → `import com.example.dcmtkdemo.activity.DetailActivity;`
- `QueryFragment`、`RetrieveFragment` 引用 `PatientAdapter` / `PatientRecord` → `import com.example.dcmtkdemo.adapter.PatientAdapter;` / `import com.example.dcmtkdemo.model.PatientRecord;`
- `DcmShowFragment`、`DetailActivity` 引用 `DcmImageAdapter` / `DicomImageRecord` → 对应 `adapter` / `model` import
- view binding 类（如 `ActivityMainBinding`）由 `com.example.dcmtkdemo.databinding` 自动生成，与类所在包无关，无需调整。

### 非 Java 文件的连带改动
- **`app/src/main/AndroidManifest.xml`**：`android:name=".MainActivity"` → `.activity.MainActivity`；`android:name=".DetailActivity"` → `.activity.DetailActivity`；`parentActivityName=".MainActivity"` → `.activity.MainActivity`。
- **`app/src/main/res/layout/activity_main.xml`**：`tools:context=".MainActivity"` → `.activity.MainActivity`（仅 IDE 预览用，但保持一致）。
- **`app/src/main/cpp/native-lib.cpp`**（Part 2 一并处理）：
  - JNI 注册类名 `kClassName = "com/example/dcmtkdemo/DcmtkJni"` → `"com/example/dcmtkdemo/jni/DcmtkJni"`；
  - `kMethods` 中 `cStore`、`cGet` 签名里的 `Lcom/example/dcmtkdemo/ProgressCallback;` → `Lcom/example/dcmtkdemo/callback/ProgressCallback;`。

> 说明：Java 侧 `DcmtkJni` 的 `native` 方法签名不变（`ProgressCallback` 参数类型仍由 import 决定），C++ 侧只需更新上述两处字符串字面量。

---

## Part 2：native-lib.cpp 业务抽取

### 设计原则
- 业务类（`PacsClient`、`DicomFileIO`、`ProgressScu`）只使用 C++ 标准类型（`std::string`、`std::vector`、`std::map`、`std::function`），**不依赖 `JNIEnv*`**，可独立阅读与单元测试。
- 所有 `JNIEnv*` 的使用（jstring↔char*、HashMap↔map、jobjectArray↔vector、Java 回调调用）集中在 `native-lib.cpp` 薄桥接层。
- 进度回调通过 `std::function<void(unsigned long sent, unsigned long total)>` 解耦：`ProgressScu` 持有该 function，`native-lib.cpp` 提供一个捕获 `env`+`callback` 的 lambda 注入。

### 新增/改动文件（均在 `app/src/main/cpp/`）

| 文件 | 职责 | 大致行数 |
|------|------|----------|
| `JniString.h`（新增，header-only） | RAII JNI 字符串辅助类（原 `class JniString`），供 `native-lib.cpp` 使用 | ~30 |
| `ProgressScu.h` / `ProgressScu.cpp`（新增） | 继承 `DcmSCU`，重写 `notifySENDProgress`/`notifyRECEIVEProgress`，通过 `std::function` 回调上报进度；不依赖 JNI | ~80 |
| `PacsClient.h` / `PacsClient.cpp`（新增） | PACS 网络业务：`connectPACS`、`cEcho`、`cStore`、`cFind`、`cMove`、`cGet`；内部封装 `addCommonTransferSyntaxes` 辅助函数与 `ProgressScu` 使用；入参/返回全为 C++ 类型 | ~360 |
| `DicomFileIO.h` / `DicomFileIO.cpp`（新增） | DICOM 文件业务：`loadFileInfo`（返回 `std::map<std::string,std::string>`）、`writeDicomFile`、`dcmToJpg`；含 JPEG 解码器一次性注册逻辑 | ~260 |
| `native-lib.cpp`（重写为薄桥接） | 保留 `getlogin`/`getlogin_r` NDK stub；保留 `JNI_OnLoad` 与 `kMethods` 注册表（更新类名/签名）；每个 `native_*` 函数仅做 JNI↔C++ 类型转换并委派给 `PacsClient`/`DicomFileIO` | ~250 |

### 关键委派映射
| JNI 函数（native-lib.cpp） | 委派目标 | 类型转换 |
|------|------|------|
| `native_initDcmtk` | 直连 DCMTK 字典加载（保留原逻辑） | jstring→path |
| `native_loadDicomFileInfo` | `DicomFileIO::loadFileInfo` | jstring→string；`std::map`→Java `HashMap` |
| `native_writeDicomFile` | `DicomFileIO::writeDicomFile` | jstring×2→string；jint→int；bool→jboolean |
| `native_connectPACS` | `PacsClient::connectPACS` | jstring×3→string；jint→int；bool→jboolean |
| `native_cEcho` | `PacsClient::cEcho` | 同上 |
| `native_cStore` | `PacsClient::cStore`（传入进度 lambda） | lambda 捕获 env+callback 调 Java `onProgress` |
| `native_cFind` | `PacsClient::cFind` | jstring×N→string；`std::vector<std::string>`→`jobjectArray` |
| `native_cMove` | `PacsClient::cMove` | jstring×N→string；bool→jboolean |
| `native_cGet` | `PacsClient::cGet`（传入进度 lambda） | 同 cStore |
| `native_dcmToJpg` | `DicomFileIO::dcmToJpg` | jstring→string；int→jint |
| `native_stringFromJNI` | 保留原样 | — |

### CMakeLists.txt 改动
在 `add_library(${CMAKE_PROJECT_NAME} SHARED ...)` 中追加三个新源文件：
```
native-lib.cpp
ProgressScu.cpp
PacsClient.cpp
DicomFileIO.cpp
```
`include_directories` 与链接库不变（业务类仍用同一批 DCMTK 头/静态库）。

### 行为保持不变
- 所有 native 方法的 Java 签名、返回值、进度回调语义、文件输出路径（如 `dir/jpg/<name>.jpg`）、C-GET 存储目录、字典加载方式均与原实现一致——本次仅为物理拆分与解耦，不改变运行时行为。

---

## 涉及修改的文件清单

**Java（迁移 + 改 package/import）**：14 个文件
`MainActivity.java`、`DetailActivity.java`、`DcmImageAdapter.java`、`PatientAdapter.java`、`DcmShowFragment.java`、`QueryFragment.java`、`RetrieveFragment.java`、`UploadFragment.java`、`ProgressCallback.java`、`FileUtil.java`、`PacsViewModel.java`、`DcmtkJni.java`、`DicomImageRecord.java`、`PatientRecord.java`

**C++（新增 + 重写）**：`JniString.h`、`ProgressScu.h/.cpp`、`PacsClient.h/.cpp`、`DicomFileIO.h/.cpp`、`native-lib.cpp`（重写）

**配置/资源**：`CMakeLists.txt`、`AndroidManifest.xml`、`res/layout/activity_main.xml`

---

## 验证方式（Verification）

1. **编译验证（首要）**：
   - Java 包迁移后，确认所有 `import` 解析正确、view binding 仍生成；
   - NDK 编译：`./gradlew :app:assembleDebug`（或 `externNativeBuild` 触发的 CMake 构建）应通过，重点观察 `RegisterNatives` 不会因类名/签名不匹配而失败；
   - 特别核对：`kClassName` 与 `ProgressCallback` 签名字符串已改为新包路径。

2. **运行时验证**（在设备/模拟器上）：
   - 启动 App，确认字典初始化、assets 拷贝正常（`MainActivity` → `FileUtil` → `DcmtkJni.initDcmtk`）；
   - 切换底部导航 4 个 Fragment（Upload/Query/Retrieve/Show）均能加载；
   - Upload：选 `.dcm` → C-STORE，进度回调与结果 Toast 正常；
   - Query：C-FIND 返回列表，`PatientAdapter` 渲染正常；
   - Retrieve：C-GET 下载，进度回调更新，下载后 `temp/` 文件信息展示正常；
   - Show：`dcmToJpg` 转换、`DcmImageAdapter` 缩略图、点击进入 `DetailActivity` 大图均正常。

3. **回归点**：进度回调（C-STORE 上传、C-GET 下载）是最易因 lambda/env 生命周期出错之处，需重点实测；本次实现中 `env`/`callback` 仅在同步 native 调用期间使用（与原实现一致，未引入全局引用），语义不变。
