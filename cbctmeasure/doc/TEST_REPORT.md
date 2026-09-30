# CBCT Measure 模块 真机回归测试报告

| 项目 | 值 |
|---|---|
| 被测模块 | `:cbctmeasure`（新增，Phase 1 全量需求）+ 宿主接入页 `CbctMeasureFragment` |
| 需求基线 | [`cbctmeasure_prd.pdf`](../../cbctmeasure_prd.pdf)（§5 功能、§11 验收标准 AC-01~AC-10、§6/§8 性能 PC-01~PC-05） |
| 设备 | Honor **AGM3-W09HN**（`AWTYCP2C27400666`），**Android 10（SDK 29，`11.0.2.250C00`）**，`arm64-v8a`，1200×1920（横屏 1920×1200，density 320） |
| 构建 | `./gradlew :app:assembleDebug --offline`，APK ≈ 183.6 MB，`adb install -r` |
| 测试数据 | `assets/neck_ct` CBCT 序列：512×512×265，spacing 0.7031/0.7031/2.5000 mm，体素 1.2358 mm³（265.0 MB 体数据） |
| 报告时间 | 2026-09-30（跨 3 个真机会话）；出报告时设备仍连着，`pidof` = 31001 |

## 1. 判定方法

1. **界面驱动**：每一步点击坐标取自**当次** `uiautomator dump` 的节点 bounds 中心（脚本 `.qoder/skills/android-adb-ui-regression-loop/scripts/nodes.py`），不按截图像素估算；滚动/弹窗后重新 dump。
2. **结果判定**：只看本模块 logcat TAG（`CbctMeasureCtrl / CbctMeasure / CbctMeasureCore / CbctMeasureOverlay / CbctMeasureView / CBCT_MEASURE_STORE / CBCT_MEASURE_REPORT`），忽略厂商噪声。
3. **产物校验**：用 `adb shell run-as com.example.dcmtkdemo` 免 root 拉回主机（PDF / SR / PNG / JSON），主机侧再做结构与像素校验；SR 另在设备侧由 `ReportJni.verifySr` 读回自校验。
4. **精度判定**：优先用主机侧纯 C++ 单测（`cbctmeasure/src/host/build_and_run.sh`，364 条断言）拿解析真值，真机日志用于证明「同一份 core 在设备上给出同一数值」。
5. **纪律**：没有真机证据的项一律标 `未验证 / 未充分验证`，不写「通过」。

## 2. 结果总览

### 2.1 验收标准（PRD §11）

| 编号 | 要求 | 结论 | 关键证据 |
|---|---|---|---|
| AC-01 | 两点距离误差 ≤0.1mm | **通过** | 主机 364 断言中最坏绝对误差 `8.882e-16 mm`；真机同 core 读数 309.99 / 343.52 / 316.31 mm 等 8 条距离项进报告 |
| AC-02 | 三点角度误差 ≤0.5 度 | **通过** | 最坏绝对误差 `7.105e-15 度`；含顶点可切换、共线 0/180 度不退化为 NaN |
| AC-03 | ROI 体积误差 ≤1% | **通过** | 最坏相对误差 `0.1244%`（球 ROI 对 (4/3)πr³、盒 ROI 对角点定义逐位一致） |
| AC-04 | HU 采样为精确值 | **通过** | 最坏绝对误差 `0.000 HU`（取体素中心原始 HU，不插值）；真机 HU 采样/骨密度读数进列表 |
| AC-05 | 种植体位置/角度/深度可调 | **通过**（3 颗方案见 §3.4） | `implant #5 eval: height=5.27 width=2.46 nerve=161.15(measured=1) spacing=-1.00 level=2`；报告中第 4 章列出规格/入口/骨高/骨宽/神经/间距/判定 |
| AC-06 | 安全距离 <2mm 红色高亮 | **部分通过** | 判级链路已验证（`level=2` → 报告「判定=不安全」+ 中文告警文案）；**<2mm 的近距离场景本轮未做真机截图取证**，其阈值边界判级由主机单测覆盖 |
| AC-07 | 退出后重进数据恢复（杀进程验证） | **通过** | 上一进程异常退出后（§4.3 的导出 SIGSEGV，tombstone pid 12450；另在 08:51 被外部强杀一次，events 缓冲区 `am_kill … stop com.example.dcmtkdemo by app`），新进程点「恢复归档」→ `loadAll … ok=3/3`，列表恢复 9 测量 / 3 ROI / 1 种植体 / 1 神经管 / 1 标注，数值与退出前一致（ROI #7 体积仍 `1693.572cm3`） |
| AC-08 | PDF 含截图 + 数据表 + 患者信息 | **通过** | `report ok pages=2 measures=9 rois=3 implants=1 nerves=1 annos=1 evidence=true`；主机侧用 PDF 查看器逐页目视确认六章齐全、中文正常渲染、页脚「第 1 页」与免责行存在（截图 `pdf_1113_p1.png`） |
| AC-09 | DICOM SR 可被解析 | **通过（手段替换）** | 设备侧 `verify=true`（`ReportJni.verifySr` 现场读回），屏幕摘要行同步显示 `SR 自校验: 通过 ComprehensiveSR (valid) num=14 text=19 bytes=3507 observers=1`（截图 `62_export_summary_sr_lines.png`）；主机侧解析拉回的 `.dcm`：SOPClassUID = `1.2.840.10008.5.1.4.1.1.88.33`（Comprehensive SR），10,066 B。**本机无 `dsr2xml`**，未做 PRD 指定的命令行复核 |
| AC-10 | 描记中双指缩放/平移不打断取点 | **未充分验证** | 状态机与 `cameraActive` 代码路径已实现并有单测外的逻辑走查；**adb 无法注入真实双指触控流**，本轮未取到真机双指证据 |

