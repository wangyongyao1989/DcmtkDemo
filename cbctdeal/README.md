# CBCTDeal Module - CBCT DICOM 序列解析与 VTK 三维可视化

本模块是 CBCT（锥形束 CT）数据的处理核心，基于 **DCMTK**（Native 解析）与 **VTK 9.1.0**（Android arm64 交叉编译静态库）实现两大能力：

1. **序列解析**：读取 CBCT DICOM 序列目录，在 Native 堆组装连续 3D 体数据（`CbctVolume`），支持 JPEG / JPEG-LS / RLE 压缩解压、断层间隙补全、CPU 切面提取（Bitmap 2D）；
2. **三维可视化**：基于 VTK 实现 **VR 体绘制**（骨骼三维渲染）与 **MPR 多平面重建**（横断/冠状/矢状切面），OpenGL ES 渲染 + 触摸手势交互，对应 `doc/Android平台VTK完整集成流程（适配CBCT三维可视化）.pdf` 的「四、JNI 层 VTK 核心适配逻辑」与「五、Android 端渲染交互适配」两章。

模块设计原则：**高内聚低耦合**——解析与渲染全部内聚在 `cbctdeal` 模块内（单一 `cbct_native.so`），对外仅暴露 Kotlin API；与 `dcmtk`、`rawpixeldeal` 模块在 Kotlin 层零依赖，仅在构建层共享 DCMTK 静态库。

---

## 0. 演示视频 (Video Demos)

真机（arm64-v8a）运行效果，演示 **VR 体绘制**（骨骼三维渲染、手势旋转/缩放/平移）与 **MPR 多平面重建**（三平面切换、层位置浏览、窗宽窗位调节）：

| 视频 | 内容说明 |
|---|---|
| [vtk3D.mp4](doc/vtk3D.mp4) | CBCT DICOM 序列解析 + VTK 三维体渲染（VR）完整流程：加载序列 → 骨骼三维重建 → 手势交互 |
| [vtk3D-1.mp4](doc/vtk3D-1.mp4) | VTK 三维可视化交互细节：VR/MPR 模式切换、切面浏览、窗宽窗位实时调节 |

> 视频无法在文档内直接预览时，请 clone 仓库后从 `cbctdeal/doc/` 目录获取原文件播放。

**相关技术文档**：
- 技术文章（CSDN 版）：[CSDN-Android平台CBCT三维可视化实战.md](CSDN-Android平台CBCT三维可视化实战.md) —— 覆盖 VTK/DCMTK 交叉编译、Android 集成、序列解析与体渲染实现全流程
- VTK 交叉编译脚本：[doc/android_vtk.sh](doc/android_vtk.sh)（NDK r25 / android-24 / OpenGL ES / 模块裁剪版）
- 参考研究报告：`doc/` 目录下三份 PDF（CBCT 原理、DICOM 序列解析、VTK 集成流程）

---

## 1. 模块结构

```
cbctdeal/
├── src/main/cpp/                          # Native 层（C++）
│   ├── CMakeLists.txt                     # 构建配置（DCMTK + VTK 静态库链接）
│   ├── cbct-native-lib.cpp                # JNI 桥：解析接口 + VTK 渲染接口
│   ├── CbctSeriesParser.cpp/.h            # 序列解析引擎（DCMTK）
│   ├── CbctVtkRenderer.cpp/.h             # VTK 渲染核心（VR/MPR + 渲染线程）
│   ├── include/CbctVolume.h               # 体数据结构（Native 堆连续内存）
│   ├── include/vtk-9.1/                   # VTK 9.1.0 头文件（交叉编译配套）
│   └── lib/libvtk*.a                      # VTK arm64-v8a 静态库（44 个，裁剪版）
├── src/main/java/com/wangyao/cbctdeal/
│   ├── jni/CbctJni.kt                     # 解析 JNI 接口（loadSeries/extract*/release）
│   ├── jni/CbctVtkJni.kt                  # VTK 渲染 JNI 接口（createRenderer/onSurface*/手势）
│   ├── model/CbctSeriesMeta.kt            # 元数据 + Volume 句柄（CbctVolumeHandle）
│   ├── engine/CbctParseEngine.kt          # 解析流程编排（IO 线程 + 进度回调）
│   ├── render/CbctVtkView.kt              # SurfaceView 渲染载体（生命周期 + 手势捕获）
│   ├── callback/CbctProgressCallback.kt   # 解析进度回调
│   └── transfer/CbctFileTransfer.kt       # 序列目录导入（SAF/本地路径）
└── doc/                                   # 参考文档（PDF 研究资料 + VTK 交叉编译脚本）
```

---

## 2. 实现原理

### 2.1 数据流水线总览

