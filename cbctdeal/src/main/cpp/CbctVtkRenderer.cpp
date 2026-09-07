// CBCT VTK 三维渲染核心实现。
//
// 对应《Android平台VTK完整集成流程（适配CBCT三维可视化）》：
//   四、JNI层VTK核心适配逻辑 —— 体数据零拷贝导入、体渲染管线、窗宽窗位传递函数；
//   五、Android端渲染交互适配 —— Surface 对接、手势旋转/缩放/平移、MPR 切面。
//
// 与 PDF 伪代码的差异（VTK 9.1.0 真实 API，PDF 标注部分内容由 AI 生成）：
//   - vtkVolumeRayCastMapper 为 VTK 8.x 类，9.1 已移除
//       -> vtkSmartVolumeMapper（GPU RayCast + CPU FixedPoint 自动回退）；
//   - vtkVolumeProperty 无 SetColorWindow/SetColorLevel
//       -> 窗宽窗位换算为 Color/Opacity TransferFunction 的取值区间；
//   - mapper 无 SetMinimumImageThreshold
//       -> 骨骼阈值（200HU）以不透明度传递函数起点实现软组织过滤。

#include "include/CbctVtkRenderer.h"

#include <android/log.h>
#include <android/native_window.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <new>

// VTK 静态库对象工厂注册（静态构建必须显式初始化）：
//   RenderingOpenGL2      -> vtkEGLRenderWindow（Android Surface 渲染窗口）
//                            + vtkOpenGLImageSliceMapper（MPR 切面显示）
//   RenderingVolumeOpenGL2 -> vtkSmartVolumeMapper（体绘制 Mapper 工厂）
#include "vtkAutoInit.h"
VTK_MODULE_INIT(vtkRenderingOpenGL2)
VTK_MODULE_INIT(vtkRenderingVolumeOpenGL2)

#include "vtkCamera.h"
#include "vtkColorTransferFunction.h"
#include "vtkImageData.h"
#include "vtkImageActor.h"
#include "vtkImageMapToColors.h"
#include "vtkImageMapper3D.h"
#include "vtkImageReslice.h"
#include "vtkLookupTable.h"
#include "vtkMath.h"
#include "vtkNew.h"
#include "vtkPiecewiseFunction.h"
#include "vtkPointData.h"
#include "vtkRenderer.h"
#include "vtkShortArray.h"
#include "vtkSmartVolumeMapper.h"
#include "vtkUnsignedShortArray.h"
#include "vtkVolume.h"
#include "vtkVolumeProperty.h"
#include "vtkEGLRenderWindow.h"
#include "vtkAndroidOutputWindow.h"
#include "vtkOutputWindow.h"

#define TAG "CbctVtk"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// =============================================================================
// 构造 / 析构 / 创建
// =============================================================================

CbctVtkRenderer::CbctVtkRenderer() = default;

CbctVtkRenderer::~CbctVtkRenderer() {
    // 兜底：Surface 未经历 destroyed 回调即被销毁时释放窗口引用
    if (window_) {
        if (renderWindow_) renderWindow_->Finalize();
        ANativeWindow_release(window_);
        window_ = nullptr;
    }
}

CbctVtkRenderer *CbctVtkRenderer::create(CbctVolume *vol) {
    if (!vol || !vol->data || vol->width <= 0 || vol->height <= 0 || vol->depth <= 0) {
        LOGE("create: invalid volume");
        return nullptr;
    }
    // 注意：析构为私有，unique_ptr 的默认删除器无权访问，故用裸指针手动管理
    CbctVtkRenderer *r = new (std::nothrow) CbctVtkRenderer();
    if (!r) return nullptr;
    if (!r->init(vol)) {
        LOGE("create: init failed");
        delete r;   // create 为类内静态成员，可访问私有析构
        return nullptr;
    }
    return r;
}