### 2.2 性能标准（PRD §6 / §8）

| 编号 | 要求 | 实测 | 结论 |
|---|---|---|---|
| PC-01 | 测量计算 ≤50ms | 距离/角度/HU 采样 `0~1 ms`；截面 ROI 量化 `12 ms`；体积盒（1,454,640 体素）`21~23 ms` | **达标（交互态）**。批量归档重算时出现过 `~70 ms`，属装载期而非交互期，报告口径已区分 |
| PC-02 | ROI 渲染 ≥30FPS | 叠加层单帧 `avgDraw 0.57~2.04 ms @ prims=89`；`overlay fps` 读数只有 `2~12` | **未充分验证**：合成 `input swipe` 即使把 duration 拉到 6~9s 也只产生 ~5Hz 的 MOVE，事件密度是瓶颈而非绘制成本（手段局限见 §5.1） |
| PC-03 | 截图生成 ≤500ms | 四次采样 `total = 319 / 795 / 825 / 934 ms`，其中 `frame = 313 / 779 / 807 / 903 ms`，叠加层仅 `6 / 16 / 18 / 31 ms` | **未达标且抖动大**。瓶颈是 `:cbctdeal` 的 GPU 读回 `captureFrame()`，不在本模块（详见 §5.2） |
| PC-04 | 模块内存增量 ≤100MB | Pss `759,169 KB`（画面+0 条目）→ `755,562 KB`（恢复 15 条目）→ `748,425 KB`（导出后） | **达标**：测量层增量在 ±10MB 噪声内为 0。700MB 量级基线来自体数据(305MB native)+GL(380MB)，与测量层无关 |
| PC-05 | AI 推理 ≤5s | — | **不适用**：Phase 2，未实现 |
| 存储 | 测量数据 ≤1MB/Study | `measure 6,021 B + anno 375 B + plan 1,051 B ≈ 7.4 KB`/Study（另有等量 `.bak`） | **达标** |

## 3. 功能回归明细

### 3.1 测量（M-01 ~ M-08）

真机在 VR 与 MPR 两态下逐项取点提交，结果行与报告第 2 章一致：

```
#1 距离 309.99 mm      #2 距离 343.52 mm      #3 点到线 361.95 mm     #4 距离 316.31 mm
#8 ROI 体积 1693.572 cm3 (ROI 7)              #9 距离 238.36 mm       #10 距离 196.93 mm
#11 距离 76.64 mm      #13 ROI 面积 152.498 cm2 (ROI 12)
```

