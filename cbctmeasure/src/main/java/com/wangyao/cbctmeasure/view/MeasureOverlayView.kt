package com.wangyao.cbctmeasure.view

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.os.SystemClock
import android.util.AttributeSet
import android.util.Log
import android.view.View
import com.wangyao.cbctmeasure.model.OverlayKind
import com.wangyao.cbctmeasure.model.OverlayPrim
import kotlin.math.hypot

/**
 * 测量/标注叠加层（PRD 7.3 的 AnnotationRenderer + 5.1.4 的实时绘制）。
 *
 * 为什么用 Android Canvas 而不是 VTK Actor：
 * 本模块不链接 VTK（同一进程内两份静态 VTK 会导致 vtkInformationKey 单例
 * 与 RTTI 分裂，原因见 cpp/CMakeLists.txt 顶部注释）。因此
 *   1) Native 侧只产出"世界坐标图元"（MeasureJni.overlayJson / overlayPoints）；
 *   2) 顶点整批交给 :cbctdeal 的 CbctVtkView.projectPoints()（内部 vtkCoordinate，
 *      与相机同源，因此叠加图形与体绘制像素严格对齐）；
 *   3) 这里只做 Canvas 绘制。
 * 好处是测量线宽、文字、箭头完全像素可控，且不受 VTK 渲染线程时序影响。
 *
 * SurfaceView 的上层视图天然显示在其表面之上，本视图保持不可点击，
 * 触摸事件由父容器 CbctMeasureView 决定"消费还是转发给渲染视图"。
 */