bool CbctVtkRenderer::init(CbctVolume *vol) {
    vol_ = vol;
    ww_ = vol->windowWidth > 0 ? vol->windowWidth : 4000.0;
    wc_ = vol->windowCenter;
    plane_ = PLANE_AXIAL;
    position_ = vol->depth / 2;
    mode_ = MODE_VR;

    // VTK 错误输出重定向到 logcat（辅助真机诊断）
    static bool outputWindowInstalled = false;
    if (!outputWindowInstalled) {
        vtkNew<vtkAndroidOutputWindow> androidOut;
        vtkOutputWindow::SetInstance(androidOut);
        outputWindowInstalled = true;
    }

    // ---------- 1) 零拷贝导入：vtkImageData 直接包装 CbctVolume 体素 ----------
    // SetArray(ptr, n, save=1)：VTK 只读借用，不接管内存释放，
    // CbctVolume 生命周期仍由解析侧管理（destroy() 先于 releaseVolume() 即可）。
    vtkNew<vtkImageData> img;
    img->SetDimensions(vol->width, vol->height, vol->depth);
    img->SetSpacing(vol->spacingX, vol->spacingY, vol->spacingZ);
    img->SetOrigin(0.0, 0.0, 0.0);   // index 域与世界域对齐，简化 MPR 轴矩阵
    const vtkIdType n = (vtkIdType) vol->width * vol->height * vol->depth;
    if (vol->pixelRepresentation != 0) {
        vtkNew<vtkShortArray> arr;
        arr->SetArray(reinterpret_cast<short *>(vol->data), n, 1);
        img->GetPointData()->SetScalars(arr);
    } else {
        vtkNew<vtkUnsignedShortArray> arr;
        arr->SetArray(reinterpret_cast<unsigned short *>(vol->data), n, 1);
        img->GetPointData()->SetScalars(arr);
    }
    imageData_ = img;

    // ---------- 2) VR 体绘制管线（PDF 四 4.2 适配） ----------
    colorTF_ = vtkSmartPointer<vtkColorTransferFunction>::New();
    opacityTF_ = vtkSmartPointer<vtkPiecewiseFunction>::New();

    volMapper_ = vtkSmartPointer<vtkSmartVolumeMapper>::New();
    volMapper_->SetInputData(imageData_);
    volMapper_->SetBlendModeToComposite();   // 组合成像（骨骼表面/半透明叠加）

    volProperty_ = vtkSmartPointer<vtkVolumeProperty>::New();
    volProperty_->SetColor(colorTF_);
    volProperty_->SetScalarOpacity(opacityTF_);
    volProperty_->SetIndependentComponents(1);  // 标量同时驱动颜色与不透明度
    volProperty_->ShadeOn();                    // 明暗着色，增强骨骼立体感
    volProperty_->SetInterpolationTypeToLinear();

    volume_ = vtkSmartPointer<vtkVolume>::New();
    volume_->SetMapper(volMapper_);
    volume_->SetProperty(volProperty_);

    // ---------- 3) MPR 切面管线（PDF 五 5.3：vtkImageReslice） ----------
    reslice_ = vtkSmartPointer<vtkImageReslice>::New();
    reslice_->SetInputData(imageData_);
    reslice_->SetOutputDimensionality(2);          // 输出单张 2D 切面
    reslice_->SetInterpolationModeToLinear();      // 各向同性体素线性采样
    reslice_->SetOutputOrigin(0.0, 0.0, 0.0);

    lut_ = vtkSmartPointer<vtkLookupTable>::New();
    lut_->SetHueRange(0.0, 0.0);        // 灰阶：色相/饱和度置零
    lut_->SetSaturationRange(0.0, 0.0);
    lut_->SetValueRange(0.0, 1.0);      // 亮度 0..1（窗宽窗位映射区间）
    lut_->SetAlphaRange(1.0, 1.0);
    lut_->SetNumberOfTableValues(256);
    lut_->Build();

    mapColors_ = vtkSmartPointer<vtkImageMapToColors>::New();
    mapColors_->SetInputConnection(reslice_->GetOutputPort());
    mapColors_->SetLookupTable(lut_);

    imageActor_ = vtkSmartPointer<vtkImageActor>::New();
    // VTK 9 的 vtkImageActor 不再直接暴露 SetInputConnection，经内部 mapper 挂接
    imageActor_->GetMapper()->SetInputConnection(mapColors_->GetOutputPort());
    imageActor_->InterpolateOff();     // 切面按原始分辨率显示，避免平滑模糊

    applyWindowLevel();   // 初始窗宽窗位（文件自带值或 CBCT 骨骼窗回退）
    applyPlane();

    // ---------- 4) 启动专用渲染线程 ----------
    try {
        thread_ = std::thread(&CbctVtkRenderer::renderLoop, this);
    } catch (const std::system_error &e) {
        LOGE("init: render thread create failed: %s", e.what());
        return false;
    }
    LOGD("init: volume %dx%dx%d spacing %.3f/%.3f/%.3f",
         vol->width, vol->height, vol->depth,
         vol->spacingX, vol->spacingY, vol->spacingZ);
    return true;
}