```
DICOM 序列目录
     │ CbctParseEngine（IO 线程）
     ▼
CbctSeriesParser（Native, DCMTK）
     │  双 Pass：Pass A 并行读元数据排序 → Pass B 逐片解码填充
     ▼
CbctVolume（Native 堆连续 Uint16 [z][y][x]，典型 500×500×500 ≈ 250MB）
     │
     ├──────────────────────────────┐
     ▼                              ▼
Bitmap 2D（CPU 切面提取）      CbctVtkRenderer（零拷贝导入 vtkImageData）
  extractAxial/extractMpr         │
  窗宽窗位映射 → Bitmap           ├─ VR：vtkSmartVolumeMapper 体绘制
                                  │    （GPU RayCast，CPU 回退）
                                  └─ MPR：vtkImageReslice 三轴切面
                                       + vtkImageMapToColors 灰阶映射
```

### 2.2 序列解析（CbctSeriesParser）

- **双 Pass 设计**：Pass A 多线程读取各切片元数据（按 `ImagePositionPatient[2]` Z 坐标排序，文件名仅作进度展示），Pass B 按序逐片解码像素填充体数据，避免整序持有全部 `DcmFileFormat` 导致的内存峰值；
- **断层补全**：Z 间距大于层厚处自动补空白切片，保证体数据几何连续（MPR/VR 正确成像的前提）；
- **压缩兼容**：注册 JPEG / JPEG-LS / RLE 解码器，覆盖常见压缩导出格式；
- **窗宽窗位回退**：文件缺失 WW/WC 时回退 CBCT 骨骼窗（WW4000 / WC600）；
- **16bit 全程保留**：体素以 Uint16 存储，HU 换算（`HU = pixel × slope + intercept`）仅在映射阶段按需进行。

### 2.3 VTK 渲染核心（CbctVtkRenderer，PDF「四」）

#### 零拷贝体数据导入

```cpp
vtkNew<vtkImageData> img;
img->SetDimensions(vol->width, vol->height, vol->depth);
img->SetSpacing(vol->spacingX, vol->spacingY, vol->spacingZ);
// SetArray(ptr, n, save=1)：VTK 只读借用，不接管内存释放
arr->SetArray(reinterpret_cast<short*>(vol->data), n, 1);
img->GetPointData()->SetScalars(arr);
```

`CbctVolume` 的数百 MB 体数据**无需任何拷贝**即成为 VTK 管线输入。代价是生命周期约束：**`destroyRenderer()` 必须先于 `releaseVolume()` 调用**（`CbctVtkView.release()` 内部已保证该顺序）。

#### VR 体绘制管线

```
vtkImageData ──> vtkSmartVolumeMapper ──> vtkVolume
                      │                       │
                      └── vtkVolumeProperty ──┘
                          ├─ vtkColorTransferFunction（颜色）
                          └─ vtkPiecewiseFunction（不透明度）
```

- `vtkSmartVolumeMapper` 自动按设备能力选择 GPU RayCast / CPU FixedPoint 路径（PDF 伪代码中的 `vtkVolumeRayCastMapper` 为 VTK 8.x 类，9.x 已移除，此处为等价替代）；
- **传递函数即窗宽窗位**：颜色/不透明度控制点由 WW/WC 换算到原始像素域（`huToRaw()`）动态生成——色彩跨度跟随窗宽窗位实时变化；
- **骨骼阈值过滤**：HU < 200 全透明（滤除软组织），200→1300 HU 不透明度渐升，高位饱和，`ShadeOn()` 明暗着色增强骨骼立体感。

#### MPR 切面管线

```
vtkImageData ──> vtkImageReslice ──> vtkImageMapToColors ──> vtkImageActor
                （三轴切面矩阵）      （vtkLookupTable 灰阶）
```

- `vtkImageReslice` 通过 `SetResliceAxesDirectionCosines` + `SetResliceAxesOrigin` 定义切面（axial 固定 Z / coronal 固定 Y / sagittal 固定 X），`SetOutputDimensionality(2)` 输出单张 2D 切面，线性插值采样；
- 灰阶 `vtkLookupTable` 的 Range 即窗宽窗位区间，切面显示与 VR 色彩跨度共用同一 WW/WC 状态；
- 平行投影正交显示，切面无透视畸变。

#### 渲染线程模型（关键设计）

EGL/OpenGL ES 上下文与线程绑定，因此**所有 GL 操作（含 VTK Render）固定在模块内部专用渲染线程**执行。外部调用（JNI/UI 线程）通过任务队列投递：

- **异步任务**（手势、参数调节等高频事件）：入队即返回，渲染线程排空队列后统一上屏——连续滑动 N 帧自动合并，不丢事件也不重复渲染；
- **同步任务**（Surface 创建/销毁等时序点）：阻塞等待渲染线程执行完毕，保证与 Android Surface 状态严格一致；
- **按需渲染**：队列排空且状态有变化（`dirty_`）才调用 `Render()`，闲置状态 0 GPU 占用（对应 PDF「闲置状态暂停渲染」）。

