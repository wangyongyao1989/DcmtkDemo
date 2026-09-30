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
//       -> 骨骼阈值（200HU）以不透明度传递函数起点实现软组织过滤；
//   - GLES3 3D 纹理无归一化 16bit 整数格式（R16/R16_SNORM 均为桌面 GL 专属），
//     vtkVolumeTexture 对 Uint16 输入在 ES 端解析出空 internalFormat，
//     体绘制静默黑屏
//       -> 解析侧已把体素换算为 float HU 存储（见 CbctSeriesParser），本层以
//          vtkFloatArray 零拷贝包装（对应 GL_R32F，ES3 核心格式），传递函数/
//          窗宽窗位全程直接工作在 HU 域；R32F 线性过滤依赖设备是否支持
//          GL_OES_texture_float_linear，ensureWindow() 检测后自动降级 Nearest。

#include "include/CbctVtkRenderer.h"

#include <android/log.h>
#include <android/native_window.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

// VTK 静态库对象工厂注册（静态构建必须显式初始化）：
//   RenderingOpenGL2      -> vtkEGLRenderWindow（Android Surface 渲染窗口）
//                            + vtkOpenGLImageSliceMapper（MPR 切面显示）
//   RenderingVolumeOpenGL2 -> vtkSmartVolumeMapper（体绘制 Mapper 工厂）
#include "vtkAutoInit.h"
VTK_MODULE_INIT(vtkRenderingOpenGL2)
VTK_MODULE_INIT(vtkRenderingVolumeOpenGL2)

#include "vtkCamera.h"
#include "vtkColorTransferFunction.h"
#include "vtkCoordinate.h"
#include "vtkFloatArray.h"
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
#include "vtkSmartVolumeMapper.h"
#include "vtkVolume.h"
#include "vtkVolumeProperty.h"
#include "vtkDataArray.h"
#include "vtkWindowToImageFilter.h"
#include "vtkEGLRenderWindow.h"
#include "vtkAndroidOutputWindow.h"
#include "vtkOutputWindow.h"
// 诊断辅助
#include "vtkActor.h"
#include "vtkCubeSource.h"
#include "vtkPolyDataMapper.h"
#include "vtkProperty.h"
#include "vtkOpenGLFramebufferObject.h"
#include "vtkOpenGLState.h"

// GL 扩展查询（OES_texture_float_linear 检测，见 ensureWindow）
#include "vtk_glew.h"

#define TAG "CbctVtk"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// 真机排障用的 GL/FBO 自检块（renderLoop 内 诊断 1/1b/1c/1d/2/3/4）。
// 它会编译一个 mini shader、重放十余次 FBO 绑定、并整屏 glReadPixels，
// 首帧和每次模式切换各触发一次（实测单帧 ~290 ms）。排障时改为 1 重新编译。
#ifndef CBCT_VTK_DIAG
#define CBCT_VTK_DIAG 0
#endif

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
    // 数据为 float HU（解析侧已完成 Rescale 换算，规避 ES3 无归一化 16bit
    // 体纹理格式的问题），对应 GL_R32F —— ES3 核心保证可用。
    vtkNew<vtkImageData> img;
    img->SetDimensions(vol->width, vol->height, vol->depth);
    img->SetSpacing(vol->spacingX, vol->spacingY, vol->spacingZ);
    img->SetOrigin(0.0, 0.0, 0.0);   // index 域与世界域对齐，简化 MPR 轴矩阵
    const vtkIdType n = (vtkIdType) vol->width * vol->height * vol->depth;
    vtkNew<vtkFloatArray> arr;
    arr->SetArray(vol->data, n, 1);
    img->GetPointData()->SetScalars(arr);
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
    imageActor_->ForceOpaqueOn();      // 强制 Opaque 队列，规避 Mali 驱动对透明队列 FBO 的兼容性问题

    // 诊断：体中心红色立方体（不透明、无纹理、无深度无关特效）。
    // 两种模式均挂载——用于二分定位：立方体可见 = 世界空间渲染正常，
    // 问题在纹理类 Prop（体纹理/切面纹理）；不可见 = 相机/裁剪/深度故障。
    {
        const double cx = vol->width * vol->spacingX / 2.0;
        const double cy = vol->height * vol->spacingY / 2.0;
        const double cz = vol->depth * vol->spacingZ / 2.0;
        const double edge = std::min(std::min(vol->width * vol->spacingX,
                                              vol->height * vol->spacingY),
                                     vol->depth * vol->spacingZ) / 6.0;
        vtkNew<vtkCubeSource> cube;
        cube->SetXLength(edge);
        cube->SetYLength(edge);
        cube->SetZLength(edge);
        cube->SetCenter(cx, cy, cz);
        vtkNew<vtkPolyDataMapper> cubeMapper;
        cubeMapper->SetInputConnection(cube->GetOutputPort());
        diagActor_ = vtkSmartPointer<vtkActor>::New();
        diagActor_->SetMapper(cubeMapper);
        diagActor_->GetProperty()->SetColor(1.0, 0.1, 0.1);
    }

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
            // 预清 GL 错误标志（Release 构建 VTK 错误宏为 no-op，错误会滞留队列，
            // 必须手动排空才能将本帧错误与历史错误隔离）
            while (glGetError() != GL_NO_ERROR) {}
            // 状态缓存强制同步：若有裸 glBindFramebuffer 绕过 vtkOpenGLState
            // 缓存（缓存值 ≠ 真实绑定），后续 Start() 里 RenderFramebuffer 的
            // Bind 会被缓存跳过（缓存认为已绑定），场景渲染进错误 FBO。
            // 将真实绑定值经 vtkglBindFramebuffer 重新过一遍缓存即可同步：
            // 缓存不一致时触发真实 Bind 并更新缓存，一致时为 no-op。
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
#if CBCT_VTK_DIAG
            const auto t0 = std::chrono::steady_clock::now();
#endif
            try {
                renderWindow_->Render();
            } catch (...) {
                LOGE("renderLoop: render exception");
            }