void CbctVtkRenderer::destroy() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (quit_) return;   // 幂等
        quit_ = true;
        cv_.notify_all();
        doneCv_.notify_all();   // 唤醒可能等待同步任务的调用方
    }
    if (thread_.joinable()) thread_.join();
    delete this;
}

// =============================================================================
// 渲染线程：任务队列 + 按需渲染
// =============================================================================

void CbctVtkRenderer::post(const std::function<void()> &task, bool synchronous) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (quit_) return;

    if (!synchronous) {
        tasks_.push_back(task);
        cv_.notify_one();
        return;
    }
    // 同步任务：包装完成标记，等待渲染线程执行完毕（Surface 时序点）
    bool done = false;
    tasks_.push_back([&task, &done, this] {
        task();
        {
            std::lock_guard<std::mutex> g(mutex_);
            done = true;
        }
        doneCv_.notify_all();
    });
    cv_.notify_one();
    doneCv_.wait(lock, [&done, this] { return done || quit_; });
}

void CbctVtkRenderer::renderLoop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        cv_.wait(lock, [this] { return quit_ || !tasks_.empty(); });
        // 排空任务队列（手势等相对增量任务不允许丢弃，仅合并渲染）
        while (!tasks_.empty()) {
            auto task = std::move(tasks_.front());
            tasks_.pop_front();
            lock.unlock();
            try {
                task();
            } catch (const std::exception &e) {
                LOGE("renderLoop: task exception: %s", e.what());
            } catch (...) {
                LOGE("renderLoop: unknown task exception");
            }
            lock.lock();
        }
        if (quit_) break;
        // 按需渲染：队列排空且状态有变化才上屏（闲置 0 GPU 占用）
        if (dirty_ && renderWindow_ && window_) {
            dirty_ = false;
            lock.unlock();
            try {
                renderWindow_->Render();
            } catch (...) {
                LOGE("renderLoop: render exception");
            }
            lock.lock();
        }
    }
    LOGD("renderLoop: exited");
}

void CbctVtkRenderer::markDirty() {
    // 仅渲染线程调用（任务体内部），无需加锁
    dirty_ = true;
}

// =============================================================================
// Surface 生命周期（同步执行：保证与 Android 侧 Surface 状态严格一致）
// =============================================================================

void CbctVtkRenderer::ensureWindow() {
    if (renderWindow_ || !window_) return;
    renderWindow_ = vtkSmartPointer<vtkEGLRenderWindow>::New();
    // Android 原生窗口（JNI 层 ANativeWindow_fromSurface 已持有引用计数）
    renderWindow_->SetWindowId(window_);
    renderWindow_->SetSize(surfW_, surfH_);
    renderWindow_->SetMultiSamples(0);   // ES 端体渲染避免 MSAA 配置兼容问题
    renderWindow_->SetSwapControl(1);    // 垂直同步，避免撕裂

    renderer_ = vtkSmartPointer<vtkRenderer>::New();
    renderer_->SetBackground(0.05, 0.06, 0.09);       // 深色阅片背景
    renderer_->SetBackground2(0.10, 0.12, 0.16);
    renderer_->GradientBackgroundOn();
    renderWindow_->AddRenderer(renderer_);

    applyRenderMode();
}