#### Surface 对接

`vtkEGLRenderWindow::SetWindowId(ANativeWindow*)` 直接挂接 Android 原生窗口：

| SurfaceView 回调 | Native 动作（同步执行） |
|---|---|
| `surfaceCreated` | `ANativeWindow_fromSurface` 取窗口 → 创建 EGL RenderWindow/Renderer → 挂载当前模式 Props → 首帧自动复位相机 |
| `surfaceChanged` | `SetSize` 适配旋转/分屏 |
| `surfaceDestroyed` | `Finalize()` 释放 EGL Surface/Context → `ANativeWindow_release` 归还引用（回调返回后 Surface 即被系统回收，禁止再有任何 GL 操作） |

### 2.4 Android 端渲染交互（CbctVtkView，PDF「五」）

`CbctVtkView` 继承 `SurfaceView`，职责严格内聚为两件事：**生命周期转递 + 手势捕获**，渲染逻辑全部在 Native 侧。

#### 手势映射（PDF 5.2）

| 手势 | VR 模式 | MPR 模式 |
|---|---|---|
| 单指滑动 | 旋转相机（Azimuth/Elevation，trackball 语义约 180°/视口高） | 平移切面 |
| 双指捏合 | 相机沿视线远近（距离钳制初始距离的 [0.1x, 10x]） | 平行缩放 |
| 双指拖动 | 模型位置偏移（捏合焦点位移即平移增量） | 同左 |
| 复位按钮 | 恢复初始 45° 俯视视角 + `ResetCamera` | 切面重新铺满视口 |

相机操作细节（`applyPan`）：按平行/透视投影分别换算世界坐标/像素比（`ParallelScale` 或视场角×距离），沿视平面基向量（`dir × up` 右手系）偏移相机位置与焦点，保证拖拽方向与内容移动方向一致。

#### 状态重放机制

`CbctVtkView` 缓存当前 UI 状态（模式/平面/层位置/窗宽窗位），`setVolume()` 注入新序列后自动重放——**跨序列切换保持浏览参数**，与 Bitmap 2D 路径的交互体验完全一致（沿袭 app 既有 UI 设计）。

---

## 3. 代码流程

### 3.1 解析 + 渲染全链路

```
[CbctParseFragment] 用户点击「Parse Series」
   │
   ▼
CbctParseEngine.parse(dir, callback)                    # IO 线程
   │
   ▼
CbctJni.loadSeries(dir, cb) ── JNI ──> CbctSeriesParser::loadSeries()
   │                                      双 Pass 解析 → CbctVolume（Native 堆）
   ▼                                      进度经 CbctProgressCallback 回调 UI
CbctJni.getVolumeMeta(ptr) ──> CbctSeriesMeta.fromMap()
   │
   ▼
CbctVolumeHandle(ptr, meta)                            # Kotlin 句柄
   │
   ▼
vtkView.setVolume(handle) ── JNI ──> CbctVtkJni.createRenderer(ptr)
   │                                      CbctVtkRenderer::create()
   │                                        零拷贝导入 + VR/MPR 管线装配 + 启动渲染线程
   ▼
surfaceCreated 回调 ── JNI ──> onSurfaceCreated()
   │                                      EGL 窗口建立 + 相机复位 + 首帧渲染
   ▼
用户交互（手势/SeekBar/模式切换）
   │  异步任务队列 ──> 渲染线程落地到 VTK 管线 ──> 按需 Render 上屏
   ▼
界面销毁：vtkView.release()（先） ──> volumeHandle.release()（后）
```

### 3.2 JNI 接口一览（CbctVtkJni）

| 方法 | 说明 | 执行方式 |
|---|---|---|
| `createRenderer(volumePtr)` | 创建渲染器（零拷贝导入） | 调用线程 |
| `onSurfaceCreated(ptr, surface, w, h)` | 绑定 EGL 渲染窗口 | 同步 |
| `onSurfaceChanged(ptr, w, h)` | 尺寸适配 | 同步 |
| `onSurfaceDestroyed(ptr)` | 释放 EGL 资源 | 同步 |
| `setRenderMode(ptr, MODE_VR/MODE_MPR)` | VR/MPR 互切（Prop 增删 + 相机适配） | 异步 |
| `setPlane(ptr, plane, position)` | MPR 平面与层位置 | 异步 |
| `setWindowLevel(ptr, ww, wc)` | 窗宽窗位（双管线共用状态） | 异步 |
| `rotate/pan/zoom(ptr, ...)` | 手势驱动相机 | 异步 |
| `resetCamera(ptr)` | 复位相机 | 异步 |
| `destroyRenderer(ptr)` | 同步停渲染线程并销毁 | 同步 |