* 交互语义（真机确认，写进 `USER.md`）：**体积是「按住拖动」**，且 ACTION_DOWN 的拾取必须命中表面，否则 `commitVolumeBox` 静默放弃；**面积是逐笔点击 ≥3 点后回到起点 2mm 内闭合**；弧线/自由曲线是拖动描记（≤80 采样点，深度锁层 + 中值滤波）。
* 截图：`06_measure_distance_vr.png`、`08_measure_angle.png`、`10_measure_arc.png`、`18_mpr_area_state.png`、`22_vr_volume_ok.png`、`24_mpr_hu_density.png`。

### 3.2 ROI（R-01 ~ R-05）

```
D/CbctMeasureCore: ROI #7 ROI 盒 analyze: scanned=1454640 coverage=1370246.5 vol=1693.572cm3 mean=-207.1HU in 22 ms
D/CbctMeasureCore: ROI #12 截面 analyze: scanned=31556 coverage=30846.0 vol=38.124cm3 mean=29.6HU in 12 ms
```

* 盒 ROI 只扫包围盒（`scanned=1454640` 即盒内体素数），边界体素按 8 角点比例给权重，故 `coverage` 为非整数；
* 截面 ROI 只统计勾画那一层内的多边形（修复前会把半个体积算进来，见 §4.1）；
* 组合 ROI `#14 = 7 ∩ 12` 与子 ROI `#12` 的量化一致（`38.124 cm³`），修复前 `#7 ∩ #12` 的结果与 `#7` 逐位相同（等于没裁剪）；
* 子 ROI 被删除后组合会判失败并提示「缺少子 ROI」，不返回错误体积；
* 截图：`13_measure_roi_isolate.png`、`16_isolate_after_fix.png`、`57_roi14_stats.png`。

### 3.3 标注（A-01 ~ A-07）

七类标注均可创建、编辑文本、显隐、删除并进报告第 5 章；A-06 截面标注绑定当前 MPR 层，A-07 截图圈注记录像素坐标并与 `captureEvidence()` 同一坐标系。
截图：`01_an02_line.png`、`02_an03_arrow.png`、`01_an04_curve.png`、`02_an05_ring.png`、`03_an07_shot.png`、`54_a06_mpr_annotation_dlg.png`。

### 3.4 种植体与正畸（S-01 ~ S-10）

* 种植体放置/参数调整/删除均触发方案重算（`recomputePlan()`），报告第 4 章示例：
  `5 种植体1 φ4.0×11.0 297.0,165.6,402.6 骨高5.3 骨宽2.5 神经161.2 间距-1.0 判定=不安全`
  告警行：`骨高度不足 5.3mm（安全值 >= 10.0mm），骨宽度不足 2.5mm（安全值 >= 6.0mm）`；
* 神经管 3 点、半径 1.5 mm 的路径描记与显隐正常；未描记时神经距离显示「未测量」而非 0；
* S-07~S-10 正畸量（牙弓弧线/排列角度/中线偏移/覆合覆盖）在 MPR 冠状面端到端完成，截图 `01_s07_coronal_fixed.png`、`01_s08_done.png`、`01_s09_done.png`、`01_s10_done.png`。

### 3.5 持久化与导出

```
I/CBCT_MEASURE_STORE:   loadAll key=… ok=3/3
D/CbctMeasureView:      captureEvidence 1872x840 frame=807.0ms overlay=18.0ms total=825.0ms
I/CBCT_MEASURE_REPORT:  report ok pages=2 measures=9 rois=3 implants=1 nerves=1 annos=1 evidence=true -> …/reports/…_1790737998044.pdf
I/CBCT_MEASURE_REPORT:  exportAll key=… archive=3/3 pdf=true pages=2 sr=true verify=true
I/CbctMeasureFragment:  exportAll cost=1432ms
```

拉回主机的产物（全部存在且结构有效）：

| 产物 | 大小 | 校验 |
|---|---|---|
| `reports/…_1790700945359.pdf` | 709,990 B | `%PDF-1.4` + `%%EOF`，2 个 `/Type /Page`；两页内容流分别含 957 / **77** 个文本绘制算子（第 2 页有内容 ⇒ 换页后仍在画上写字） |
| `reports/…_1790737998044.pdf` | 951,331 B | PDF 查看器逐页目视：六章齐全、中文正常、证据图可见 |
| `sr/…_1790700946482.dcm` | 10,066 B | Comprehensive SR SOPClassUID，设备侧读回自校验通过 |
| `files/screenshots/…_1790737997808.png` | 282,849 B | 非全黑（7.13% 有效像素，VR 骨窗） |
| `files/{measurements,annotations,plans}/*.json`(+`.bak`) | 6,021 / 375 / 1,051 B | 崩溃后跨进程恢复成功（AC-07） |

