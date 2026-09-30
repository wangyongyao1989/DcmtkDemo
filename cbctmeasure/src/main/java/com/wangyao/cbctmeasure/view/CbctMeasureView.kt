package com.wangyao.cbctmeasure.view

import android.content.Context
import android.graphics.Bitmap
import android.os.SystemClock
import android.util.AttributeSet
import android.util.Log
import android.view.MotionEvent
import android.widget.FrameLayout
import com.wangyao.cbctdeal.model.CbctVolumeHandle
import com.wangyao.cbctdeal.render.CbctVtkView

/**
 * 测量页的渲染容器：CbctVtkView（体绘制/切面）在下，MeasureOverlayView（测量图形）在上。
 *
 * 为什么需要一个自定义容器而不是让上层直接摆两个 View：
 * 触摸事件必须"一处判定、单向下发"。SurfaceView 拿到的是原始 MotionEvent，
 * 而测量态需要把单指手势解释成取点/拖框，两者会抢同一根手指。
 * 判定规则（PRD 8.5 / AC-10）写在 dispatchTouchEvent 里：
 *   - 双指（含缩放过程中的第二指按下）：永远给 CbctVtkView，保证任何测量态都能转视角；
 *   - 单指且当前态属于测量态：交给 MeasureToolController，不再下发给渲染视图；
 *   - 其余情况（浏览态、ROI 编辑态）：原样转给 CbctVtkView。
 * 相机姿态每帧变化后调用 controller.syncOverlay()，让叠加图形跟着模型走。
 */