### 3.3 构建配置要点（CMakeLists.txt）

- **VTK 静态库链接**：44 个 `libvtk*.a` 全量收集，以 `-Wl,--start-group ... -Wl,--end-group` 分组解决 VTK 模块间循环依赖；
- **系统依赖**：`EGL` / `GLESv2` / `GLESv3`（VTK OpenGL ES 渲染窗口所需）；
- **包体积裁剪**：`-ffunction-sections -fdata-sections` + `-Wl,--gc-sections` 链接期回收未引用的 VTK 代码路径；
- **ABI**：仅 `arm64-v8a`（VTK 静态库交叉编译目标，见 `doc/android_vtk.sh`）。

---

## 4. 使用操作流程（App 端）

入口：主界面「CBCT Parse」页（`CbctParseFragment`）。

### 4.1 解析序列

1. **选择序列源**：点击「选择目录 (SAF)」从安全存储选取 CBCT 序列目录，或在输入框直接填写绝对路径；
2. **点击「Parse Series」**：进度条展示 Pass A/B 双阶段进度，完成后显示患者信息、体数据维度、层间距、解析耗时等摘要；
3. 解析失败会提示原因（目录无有效 DICOM / 路径不可访问等）。

### 4.2 三维可视化

1. **切换渲染载体**：查看器控制区「Bitmap 2D / VTK 3D」单选——
   - **Bitmap 2D**：CPU 切面提取路径（原有能力），图像显示在 ImageView；
   - **VTK 3D**：SurfaceView 渲染路径，下方显示手势提示；
2. **VTK 子模式**（仅 VTK 3D 下可见）：
   - **VR 体绘制**：默认模式，斜上方 45° 初始视角观察骨骼三维结构；平面切换与层位置控件自动隐藏（无意义）；
   - **MPR 切面**：三平面（横断/冠状/矢状）单选 + 层位置 SeekBar 逐层浏览；
3. **窗宽窗位**：窗宽/窗位 SeekBar 实时生效——VR 驱动传递函数色彩跨度，MPR 驱动切面灰阶区间，Bitmap 驱动提取映射，三者状态同步；
4. **手势操作**（VTK 3D 下）：单指滑动旋转（VR）/平移（MPR）、双指捏合缩放、双指拖动平移；
5. **复位相机**：「复位相机 (VTK)」按钮恢复默认视角/铺满切面。

### 4.3 代码接入示例（其他页面复用）

```kotlin
// 1. 解析（IO 线程）
val handle = CbctParseEngine().parse(dir, callback)   // 返回 CbctVolumeHandle

// 2. 渲染（布局中放置 com.wangyao.cbctdeal.render.CbctVtkView）
vtkView.setVolume(handle)
vtkView.setRenderMode(CbctVtkJni.MODE_VR)
vtkView.setWindowLevel(4000.0, 600.0)

// 3. 释放（顺序严格：先渲染器后 Volume）
vtkView.release()
handle.release()
```

---

## 5. 性能与稳定性设计

| 项 | 措施 |
|---|---|
| 内存 | 零拷贝导入（数百 MB 体数据无重复占用）；16bit 体素不预转换 HU |
| CPU | 双 Pass 并行解析；渲染闲置 0 GPU 占用（按需渲染） |
| 流畅度 | 高频手势事件队列合并渲染；垂直同步（SwapControl=1）防撕裂 |
| 线程安全 | 所有 GL/VTK 操作固定渲染线程；Surface 时序点同步执行 |
| 健壮性 | 渲染器/Volume 释放顺序强约束（View 内部已保证）；任务异常捕获不崩线程；`onDetachedFromWindow` 兜底释放防泄漏 |
| 兼容性 | GPU/CPU 体绘制自动回退；MSAA 关闭规避 ES 端体渲染配置兼容问题 |

## 6. 注意事项

1. **生命周期顺序**：`CbctVtkView.release()` 必须先于 `CbctVolumeHandle.release()`（零拷贝引用体素）——Fragment 销毁路径已按此实现；
2. **真机诊断**：VTK 错误输出已重定向 logcat（tag `CbctVtk` / `CbctNative`）；
3. **模拟器**：VR 依赖 OpenGL ES 3.0，建议真机（arm64）验证；MPR 灰阶路径兼容性更好；
4. **序列要求**：目录内需为同一序列的 DICOM 切片（按 Z 坐标排序组装），混合目录会按非 DICOM/损坏文件过滤；
5. **重新编译 VTK**：交叉编译脚本见 `doc/android_vtk.sh`（NDK r25 / android-24 / OpenGL ES / 裁剪配置）。