void CbctVtkRenderer::onSurfaceCreated(ANativeWindow *win, int width, int height) {
    post([this, win, width, height] {
        window_ = win;   // 引用计数由 JNI 层 fromSurface 持有
        surfW_ = width;
        surfH_ = height;
        ensureWindow();
        if (renderWindow_) {
            renderWindow_->SetWindowId(window_);
            renderWindow_->SetSize(surfW_, surfH_);
        }
        if (!cameraReady_) {
            setupCameraForMode();   // 首帧自动复位相机
            cameraReady_ = true;
        }
        markDirty();
    }, true);
}

void CbctVtkRenderer::onSurfaceChanged(int width, int height) {
    post([this, width, height] {
        surfW_ = width;
        surfH_ = height;
        if (renderWindow_) renderWindow_->SetSize(surfW_, surfH_);
        markDirty();
    }, true);
}

void CbctVtkRenderer::onSurfaceDestroyed() {
    post([this] {
        if (renderWindow_) {
            renderWindow_->Finalize();   // 释放 EGL Surface/Context（Surface 即将失效）
        }
        if (window_) {
            ANativeWindow_release(window_);   // 归还 JNI 层 fromSurface 的引用
            window_ = nullptr;
        }
        surfW_ = 0;
        surfH_ = 0;
    }, true);   // 必须同步：回调返回后 Surface 即被 Android 回收
}

// =============================================================================
// 渲染状态（异步任务落地到 VTK 管线）
// =============================================================================

void CbctVtkRenderer::setRenderMode(int mode) {
    post([this, mode] {
        if (mode != MODE_VR && mode != MODE_MPR) return;
        if (mode_ == mode && renderer_) return;
        mode_ = mode;
        if (renderer_) applyRenderMode();
        markDirty();
    }, false);
}

void CbctVtkRenderer::setPlane(int plane, int position) {
    post([this, plane, position] {
        if (plane != PLANE_AXIAL && plane != PLANE_CORONAL && plane != PLANE_SAGITTAL) return;
        plane_ = plane;
        position_ = position;
        applyPlane();
        markDirty();
    }, false);
}

void CbctVtkRenderer::setWindowLevel(double ww, double wc) {
    post([this, ww, wc] {
        ww_ = std::max(1.0, ww);
        wc_ = wc;
        applyWindowLevel();
        markDirty();
    }, false);
}

void CbctVtkRenderer::resetCamera() {
    post([this] {
        setupCameraForMode();
        markDirty();
    }, false);
}

// =============================================================================
// 手势交互（PDF 五 5.2：Java 层手势 -> JNI -> VTK 相机）
// =============================================================================

void CbctVtkRenderer::rotate(double dx, double dy) {
    post([this, dx, dy] {
        applyRotate(dx, dy);
        markDirty();
    }, false);
}

void CbctVtkRenderer::pan(double dx, double dy) {
    post([this, dx, dy] {
        applyPan(dx, dy);
        markDirty();
    }, false);
}

void CbctVtkRenderer::zoom(double factor) {
    post([this, factor] {
        applyZoom(factor);
        markDirty();
    }, false);
}

// =============================================================================
// 管线操作（渲染线程内执行）
// =============================================================================

double CbctVtkRenderer::huToRaw(double hu) const {
    const double slope = (vol_ && vol_->slope != 0.0) ? vol_->slope : 1.0;
    const double intercept = vol_ ? vol_->intercept : 0.0;
    return (hu - intercept) / slope;
}

void CbctVtkRenderer::applyRenderMode() {
    if (!renderer_) return;
    if (mode_ == MODE_VR) {
        renderer_->RemoveViewProp(imageActor_);
        renderer_->AddVolume(volume_);
    } else {
        renderer_->RemoveVolume(volume_);
        renderer_->AddViewProp(imageActor_);
    }
    setupCameraForMode();
}