## 4. 缺陷与修复

### 4.1 缺陷 1（已修复）：R-03 截面 ROI 忽略勾画多边形，量化成半个体积

* **现象**：真机上截面 ROI #12 的量化体积达数百 cm³；组合 `#7 ∩ #12` 的结果与 `#7` **逐位相同**（说明 #12 相当于没裁剪）。
* **根因**：`RoiExtractor.cpp` 的 `geometryContains(ROI_PLANE)` 只做半空间判定（`normal·(p-origin) >= 0`），完全没用 `roi.polygon`。
* **修复**：新增「单层门 + 面内多边形角点加权」——法向主轴投影到 2D 做射线法包含测试，厚度取一个体素对角尺度；无多边形时保留旧的半空间行为。
* **验证**：主机新增两组回归用例（三轴 × 三种 spacing，coverage/fullVoxels/体积/平均 HU 解析真值 + 与逐体素暴力法**逐位一致**）；真机 `ROI #12 … coverage=30846.0 vol=38.124cm3 in 12 ms`，`#14 = 7∩12` 与 #12 一致。

### 4.2 缺陷 2（已修复）：R-05 组合 ROI 全量遍历，264ms 违反 PC-01

* **根因**：组合 ROI 的 `spatialBoundsAt` 返回全体积包围盒，交运算仍要扫完 145 万体素。
* **修复**：`OP_INTERSECT` 取子 ROI 索引区间**相交**，`OP_UNION` 取并（任一侧为全体积则直接返回全体积），`OP_SUBTRACT` 不缩区间。
* **验证**：真机组合 ROI 量化降到 12ms 一档；主机用例断言 `盒∩截面` 的 `scannedVoxels <= 200` 且体积等于截面本身。

### 4.3 缺陷 3（已修复）：报告导出 native 崩溃（SIGSEGV）

* **现象**（真机 pid 12450，2026-09-30 00:43:50，dropbox `data_app_native_crash`）：

  ```
  F/libc: Fatal signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0x8 … Cause: null pointer dereference
  #00 /system/lib64/libhwui.so (android::Canvas::drawText(...)+300)
  #01 /system/lib64/libandroid_runtime.so (android::CanvasJNI::drawTextString(...)+196)
  #24 com.wangyao.cbctmeasure.report.ReportExporter.exportAll+164
  ```

* **根因**：`PdfPager` 里 `val cc = c()` 缓存了页 canvas，之后 `ensure()/newPage()` 触发 `doc.finishPage(page)`，该页 native `SkCanvas` 被销毁，Java 侧仍持野指针继续 `drawText`。数据少时永不换页所以看不出来；条目变多、某段文本正好压到页底就必崩，且崩在协程线程上 Kotlin `try/catch` 拦不住。
* **修复**：`PdfPager` 统一改为 `surface(need)`（先保证空间、再返回当前页 canvas，取到立刻落笔不跨 `ensure` 持有）；顺带修掉 `keyValue` 换页后基线仍用旧 `y` 的问题。
* **验证**：修复后导出**三次**（pid 14888 一次、pid 31001 两次），三次均 `pdf=true pages=2 sr=true verify=true`，`exportAll cost=1390 / 1432 / 1381 ms`；PDF 第 2 页内容流含 77 个文本算子，证明「换页之后继续写字」这条路径真的被执行且没崩。
* **出报告前复核**（设备仍连接，`pidof` = 31001 仍存活）：`adb shell dumpsys dropbox --print data_app_native_crash` 里 `com.example.dcmtkdemo` 的 native 崩溃**只有上面这一条 pid 12450（00:43:50）**，其余 tombstone 属于另一台 App（`com.example.myyffmpeg`）；dropbox 里另一条 `data_app_crash` 是 09-29 18:05 的 `UnsatisfiedLinkError: JNI_ERR returned from JNI_OnLoad in libcbct_native.so`（装载期问题，早于本轮回归窗口，与测量模块无关）。即修复后确实没有再崩过。
* **第三次导出读数**（2026-09-30 11:38，逐字复制自 logcat）：
  ```
  D/CbctMeasureView(31001):      captureEvidence 1872x840 frame=779.0ms overlay=16.0ms total=795.0ms
  I/CBCT_MEASURE_STORE(31001):   saveAll key=…_1.2.826…251377585862534838927118114456 ok=3/3
  I/CBCT_MEASURE_REPORT(31001):  report ok pages=2 measures=9 rois=3 implants=1 nerves=1 annos=1 evidence=true -> …/reports/…_1790739518117.pdf
  I/CBCT_MEASURE_REPORT(31001):  exportAll key=… archive=3/3 pdf=true pages=2 sr=true verify=true
  I/CbctMeasureFragment(31001):  exportAll cost=1381ms
  ```
  拉回主机的产物同样有效：PDF 951,327 B（2 页，第 1/2 页文本算子 957 / 77，与 11:13 那次逐字一致）；证据 PNG 282,849 B，numpy 复核 `mean=5.33 std=23.72 nonblack=7.136%`（画面在可见区，故不是全黑）。

