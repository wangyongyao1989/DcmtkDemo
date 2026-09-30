package com.wangyao.cbctdeal.jni

import android.graphics.Bitmap
import android.view.Surface

/**
 * CBCT VTK 三维渲染 JNI 接口（《Android平台VTK完整集成流程》四、五两节的 Kotlin 桥）。
 *
 * 数据流（渲染线程内聚在 Native 侧）：
 *  createRenderer  -> 基于已解析 Volume 创建渲染器（零拷贝导入 vtkImageData）
 *  onSurface*      -> SurfaceView 生命周期转递（EGL 渲染窗口随 Surface 增建/销毁）
 *  setRenderMode / setPlane / setWindowLevel -> 渲染状态（自动触发按需重绘）
 *  rotate / pan / zoom / resetCamera          -> 手势驱动 VTK 相机
 *  destroyRenderer -> 同步销毁渲染器（必须先于 releaseVolume 调用）
 *
 * 渲染模式说明：
 *  - VR：vtkSmartVolumeMapper 体绘制（GPU RayCast，CPU FixedPoint 自动回退），
 *    骨骼阈值 200HU 过滤软组织，色彩跨度跟随窗宽窗位；
 *  - MPR：vtkImageReslice 三轴切面 + 灰阶窗宽窗位映射，平行投影正交显示。
 */
object CbctVtkJni {

    init {
        // 与 CbctJni 同库：cbct_native.so 内含 DCMTK 解析 + VTK 渲染双能力
        System.loadLibrary("cbct_native")
    }

    /** 渲染模式：三维体绘制（VR） */
    const val MODE_VR = 0

    /** 渲染模式：MPR 切面（vtkImageReslice） */
    const val MODE_MPR = 1

    /** MPR 平面：横断面（固定 Z） */
    const val PLANE_AXIAL = 0

    /** MPR 平面：冠状面（固定 Y） */
    const val PLANE_CORONAL = 1

    /** MPR 平面：矢状面（固定 X） */
    const val PLANE_SAGITTAL = 2

    /**
     * 创建 VTK 渲染器（零拷贝包装 Volume 体素，不接管所有权）。
     * @param volumePtr [com.wangyao.cbctdeal.jni.CbctJni.loadSeries] 返回的 Volume 指针
     * @return 渲染器指针，0 表示失败
     */
    @JvmStatic
    external fun createRenderer(volumePtr: Long): Long

    /** Surface 可用：绑定 EGL 渲染窗口（SurfaceView 回调转递） */
    @JvmStatic
    external fun onSurfaceCreated(rendererPtr: Long, surface: Surface, width: Int, height: Int)

    /** Surface 尺寸变化（旋转 / 分屏等） */
    @JvmStatic
    external fun onSurfaceChanged(rendererPtr: Long, width: Int, height: Int)

    /** Surface 销毁：同步释放 EGL 资源（回调返回后 Surface 即失效） */
    @JvmStatic
    external fun onSurfaceDestroyed(rendererPtr: Long)

    /**
     * 切换渲染模式。
     * @param mode [MODE_VR] / [MODE_MPR]
     */
    @JvmStatic
    external fun setRenderMode(rendererPtr: Long, mode: Int)

    /**
     * 设置 MPR 平面与层位置。
     * @param plane [PLANE_AXIAL] / [PLANE_CORONAL] / [PLANE_SAGITTAL]
     * @param position 像素坐标（axial: [0,depth)，coronal: [0,height)，sagittal: [0,width)）
     */
    @JvmStatic
    external fun setPlane(rendererPtr: Long, plane: Int, position: Int)

    /** 设置窗宽窗位（HU 域；VR 驱动传递函数色彩跨度，MPR 驱动灰阶区间） */
    @JvmStatic
    external fun setWindowLevel(rendererPtr: Long, ww: Double, wc: Double)

    /** 单指滑动（dx/dy 像素位移）：VR 旋转 / MPR 平移 */
    @JvmStatic
    external fun rotate(rendererPtr: Long, dx: Double, dy: Double)

    /** 双指平移（dx/dy 像素位移）：模型位置偏移 */
    @JvmStatic
    external fun pan(rendererPtr: Long, dx: Double, dy: Double)

    /** 双指捏合：factor > 1 放大（相机靠近 / 平行缩放减小） */
    @JvmStatic
    external fun zoom(rendererPtr: Long, factor: Double)

    /** 复位相机（VR 恢复初始视角，MPR 重新铺满切面） */
    @JvmStatic
    external fun resetCamera(rendererPtr: Long)

    // -------------------------------------------------------------------------
    // 坐标内省与取图（只读能力，供上层扩展模块叠加测量/标注图形）
    //
    // 出入参全部是 double / Bitmap 等 POD，不跨模块传递任何 VTK 对象：
    // 扩展模块即使各自静态链接一份 VTK，也不会出现两套 VTK 单例互相污染；
    // 世界<->屏幕换算由本模块内的 vtkCoordinate 完成，与实际渲染共用同一相机，
    // 因此叠加图形与体绘制/切面像素严格对齐。
    // 均在渲染线程同步执行，读到的一定是"屏幕上那一帧"的状态。
    // -------------------------------------------------------------------------

    /** 世界坐标(mm, [x,y,z]*n) -> 显示坐标(像素、左上原点, [x,y]*n)；未就绪返回 null */
    @JvmStatic
    external fun projectPoints(rendererPtr: Long, xyz: DoubleArray): DoubleArray?

    /** 显示坐标(像素、左上原点) -> 世界拾取射线 [ox,oy,oz,dx,dy,dz]（方向单位化） */
    @JvmStatic
    external fun displayToRay(rendererPtr: Long, x: Double, y: Double): DoubleArray?

    /**
     * 显示坐标 -> 当前 MPR 切面上的世界点 [x,y,z]（mm）。
     * 冠状/矢状切面的平面法线与 MPR 视线平行，射线求交无解，必须用这条反算路径。
     * 非 MPR 模式或点在切面图像之外时返回 null。
     */
    @JvmStatic
    external fun displayToSliceWorld(rendererPtr: Long, x: Double, y: Double): DoubleArray?

    /**
     * 渲染状态快照：
     * [0]mode [1]plane [2]position [3]宽 [4]高 [5]窗宽 [6]窗位 [7]是否平行投影
     */
    @JvmStatic
    external fun getRenderSnapshot(rendererPtr: Long): DoubleArray?

    /** 隔离显示：隐藏/恢复体数据与 MPR 切面 Prop（叠加层单独成图） */
    @JvmStatic
    external fun setVolumeVisible(rendererPtr: Long, visible: Boolean)

    /** HU 阈值分割显示：区间外不透明度置 0，feather 为边沿渐变宽度（HU） */
    @JvmStatic
    external fun setSegmentHuRange(rendererPtr: Long, huMin: Double, huMax: Double, feather: Double)

    /** 恢复模块默认骨窗不透明度曲线（HU<200 全透明） */
    @JvmStatic
    external fun resetSegmentHuRange(rendererPtr: Long)

    /** 抓取当前帧（截图报告用，含标注叠加前的是原始渲染帧） */
    @JvmStatic
    external fun captureFrame(rendererPtr: Long): Bitmap?

    /** 销毁渲染器（同步停渲染线程；必须在 releaseVolume 之前调用） */
    @JvmStatic
    external fun destroyRenderer(rendererPtr: Long)
}
