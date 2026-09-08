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
            const auto t0 = std::chrono::steady_clock::now();
            try {
                renderWindow_->Render();
            } catch (...) {
                LOGE("renderLoop: render exception");
            }
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
    renderer_->GradientBackgroundOn();
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
        floatLinear_ = true;
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
        if (mode_ == mode && renderer_) return;
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
        // 真机排障：复位按钮同时循环切换诊断假设（见头文件 diagMode_ 注释）
        diagMode_ = (diagMode_ + 1) % 4;
        switch (diagMode_) {
            case 1:   // 全值域不透明红色：判别 3D 纹理采样是否有输出
                if (colorTF_) {
                    colorTF_->RemoveAllPoints();
                    colorTF_->AddRGBPoint(-4000.0, 1.0, 0.05, 0.05);
                    colorTF_->AddRGBPoint(5000.0, 1.0, 0.05, 0.05);
                }
                if (opacityTF_) {
                    opacityTF_->RemoveAllPoints();
                    opacityTF_->AddPoint(-4000.0, 0.85);
                    opacityTF_->AddPoint(5000.0, 0.85);
                }
                if (volume_) volume_->Modified();
                break;
            case 2:   // CPU RayCast：判别 GPU 管线专属问题
                if (volMapper_) volMapper_->SetRequestedRenderModeToRayCast();
                applyWindowLevel();
                break;
            case 3:   // GPU + Linear 插值：判别 Nearest 降级副作用
                if (volMapper_) volMapper_->SetRequestedRenderModeToDefault();
                if (volProperty_) volProperty_->SetInterpolationTypeToLinear();
                applyWindowLevel();
                break;
            case 0:   // 恢复基线：GPU 默认 + 按设备能力定插值 + 骨窗 TF
            default:
                if (volMapper_) volMapper_->SetRequestedRenderModeToDefault();
                if (volProperty_) {
                    if (floatLinear_) volProperty_->SetInterpolationTypeToLinear();
                    else volProperty_->SetInterpolationTypeToNearest();
                }
                applyWindowLevel();
                break;
        }
        LOGW("diagMode=%d (0=GPU基线 1=全不透明红GPU 2=CPU 3=GPULinear)",
             diagMode_);
        setupCameraForMode();
        diagPending_ = true;
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
    if (opacityTF_) {
        // 不透明度：HU<200 全透明（PDF 骨骼阈值过滤软组织），
        // 200->1300 HU 渐升（高密度骨骼渲染权重调高），高位饱和
        opacityTF_->RemoveAllPoints();
        opacityTF_->AddPoint(-1024.0, 0.00);
        opacityTF_->AddPoint(200.0, 0.00);
        opacityTF_->AddPoint(1300.0, 0.85);
        opacityTF_->AddPoint(4000.0, 0.90);
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