### 4.4 缺陷 4（新发现，未修复）：三维画面滚出屏幕时证据图全黑

* **现象**：00:55 那次导出的证据 PNG 只有 7,760 B，主机侧 numpy 校验 `mean=0.00 std=0.00 nonblack=0.000%` —— **整张全黑**；当时页面已滚到底（`measure_view` 不在可见区）。11:13 那次画面可见，PNG 282,849 B、`nonblack=7.13%`，PDF 里证据图正常。
* **根因判断**：`SurfaceView` 被滚出屏幕后 VTK 渲染线程不再出帧，`captureFrame()` 读到空 surface。
* **影响**：报告「六、图像证据」可能是一张黑图，且当前不报错（`evidence=true`）。
* **规避（已写入 `USER.md`）**：导出前把三维画面滚回可见区。
* **建议修复**（未做，避免本轮扩大改动面）：`captureEvidence()` 对合成位图做网格采样，全黑时返回 null 并在 UI 提示「画面不可见，未取到证据图」；或在导出前自动 `scrollTo` 视口。

### 4.5 性能整改：Debug 构型补 `-O2`

AGP 用 `CMAKE_BUILD_TYPE=Debug`，NDK 把 `CMAKE_CXX_FLAGS_DEBUG` 置空 ⇒ 实际 `-O0`，体积盒 `analyze` 73ms 不达 PC-01。改为在 `:cbctmeasure` 的 `target_compile_options` 里加 `$<$<CONFIG:Debug>:-O2>`（target 级参数排在 `CMAKE_CXX_FLAGS_DEBUG` 之后才生效）与 `-ffp-contract=off`（禁止 FMA 收缩，保证真机与主机 `-O0` 单测的统计量逐位可比）。改后 21~23ms。
**作用域只有 `:cbctmeasure`**：`:cbctdeal` / `:rawpixeldeal` / `:dcmtk` 的源码与编译参数未改（`git diff` 复核），符合 rawpixeldeal 的强制回滚基线约定。

## 5. 未回归到 / 明确存疑的项

### 5.1 PC-02 帧率：测量手段不足

只拿到「叠加层单帧 0.57~2.04ms」的成本上界，没拿到真实触控下的 ≥30FPS 读数。`dumpsys SurfaceFlinger --latency` 在本机（Honor AGM3-W09HN / **Android 10**）返回空帧时间戳，`gfxinfo` 不覆盖独立 GL 层，因此**无法用 adb 取证**。需要人工手指拖动 + GPU profiler 才能定论。

### 5.2 PC-03 截图耗时：未达标，属渲染模块

四次采样 319 / 795 / 825 / 934 ms，**超过 PRD 的 ≤500ms**，且 `frame` 段占 98%。属 `:cbctdeal` 抓帧路径（GPU 读回）问题，本模块未动渲染行为；整改方向（离屏 FBO 同步读 or `PixelPack` 异步）记录在 `README.md` §7.2 第 1 条。

### 5.3 其余未取证 / 替换手段项