#if CBCT_VTK_DIAG
            if (frameCount_ == 0 || diagPending_) {
                diagPending_ = false;
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
                double range[2] = {0, 0};
                imageData_->GetScalarRange(range);
                LOGD("diag: frame #%d in %lld ms, scalarType=%s range=[%.0f, %.0f], VR mode=%d",
                     frameCount_, (long long) ms, imageData_->GetScalarTypeAsString(),
                     range[0], range[1], (int) volMapper_->GetLastUsedRenderMode());

                // ---- 诊断 1：Render() 后立即排空 GL 错误 ----
                // 必须在任何 VTK 回读之前（ReadPixels 内部会清空错误队列吞掉证据）
                {
                    int nErr = 0;
                    GLenum e;
                    while ((e = glGetError()) != GL_NO_ERROR && nErr < 8) {
                        LOGW("diag: post-render GL error #%d = 0x%04x "
                             "(0x500=INVALID_ENUM 0x501=INVALID_VALUE 0x502=INVALID_OP "
                             "0x505=OUT_OF_MEM 0x506=INVALID_FB_OP)",
                             nErr, (unsigned) e);
                        nErr++;
                    }
                    if (nErr == 0) LOGD("diag: no GL errors after Render");
                }

                // ---- 诊断 1b：程序验证 + FBO 附件与纹理单元绑定关系 ----
                // 目标：定位 MPR 模式 glDrawElements 0x0502（INVALID_OPERATION）。
                // glValidateProgram 会按当前 GL 状态检查采样器/纹理/FBO 反馈环，
                // ES 上是绘制失败的标准诊断入口。
                {
                    GLint prog = 0, drawFbo = 0;
                    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
                    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
                    LOGD("diag1b: prog=%d drawFbo=%d", prog, drawFbo);
                    if (prog) {
                        glValidateProgram((GLuint) prog);
                        GLint vs = 0;
                        glGetProgramiv((GLuint) prog, GL_VALIDATE_STATUS, &vs);
                        char vlog[512] = {0};
                        glGetProgramInfoLog((GLuint) prog, 511, nullptr, vlog);
                        LOGD("diag1b: validate=%d log=%.200s", vs, vlog);
                        // 枚举全部 active uniform（PDM 诊断的采样器过滤表可能漏类型）
                        GLint nU = 0;
                        glGetProgramiv((GLuint) prog, GL_ACTIVE_UNIFORMS, &nU);
                        for (int i = 0; i < nU; i++) {
                            char name[256] = {0};
                            GLsizei len = 0; GLint sz = 0; GLenum ty = 0;
                            glGetActiveUniform((GLuint) prog, (GLuint) i, 255,
                                               &len, &sz, &ty, name);
                            if (ty == GL_SAMPLER_2D || ty == GL_SAMPLER_3D ||
                                ty == GL_SAMPLER_CUBE || ty == GL_SAMPLER_2D_ARRAY ||
                                ty == GL_SAMPLER_2D_SHADOW ||
                                ty == GL_INT_SAMPLER_2D ||
                                ty == GL_UNSIGNED_INT_SAMPLER_2D) {
                                GLint loc = glGetUniformLocation((GLuint) prog, name);
                                GLint unit = -1;
                                if (loc >= 0)
                                    glGetUniformiv((GLuint) prog, loc, &unit);
                                LOGD("diag1b: uniform '%s' type=0x%04x unit=%d",
                                     name, (unsigned) ty, unit);
                            }
                        }
                    }
                    if (drawFbo) {
                        const GLenum atts[] = {GL_COLOR_ATTACHMENT0, GL_DEPTH_ATTACHMENT};
                        const char *names[] = {"color0", "depth"};
                        for (int a = 0; a < 2; a++) {
                            GLint otype = 0, oname = 0;
                            glGetFramebufferAttachmentParameteriv(
                                GL_DRAW_FRAMEBUFFER, atts[a],
                                GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &otype);
                            glGetFramebufferAttachmentParameteriv(
                                GL_DRAW_FRAMEBUFFER, atts[a],
                                GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &oname);
                            LOGD("diag1b: FBO%d %s type=0x%04x obj=%d",
                                 drawFbo, names[a], (unsigned) otype, oname);
                        }
                    }
                    // 各单元 2D 纹理绑定（与 FBO 附件对比查反馈环）
                    for (int u = 0; u < 8; u++) {
                        glActiveTexture((GLenum) (GL_TEXTURE0 + u));
                        GLint b2d = 0, b3d = 0, bArr = 0, bSampler = 0;
                        glGetIntegerv(GL_TEXTURE_BINDING_2D, &b2d);
                        glGetIntegerv(GL_TEXTURE_BINDING_3D, &b3d);
                        glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &bArr);
                        glGetIntegerv(GL_SAMPLER_BINDING, &bSampler);
                        if (b2d || b3d || bArr || bSampler)
                            LOGD("diag1b: unit%d tex2D=%d tex3D=%d tex2DArr=%d samplerObj=%d",
                                 u, b2d, b3d, bArr, bSampler);
                    }
                    // 恢复默认活动单元，避免污染 VTK 状态缓存
                    glActiveTexture(GL_TEXTURE0);
                    while (glGetError() != GL_NO_ERROR) {}
                }

                // ---- 诊断 1d：VTK FBO 身份 ----
                // PDM-diag 显示失败绘制时 drawFbo=4；对照 VTK 侧
                // RenderFramebuffer / DisplayFramebuffer 的真实索引即可确定
                // FBO4 身份（若都不是 4，则 FBO4 是体渲染 Mapper 的
                // DepthCopyFBO，说明绘制绑定点漂移到了错误 FBO）。
                {
                    auto *rfbo = renderWindow_ ? renderWindow_->GetRenderFramebuffer() : nullptr;
                    auto *dfbo = renderWindow_ ? renderWindow_->GetDisplayFramebuffer() : nullptr;
                    GLint realDraw = 0, realRead = 0;
                    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &realDraw);
                    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &realRead);
                    LOGD("diag1d: RenderFB=%u DisplayFB=%u realDraw=%d realRead=%d",
                         rfbo ? rfbo->GetFBOIndex() : 0,
                         dfbo ? dfbo->GetFBOIndex() : 0,
                         realDraw, realRead);
                }

                // ---- 诊断 1c：FBO 附件全量枚举 + 失败绘制状态重放 ----
                // PDM-diag 给出失败绘制时的状态：prog=12 vao=3 drawFbo=4 unit0=tex2D=8。
                // 此处先枚举各 FBO 的颜色/深度附件（查反馈环：被采样纹理是否
                // 同时挂在当前绘制 FBO 上），再绑定相同状态重放 glDrawArrays，
                // 用 glValidateProgram 拿到 ES 驱动的一手诊断信息。
                {
                    for (GLuint f = 1; f <= 8; f++) {
                        if (!glIsFramebuffer(f)) continue;
                        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, f);
                        GLint cType = 0, cObj = 0, dType = 0, dObj = 0;
                        glGetFramebufferAttachmentParameteriv(
                            GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                            GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &cType);
                        glGetFramebufferAttachmentParameteriv(
                            GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                            GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &cObj);
                        glGetFramebufferAttachmentParameteriv(
                            GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                            GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &dType);
                        glGetFramebufferAttachmentParameteriv(
                            GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                            GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &dObj);
                        LOGD("diag1c: FBO%d color(type=0x%04x obj=%d) depth(type=0x%04x obj=%d)",
                             f, (unsigned) cType, cObj, (unsigned) dType, dObj);
                        while (glGetError() != GL_NO_ERROR) {}
                    }
                    // 失败绘制重放（id 来自 PDM-diag 日志，glIsXxx 保证安全）
                    if (glIsProgram(12) && glIsVertexArray(3) && glIsFramebuffer(4)) {
                        GLint svProg = 0, svVao = 0, svFbo = 0;
                        glGetIntegerv(GL_CURRENT_PROGRAM, &svProg);
                        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &svVao);
                        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &svFbo);
                        glUseProgram(12);
                        glBindVertexArray(3);
                        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 4);
                        glValidateProgram(12);
                        GLint vs = 0;
                        glGetProgramiv(12, GL_VALIDATE_STATUS, &vs);
                        char vlog[512] = {0};
                        glGetProgramInfoLog(12, 511, nullptr, vlog);
                        LOGD("diag1c: replay validate(12)=%d log=%.300s", vs, vlog);
                        while (glGetError() != GL_NO_ERROR) {}
                        // 枚举 program 12 全部 active uniform（不限采样器）
                        GLint nU = 0;
                        glGetProgramiv(12, GL_ACTIVE_UNIFORMS, &nU);
                        LOGD("diag1c: prog12 activeUniforms=%d", nU);
                        for (int i = 0; i < nU; i++) {
                            char name[256] = {0};
                            GLsizei len = 0; GLint sz = 0; GLenum ty = 0;
                            glGetActiveUniform(12, (GLuint) i, 255, &len, &sz, &ty, name);
                            LOGD("diag1c: prog12 u[%d] '%s' type=0x%04x size=%d",
                                 i, name, (unsigned) ty, sz);
                        }
                        // uniform block（未绑定 buffer 的活跃块在 ES 上会导致绘制
                        // INVALID_OPERATION——WebGL2 明确规定，Mali 同样实现）
                        GLint nBlk = 0;
                        glGetProgramiv(12, GL_ACTIVE_UNIFORM_BLOCKS, &nBlk);
                        LOGD("diag1c: prog12 activeUniformBlocks=%d", nBlk);
                        for (int i = 0; i < nBlk; i++) {
                            char name[256] = {0};
                            GLsizei len = 0;
                            glGetActiveUniformBlockName(12, (GLuint) i, 255, &len, name);
                            LOGD("diag1c: prog12 ubo[%d] '%s'", i, name);
                        }
                        // 变体 A：原样重放
                        glDrawArrays(GL_TRIANGLES, 0, 3);
                        LOGD("diag1c: replayA err=0x%04x", (unsigned) glGetError());
                        // 变体 B：解除单元 0 的 2D 纹理绑定再画
                        glActiveTexture(GL_TEXTURE0);
                        glBindTexture(GL_TEXTURE_2D, 0);
                        glDrawArrays(GL_TRIANGLES, 0, 3);
                        LOGD("diag1c: replayB(no tex on unit0) err=0x%04x",
                             (unsigned) glGetError());
                        // 变体 C：换绑其他纹理（FBO2 的 color=3）
                        glBindTexture(GL_TEXTURE_2D, 3);
                        glDrawArrays(GL_TRIANGLES, 0, 3);
                        LOGD("diag1c: replayC(tex3 on unit0) err=0x%04x",
                             (unsigned) glGetError());
                        // 变体 D/E/F：交叉替换 VAO / 程序 / FBO 定位坏状态
                        {
                            // 极简程序：只有 pos 属性，无采样器无矩阵
                            const char *vss = "#version 300 es\n"
                                              "in vec3 pos;\n"
                                              "void main(){ gl_Position = vec4(pos,1.0); }";
                            const char *fss = "#version 300 es\n"
                                              "precision mediump float;\n"
                                              "out vec4 o;\n"
                                              "void main(){ o = vec4(1.0,0.0,0.0,1.0); }";
                            GLuint vs = glCreateShader(GL_VERTEX_SHADER);
                            glShaderSource(vs, 1, &vss, nullptr);
                            glCompileShader(vs);
                            GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
                            glShaderSource(fs, 1, &fss, nullptr);
                            glCompileShader(fs);
                            GLuint mp = glCreateProgram();
                            glAttachShader(mp, vs);
                            glAttachShader(mp, fs);
                            glLinkProgram(mp);
                            GLint ok = 0;
                            glGetProgramiv(mp, GL_LINK_STATUS, &ok);
                            if (!ok) { LOGD("diag1c: mini prog link FAILED"); }
                            // 极简 VAO：3 顶点三角形
                            GLuint mvao = 0, mvbo = 0;
                            glGenVertexArrays(1, &mvao);
                            glGenBuffers(1, &mvbo);
                            const float tri[9] = {-0.5f, -0.5f, 0.f, 0.5f, -0.5f, 0.f, 0.f, 0.5f, 0.f};
                            glBindVertexArray(mvao);
                            glBindBuffer(GL_ARRAY_BUFFER, mvbo);
                            glBufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);
                            glEnableVertexAttribArray(0);
                            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
                            while (glGetError() != GL_NO_ERROR) {}
                            if (ok) {
                                // D：prog12 + 极简 VAO（定位 VAO3）
                                glUseProgram(12);
                                glBindVertexArray(mvao);
                                glDrawArrays(GL_TRIANGLES, 0, 3);
                                LOGD("diag1c: replayD(prog12+miniVAO) err=0x%04x",
                                     (unsigned) glGetError());
                                // E：极简程序 + VAO3（定位 prog12）
                                glUseProgram(mp);
                                glBindVertexArray(3);
                                glDrawArrays(GL_TRIANGLES, 0, 3);
                                LOGD("diag1c: replayE(miniProg+vao3) err=0x%04x",
                                     (unsigned) glGetError());
                                // F：极简程序 + 极简 VAO + FBO4（定位 FBO4）
                                glUseProgram(mp);
                                glBindVertexArray(mvao);
                                glDrawArrays(GL_TRIANGLES, 0, 3);
                                LOGD("diag1c: replayF(miniProg+miniVAO+fbo4) err=0x%04x",
                                     (unsigned) glGetError());
                                // FBO4 附件格式检查：COMPONENT_TYPE 直接给出
                                // FLOAT/INT/UNSIGNED_INT/UNSIGNORM（ES3 核心查询）。
                                // 嫌疑：整数颜色附件 + blend 开启 = 绘制期
                                // INVALID_OPERATION（ES3 明确规则；VR 模式体渲染
                                // 关闭 blend 故不受影响）。
                                {
                                    GLint compType = 0, enc = 0, layered = 0;
                                    glGetFramebufferAttachmentParameteriv(
                                        GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                        GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &compType);
                                    glGetFramebufferAttachmentParameteriv(
                                        GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                        GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING, &enc);
                                    LOGD("diag1c: FBO4 color0 compType=0x%04x "
                                         "(0x1406=FLOAT 0x1404=INT 0x1405=UINT "
                                         "0x8F9C=UNSIGNORM) encoding=0x%04x",
                                         (unsigned) compType, (unsigned) enc);
                                    while (glGetError() != GL_NO_ERROR) {}
                                    // 深度附件同为纹理：查其过滤状态（ES3 深度纹理
                                    // LINEAR 不可过滤 → 不完整 → 采样恒 0）
                                    GLint dObj = 0;
                                    glGetFramebufferAttachmentParameteriv(
                                        GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &dObj);
                                    if (dObj) {
                                        glActiveTexture(GL_TEXTURE0);
                                        glBindTexture(GL_TEXTURE_2D, (GLuint) dObj);
                                        GLint dMin = 0, dMag = 0;
                                        glGetTexParameteriv(GL_TEXTURE_2D,
                                            GL_TEXTURE_MIN_FILTER, &dMin);
                                        glGetTexParameteriv(GL_TEXTURE_2D,
                                            GL_TEXTURE_MAG_FILTER, &dMag);
                                        LOGD("diag1c: FBO4 depthTex=%d minF=0x%04x "
                                             "magF=0x%04x (0x2600=LINEAR 不可过滤)",
                                             dObj, (unsigned) dMin, (unsigned) dMag);
                                        glBindTexture(GL_TEXTURE_2D, 0);
                                        while (glGetError() != GL_NO_ERROR) {}
                                    }
                                }
                                // H：FBO4 + blend 关闭（整数附件嫌疑对照实验）
                                {
                                    GLboolean svBlend = glIsEnabled(GL_BLEND);
                                    glDisable(GL_BLEND);
                                    glDrawArrays(GL_TRIANGLES, 0, 3);
                                    LOGD("diag1c: replayH(fbo4+blendOFF) err=0x%04x",
                                         (unsigned) glGetError());
                                    // I：FBO4 + blend 开启（复现基线）
                                    glEnable(GL_BLEND);
                                    glDrawArrays(GL_TRIANGLES, 0, 3);
                                    LOGD("diag1c: replayI(fbo4+blendON) err=0x%04x",
                                         (unsigned) glGetError());
                                    // J：默认帧缓冲 + blend 开启（对照组）
                                    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
                                    glDrawArrays(GL_TRIANGLES, 0, 3);
                                    LOGD("diag1c: replayJ(defaultFB+blendON) err=0x%04x",
                                         (unsigned) glGetError());
                                    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 4);
                                    if (svBlend) glEnable(GL_BLEND);
                                    else glDisable(GL_BLEND);
                                    while (glGetError() != GL_NO_ERROR) {}
                                }
                                // G：极简程序 + 极简 VAO + 默认帧缓冲
                                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
                                glDrawArrays(GL_TRIANGLES, 0, 3);
                                LOGD("diag1c: replayG(mini+defaultFB) err=0x%04x",
                                     (unsigned) glGetError());
                                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 4);
                            }
                            while (glGetError() != GL_NO_ERROR) {}
                            glDeleteProgram(mp);
                            glDeleteShader(vs);
                            glDeleteShader(fs);
                            glDeleteVertexArrays(1, &mvao);
                            glDeleteBuffers(1, &mvbo);
                        }
                        LOGD("diag1c: rasterizerDiscard=%d",
                             (int) glIsEnabled(GL_RASTERIZER_DISCARD));
                        while (glGetError() != GL_NO_ERROR) {}
                        glUseProgram((GLuint) svProg);
                        glBindVertexArray((GLuint) svVao);
                        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint) svFbo);
                    } else {
                        LOGD("diag1c: prog12/vao3/fbo4 not all valid, skip replay");
                    }
                    while (glGetError() != GL_NO_ERROR) {}
                }

                // ---- 诊断 2：FBO 完整性 + 关键 GL 状态 ----
                if (renderWindow_) {
                    auto *rfbo = renderWindow_->GetRenderFramebuffer();
                    auto *dfbo = renderWindow_->GetDisplayFramebuffer();
                    if (rfbo) {
                        rfbo->Bind(GL_FRAMEBUFFER);
                        LOGD("diag: RenderFBO  status=0x%04x (0x8CD5=complete)",
                             (unsigned) glCheckFramebufferStatus(GL_FRAMEBUFFER));
                    }
                    if (dfbo) {
                        dfbo->Bind(GL_FRAMEBUFFER);
                        LOGD("diag: DisplayFBO status=0x%04x (0x8CD5=complete)",
                             (unsigned) glCheckFramebufferStatus(GL_FRAMEBUFFER));
                    }
                    int vp[4] = {0, 0, 0, 0};
                    glGetIntegerv(GL_VIEWPORT, vp);
                    int scissor = glIsEnabled(GL_SCISSOR_TEST);
                    int sb[4] = {0, 0, 0, 0};
                    if (scissor) glGetIntegerv(GL_SCISSOR_BOX, sb);
                    LOGD("diag: viewport=[%d,%d,%d,%d] scissor=%d box=[%d,%d,%d,%d] "
                         "depthTest=%d blend=%d",
                         vp[0], vp[1], vp[2], vp[3], scissor, sb[0], sb[1], sb[2], sb[3],
                         (int) glIsEnabled(GL_DEPTH_TEST), (int) glIsEnabled(GL_BLEND));
                }

                // ---- 诊断 3：相机状态（裁剪面是黑屏高发点） ----
                if (renderer_) {
                    vtkCamera *c = renderer_->GetActiveCamera();
                    if (c) {
                        double pos[3], fp[3], cr[2];
                        c->GetPosition(pos);
                        c->GetFocalPoint(fp);
                        c->GetClippingRange(cr);
                        LOGD("diag: cam pos=(%.1f,%.1f,%.1f) fp=(%.1f,%.1f,%.1f) "
                             "dist=%.1f clip=[%.2f, %.2f] parallel=%d pScale=%.1f",
                             pos[0], pos[1], pos[2], fp[0], fp[1], fp[2],
                             c->GetDistance(), cr[0], cr[1],
                             (int) c->GetParallelProjection(), c->GetParallelScale());
                    }
                }

                // ---- 诊断 4：DisplayFBO 内容回读 ----
                // GL_RGBA/UNSIGNED_BYTE 是 ES3 对 RGBA8 附件唯一保证的回读组合
                // （GetPixelData 用的 GL_RGB 组合在 ES3 不保证，可能静默失败——
                //  之前 avgR=0 maxR=0 的结论不可信即因如此）。
                // DisplayFBO 在 Frame() 合成链末端，持有「即将上屏」的最终图像。
                if (renderWindow_ && surfW_ > 0 && surfH_ > 0) {
                    auto *dfbo = renderWindow_->GetDisplayFramebuffer();
                    if (dfbo) {
                        dfbo->Bind(GL_READ_FRAMEBUFFER);
                        dfbo->ActivateReadBuffer(0);
                        std::vector<unsigned char> buf((size_t) surfW_ * surfH_ * 4);
                        glReadPixels(0, 0, surfW_, surfH_,
                                     GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
                        const GLenum rErr = glGetError();
                        unsigned maxV = 0;
                        unsigned long long sum = 0;
                        size_t cnt = 0;
                        unsigned maxA = 0;
                        for (size_t i = 0; i + 3 < buf.size(); i += 4) {
                            sum += buf[i];
                            if (buf[i] > maxV) maxV = buf[i];
                            if (buf[i + 3] > maxA) maxA = buf[i + 3];
                            cnt++;
                        }
                        LOGD("diag: DisplayFBO readback err=0x%04x avgR=%llu maxR=%u "
                             "maxA=%u (maxR≈41 为纯背景, 红色立方体应使 maxR≥200)",
                             (unsigned) rErr, sum / (cnt ? cnt : 1), maxV, maxA);
                    }
                }
            }
