package com.wangyao.cbctdeal.render

import android.annotation.SuppressLint
import android.content.Context
import android.util.AttributeSet
import android.view.GestureDetector
import android.view.MotionEvent
import android.view.ScaleGestureDetector
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import com.wangyao.cbctdeal.jni.CbctVtkJni
import com.wangyao.cbctdeal.model.CbctVolumeHandle

/**
 * CBCT VTK 三维渲染载体（SurfaceView，《Android平台VTK完整集成流程》五、Android端渲染交互适配）。
 *
 * 职责内聚：Surface 生命周期转递 + 触摸手势捕获，渲染逻辑全部在
 * Native 侧专用渲染线程内（EGL 上下文线程绑定），Java 层不做任何 GL 操作。
 *
 * 手势映射（PDF 5.2）：
 *  - 单指滑动：VR 模式旋转相机（Azimuth/Elevation）；MPR 模式平移切面；
 *  - 双指捏合：缩放（VR 相机远近 / MPR 平行缩放）；
 *  - 双指拖动：模型位置偏移（与捏合并存：焦点位移即平移）。
 *
 * 使用方式：
 * ```
 * vtkView.setVolume(volumeHandle)        // 解析成功后注入（可重复注入新序列）
 * vtkView.setRenderMode(CbctVtkJni.MODE_VR / MODE_MPR)
 * vtkView.setPlane(plane, position)      // MPR 切面
 * vtkView.setWindowLevel(ww, wc)         // 窗宽窗位实时同步
 * ...
 * vtkView.release()                      // 必须在 volumeHandle.release() 之前
 * ```
 */
