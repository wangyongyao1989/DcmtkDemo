#ifndef DCMTKDEMO_CBCTVTKRENDERER_H
#define DCMTKDEMO_CBCTVTKRENDERER_H

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#include "vtkSmartPointer.h"

#include "CbctVolume.h"

struct ANativeWindow;
class vtkImageData;
class vtkEGLRenderWindow;
class vtkRenderer;
class vtkSmartVolumeMapper;
class vtkVolume;
class vtkVolumeProperty;
class vtkColorTransferFunction;
class vtkPiecewiseFunction;
class vtkImageReslice;
class vtkImageMapToColors;
class vtkLookupTable;
class vtkImageActor;
class vtkActor;

/**
 * CBCT VTK 三维渲染核心（纯 C++，无 JNI 依赖，可独立复用/测试）。
 *
 * 职责（《Android平台VTK完整集成流程》四、五两节的落地）：
 *  1) 将 CbctSeriesParser 解析出的 CbctVolume 以零拷贝方式导入 vtkImageData
 *     （SetArray save=1，VTK 不接管内存，Volume 生命周期仍由解析侧管理）；
 *  2) VR 体绘制管线：vtkSmartVolumeMapper + 传递函数（骨窗 WW/WC 驱动色彩跨度，
 *     骨骼阈值 200HU 过滤软组织，ShadeOn 明暗着色）；
 *  3) MPR 管线：vtkImageReslice 三轴切面 + vtkImageMapToColors 窗宽窗位灰阶映射；
 *  4) 手势交互：单指旋转 / 双指缩放 / 平移（相机 Azimuth/Elevation/Dolly/Pan）；
 *  5) Android Surface 对接：vtkEGLRenderWindow::SetWindowId(ANativeWindow)。
 *
 * 线程模型：所有 EGL/OpenGL 操作（含 VTK Render）固定在内部专用渲染线程执行
 * （EGL 上下文与线程绑定）。外部调用（JNI 线程）通过 post() 投递任务：
 *  - 异步任务：手势 / 参数调节等高频事件，队列排空后才渲染（自动合并，
 *    闲置不占 GPU，对应 PDF 6.2「闲置状态暂停渲染」）；
 *  - 同步任务：surface 创建 / 销毁等必须等待完成的时序点。
 *
 * 生命周期：create() 创建 -> onSurfaceCreated/Changed/Destroyed 跟随 Surface ->
 *            destroy() 同步停线程并释放（必须在 CbctVolume release 之前调用）。
 */
class CbctVtkRenderer {
public:
    /** 渲染模式 */
    enum RenderMode {
        MODE_VR = 0,    // 三维体绘制（SmartVolumeMapper RayCast）
        MODE_MPR = 1,   // vtkImageReslice 切面 + 窗宽窗位灰阶
    };

    /** MPR 平面（与 Kotlin 侧 CbctVtkJni 常量一一对应） */
    enum MprPlane {
        PLANE_AXIAL = 0,     // 横断面（固定 Z）
        PLANE_CORONAL = 1,   // 冠状面（固定 Y）
        PLANE_SAGITTAL = 2,  // 矢状面（固定 X）
    };

    /**
     * 创建渲染器并导入体数据。
     * @param vol 已解析完成的 CbctVolume（渲染器不接管所有权，调用方保证
     *            在 destroy() 之前 vol 不被 release）
     * @return 渲染器实例，失败返回 nullptr（vol 为空 / 维度非法）
     */
    static CbctVtkRenderer *create(CbctVolume *vol);

    /** 销毁渲染器（同步等待渲染线程退出，幂等安全） */
    void destroy();

    // ---------- Surface 生命周期（由 CbctVtkView 转发，均同步执行） ----------
    void onSurfaceCreated(ANativeWindow *win, int width, int height);
    void onSurfaceChanged(int width, int height);
    void onSurfaceDestroyed();

    // ---------- 渲染状态（异步执行，自动触发重渲染） ----------
    void setRenderMode(int mode);                 // MODE_VR / MODE_MPR
    void setPlane(int plane, int position);       // MPR 平面与层位置（像素坐标）
    void setWindowLevel(double ww, double wc);    // 窗宽窗位（HU 域，双管线共用）
    void resetCamera();