#endif
            frameCount_++;
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
    renderer_->GradientBackgroundOff();               // 关键修复：关闭渐变背景，规避 Mali 等 GPU 触发 Float FBO 导致的 Blend/Blit 兼容性黑屏
    renderWindow_->AddRenderer(renderer_);

    // Initialize() 创建 EGL Context/Surface 并 MakeCurrent（渲染线程），
    // 之后即可查询 GL 扩展。体数据为 R32F float 纹理，线性过滤是否可用
    // 取决于设备是否支持 GL_OES_texture_float_linear，不支持则降级 Nearest，
    // 否则 LINEAR 过滤不完整纹理会导致采样全黑。
    renderWindow_->Initialize();
    const char *exts = (const char *) glGetString(GL_EXTENSIONS);
    const char *renderer = (const char *) glGetString(GL_RENDERER);
    const char *glVer = (const char *) glGetString(GL_VERSION);
    const char *glslVer = (const char *) glGetString(GL_SHADING_LANGUAGE_VERSION);
    LOGD("ensureWindow: GL_VERSION=%s | GLSL=%s", glVer ? glVer : "?", glslVer ? glslVer : "?");
    {
        // 诊断：默认帧缓冲深度/模板精度（ES blit 深度要求格式一致；
        // D32 纹理在 ES 不可过滤，采样返回 0 曾导致 GPU 体绘制全黑）
        GLint db = 0, sb = 0;
        glGetIntegerv(GL_DEPTH_BITS, &db);
        glGetIntegerv(GL_STENCIL_BITS, &sb);
        LOGD("ensureWindow: default FB depthBits=%d stencilBits=%d", db, sb);
    }
    if (exts && strstr(exts, "GL_OES_texture_float_linear")) {
        LOGD("ensureWindow: %s | OES_texture_float_linear: yes -> Linear", renderer);
    } else {
        LOGW("ensureWindow: %s | OES_texture_float_linear: NO -> Nearest", renderer);
        if (volProperty_) volProperty_->SetInterpolationTypeToNearest();
    }

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
        // 移除 mode_ == mode 拦截，允许 UI 层通过重复调用强制同步状态
        mode_ = mode;
        if (renderer_) {
            applyRenderMode();
            setupCameraForMode();
        }
        markDirty();
    }, false);
}