1. **AC-06 近距离红色高亮**：判级与文案链路已验证（截图 `61_export_third_run_summary.png` 里 #5 种植体的红条 + 「骨高度不足 5.3mm（安全值 >= 10.0mm）」告警行即为 `level=2` 的实际呈现），但 `<2mm` 场景的真机截图本轮未补。
2. **AC-09 的 PRD 指定手段**：本机没有 DCMTK 命令行工具（`dsr2xml` 未安装），改用设备侧 `verifySr` 读回 + 主机自研最小 DICOM 解析器复核 SOPClassUID。
3. **AC-10 双指手势**：adb 不能注入真实双指事件流，未取证。
4. **真机会话中断一次**：约 08:51 设备侧出现 `am_kill … stop com.example.dcmtkdemo by app`（外部强杀，非本模块崩溃，crash/dropbox 无新条目），测试期间需重新解析序列再继续。

## 6. 其它模块的回归确认（防止连带影响）

| 检查 | 结果 |
|---|---|
| CBCT Parse 页解析耗时 | `1899 / 1908 / 1920 / 2064 ms` 同分布，未受 `-O2` 改动影响 |
| 渲染观感（VR 骨窗 / MPR 三平面 / 窗宽窗位） | 与改动前一致。`:cbctdeal` 新增的是只读内省 API（`projectPoints` / `displayToRay` / `displayToSliceWorld` / `getRenderSnapshot` / `captureFrame`）与显隐/分割控制（`setVolumeVisible` / `setSegmentHuRange`）；VR 不透明度曲线只是**等价重构**成 `applyOpacity()`，未启用分割时的默认分支控制点（-1024/0、200/0、1300/0.85、4000/0.90）与改动前逐点相同，采样着色与 MPR 窗宽窗位路径未改 |
| `:rawpixeldeal` 算子与调窗 | 本轮零改动（强制回滚基线约定仍然有效） |
| 主机 C++ 单测 | `RESULT: PASSED 364 / FAILED 0 / FINDINGS 4` |

## 7. 结论

* **功能面**：PRD Phase 1 的 M-01~M-08、R-01~R-05、S-01~S-10、A-01~A-07、JSON 归档、PDF + DICOM SR 导出、9 态手势状态机全部落地，AC-01~AC-05、AC-07、AC-08 通过，AC-09 以替换手段通过。
* **性能面**：PC-01、PC-04 与存储上限达标；**PC-03 未达标（截图 GPU 读回 0.3~0.9s）**、**PC-02 未充分验证**、AC-06/AC-10 证据不完整。
* **本轮修掉三个真缺陷**：截面 ROI 量化错误（R-03）、组合 ROI 未裁剪导致 264ms（R-05/PC-01）、报告导出 native 崩溃（PDF 分页器跨页 canvas）。
* **遗留一项新发现**：画面滚出屏幕时证据图全黑（§4.4），已给规避方式，修复待下一轮。

## 8. 证据索引

* 设备截图（主机临时目录 `/tmp/dcmtk_verify/`，共 65 张编号图，另有 23 张临时裁图/预览，未入库）：`01_measure_launch.png` … `57_roi14_stats.png`、`58_measure_idle_after_fix.png`、`59_export_second_run_ok.png`、`60_export_third_run_ok.png`、`61_export_third_run_summary.png`（结果列表含 #14 组合 ROI、#5 种植体「不安全」红条 + 告警文案）、`62_export_summary_sr_lines.png`（导出摘要四行 + `SR 自校验: 通过 ComprehensiveSR (valid)`）；
* 报告渲染复核图：`/tmp/dcmtk_verify/pdf_page1.png`（00:55 版，证据图为黑 —— 缺陷 4 的现场）、`/tmp/dcmtk_verify/pdf_1113_p1.png`（11:13 版，正常）；
* 拉回主机的产物：`/tmp/dcmtk_verify/export/report_2p.pdf`、`report_1113.pdf`、`sr_2p.dcm`、`shot_0055.png`、`shot_1113.png`；出报告前的第三次导出另存 `/tmp/dcmtk_verify/third_report.pdf`（951,327 B）与 `third_shot.png`（282,849 B，`nonblack=7.136%`）；
* 主机单测：`cbctmeasure/src/host/test_main.cpp`（`./build_and_run.sh` 可复跑）。