class MeasureOverlayView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0,
) : View(context, attrs, defStyleAttr) {

    /** 世界坐标 -> 显示坐标的投影器（由 CbctMeasureView 注入，绑定同一个 VTK 渲染器） */
    fun interface Projector {
        fun project(world: DoubleArray): DoubleArray?
    }

    /** 相机 surface 尺寸（renderSnapshot 的 [3]=w, [4]=h），用于视图与表面不一致时换算 */
    private var surfaceW = 0
    private var surfaceH = 0

    /** 相机变化时由 CbctMeasureView 调 reproject()；投影能力由父容器注入 */
    var projector: Projector? = null

    private val primPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeCap = Paint.Cap.ROUND
        strokeJoin = Paint.Join.ROUND
    }
    private val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
    }
    private val textPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE
        setShadowLayer(2f, 0f, 0f, Color.argb(200, 0, 0, 0))
    }

    private val density = resources.displayMetrics.density.coerceAtLeast(1f)

    /**
     * 本帧已画出的文字框（屏幕坐标），用于避让重叠。
     *
     * 多颗种植体/AI 推荐位点挨得很近时，它们的文字标签会投影到同一行，
     * 真机回归截图里 "AI推荐 上颌3-2 …" 与相邻标签直接叠成一片，等于没有信息。
     * 每帧 onDraw 开头清空，所以避让只跟当前相机有关，不会累积历史。
     */
    private val labelRects = ArrayList<RectF>(16)

    /** drawLabel 的求交工作区（避免在 onDraw 路径里分配） */
    private val scratch = RectF()

    /** 已提交的图元（Native 产出）与其投影后的屏幕点 */
    private var prims: List<OverlayPrim> = emptyList()
    private var worldPts: DoubleArray = DoubleArray(0)
    private var screenPts: FloatArray = FLOAT0

    /** 正在绘制中的草稿（手势还没结束的即时反馈） */
    private var draft: Draft? = null
    private var draftScreenPts: FloatArray = FLOAT0

    /** 选中高亮（测量列表点击某一项时，对应图元加粗；PRD 5.1.4） */
    private var highlightOwner: Int = 0
    private var highlightId: Int = -1

    /** PC-02 取证：onDraw 帧数 / 单帧绘制耗时累计 / 统计窗口起点（0 表示还没开窗） */
    private var fpsFrames = 0
    private var fpsCostNs = 0L
    private var fpsWindowStart = 0L

    /** 状态提示文字（左上角，如"再点一次确定第二个端点"） */
    private var hint: String = ""

    /**
     * 草稿。
     * @param worldPoints 已吸附的世界坐标（扁平 3*n）——随相机一起动，所以存世界系
     * @param livePath    手指当前轨迹（屏幕坐标，尚未吸附成世界点）
     * @param radiusMm    球形/环形半径（毫米，R-03/R-04 预览；<=0 不画）
     *
     * 注意 data class 的 copy() 会重算 equals/hashCode 且对 DoubleArray 无意义，
     * 这里只在指针变化时整体替换，不要用 copy 做局部更新。
     */
    class Draft(
        val worldPoints: DoubleArray,
        val livePath: FloatArray,
        val color: Int,
        val kind: Int,
        val radiusMm: Double = 0.0,
    )

    init {
        // 关键：本视图不参与触摸，事件要落到下层的 VTK SurfaceView
        isClickable = false
        isLongClickable = false
        isEnabled = false
    }

    /** 相机 surface 尺寸（renderSnapshot 的 w/h），视图与其不一致时按此缩放投影结果 */
    fun setSurfaceSize(w: Int, h: Int) {
        if (w <= 0 || h <= 0) return
        if (surfaceW != w || surfaceH != h) {
            surfaceW = w
            surfaceH = h
            reproject()
        }
    }

    /**
     * 接收 Native 叠加数据。
     * @param newPrims 已由调用方解析好（避免每帧重复解析）
     * @param worldPoints 扁平 3*n 世界坐标，与 prims 的 from/count 对应
     */
    fun setData(newPrims: List<OverlayPrim>, worldPoints: DoubleArray) {
        prims = newPrims
        worldPts = worldPoints
        reproject()
    }

    fun setDraft(d: Draft?) {
        draft = d
        reprojectDraft()
        invalidate()
    }

    fun setHint(text: String) {
        if (hint == text) return
        hint = text
        invalidate()
    }

    /**
     * 高亮某个归属对象的全部图元。
     * 早期实现是在 prim.text 里找 "#id" 子串，用户重命名或 id 互为前缀就会误高亮，
     * 现在由 Native 侧给每个图元打 owner/ownerId（MeasureTypes.h 的 OverlayOwner）。
     */
    fun setHighlight(owner: Int, measureId: Int) {
        if (highlightOwner == owner && highlightId == measureId) return
        highlightOwner = owner
        highlightId = measureId
        invalidate()
    }

    /** 清空（会话销毁 / 切序列时调用，避免残留上一序列的图形） */
    fun clearAll() {
        prims = emptyList()
        worldPts = DoubleArray(0)
        screenPts = FLOAT0
        draft = null
        draftScreenPts = FLOAT0
        hint = ""
        highlightOwner = 0
        highlightId = -1
        invalidate()
    }

    /** 相机变化时重投影（CbctMeasureView 在每次手势里按帧调用） */
    fun reproject() {
        screenPts = projectToWorld(worldPts)
        reprojectDraft()
        invalidate()
    }

    private fun reprojectDraft() {
        // 草稿的世界点同样要重投影；livePath 是屏幕系，手指在哪就是哪
        val d = draft ?: run {
            draftScreenPts = FLOAT0
            return
        }
        draftScreenPts = projectToWorld(d.worldPoints)
    }

    private fun projectToWorld(world: DoubleArray): FloatArray {
        if (world.isEmpty()) return FLOAT0
        val p = projector ?: return FLOAT0
        return toScaledScreen(p.project(world))
    }

    /**
     * 显示坐标（可能来自与视图不同尺寸的 surface）-> 本视图像素。
     * 视图与渲染表面同尺寸时是一次乘法都不做的直通；只有在分屏/旋转后
     * SurfaceHolder 尺寸还没跟上时才会走缩放分支。
     */
    private fun toScaledScreen(disp: DoubleArray?): FloatArray {
        if (disp == null || disp.size < 2) return FLOAT0
        val out = FloatArray(disp.size)
        val sx = if (surfaceW > 0 && width > 0) width.toFloat() / surfaceW else 1f
        val sy = if (surfaceH > 0 && height > 0) height.toFloat() / surfaceH else 1f
        var i = 0
        while (i + 1 < disp.size) {
            out[i] = (disp[i] * sx).toFloat()
            out[i + 1] = (disp[i + 1] * sy).toFloat()
            i += 2
        }
        return out
    }

    override fun onDraw(canvas: Canvas) {
        // PC-02 取证（要求 >=30FPS）：统计叠加层自己"每秒真正画了几帧、单帧多久"。
        // 相机被操作时上层每帧 invalidate()，空闲时不重绘，所以窗口里的帧数就是
        // 用户实际看到的叠加刷新率；每满 1s 打一行，空闲不打，避免刷屏。
        val t0 = SystemClock.elapsedRealtimeNanos()
        super.onDraw(canvas)
        labelRects.clear()
        if (screenPts.size >= 2) {
            for (prim in prims) drawPrim(canvas, prim, screenPts)
        }
        draft?.let { drawDraft(canvas, it) }
        drawHint(canvas)
        val costNs = SystemClock.elapsedRealtimeNanos() - t0
        val now = SystemClock.elapsedRealtime()
        if (fpsWindowStart == 0L) fpsWindowStart = now
        fpsFrames++
        fpsCostNs += costNs
        val elapsed = now - fpsWindowStart
        if (elapsed >= FPS_WINDOW_MS && fpsFrames > 0) {
            Log.i(TAG, "overlay fps=%d avgDraw=%.2fms frames=%d window=%dms prims=%d".format(
                fpsFrames * 1000 / elapsed,
                fpsCostNs / 1_000_000.0 / fpsFrames,
                fpsFrames, elapsed, prims.size))
            fpsFrames = 0
            fpsCostNs = 0L
            fpsWindowStart = now
        }
    }

    // =========================================================================
    // 图元绘制
    // =========================================================================

    private fun drawPrim(canvas: Canvas, prim: OverlayPrim, pts: FloatArray) {
        if (prim.count <= 0) return
        val from = prim.from.coerceAtLeast(0)
        val to = (prim.from + prim.count).coerceAtMost(pts.size / 2)
        if (to - from <= 0) return
        val selected = highlightId >= 0 && prim.ownerKind == highlightOwner && prim.ownerId == highlightId
        primPaint.color = prim.color
        primPaint.strokeWidth = prim.widthPx.toFloat().coerceAtLeast(1f) * (if (selected) 1.8f else 1f)
        fillPaint.color = alpha(prim.color, 0.18f)

        when (prim.kind) {
            OverlayKind.POINT -> for (i in from until to) drawMarkPoint(canvas, pts, i)
            OverlayKind.LINE -> {
                drawPolyline(canvas, pts, from, to, close = false)
                if (to - from >= 2) drawEndCaps(canvas, pts, from, to - 1)
                if (prim.arrowHead) drawArrowHead(canvas, pts, to - 2, to - 1)
            }
            OverlayKind.POLYLINE, OverlayKind.TUBE ->
                drawPolyline(canvas, pts, from, to, close = false)
            OverlayKind.POLYGON, OverlayKind.RING -> {
                drawPolyline(canvas, pts, from, to, close = true)
                if (prim.fill) drawFilled(canvas, pts, from, to)
                if (prim.arrowHead && to - from >= 2) drawArrowHead(canvas, pts, to - 2, to - 1)
            }
            OverlayKind.BOX -> drawBox(canvas, pts, from)
            OverlayKind.CAPSULE -> drawCapsule(canvas, pts, from, to)
            // A-07 屏幕空间圆：Native 把 radiusMm 字段当作"半径像素"传（MeasureTypes.h 注释），
            // 所以这里读 widthPx 而不是 world 距离。
            OverlayKind.CIRCLE_PX -> drawScreenCircle(canvas, pts, from, prim)
            OverlayKind.TEXT -> { /* 文字统一走下面的 drawLabel，避免与线框的遮挡顺序问题 */ }
        }
        if (prim.text.isNotEmpty()) drawLabel(canvas, prim, pts, from, to)
    }

    private fun drawMarkPoint(canvas: Canvas, pts: FloatArray, i: Int) {
        val x = pts[i * 2]
        val y = pts[i * 2 + 1]
        val r = 5f * density / 2f
        canvas.drawLine(x - r * 2, y, x + r * 2, y, primPaint)
        canvas.drawLine(x, y - r * 2, x, y + r * 2, primPaint)
        canvas.drawCircle(x, y, r, primPaint)
    }

    private fun drawPolyline(canvas: Canvas, pts: FloatArray, from: Int, to: Int, close: Boolean) {
        if (to - from < 2) return
        val path = pathOf(pts, from, to)
        if (close) path.close()
        canvas.drawPath(path, primPaint)
    }

    private fun drawFilled(canvas: Canvas, pts: FloatArray, from: Int, to: Int) {
        if (to - from < 3) return
        val path = pathOf(pts, from, to)
        path.close()
        canvas.drawPath(path, fillPaint)
    }

    private fun pathOf(pts: FloatArray, from: Int, to: Int): Path {
        val path = Path()
        path.moveTo(pts[from * 2], pts[from * 2 + 1])
        for (i in from + 1 until to) path.lineTo(pts[i * 2], pts[i * 2 + 1])
        return path
    }

    /** 测距线两端的垂直刻度（放射测量惯例：端点短线比纯线段更易读） */
    private fun drawEndCaps(canvas: Canvas, pts: FloatArray, a: Int, b: Int) {
        val ax = pts[a * 2]; val ay = pts[a * 2 + 1]
        val bx = pts[b * 2]; val by = pts[b * 2 + 1]
        val len = hypot(bx - ax, by - ay)
        if (len < 1e-3f) return
        val nx = -(by - ay) / len
        val ny = (bx - ax) / len
        val cap = 6f * density
        canvas.drawLine(ax - nx * cap, ay - ny * cap, ax + nx * cap, ay + ny * cap, primPaint)
        canvas.drawLine(bx - nx * cap, by - ny * cap, bx + nx * cap, by + ny * cap, primPaint)
    }

    private fun drawArrowHead(canvas: Canvas, pts: FloatArray, a: Int, b: Int) {
        val ax = pts[a * 2]; val ay = pts[a * 2 + 1]
        val bx = pts[b * 2]; val by = pts[b * 2 + 1]
        val len = hypot(bx - ax, by - ay)
        if (len < 1e-3f) return
        val ux = (bx - ax) / len
        val uy = (by - ay) / len
        val size = (10f * density).coerceAtMost(len * 0.6f)
        val wing = size * 0.5f
        canvas.drawLine(bx, by, bx - ux * size + uy * wing, by - uy * size - ux * wing, primPaint)
        canvas.drawLine(bx, by, bx - ux * size - uy * wing, by - uy * size + ux * wing, primPaint)
    }

    /** 长方体线框：8 顶点按 xyz 位掩码排列，12 条棱表见 OverlayKind.BOX_EDGES */
    private fun drawBox(canvas: Canvas, pts: FloatArray, from: Int) {
        val edges = OverlayKind.BOX_EDGES
        var e = 0
        while (e + 1 < edges.size) {
            val a = from + edges[e]
            val b = from + edges[e + 1]
            if (b * 2 + 1 >= pts.size) break
            canvas.drawLine(pts[a * 2], pts[a * 2 + 1], pts[b * 2], pts[b * 2 + 1], primPaint)
            e += 2
        }
    }

    /** 圆柱：Native 给出两个圆环（各 n 点），画两个环 + 对应母线 */
    private fun drawCapsule(canvas: Canvas, pts: FloatArray, from: Int, to: Int) {
        val total = to - from
        if (total < 4) return
        val ring = total / 2
        drawPolyline(canvas, pts, from, from + ring, close = true)
        drawPolyline(canvas, pts, from + ring, to, close = true)
        // 只画 4 条母线：全画会在小屏幕上糊成一团实心柱
        val step = (ring / 4).coerceAtLeast(1)
        var i = 0
        while (i < ring) {
            val a = from + i
            val b = from + ring + i
            canvas.drawLine(pts[a * 2], pts[a * 2 + 1], pts[b * 2], pts[b * 2 + 1], primPaint)
            i += step
        }
    }

    private fun drawScreenCircle(canvas: Canvas, pts: FloatArray, from: Int, prim: OverlayPrim) {
        val r = prim.widthPx.toFloat().coerceIn(1f, 4096f)
        canvas.drawCircle(pts[from * 2], pts[from * 2 + 1], r, primPaint)
    }

    /** 文字锚在末点：纯文字图元只有一个锚点，线框类图元锚在最后一个顶点 */
    private fun drawLabel(canvas: Canvas, prim: OverlayPrim, pts: FloatArray, from: Int, to: Int) {
        if (to - from <= 0) return
        val anchor = to - 1
        val x = pts[anchor * 2] + 8f * density
        var y = pts[anchor * 2 + 1] - 6f * density
        textPaint.color = if (Color.alpha(prim.color) > 200) prim.color else Color.WHITE
        textPaint.textSize = 12f * density * if (prim.kind == OverlayKind.TEXT) 1.05f else 0.95f
        val w = textPaint.measureText(prim.text)
        val lineH = textPaint.textSize * 1.25f
        // 与已经画过的标签压字就整行下移。求交用成员 scratch，不挪位时零分配；
        // 只有真正落位的标签才 new 一个 RectF 登记，即每帧分配数 <= 标签数
        // （真机 prims=33 时 avgDraw 0.46~1.49ms，见 UI_SPLIT_LAYOUT_REGRESSION_REPORT §6，
        //  没到需要在 PC-02 的 30FPS 预算里抠这一份的程度）
        var tries = 0
        while (tries < LABEL_AVOID_TRIES) {
            scratch.set(x, y - textPaint.textSize, x + w, y + textPaint.textSize * 0.35f)
            if (labelRects.none { RectF.intersects(it, scratch) }) {
                labelRects.add(RectF(scratch))
                break
            }
            y += lineH
            tries++
        }
        canvas.drawText(prim.text, x, y, textPaint)
    }

    private fun alpha(c: Int, a: Float): Int =
        Color.argb((a * 255).toInt().coerceIn(0, 255), Color.red(c), Color.green(c), Color.blue(c))

    // =========================================================================
    // 草稿与提示
    // =========================================================================

    private fun drawDraft(canvas: Canvas, d: Draft) {
        primPaint.color = d.color
        primPaint.strokeWidth = 2f * density
        fillPaint.color = alpha(d.color, 0.15f)

        // 已吸附的世界点（随相机）
        val wp = draftScreenPts
        val n = wp.size / 2
        if (n >= 1) {
            for (i in 0 until n) drawMarkPoint(canvas, wp, i)
            when (d.kind) {
                OverlayKind.POLYGON -> drawPolyline(canvas, wp, 0, n, close = true)
                OverlayKind.BOX -> if (n >= 8) drawBox(canvas, wp, 0)
                OverlayKind.POLYLINE, OverlayKind.LINE, OverlayKind.RING ->
                    drawPolyline(canvas, wp, 0, n, close = false)
            }
            if (d.radiusMm > 0.0 && n >= 1) {
                // 球形 ROI 预览：半径是世界毫米，这里用两向投影差近似换算成像素半径，
                // 只为拖拽时的即时反馈；精确轮廓由提交后的 Native 图元负责。
                val cx = wp[0]; val cy = wp[1]
                val rp = radiusPx(d.radiusMm)
                canvas.drawCircle(cx, cy, rp, primPaint)
            }
        }
        // 手指实时轨迹（屏幕系）
        val lp = d.livePath
        if (lp.size >= 4) {
            val path = Path()
            path.moveTo(lp[0], lp[1])
            var i = 2
            while (i + 1 < lp.size) {
                path.lineTo(lp[i], lp[i + 1])
                i += 2
            }
            canvas.drawPath(path, primPaint)
        }
    }

    /**
     * 世界毫米 -> 屏幕像素的粗略换算：投影器可用时拿两个相距 10mm 的点算比例，
     * 不可用（相机还没就绪）时退回 density。草稿预览用，不参与任何测量数值。
     */
    private fun radiusPx(mm: Double): Float {
        val p = projector ?: return (mm * density).toFloat().coerceIn(2f, 2000f)
        val probe = p.project(doubleArrayOf(0.0, 0.0, 0.0, 10.0, 0.0, 0.0))
            ?: return (mm * density).toFloat().coerceIn(2f, 2000f)
        if (probe.size < 6) return (mm * density).toFloat().coerceIn(2f, 2000f)
        val pxPer10Mm = hypot((probe[2] - probe[0]).toFloat(), (probe[3] - probe[1]).toFloat())
        val sx = if (surfaceW > 0 && width > 0) width.toFloat() / surfaceW else 1f
        val perMm = pxPer10Mm / 10f * sx
        return (mm * perMm).toFloat().coerceIn(2f, 4000f)
    }

    private fun drawHint(canvas: Canvas) {
        if (hint.isEmpty()) return
        textPaint.textSize = 13f * density
        textPaint.color = Color.rgb(0x00, 0xE5, 0xFF)
        canvas.drawText(hint, 10f * density, 20f * density, textPaint)
    }

    private companion object {
        val FLOAT0 = FloatArray(0)
        const val TAG = "CbctMeasureOverlay"

        /** PC-02 统计窗口长度（ms）：1s 内的 onDraw 次数即叠加层刷新帧率 */
        const val FPS_WINDOW_MS = 1000L

        /** 标签避让的最大下移次数：再挤就不挪了，宁可压字也不让文字飘出图元 */
        const val LABEL_AVOID_TRIES = 6
    }
}