void CbctVtkRenderer::setPlane(int plane, int position) {
    post([this, plane, position] {
        if (plane != PLANE_AXIAL && plane != PLANE_CORONAL && plane != PLANE_SAGITTAL) return;
        bool planeChanged = (plane_ != plane);
        plane_ = plane;
        position_ = position;
        applyPlane();
        // 如果平面发生变化，重新自适应相机（不同维度的切面 bounds 不同）
        if (planeChanged && mode_ == MODE_MPR && renderer_) {
            setupCameraForMode();
        }
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
        // 这里原本还顺带循环切换 4 组真机排障假设（diagMode_）：用户点一次「复位相机」
        // 就可能把体绘制切成全不透明红色、CPU RayCast 或改掉插值方式，且界面上毫无提示。
        // 排障假设只应在 init 阶段按设备能力确定，复位按钮只做复位相机这一件事。
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

void CbctVtkRenderer::applyRenderMode() {
    if (!renderer_) return;
    if (mode_ == MODE_VR) {
        renderer_->RemoveViewProp(imageActor_);
        renderer_->AddVolume(volume_);
    } else {
        renderer_->RemoveVolume(volume_);
        renderer_->AddViewProp(imageActor_);
    }
    // 诊断立方体不再挂载：世界空间渲染已验证正常，避免污染 DisplayFBO 回读评估
    diagPending_ = true;                               // 模式切换后下一帧输出自检
    // 诊断：确认 Prop 挂载与可见性（排查黑屏用）
    {
        double bounds[6] = {0, 0, 0, 0, 0, 0};
        if (mode_ == MODE_VR) volume_->GetBounds(bounds);
        else imageActor_->GetBounds(bounds);
        LOGD("applyRenderMode: mode=%d volCnt=%d propCnt=%d bounds=[%.1f %.1f][%.1f %.1f][%.1f %.1f]",
             (int) mode_, renderer_->GetVolumes()->GetNumberOfItems(),
             renderer_->GetViewProps()->GetNumberOfItems(),
             bounds[0], bounds[1], bounds[2], bounds[3], bounds[4], bounds[5]);
    }
    setupCameraForMode();
    applyVisibility();   // 重放叠加层可见性（Prop 刚被重新挂载）
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

    // 诊断：检查 MPR 切面管线输出
    if (mapColors_) {
        mapColors_->Update();
        vtkImageData *out = mapColors_->GetOutput();
        if (out && out->GetPointData()->GetScalars()) {
            double rng[2] = {0, 0};
            out->GetPointData()->GetScalars()->GetRange(rng, 0);
            int *ext = out->GetExtent();
            LOGD("applyPlane: MPR out ext=[%d,%d,%d,%d] type=%s range=[%.0f,%.0f]",
                 ext[0], ext[1], ext[2], ext[3],
                 out->GetScalarTypeAsString(), rng[0], rng[1]);
        } else {
            LOGW("applyPlane: MPR out has NO scalars");
        }
    }
}

void CbctVtkRenderer::applyWindowLevel() {
    // 体数据已是 HU 域（float），窗宽窗位/传递函数全程直接使用 HU
    const double lo = wc_ - ww_ / 2.0;
    const double hi = wc_ + ww_ / 2.0;
    const double mid = 0.5 * (lo + hi);

    // ---- VR：色彩跨度跟随窗宽窗位（PDF 骨骼窗 WW4000/WC600 的传递函数化） ----
    if (colorTF_) {
        colorTF_->RemoveAllPoints();
        // 暖灰阶骨窗：低密度暗色 -> 高密度亮白，贴合 CBCT 骨骼阅片
        colorTF_->AddRGBPoint(lo, 0.02, 0.02, 0.04);
        colorTF_->AddRGBPoint(mid, 0.68, 0.64, 0.58);
        colorTF_->AddRGBPoint(hi, 1.00, 0.99, 0.96);
    }
    if (opacityTF_) applyOpacity();
    if (volume_) volume_->Modified();   // 触发 Mapper 重建纹理查找表

    // ---- MPR：灰阶 LUT 区间 = 窗宽窗位（切面实时同步 WW/WC） ----
    if (lut_) {
        lut_->SetRange(lo, hi);
        lut_->Build();
    }
    if (mapColors_) mapColors_->Modified();   // 触发切面重新映射
}

void CbctVtkRenderer::applyOpacity() {
    // 不透明度曲线二选一（均在渲染线程内调用）：
    //  - 默认骨窗曲线：HU<200 全透明（PDF 骨骼阈值过滤软组织），200->1300 渐升；
    //  - 分割区间曲线：仅 [segMin_, segMax_] 内不透明，两侧按 feather 渐变。
    // 未调用 setSegmentHuRange() 时走默认分支，与模块原有行为逐点一致。
    if (!opacityTF_) return;
    opacityTF_->RemoveAllPoints();
    if (!segActive_) {
        opacityTF_->AddPoint(-1024.0, 0.00);
        opacityTF_->AddPoint(200.0, 0.00);
        opacityTF_->AddPoint(1300.0, 0.85);
        opacityTF_->AddPoint(4000.0, 0.90);
        return;
    }
    const double f = segFeather_ > 0.0 ? segFeather_ : 1.0;
    const double alphaIn = 0.90;
    opacityTF_->RemoveAllPoints();
    opacityTF_->AddPoint(segMin_ - f, 0.00);
    opacityTF_->AddPoint(segMin_, alphaIn);
    opacityTF_->AddPoint(segMax_, alphaIn);
    opacityTF_->AddPoint(segMax_ + f, 0.00);
    // 区间外两端兜底，避免 VTK 在曲线端点之外沿用最后一个控制点的值
    opacityTF_->AddPoint(segMin_ - 4.0 * f, 0.00, 1.0, 0.0);
    opacityTF_->AddPoint(segMax_ + 4.0 * f, 0.00, 1.0, 1.0);
    if (volume_) volume_->Modified();
}

void CbctVtkRenderer::applyVisibility() {
    // 叠加层单独显示（隔离显示 ROI 子集）时隐藏体数据/切面 Prop。
    // 模式切换会重建 Prop 挂载，因此 applyRenderMode() 末尾也需重放本状态。
    //
    // 关键约束：HU 阈值分割（setSegmentHuRange）没有独立 Actor，它是把 volume_ 的
    // 不透明度曲线改成"区间内不透明、区间外全透明"。此时 volume_ 本身就是 ROI 子集，
    // 再按 volumeVisible_=false 隐藏它会把分割结果一起抹掉（真机表现为纯白屏）。
    // 因此分割生效期间强制保留体数据可见，调用方只应通过 TF 区间控制显隐。
    const bool volumeShown = volumeVisible_ || segActive_;
    if (volume_) volume_->SetVisibility(volumeShown ? 1 : 0);
    if (imageActor_) imageActor_->SetVisibility(volumeVisible_ ? 1 : 0);
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
        // 平行投影：切面（XY 平面）正交显示
        cam->ParallelProjectionOn();
        // 关键修复：确保 MPR 管线已更新，否则 ResetCamera 得到的 bounds 可能为 0 导致黑屏
        if (imageActor_ && imageActor_->GetMapper()) {
            imageActor_->GetMapper()->Update();
        }
        // 初始看向坐标系原点（Reslice 输出图像的左下角）
        cam->SetFocalPoint(0.0, 0.0, 0.0);
        cam->SetPosition(0.0, 0.0, 1.0);
        cam->SetViewUp(0.0, 1.0, 0.0);
        renderer_->ResetCamera();

        // 关键修复：对于 2D 切面，ResetCamera 计算的 ClippingRange 可能过小（尤其是 far plane），
        // 导致切面被裁减掉。使用内置自适应计算，确保所有 Prop 都在视锥内。
        renderer_->ResetCameraClippingRange();
    }
    initDist_ = cam->GetDistance();
    diagPending_ = true; // 触发一帧诊断日志
}

void CbctVtkRenderer::applyRotate(double dx, double dy) {
    if (!renderer_) return;
    if (mode_ != MODE_VR) {
        applyPan(dx, dy);   // MPR 模式单指滑动即平移切面
        return;
    }
    LOGD("applyRotate: dx=%.2f dy=%.2f", dx, dy);
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
    LOGD("applyPan: dx=%.2f dy=%.2f", dx, dy);
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
        const double oldScale = cam->GetParallelScale();
        cam->SetParallelScale(oldScale / factor);
        LOGD("applyZoom (Parallel): factor=%.4f scale %.1f -> %.1f",
             factor, oldScale, cam->GetParallelScale());
    } else {
        const double oldDist = cam->GetDistance();
        cam->Dolly(factor);
        LOGD("applyZoom (Perspective): factor=%.4f dist %.1f -> %.1f",
             factor, oldDist, cam->GetDistance());
    }
}

// =============================================================================
// 坐标内省与取图（只读能力，供上层扩展模块叠加测量/标注图形）
//
// 全部通过 post(task, synchronous=true) 在渲染线程执行：
//  1) 与相机/管线状态的写入严格串行，读到的就是"屏幕上那一帧"的状态；
//  2) GL 相关调用（回读帧缓冲）必须落在 EGL 上下文所在线程。
// 世界<->屏幕换算交给 vtkCoordinate（VTK 自己的投影逆运算），因此叠加层与
// 体绘制/切面使用同一套投影矩阵，不会因为扩展模块另写一份相机模型而错位。
// =============================================================================

bool CbctVtkRenderer::projectToDisplay(const double *xyz, int count, double *xy) {
    if (!xyz || !xy || count <= 0) return false;
    bool ok = false;
    post([this, xyz, count, xy, &ok] {
        if (!renderer_ || !renderWindow_) return;
        const int *winSize = renderWindow_->GetSize();
        if (!winSize || winSize[0] <= 0 || winSize[1] <= 0) return;
        vtkNew<vtkCoordinate> co;
        co->SetCoordinateSystemToWorld();
        // MPR 下切面 actor 摆在世界 XY 平面上（真机实测冠状面 bounds=[0 359.3][0 660][0 0]）：
        // 冠状面的"纵向屏幕轴"装的是体数据 z，矢状面的"横向屏幕轴"装的是体数据 y。
        // 所以叠加层必须做 displayToSliceWorld 的同一套面内轴变换（含 actor 原点偏移），
        // 否则拾取到的冠状面点画回屏幕会塌成一条水平线（世界 y 恒等于层位）。
        // 轴位是恒等映射，与旧路径逐位一致。
        // 前提：MPR 只做平移不做旋转（applyRotate 在非 VR 模式退化为 applyPan），
        // 相机视线恒为 -Z，故屏幕坐标只取传入点的 x/y，z 不参与。
        const bool slice = (mode_ == MODE_MPR);
        const int plane = plane_;
        double ox = 0.0, oy = 0.0, oz = 0.0;
        if (slice && imageActor_) {
            const double *ab = imageActor_->GetBounds();
            if (ab && ab[1] > ab[0] && ab[3] > ab[2]) {
                ox = ab[0];
                oy = ab[2];
                oz = ab[4];
            }
        }
        for (int i = 0; i < count; ++i) {
            const double wx = xyz[i * 3 + 0], wy = xyz[i * 3 + 1], wz = xyz[i * 3 + 2];
            if (!slice) {
                co->SetValue(wx, wy, wz);
            } else {
                double px = wx, py = wy;
                if (plane == PLANE_CORONAL) { px = wx; py = wz; }
                else if (plane == PLANE_SAGITTAL) { px = wy; py = wz; }
                co->SetValue(px + ox, py + oy, oz);
            }
            double *d = co->GetComputedDoubleDisplayValue(renderer_);
            // VTK display 原点在左下、Y 向上；Android 视图原点在左上、Y 向下
            xy[i * 2 + 0] = d[0];
            xy[i * 2 + 1] = winSize[1] - d[1];
        }
        ok = true;
    }, true);
    return ok;
}

bool CbctVtkRenderer::displayToRay(double x, double y, double origin[3], double dir[3]) {
    if (!origin || !dir) return false;
    bool ok = false;
    post([this, x, y, origin, dir, &ok] {
        if (!renderer_ || !renderWindow_) return;
        vtkCamera *cam = renderer_->GetActiveCamera();
        if (!cam) return;
        const int *winSize = renderWindow_->GetSize();
        if (!winSize || winSize[0] <= 0 || winSize[1] <= 0) return;

        const double *clip = cam->GetClippingRange();
        const double nearZ = clip[0], farZ = clip[1];
        if (!(farZ > nearZ)) return;

        vtkNew<vtkCoordinate> co;
        co->SetCoordinateSystemToDisplay();
        // 显示 Y 翻回 VTK 的左下原点，再分别取近/远裁剪面上的世界点
        const double vy = winSize[1] - y;
        co->SetValue(x, vy, nearZ);
        double *p0 = co->GetComputedWorldValue(renderer_);
        const double o[3] = {p0[0], p0[1], p0[2]};
        co->SetValue(x, vy, farZ);
        double *p1 = co->GetComputedWorldValue(renderer_);

        double d[3] = {p1[0] - o[0], p1[1] - o[1], p1[2] - o[2]};
        const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (len < 1e-9) return;
        origin[0] = o[0]; origin[1] = o[1]; origin[2] = o[2];
        dir[0] = d[0] / len; dir[1] = d[1] / len; dir[2] = d[2] / len;
        ok = true;
    }, true);
    return ok;
}

/**
 * 显示坐标 -> 当前 MPR 切面上的世界点（只读内省，测量层用）。
 *
 * 为什么不能沿用"射线 ∩ 世界平面"：
 *  1) MPR 相机固定沿 -Z 正交俯视切面（见 setupCameraForMode），而冠状/矢状切面的
 *     世界平面法线是 Y / X，射线与它们平行，永远求不出交点——MeasurePicker::pickPlane
 *     对这两类面只会返回 "ray parallel to plane"。
 *  2) 就算求交也不能靠裁剪面参数 t：切面是退化的 2D Prop（z 厚度为 0），
 *     ResetCameraClippingRange 给在近裁剪面上。真机日志显示 dir=(0,0,-1) 时射线的
 *     世界起点 z 已经是负数（"slice plane behind camera"），即近裁剪面落到了切面之后。
 *     平行投影下射线的 x/y 沿程不变，所以直接取起点分量即可，完全不看 t。
 *
 * 屏幕点真正对应的是"切面图像的面内物理坐标"，按 applyPlane() 里每个面的
 * ResliceAxesDirectionCosines 反算：
 *   轴位  面内(i,j) = 世界(x, y)，平面 z = pos*sz
 *   冠状  面内(i,j) = 世界(x, z)，平面 y = pos*sy
 *   矢状  面内(i,j) = 世界(y, z)，平面 x = pos*sx
 * 面内坐标以 imageActor_ 实际 bounds 的左下角为基准：Reslice 输出图像的原点不必落在
 * 世界 0，而三个面的方向余弦都把体数据原点放在面内 (0,0)，所以"相对 actor 左下"才是
 * 对应体数据索引的那个量。轴位路径与旧的射线-平面求交结果一致（同一条 -Z 射线、同一个
 * x/y、同一个 z=pos*sz），因此这一改动对已验证的轴面拾取等价，只是把冠状/矢状从
 * "拾取不到"变成可用。
 */
bool CbctVtkRenderer::displayToSliceWorld(double x, double y, double out[3]) {
    if (!out) return false;
    double o[3] = {0, 0, 0}, d[3] = {0, 0, 0};
    if (!displayToRay(x, y, o, d)) {
        LOGW("displayToSliceWorld: displayToRay failed at disp(%.0f,%.0f)", x, y);
        return false;
    }
    const double ray[6] = {o[0], o[1], o[2], d[0], d[1], d[2]};
    bool ok = false;
    post([this, ray, out, &ok] {
        const double *o = ray, *d = ray + 3;
        const char *why = nullptr;
        double a = 0.0, b = 0.0, lim0 = 0.0, lim1 = 0.0;
        double bnd[6] = {0, 0, 0, 0, 0, 0};
        do {
            if (mode_ != MODE_MPR) { why = "not MPR"; break; }
            if (!vol_) { why = "no volume"; break; }
            // 视线的 x/y 分量非零说明被旋转过，面内坐标不再等于世界分量
            if (std::fabs(d[0]) > 1e-6 || std::fabs(d[1]) > 1e-6 ||
                std::fabs(d[2]) < 1e-6) { why = "view not along Z"; break; }
            if (!imageActor_) { why = "no slice actor"; break; }
            const double *ab = imageActor_->GetBounds();
            if (!ab) { why = "no actor bounds"; break; }
            for (int i = 0; i < 6; ++i) bnd[i] = ab[i];
            lim0 = bnd[1] - bnd[0];
            lim1 = bnd[3] - bnd[2];
            if (lim0 <= 1e-9 || lim1 <= 1e-9) { why = "actor bounds empty"; break; }
            a = o[0] - bnd[0];                  // 切面图像横向物理坐标（mm，从面内原点起）
            b = o[1] - bnd[2];                  // 切面图像纵向物理坐标（mm）
            // 与 MeasurePicker::pickPlane 一致：允许一个体素的越界，容忍指尖误差
            const double slack = std::max(vol_->spacingX, std::max(vol_->spacingY, vol_->spacingZ));
            if (a < -slack || a > lim0 + slack || b < -slack || b > lim1 + slack) {
                why = "point outside slice"; break;
            }
            const double sx = vol_->spacingX, sy = vol_->spacingY, sz = vol_->spacingZ;
            double wx, wy, wz;
            switch (plane_) {
                case PLANE_CORONAL:
                    wx = a; wy = position_ * sy; wz = b;
                    break;
                case PLANE_SAGITTAL:
                    wx = position_ * sx; wy = a; wz = b;
                    break;
                default:   // PLANE_AXIAL
                    wx = a; wy = b; wz = position_ * sz;
                    break;
            }
            out[0] = wx; out[1] = wy; out[2] = wz;
            ok = true;
        } while (false);
        // 真机上"点不上切面"必须能一眼分清是相机、越界还是模式问题
        LOGD("displayToSliceWorld: mode=%d plane=%d pos=%d o=(%.2f,%.2f,%.2f) "
             "dir=(%.3f,%.3f,%.3f) actor=[%.1f %.1f][%.1f %.1f][%.1f %.1f] "
             "in=(%.2f,%.2f) lim=(%.1f,%.1f) -> %s",
             (int) mode_, (int) plane_, position_, o[0], o[1], o[2], d[0], d[1], d[2],
             bnd[0], bnd[1], bnd[2], bnd[3], bnd[4], bnd[5], a, b, lim0, lim1,
             why ? why : "ok");
    }, true);
    return ok;
}

bool CbctVtkRenderer::getRenderSnapshot(double out[8]) {
    if (!out) return false;
    bool ok = false;
    post([this, out, &ok] {
        int w = surfW_, h = surfH_;
        if (renderWindow_) {
            const int *sz = renderWindow_->GetSize();
            if (sz) {
                w = sz[0];
                h = sz[1];
            }
        }
        out[0] = mode_;
        out[1] = plane_;
        out[2] = position_;
        out[3] = w;
        out[4] = h;
        out[5] = ww_;
        out[6] = wc_;
        out[7] = (renderer_ && renderer_->GetActiveCamera() &&
                  renderer_->GetActiveCamera()->GetParallelProjection()) ? 1.0 : 0.0;
        ok = true;
    }, true);
    return ok;
}

void CbctVtkRenderer::setVolumeVisible(bool visible) {
    post([this, visible] {
        volumeVisible_ = visible;
        applyVisibility();
        markDirty();
    }, false);
}

void CbctVtkRenderer::setSegmentHuRange(double huMin, double huMax, double feather) {
    post([this, huMin, huMax, feather] {
        segMin_ = huMin;
        segMax_ = std::max(huMin + 1.0, huMax);
        segFeather_ = std::max(1.0, feather);
        segActive_ = true;
        applyOpacity();
        applyVisibility();   // 分割生效 => 体数据 Actor 必须保持可见（见函数内注释）
        if (volume_) volume_->Modified();
        markDirty();
    }, false);
}

void CbctVtkRenderer::resetSegmentHuRange() {
    post([this] {
        segActive_ = false;
        applyOpacity();
        applyVisibility();   // 分割关闭后回落到调用方设置的 volumeVisible_
        if (volume_) volume_->Modified();
        markDirty();
    }, false);
}

bool CbctVtkRenderer::captureFrame(std::vector<uint8_t> &rgba, int &w, int &h) {
    w = 0;
    h = 0;
    rgba.clear();
    bool ok = false;
    post([this, &rgba, &w, &h, &ok] {
        if (!renderWindow_ || !window_) return;
        // vtkWindowToImageFilter 内部会 InvokeEvent(RenderStart/End) 触发一次
        // Render() 并按渲染窗口像素格式回读，是 VTK 官方的截图路径；
        // 必须在渲染线程调用（EGL 上下文绑定线程）。
        vtkNew<vtkWindowToImageFilter> winToImg;
        winToImg->SetInput(renderWindow_);
        winToImg->SetInputBufferTypeToRGBA();
        winToImg->ReadFrontBufferOff();   // 直接取后台缓冲（刚渲染完那一帧）
        winToImg->Update();
        vtkImageData *img = winToImg->GetOutput();
        if (!img || !img->GetPointData()->GetScalars()) {
            LOGW("captureFrame: window-to-image output empty");
            return;
        }
        const int *ext = img->GetExtent();
        const int iw = ext[1] - ext[0] + 1;
        const int ih = ext[3] - ext[2] + 1;
        unsigned char *src = static_cast<unsigned char *>(img->GetScalarPointer());
        if (iw <= 0 || ih <= 0 || !src) return;
        rgba.assign((size_t) iw * ih * 4, 0);
        // VTK 图像第 0 行在底部，Android Bitmap 第 0 行在顶部：逐行翻转拷贝
        for (int row = 0; row < ih; ++row) {
            const unsigned char *s = src + (size_t)(ih - 1 - row) * iw * 4;
            uint8_t *d = rgba.data() + (size_t) row * iw * 4;
            memcpy(d, s, (size_t) iw * 4);
        }
        w = iw;
        h = ih;
        ok = true;
        LOGD("captureFrame: %dx%d RGBA", w, h);
    }, true);
    return ok;
}