void CbctVtkRenderer::applyPlane() {
    if (!reslice_ || !vol_) return;
    const int w = vol_->width, h = vol_->height, d = vol_->depth;
    int pos = position_;
    switch (plane_) {
        case PLANE_AXIAL:
            pos = std::min(std::max(pos, 0), d - 1);
            // 切面轴：x=(1,0,0) y=(0,1,0) z=(0,0,1)，穿过点 (0,0,pos*sz)
            reslice_->SetResliceAxesDirectionCosines(1, 0, 0, 0, 1, 0, 0, 0, 1);
            reslice_->SetResliceAxesOrigin(0.0, 0.0, pos * vol_->spacingZ);
            reslice_->SetOutputSpacing(vol_->spacingX, vol_->spacingY, vol_->spacingZ);
            reslice_->SetOutputExtent(0, w - 1, 0, h - 1, 0, 0);
            break;
        case PLANE_CORONAL:
            pos = std::min(std::max(pos, 0), h - 1);
            // 输出 x 轴取输入 x，输出 y 轴取输入 z（横向 x 纵向 z）
            reslice_->SetResliceAxesDirectionCosines(1, 0, 0, 0, 0, 1, 0, 1, 0);
            reslice_->SetResliceAxesOrigin(0.0, pos * vol_->spacingY, 0.0);
            reslice_->SetOutputSpacing(vol_->spacingX, vol_->spacingZ, vol_->spacingY);
            reslice_->SetOutputExtent(0, w - 1, 0, d - 1, 0, 0);
            break;
        case PLANE_SAGITTAL:
        default:
            pos = std::min(std::max(pos, 0), w - 1);
            // 输出 x 轴取输入 y，输出 y 轴取输入 z（横向 y 纵向 z）
            reslice_->SetResliceAxesDirectionCosines(0, 1, 0, 0, 0, 1, 1, 0, 0);
            reslice_->SetResliceAxesOrigin(pos * vol_->spacingX, 0.0, 0.0);
            reslice_->SetOutputSpacing(vol_->spacingY, vol_->spacingZ, vol_->spacingX);
            reslice_->SetOutputExtent(0, h - 1, 0, d - 1, 0, 0);
            break;
    }
    position_ = pos;
}

void CbctVtkRenderer::applyWindowLevel() {
    const double lo = huToRaw(wc_ - ww_ / 2.0);
    const double hi = huToRaw(wc_ + ww_ / 2.0);
    const double mid = 0.5 * (lo + hi);

    // ---- VR：色彩跨度跟随窗宽窗位（PDF 骨骼窗 WW4000/WC600 的传递函数化） ----
    if (colorTF_) {
        colorTF_->RemoveAllPoints();
        // 暖灰阶骨窗：低密度暗色 -> 高密度亮白，贴合 CBCT 骨骼阅片
        colorTF_->AddRGBPoint(lo, 0.02, 0.02, 0.04);
        colorTF_->AddRGBPoint(mid, 0.68, 0.64, 0.58);
        colorTF_->AddRGBPoint(hi, 1.00, 0.99, 0.96);
    }
    if (opacityTF_) {
        // 不透明度：HU<200 全透明（PDF 骨骼阈值过滤软组织），
        // 200->1300 HU 渐升（高密度骨骼渲染权重调高），高位饱和
        opacityTF_->RemoveAllPoints();
        opacityTF_->AddPoint(huToRaw(-1024.0), 0.00);
        opacityTF_->AddPoint(huToRaw(200.0), 0.00);
        opacityTF_->AddPoint(huToRaw(1300.0), 0.85);
        opacityTF_->AddPoint(huToRaw(4000.0), 0.90);
    }
    if (volume_) volume_->Modified();   // 触发 Mapper 重建纹理查找表

    // ---- MPR：灰阶 LUT 区间 = 窗宽窗位（切面实时同步 WW/WC） ----
    if (lut_) {
        lut_->SetRange(lo, hi);
        lut_->Build();
    }
    if (mapColors_) mapColors_->Modified();   // 触发切面重新映射
}

