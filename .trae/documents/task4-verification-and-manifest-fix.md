# Task 4: AndroidManifest 修复 + 编译验证

## Summary

前置的 Java 包细粒度划分（14 个 .java 文件迁移到 8 个子包）与 native-lib.cpp 业务抽取（拆分为 PacsClient / DicomFileIO / ProgressScu / JniString + 瘦 JNI 桥）均已落地完成。本次探索性核查发现 **唯一遗留问题**：`AndroidManifest.xml` 中 `DetailActivity` 声明的 `android:name` 与 `android:parentActivityName` 仍指向旧包路径，会导致运行期 `ClassNotFoundException`。本计划修复该问题并执行 gradle 编译验证，闭合整个重构任务。

## Current State Analysis

经逐项核查，当前代码状态如下：

### ✅ 已正确完成的部分
- **Java 包结构**：`app/src/main/java/com/example/dcmtkdemo/` 根目录下 0 个 .java 文件；8 个子包（activity/adapter/callback/fragment/jni/model/utils/viewmodel）共 14 个 .java 文件，每个 `package` 声明与其目录位置一致。
- **跨包 import**：所有 `com.example.dcmtkdemo.*` 引用均已更新到新子包路径（如 `MainActivity` 引用 `fragment.*`、`jni.DcmtkJni`、`utils.FileUtil`、`viewmodel.PacsViewModel`；`DcmShowFragment` 引用 `activity.DetailActivity`、`adapter.DcmImageAdapter` 等）。
- **C++ 文件**：9 个文件齐全且非空 — `native-lib.cpp`、`ProgressScu.h/.cpp`、`PacsClient.h/.cpp`、`DicomFileIO.h/.cpp`、`JniString.h`、`CMakeLists.txt`。
- **`kClassName`**：`native-lib.cpp:225` = `"com/example/dcmtkdemo/jni/DcmtkJni"`，与 Java 实际位置匹配。
- **`RegisterNatives` 签名表**（11 个方法）：所有签名字符串与 `DcmtkJni.java` 的 native 方法声明一一对应；特别是 `cStore` / `cGet` 中的 `Lcom/example/dcmtkdemo/callback/ProgressCallback;` 已正确指向新子包。
- **`CMakeLists.txt`**：`add_library` 已列出全部 4 个 .cpp 源文件（native-lib.cpp / ProgressScu.cpp / PacsClient.cpp / DicomFileIO.cpp）。
- **`activity_main.xml`**：`tools:context=".activity.MainActivity"` 已更新。

### ❌ 发现的遗留问题（仅此 2 处）

文件：`app/src/main/AndroidManifest.xml`

| 行号 | 当前值（错误） | 期望值（修复后） | 影响 |
|------|----------------|------------------|------|
| 28 | `android:name=".DetailActivity"` | `android:name=".activity.DetailActivity"` | 运行期 `ClassNotFoundException`：当 `DcmShowFragment` 调用 `startActivity(new Intent(getContext(), DetailActivity.class))` 时崩溃 |
| 30 | `android:parentActivityName=".MainActivity"` | `android:parentActivityName=".activity.MainActivity"` | Up/Back 导航层级失效 |

> 注：`MainActivity` 声明（line 19 `android:name=".activity.MainActivity"`）已正确，无需改动。这两个 bug 是上一轮迁移时漏改 `DetailActivity` 节点所致。

## Proposed Changes

### Change 1: 修复 AndroidManifest.xml 的 DetailActivity 引用
- **文件**：`app/src/main/AndroidManifest.xml`
- **改什么**：将 `<activity android:name=".DetailActivity" ...>` 节点的 `android:name` 改为 `.activity.DetailActivity`；将其 `android:parentActivityName` 改为 `.activity.MainActivity`。
- **为什么**：`DetailActivity` 已迁移到 `com.example.dcmtkdemo.activity` 子包，manifest 必须使用相对 namespace 的全路径 `.activity.DetailActivity`，否则 APK 虽能编译通过但运行时无法定位 Activity 类。
- **怎么做**：使用 Edit 工具针对 line 28 和 line 30 做两处精确替换，保留其它属性（`android:exported="false"` 等）不变。

修改后该 `<activity>` 节点应为：
```xml
<activity
    android:name=".activity.DetailActivity"
    android:exported="false"
    android:parentActivityName=".activity.MainActivity" />
```

### Change 2: 执行 gradle 编译验证
- **命令**：`gradlew.bat :app:assembleDebug`（Windows 环境，使用项目根目录下的 wrapper）
- **工作目录**：`D:\TestDemo\DcmtkDemo`
- **为什么**：验证 Java 包迁移、JNI `RegisterNatives` 类名/签名匹配、NDK C++ 拆分编译、CMakeLists 配置整体无误。
- **重点关注**：
  1. Java 编译无 `cannot find symbol` 错误（验证 import 正确）
  2. NDK 编译无 `undefined reference` / `fatal error` 错误（验证 C++ 拆分与头文件包含）
  3. `RegisterNatives` 返回 11（方法数），不会因类名/签名不匹配而失败 — 这部分需在 build 成功后通过日志或运行期日志确认（如 build 本身失败则先解决 build 问题）
- **超时设置**：NDK 首次编译可能较慢，设 600000ms（10 分钟）超时；若超时则改为后台运行。

## Assumptions & Decisions

1. **假设**：Android SDK 已正确安装于 `D:\AndroidDev\Sdk`（`local.properties` 已确认），且 `gradlew.bat` wrapper 可用。
2. **假设**：DCMTK 静态库（`app/src/main/cpp/lib/arm64-v8/*.a`）与头文件（`app/src/main/cpp/include/`）未受本次重构影响，仍处于可用状态。
3. **决策**：只修复明确漏改的 2 处 manifest 引用，不改动任何其它文件 — 严格遵循用户原始要求 "Follow the plan strictly. Do not deviate, re-plan, or add unrequested features."
4. **决策**：不运行应用/emulator 做运行期验证（超出当前工具能力范围），仅以 `assembleDebug` 编译通过作为验证标准；如用户需要运行期验证可单独提出。
5. **决策**：如编译失败，逐项排查并修复（按错误信息定位），但仅修复本次重构引入的问题，不修复重构前已存在的 bug。

## Verification Steps

1. **Manifest 修复后自检**：Read `AndroidManifest.xml` 全文，确认 line 28/30 已更新且 XML 仍为合法格式。
2. **Gradle 编译**：执行 `gradlew.bat :app:assembleDebug`，期望输出 `BUILD SUCCESSFUL`。
3. **失败处理**：若失败，按错误类型分别处理：
   - Java 编译错误 → 检查对应 .java 文件的 import / 包声明
   - NDK 编译错误 → 检查对应 .cpp/.h 文件的 #include 与符号定义
   - 资源/manifest 错误 → 检查 XML 引用
4. **成功后**：向用户汇报最终结果，包含：编译耗时、APK 产物路径（`app/build/outputs/apk/debug/app-debug.apk`）、本次修复的 manifest 问题说明。
