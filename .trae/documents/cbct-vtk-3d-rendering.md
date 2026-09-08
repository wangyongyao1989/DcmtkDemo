# cbctdeal 模块 VTK 三维可视化实施计划

## Context

DcmtkDemo 项目已完成 CBCT DICOM 序列解析（dcmtk + cbctdeal 模块，Native 多线程双 Pass，产出 Native 堆上的 `CbctVolume` 连续 16bit 体数据）。现需按《Android平台VTK完整集成流程（适配CBCT三维可视化）》PDF 的「四、JNI层VTK核心适配逻辑」与「五、Android端渲染交互适配」，在 cbctdeal 模块内实现基于 VTK 9.1.0 静态库的三维体渲染（VR）、VTK-MPR 切面渲染与手势交互，保持模块化内聚、沿袭现有 UI 风格，最后编写模块 README。

**已确认的技术事实**（探索结论）：
- VTK 9.1.0 静态库（arm64-v8a，NDK 25 编译）位于 `cbctdeal/src/main/cpp/lib/`，头文件在 `include/vtk-9.1/`
- `libvtkRenderingOpenGL2-9.1.a` 含 `ANativeWindow_setBuffersGeometry`、`eglCreateWindowSurface` 未定义符号 → **vtkEGLRenderWindow 已带 Android Surface 支持**（VTK_USE_ANDROID_CONTEXT 已编译）
- VTK 9.1 无 `vtkVolumeRayCastMapper`（PDF 伪代码为 VTK 8.x 类）→ 用 `vtkSmartVolumeMapper`（GPU RayCast + CPU FixedPoint 自动回退）；`vtkVolumeProperty` 无 SetColorWindow/SetColorLevel → 用 Color/Opacity TransferFunction 实现 WW/WC
- vtkEGLRenderWindow/vtkOpenGLImageSliceMapper ∈ RenderingOpenGL2；vtkSmartVolumeMapper ∈ RenderingVolumeOpenGL2；vtkImageReslice/vtkImageMapToColors ∈ ImagingCore；vtkImageActor ∈ RenderingCore（头文件均存在）
- 项目 NDK 28.2 编译（VTK 为 NDK 25 编译，静态库混链可行）；现有 JNI 动态注册风格、SurfaceView 交互、模块 Kotlin 层零依赖原则

## 实现方案

### 1. Native 渲染核心（新增 `cpp/include/CbctVtkRenderer.h` + `cpp/CbctVtkRenderer.cpp`）

纯 C++ 类（无 JNI 依赖，可独立测试），核心设计：

**零拷贝体数据接入**：`vtkImageData::SetDimensions/SetSpacing`（origin 固定 0,0,0）+ `vtkUnsignedShortArray::SetArray(vol->data, n, 1)`（save=1，VTK 不释放）或 `vtkSignedShortArray`（pixelRepresentation=1 时）。TF/LUT 的 x 域用原始值，HU↔raw 换算 `(hu - intercept)/slope`。

**专用渲染线程**：`std::thread` + `std::mutex` + `std::condition_variable` 任务队列。所有 EGL/GL 操作只在渲染线程执行（EGL 上下文线程绑定）。`post(task, sync)` 支持同步等待（surface 销毁/destroy 时必须同步）。任务队列排空后才 Render（手势高频事件自动合并，符合 PDF 6.2 闲置暂停渲染）。手势任务超压时丢弃旧任务。

**VR 管线**（PDF 4.2 适配）：
- `vtkSmartVolumeMapper`（SetInputData、Composite 混合）+ `vtkVolume` + `vtkVolumeProperty`（ShadeOn、线性插值）
- `vtkColorTransferFunction`：色带横跨 [wc-ww/2, wc+ww/2]，骨窗暖灰阶
- `vtkPiecewiseFunction` 不透明度：HU<200 全透明（PDF 骨骼阈值过滤软组织），200→1300 HU 渐升到 0.85，贴合 CBCT 骨骼渲染权重调优
- 相机：透视投影，初始视角 (1, 0.6, 1)，ResetCamera

**MPR 管线**（PDF 5.3，vtkImageReslice）：
- `vtkImageReslice` + SetOutputDimensionality(2)：按平面设置 ResliceAxesDirectionCosines（axial=单位阵 / coronal=x,z→x,y / sagittal=y,z→x,y）+ ResliceAxesOrigin（切片穿过点）+ OutputSpacing/Extent；线性插值
- `vtkImageMapToColors` + `vtkLookupTable`（灰阶，Range=[raw(wc-ww/2), raw(wc+ww/2)]，WW/WC 实时同步）
- `vtkImageActor` 显示；相机平行投影沿 -Z，ResetCamera 适配切面

**相机交互**（PDF 5.2，Native 方法供 JNI 调用）：
- rotate(dx,dy)：Azimuth/Elevation（trackball 语义）
- zoom(factor)：VR Dolly（距离钳制）/ MPR Zoom（平行缩放）
- pan(dx,dy)：位置+焦点沿相机 right/up 平移
- resetCamera()