void CbctVtkRenderer::setupCameraForMode() {
    if (!renderer_ || !vol_) return;
    vtkCamera *cam = renderer_->GetActiveCamera();
    if (!cam) return;

    if (mode_ == MODE_VR) {
        // 透视投影 + 斜上方初始视角（默认 45° 俯视，符合三维阅片习惯）
        cam->ParallelProjectionOff();
        const double cx = vol_->width * vol_->spacingX / 2.0;
        const double cy = vol_->height * vol_->spacingY / 2.0;
        const double cz = vol_->depth * vol_->spacingZ / 2.0;
        cam->SetFocalPoint(cx, cy, cz);
        cam->SetPosition(cx + 1.0, cy + 0.6, cz + 1.0);   // 仅定视角方向
        cam->SetViewUp(0.0, 0.0, 1.0);
        renderer_->ResetCamera();   // 按包围盒自动取距
    } else {
        // 平行投影：切面（XY 平面）正交显示，ResetCamera 自适应铺满
        cam->ParallelProjectionOn();
        cam->SetFocalPoint(0.0, 0.0, 0.0);
        cam->SetPosition(0.0, 0.0, 1.0);
        cam->SetViewUp(0.0, 1.0, 0.0);
        renderer_->ResetCamera();
    }
    initDist_ = cam->GetDistance();
}

void CbctVtkRenderer::applyRotate(double dx, double dy) {
    if (!renderer_) return;
    if (mode_ != MODE_VR) {
        applyPan(dx, dy);   // MPR 模式单指滑动即平移切面
        return;
    }
    vtkCamera *cam = renderer_->GetActiveCamera();
    if (!cam) return;
    // trackball 语义：约 180°/视口高的灵敏度
    const double k = 180.0 / (double) (surfH_ > 0 ? surfH_ : 720);
    cam->Azimuth(-dx * k);
    cam->Elevation(dy * k);
    cam->OrthogonalizeViewUp();
}

void CbctVtkRenderer::applyPan(double dx, double dy) {
    if (!renderer_) return;
    vtkCamera *cam = renderer_->GetActiveCamera();
    if (!cam) return;

    // 世界坐标/像素比：平行投影用平行缩放，透视用视场角换算
    const double viewH = (double) (surfH_ > 0 ? surfH_ : 720);
    double scale;
    if (cam->GetParallelProjection()) {
        scale = 2.0 * cam->GetParallelScale() / viewH;
    } else {
        const double angle = cam->GetViewAngle() * vtkMath::Pi() / 180.0;
        scale = 2.0 * cam->GetDistance() * std::tan(angle / 2.0) / viewH;
    }

    // 视平面基向量：right = dir x up（右手系）
    double dop[3], vu[3];
    cam->GetDirectionOfProjection(dop);
    cam->GetViewUp(vu);
    double right[3];
    vtkMath::Cross(dop, vu, right);

    // 内容跟随手指：拖右->场景右移（相机左移），拖下->场景下移（相机上移）
    const double mx = -dx * scale;
    const double my = dy * scale;
    double pos[3], fp[3];
    cam->GetPosition(pos);
    cam->GetFocalPoint(fp);
    for (int i = 0; i < 3; ++i) {
        const double delta = right[i] * mx + vu[i] * my;
        pos[i] += delta;
        fp[i] += delta;
    }
    cam->SetPosition(pos);
    cam->SetFocalPoint(fp);
}

void CbctVtkRenderer::applyZoom(double factor) {
    if (!renderer_ || factor <= 0.0) return;
    vtkCamera *cam = renderer_->GetActiveCamera();
    if (!cam) return;
    if (cam->GetParallelProjection()) {
        // 平行投影：捏合放大 -> 平行缩放减小（视野变窄）
        cam->SetParallelScale(cam->GetParallelScale() / factor);
    } else {
        // 透视投影：捏合放大 -> 相机沿视线前移，距离钳制 [0.1x, 10x] 初始距离
        double newDist = cam->GetDistance() / factor;
        if (initDist_ > 0.0) {
            newDist = std::max(initDist_ * 0.1, std::min(initDist_ * 10.0, newDist));
        }
        cam->SetDistance(newDist);
    }
}