@SuppressLint("ClickableViewAccessibility")
class CbctVtkView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0,
) : SurfaceView(context, attrs, defStyleAttr), SurfaceHolder.Callback {

    /** Native 渲染器指针（0 = 未创建/已销毁） */
    private var rendererPtr = 0L

    /** Surface 是否处于可用状态（决定新渲染器是否需要补发 onSurfaceCreated） */
    private var surfaceReady = false
    private var surfW = 0
    private var surfH = 0

    // 当前 UI 状态（新渲染器创建后重放，保持跨序列的浏览参数）
    private var curMode = CbctVtkJni.MODE_VR
    private var curPlane = CbctVtkJni.PLANE_AXIAL
    private var curPosition = 0
    private var curWw = 4000.0
    private var curWc = 600.0

    // ---------- 手势（双指：捏合缩放 + 焦点位移平移） ----------
    private var lastFocusX = 0f
    private var lastFocusY = 0f

    private val scaleDetector = ScaleGestureDetector(context,
        object : ScaleGestureDetector.SimpleOnScaleGestureListener() {
            override fun onScale(detector: ScaleGestureDetector): Boolean {
                val ptr = rendererPtr
                if (ptr == 0L) return true
                // 双指平移：捏合过程中焦点位移即模型偏移
                val fx = detector.focusX
                val fy = detector.focusY
                val dxFocus = fx - lastFocusX
                val dyFocus = fy - lastFocusY
                lastFocusX = fx
                lastFocusY = fy
                if (kotlin.math.abs(dxFocus) > 0.5f || kotlin.math.abs(dyFocus) > 0.5f) {
                    CbctVtkJni.pan(ptr, dxFocus.toDouble(), dyFocus.toDouble())
                }
                // 双指缩放：相机远近调节
                CbctVtkJni.zoom(ptr, detector.scaleFactor.toDouble())
                return true
            }
        })

    private val gestureDetector = GestureDetector(context,
        object : GestureDetector.SimpleOnGestureListener() {
            /** 单指滑动：distance = 上次 - 本次，取反得手指位移 */
            override fun onScroll(
                e1: MotionEvent?, e2: MotionEvent, distanceX: Float, distanceY: Float,
            ): Boolean {
                val ptr = rendererPtr
                if (ptr == 0L) return true
                val dx = -distanceX.toDouble()
                val dy = -distanceY.toDouble()
                if (curMode == CbctVtkJni.MODE_VR) {
                    CbctVtkJni.rotate(ptr, dx, dy)
                } else {
                    CbctVtkJni.pan(ptr, dx, dy)
                }
                return true
            }
        })

    init {
        holder.addCallback(this)
        holder.setFormat(android.graphics.PixelFormat.RGBA_8888)
    }

    // =========================================================================
    // 对外 API
    // =========================================================================

    /**
     * 注入解析完成的 Volume（可多次调用切换序列：内部先销毁旧渲染器）。
     * 必须在主线程调用；注入的 handle 释放前必须先调用 [release]。
     */
    fun setVolume(handle: CbctVolumeHandle) {
        destroyRendererInternal()
        rendererPtr = CbctVtkJni.createRenderer(handle.ptr)
        if (rendererPtr == 0L) return

        // 重放当前 UI 状态（保持平面/窗宽窗位等浏览参数）
        CbctVtkJni.setRenderMode(rendererPtr, curMode)
        CbctVtkJni.setPlane(rendererPtr, curPlane, curPosition)
        CbctVtkJni.setWindowLevel(rendererPtr, curWw, curWc)

        // Surface 已就绪（序列切换场景）：补发生命周期，立即挂载渲染
        if (surfaceReady) {
            val surface: Surface? = holder.surface
            if (surface != null && surface.isValid) {
                CbctVtkJni.onSurfaceCreated(rendererPtr, surface, surfW, surfH)
                CbctVtkJni.onSurfaceChanged(rendererPtr, surfW, surfH)
            }
        }
    }

    /** 切换渲染模式：[CbctVtkJni.MODE_VR] / [CbctVtkJni.MODE_MPR] */
    fun setRenderMode(mode: Int) {
        curMode = mode
        if (rendererPtr != 0L) CbctVtkJni.setRenderMode(rendererPtr, mode)
    }

    /** 设置 MPR 平面与层位置（axial: [0,depth)，coronal: [0,height)，sagittal: [0,width)） */
    fun setPlane(plane: Int, position: Int) {
        curPlane = plane
        curPosition = position
        if (rendererPtr != 0L) CbctVtkJni.setPlane(rendererPtr, plane, position)
    }

    /** 窗宽窗位实时同步（HU 域） */
    fun setWindowLevel(ww: Double, wc: Double) {
        curWw = ww
        curWc = wc
        if (rendererPtr != 0L) CbctVtkJni.setWindowLevel(rendererPtr, ww, wc)
    }

    /** 复位相机（VR 恢复初始视角，MPR 重新铺满切面） */
    fun resetCamera() {
        if (rendererPtr != 0L) CbctVtkJni.resetCamera(rendererPtr)
    }

    /** 渲染器是否就绪（Surface 与 Volume 均已挂载） */
    val isReady: Boolean
        get() = rendererPtr != 0L && surfaceReady

    /**
     * 销毁渲染器（幂等安全）。
     * 必须在对应 [CbctVolumeHandle.release] 之前调用（渲染器零拷贝引用 Volume 体素）。
     */
    fun release() {
        destroyRendererInternal()
    }

    private fun destroyRendererInternal() {
        if (rendererPtr != 0L) {
            val ptr = rendererPtr
            rendererPtr = 0L
            // 同步销毁（Surface 存活时先走 onSurfaceDestroyed 释放 EGL 资源）
            if (surfaceReady) CbctVtkJni.onSurfaceDestroyed(ptr)
            CbctVtkJni.destroyRenderer(ptr)
        }
    }

    // =========================================================================
    // Surface 生命周期（转递 Native，建立/销毁 EGL 渲染窗口）
    // =========================================================================

    override fun surfaceCreated(holder: SurfaceHolder) {
        surfaceReady = true
        val rect = holder.surfaceFrame
        surfW = rect.width()
        surfH = rect.height()
        val surface = holder.surface
        if (rendererPtr != 0L && surface.isValid) {
            CbctVtkJni.onSurfaceCreated(rendererPtr, surface, surfW, surfH)
        }
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        surfW = width
        surfH = height
        if (rendererPtr != 0L) {
            CbctVtkJni.onSurfaceChanged(rendererPtr, width, height)
        }
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        // 同步释放 EGL（回调返回后 Surface 即被系统回收，禁止再有任何 GL 操作）
        if (rendererPtr != 0L) {
            CbctVtkJni.onSurfaceDestroyed(rendererPtr)
        }
        surfaceReady = false
    }

    // =========================================================================
    // 触摸手势：捕获后经 JNI 驱动 Native 侧 VTK 相机
    // =========================================================================

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (rendererPtr == 0L) return false

        scaleDetector.onTouchEvent(event)
        if (event.pointerCount >= 2) {
            // 双指阶段：记录捏合焦点初值，供 onScale 计算平移增量
            if (event.actionMasked == MotionEvent.ACTION_POINTER_DOWN) {
                lastFocusX = scaleDetector.focusX
                lastFocusY = scaleDetector.focusY
            }
            return true
        }
        // 单指：旋转（VR）/ 平移（MPR）
        gestureDetector.onTouchEvent(event)
        return true
    }

    override fun onDetachedFromWindow() {
        super.onDetachedFromWindow()
        // 兜底释放，防止宿主遗漏 release() 造成 Native 泄漏
        release()
    }
}