class CbctMeasureView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0,
) : FrameLayout(context, attrs, defStyleAttr) {

    val vtkView: CbctVtkView = CbctVtkView(context)
    val overlayView: MeasureOverlayView = MeasureOverlayView(context)

    /**
     * 手势状态机。宿主界面（Fragment）提供 Host 回调；
     * 容器只负责"把事件分给谁"和"相机变了就重投影"。
     */
    var controller: MeasureToolController? = null

    /** 相机是否正在被操作（决定 onGlobalLayout/绘制后要不要持续刷新叠加层） */
    private var cameraActive = false

    init {
        addView(vtkView, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT))
        addView(overlayView, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT))
        // 叠加层的投影能力绑定到同一个渲染器：与体绘制共用相机，像素级对齐
        overlayView.projector = MeasureOverlayView.Projector { world -> vtkView.projectPoints(world) }
        // 首帧之后才有 surface 尺寸，用一次 post 保证 setSurfaceSize 拿到有效值
        post { refreshAfterCamera() }
    }

    /** 装载体数据（转交给渲染视图；会话绑定由上层用 handle.ptr 调 MeasureJni.bindVolume） */
    fun setVolume(handle: CbctVolumeHandle) {
        vtkView.setVolume(handle)
        post { refreshAfterCamera() }
    }

    fun setRenderMode(mode: Int) {
        vtkView.setRenderMode(mode)
        post { refreshAfterCamera() }
    }

    fun setPlane(plane: Int, position: Int) {
        vtkView.setPlane(plane, position)
        post { refreshAfterCamera() }
    }

    fun setWindowLevel(ww: Double, wc: Double) {
        vtkView.setWindowLevel(ww, wc)
    }

    fun resetCamera() {
        vtkView.resetCamera()
        post { refreshAfterCamera() }
    }

    /** 报告截图：抓当前渲染帧，再把叠加层画上去（证据图必须与屏幕所见一致） */
    fun captureEvidence(): Bitmap? {
        // PC-03 取证（要求截图 <=500ms）：抓帧要等渲染线程，是把 GPU surface 读回内存的
        // 那一段；叠加层只是 Canvas 绘制，单独打一行便于分清瓶颈。
        val t0 = SystemClock.elapsedRealtime()
        val frame = vtkView.captureFrame() ?: return null
        val tFrame = SystemClock.elapsedRealtime()
        val out = Bitmap.createBitmap(overlayView.width.coerceAtLeast(frame.width),
            overlayView.height.coerceAtLeast(frame.height), Bitmap.Config.ARGB_8888)
        val canvas = android.graphics.Canvas(out)
        canvas.drawColor(android.graphics.Color.BLACK)
        // 抓取帧是渲染 surface 尺寸，叠加层是视图尺寸：按视图尺寸缩放绘制
        canvas.drawBitmap(frame, null, android.graphics.Rect(0, 0, out.width, out.height), null)
        overlayView.draw(canvas)
        Log.d(TAG, "captureEvidence %dx%d frame=%.1fms overlay=%.1fms total=%.1fms".format(
            out.width, out.height,
            (tFrame - t0).toDouble(), costMs(tFrame), costMs(t0)))
        return out
    }

    private fun costMs(t0: Long): Double = (SystemClock.elapsedRealtime() - t0).toDouble()

    companion object {
        private const val TAG = "CbctMeasureView"
    }

    override fun dispatchTouchEvent(ev: MotionEvent): Boolean {
        // 页面通常在 NestedScrollView 里，不禁止父级拦截的话，手势一动就被滚动抢走。
        // 必须在 ACTION_DOWN 就 disallow：父级是先问自己要不要拦截再下发给子 View，
        // 等第一个 MOVE 才请求（旧写法）时父级早已越过 touchSlop 开始滚动，
        // 子 View 只收到 CANCEL —— 真机上表现为"弧线/体积拖动没有任何取点日志"。
        // 抬手后恢复 false，页面其它区域（视口上下的行）仍可正常滚动。
        when (ev.actionMasked) {
            MotionEvent.ACTION_DOWN -> parent?.requestDisallowInterceptTouchEvent(true)
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL ->
                parent?.requestDisallowInterceptTouchEvent(false)
            else -> Unit
        }
        if (ev.pointerCount > 1 || ev.actionMasked == MotionEvent.ACTION_MOVE) {
            parent?.requestDisallowInterceptTouchEvent(true)
        }
        val c = controller
        val twoPointer = ev.pointerCount > 1
        if (c != null && !twoPointer && c.consumesSinglePointer(ev)) {
            // 测量态吃掉单指手势：只驱动状态机，不让渲染视图同时旋转
            handleCameraTouchState(ev)
            c.onTouchEvent(ev)
            return true
        }
        handleCameraTouchState(ev)
        val consumed = vtkView.dispatchTouchEvent(ev)
        if (cameraActive) c?.reprojectOnly()
        return consumed
    }

    /**
     * 相机手势期间的叠加层跟随。
     *
     * 不在 onDraw 里主动拉 Native：VTK 在自己的渲染线程出帧，本视图只在
     * 事件流里"跟着重投影一次"，这样投影与相机最多差一帧，肉眼不可见，
     * 又不会把 projectPoints 变成每帧必调的开销。
     */
    private fun handleCameraTouchState(ev: MotionEvent) {
        when (ev.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> cameraActive = true
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                cameraActive = false
                controller?.syncOverlay()
            }
            MotionEvent.ACTION_POINTER_UP -> {
                // 双指抬起一根后仍有单指在屏上：保持 cameraActive，直到真正抬手
                if (ev.pointerCount <= 2) cameraActive = true
            }
            else -> Unit
        }
    }

    /** 相机/数据变化后的统一刷新入口（上层切层、改窗宽窗位后也应调用） */
    fun refreshAfterCamera() {
        val snap = if (vtkView.isReady) vtkView.renderSnapshot() else null
        if (snap != null && snap.size > 4) {
            overlayView.setSurfaceSize(snap[3].toInt(), snap[4].toInt())
        }
        overlayView.reproject()
        controller?.syncOverlay()
    }

    override fun onDetachedFromWindow() {
        // 顺序要求：先清叠加层引用，再由上层销毁会话（Native 端会话必须早于体数据释放）
        overlayView.clearAll()
        super.onDetachedFromWindow()
    }
}