    // ---------- 手势交互（像素位移，渲染线程内换算相机参数） ----------
    void rotate(double dx, double dy);   // 单指滑动：VR 旋转（MPR 模式退化为平移）
    void pan(double dx, double dy);      // 双指平移：位置偏移
    void zoom(double factor);            // 双指捏合：相机远近 / 平行缩放

private:
    CbctVtkRenderer();
    ~CbctVtkRenderer();
    CbctVtkRenderer(const CbctVtkRenderer &) = delete;
    CbctVtkRenderer &operator=(const CbctVtkRenderer &) = delete;

    bool init(CbctVolume *vol);   // 零拷贝导入 + VR/MPR 管线装配 + 启动渲染线程

    // ---------- 渲染线程基础设施 ----------
    void post(const std::function<void()> &task, bool synchronous);
    void renderLoop();
    void markDirty();   // 渲染线程内调用：标记需要重绘

    // ---------- 以下方法均在渲染线程内执行 ----------
    void ensureWindow();            // 惰性创建 EGL 渲染窗口/渲染器并挂载当前 Props
    void applyRenderMode();         // VR <-> MPR Prop 互切 + 相机适配
    void applyPlane();              // 更新 vtkImageReslice 切面参数
    void applyWindowLevel();        // 更新传递函数（VR）与 LUT（MPR）
    void setupCameraForMode();      // 按模式复位相机（视角/投影/距离）
    void applyRotate(double dx, double dy);
    void applyPan(double dx, double dy);
    void applyZoom(double factor);

    // ---------- 输入与窗口 ----------
    CbctVolume *vol_ = nullptr;        // 非拥有：destroy() 必须先于 Volume release
    ANativeWindow *window_ = nullptr;  // 拥有 JNI 层 fromSurface 的引用计数
    int surfW_ = 0;
    int surfH_ = 0;

    // ---------- VTK 管线（渲染线程内访问） ----------
    vtkSmartPointer<vtkImageData> imageData_;              // 零拷贝包装 CbctVolume
    vtkSmartPointer<vtkEGLRenderWindow> renderWindow_;
    vtkSmartPointer<vtkRenderer> renderer_;
    // VR 体绘制
    vtkSmartPointer<vtkSmartVolumeMapper> volMapper_;
    vtkSmartPointer<vtkVolume> volume_;
    vtkSmartPointer<vtkVolumeProperty> volProperty_;
    vtkSmartPointer<vtkColorTransferFunction> colorTF_;
    vtkSmartPointer<vtkPiecewiseFunction> opacityTF_;
    // MPR 切面
    vtkSmartPointer<vtkImageReslice> reslice_;
    vtkSmartPointer<vtkImageMapToColors> mapColors_;
    vtkSmartPointer<vtkLookupTable> lut_;
    vtkSmartPointer<vtkImageActor> imageActor_;
    // 诊断：世界空间红色立方体（二分定位：世界空间渲染 vs 纹理渲染故障）
    vtkSmartPointer<vtkActor> diagActor_;

    // ---------- 渲染状态（渲染线程内落地到管线） ----------
    int mode_ = MODE_VR;
    int plane_ = PLANE_AXIAL;
    int position_ = 0;
    double ww_ = 4000.0;
    double wc_ = 600.0;
    double initDist_ = 1.0;    // ResetCamera 后的相机距离（zoom 钳制基准）
    bool cameraReady_ = false; // 首帧自动复位相机

    // ---------- 渲染线程 ----------
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;         // 唤醒渲染线程处理任务
    std::condition_variable doneCv_;     // 同步任务完成通知
    std::deque<std::function<void()>> tasks_;
    bool quit_ = false;
    bool dirty_ = false;
    int frameCount_ = 0;                 // 诊断：已渲染帧计数
    bool diagPending_ = true;            // 诊断：下一帧输出 GL/FBO/相机自检日志

    // ---------- 真机排障：复位按钮循环切换诊断假设 ----------
    // 0=常规 GPU RayCast + 骨窗 TF（基线）
    // 1=GPU RayCast + 全值域不透明红色 TF（判别：采样是否有输出）
    // 2=CPU RayCast + 骨窗 TF（判别：GPU 管线专属问题）
    // 3=GPU RayCast + Linear 插值（判别：Nearest 降级副作用）
    int diagMode_ = 0;
    bool floatLinear_ = false;           // 设备是否支持 OES_texture_float_linear
};

#endif // DCMTKDEMO_CBCTVTKRENDERER_H