**模式切换**：MODE_VR ↔ MODE_MPR，RemoveVolume/AddVolume 与 RemoveViewProp/AddViewProp 互切 + 相机重置。

**Surface 生命周期**：onSurfaceCreated(ANativeWindow)（存窗、SetWindowId、应用当前状态）、onSurfaceChanged（SetSize+Render）、onSurfaceDestroyed（同步 Finalize）。渲染器不拥有 CbctVolume，destroy 前必须保证 Volume 存活（README 注明释放顺序）。

### 2. JNI 桥（编辑 `cpp/cbct-native-lib.cpp` + CMake）

- 新增 `CbctVtkJni` 类的 JNI 方法：createRenderer / onSurfaceCreated(Surface) / onSurfaceChanged / onSurfaceDestroyed / setRenderMode / setPlane / setWindowLevel / rotate / zoom / pan / resetCamera / destroyRenderer，沿用现有动态注册表风格（kMethods 追加）
- Surface → `ANativeWindow_fromSurface`，销毁时 Finalize 后 `ANativeWindow_release`
- `CMakeLists.txt`：新增 VTK 头文件路径、`file(GLOB libvtk*.a)` 全量 `--start-group` 链接（静态库循环依赖自动解）、链接 `EGL GLESv2 GLESv3`、加 `-ffunction-sections -fdata-sections` + `-Wl,--gc-sections`（PDF 6.1 裁包体积）、新增 CbctVtkRenderer.cpp 源文件

### 3. Kotlin 层（cbctdeal 模块）

- **新增 `jni/CbctVtkJni.kt`**：external 声明 + 常量（MODE_VR=0/MODE_MPR=1；PLANE_AXIAL=0/CORONAL=1/SAGITTAL=2）
- **新增 `render/CbctVtkView.kt`**：SurfaceView + SurfaceHolder.Callback + 手势（GestureDetector onScroll → VR 旋转/MPR 平移；ScaleGestureDetector → 缩放；双指焦点位移 → 平移）。对外 API：setVolume(handle) / setRenderMode / setPlane / setWindowLevel / resetCamera / release()。渲染能力全部内聚模块内，UI 只调接口

### 4. app 模块集成（沿袭现有 UI 风格）

- **编辑 `fragment_cbct_parse.xml`**：在 MPR Viewer 与摘要之间新增「4) VTK 3D Visualization」段——渲染载体 RadioGroup（Bitmap 2D / VTK 3D）、VTK 模式 RadioGroup（VR 体绘制 / MPR 切面，仅 VTK 模式可见）、复位相机按钮、`CbctVtkView`（420dp，沿用 ZoomImageView 的排布样式）、手势提示文字。现有 rg_plane / sb_position / sb_ww / sb_wc 控件复用，按当前激活视图路由
- **编辑 `CbctParseFragment.kt`**：解析成功后 setVolume；视图模式切换控制互斥显示与状态路由（activeViewer 分发 plane/position/ww/wc）；VR 模式隐藏位置行；onDestroyView 先 vtkView.release() 再 volumeHandle.release()（保证释放顺序）

### 5. README（新增 `cbctdeal/README.md`）

模块概述、分层架构（transfer/engine/jni/render/native 各层职责与数据流）、Native 双 Pass 解析原理、VTK 零拷贝接入、渲染线程模型、VR 传递函数与 WW/WC 适配原理、MPR Reslice 坐标矩阵推导、手势映射、Surface 生命周期与释放顺序、构建配置说明（VTK 静态库/CMake 链接）、操作使用流程。

## 关键文件清单

| 操作 | 文件 |
|---|---|
| 新增 | `cbctdeal/src/main/cpp/include/CbctVtkRenderer.h` |
| 新增 | `cbctdeal/src/main/cpp/CbctVtkRenderer.cpp` |
| 编辑 | `cbctdeal/src/main/cpp/cbct-native-lib.cpp`（追加 JNI 方法） |
| 编辑 | `cbctdeal/src/main/cpp/CMakeLists.txt`（VTK 链接） |
| 新增 | `cbctdeal/src/main/java/com/wangyao/cbctdeal/jni/CbctVtkJni.kt` |
| 新增 | `cbctdeal/src/main/java/com/wangyao/cbctdeal/render/CbctVtkView.kt` |
| 编辑 | `app/src/main/res/layout/fragment_cbct_parse.xml` |
| 编辑 | `app/src/main/java/com/example/dcmtkdemo/fragment/CbctParseFragment.kt` |
| 新增 | `cbctdeal/README.md` |

## 验证

1. `./gradlew :cbctdeal:externalNativeBuildDebug` 先单独验证 Native 编译链接（VTK 静态库全量链接 + gc-sections）
2. `./gradlew :app:assembleDebug` 全量编译，确认 libcbct_native.so 生成并打包（检查 arm64-v8a so 体积与符号）
3. 代码走查：释放顺序（renderer → volume）、surface 销毁同步、手势任务合并逻辑、WW/WC 与传递函数/LUT 的 HU↔raw 换算正确性
4. 无法真机验证渲染效果，以编译通过 + 逻辑走查为验收标准（与用户约定一致），README 中说明真机操作流程
