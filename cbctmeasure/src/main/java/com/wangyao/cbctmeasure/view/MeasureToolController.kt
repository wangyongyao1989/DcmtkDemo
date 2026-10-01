package com.wangyao.cbctmeasure.view

import android.os.SystemClock
import android.util.Log
import android.view.MotionEvent
import com.wangyao.cbctdeal.render.CbctVtkView
import com.wangyao.cbctmeasure.jni.AnnotationJni
import com.wangyao.cbctmeasure.jni.MeasureJni
import com.wangyao.cbctmeasure.jni.RoiJni
import com.wangyao.cbctmeasure.jni.SurgeryPlanJni
import com.wangyao.cbctmeasure.model.AnnotationItem
import com.wangyao.cbctmeasure.model.AnnotationType
import com.wangyao.cbctmeasure.model.ImplantItem
import com.wangyao.cbctmeasure.model.MeasurePlane
import com.wangyao.cbctmeasure.model.MeasureType
import com.wangyao.cbctmeasure.model.OverlayKind
import com.wangyao.cbctmeasure.model.OverlayPrim
import com.wangyao.cbctmeasure.model.PickHit
import com.wangyao.cbctmeasure.model.PickMode
import com.wangyao.cbctmeasure.model.RoiItem
import com.wangyao.cbctmeasure.model.RoiType
import com.wangyao.cbctmeasure.model.ToolState
import com.wangyao.cbctmeasure.model.WorldPoint
import org.json.JSONArray
import org.json.JSONObject
import kotlin.math.hypot

/**
 * 九态手势状态机（PRD 8.5，验收项 AC-10）。
 *
 * 职责边界：
 *   - 只"解释"手势 -> 世界坐标点 -> 调 JNI 提交；不算数值、不画图形；
 *   - 数值一律来自 core/（MeasureJni.addMeasure 内部重算后回传），
 *     界面显示的读数与报告/SR 里的数值同源；
 *   - 叠加层刷新走 syncOverlay()，投影由 MeasureOverlayView 完成。
 *
 * 与相机的关系（AC-10 的关键）：单指在测量态被本状态机消费，双指手势
 * 永远转发给 :cbctdeal 的 CbctVtkView，因此任何测量态下都能旋转/缩放，
 * 已提交的图形靠 reproject() 跟着相机走，不会"画上去就粘在屏幕上"。
 */
class MeasureToolController(
    private val host: Host,
    private val overlay: MeasureOverlayView,
) {

    /** 与宿主界面（Fragment / CbctMeasureView）的通信契约 */
    interface Host {
        /** 当前 VTK 渲染视图；未创建 Surface 时可以返回 null */
        fun vtkView(): CbctVtkView?

        /** Native 会话句柄；0 表示尚未绑定体数据 */
        fun sessionHandle(): Long

        /** 状态提示文案变化（同步到界面提示条） */
        fun onHintChanged(hint: String)

        /** 工具态变化（同步按钮高亮） */
        fun onStateChanged(state: Int)

        /** 有数据提交：结果列表需要重取 */
        fun onDataChanged()

        /**
         * A-01/A-02/A-04 这类需要用户输入文字的标注。
         * 宿主弹窗，确认后回调 done(text)；空串表示"只画图形不写字"。
         */
        fun promptAnnotationText(type: Int, points: List<WorldPoint>, done: (String) -> Unit)
    }

    /** 当前工具态 */
    var state: Int = ToolState.VIEW
        private set

    /**
     * 点选类测量态实际测哪一种。
     * PRD 8.5 把 M-01/M-02/M-03/M-06/M-07 归到同一种"点击取点"手势族，
     * 只是需要的点数与最终算子不同，所以 MEASURE_DISTANCE / MEASURE_ANGLE
     * 两个态共用这条提交路径，差别只在 requiredPoints()。
     */
    var measureType: Int = MeasureType.DISTANCE
        private set

    /** ANNOTATE 态下当前标注子类型（A-01 ~ A-07） */
    var annotationType: Int = AnnotationType.TEXT_LABEL
        private set

    /** NERVE_TRACE 态描记的目标路径（界面先建路径再进入本态） */
    var nervePathId: Int = 0
        private set

    /** 新测量 / 新标注的默认色，界面可切换 */
    var penColor: Int = DEFAULT_COLOR
        private set

    // ---- 进行中的草稿 ----
    private val pending = ArrayList<WorldPoint>()
    private val liveScreen = ArrayList<Float>(64)
    private var boxAnchor: WorldPoint? = null
    private var boxLive: WorldPoint? = null
    private var ringCenter: WorldPoint? = null
    private var sampling = false
    private var gestureBlocked = false
    private var hint = ""

    /** 提交读数/失败提示；非空时 updateHint() 不覆盖它，下一次 ACTION_DOWN 清空 */
    private var stickyMissHint: String? = null

    /** 上一次拉取的叠加层版本号：数据没变就不重复解析 JSON */
    private var overlaySerial = -1L

    /** 拖动描记的原始候选（命中点 + 相机深度），中值滤波后写进 pending */
    private val dragRaw = ArrayList<Pair<WorldPoint, Double>>()

    /** dragRaw 里已经过滤波处理的个数（前缀游标，避免重复处理同一采样） */
    private var dragFlushed = 0

    /** 拖动中连续未命中的采样数：用于放宽深度锁层窗口，见 appendDragPoint */
    private var dragMissRun = 0

    fun setPenColor(color: Int) {
        penColor = color
    }

    /**
     * 切换工具态。
     * @param nerveId NERVE_TRACE 态要写入的路径 id，其它态忽略
     */
    fun switchState(next: Int, nerveId: Int = 0) {
        if (next == state && (next != ToolState.NERVE_TRACE || nerveId == nervePathId)) {
            updateHint()
            return
        }
        abortDraft()
        state = next
        nervePathId = if (next == ToolState.NERVE_TRACE) nerveId else 0
        // 态与测量类型对齐：进入测角态就测角度，回到测距态默认两点距离
        measureType = when (next) {
            ToolState.MEASURE_ANGLE -> MeasureType.ANGLE
            ToolState.MEASURE_DISTANCE -> if (measureType == MeasureType.ANGLE) MeasureType.DISTANCE else measureType
            else -> measureType
        }
        host.onStateChanged(state)
        updateHint()
        pushDraft()
    }

    /** 指定测量类型（只在需要点选的测量态有意义） */
    fun setMeasureType(type: Int) {
        measureType = type
        abortDraft()
        updateHint()
        pushDraft()
    }

    /** 指定标注子类型（ANNOTATE 态） */
    fun setAnnotationType(type: Int) {
        annotationType = type
        abortDraft()
        updateHint()
        pushDraft()
    }

    /** 撤销最后一笔：丢掉最后一个待提交点 */
    fun undoPendingPoint() {
        if (pending.isNotEmpty()) pending.removeAt(pending.size - 1)
        liveScreen.clear()
        stickyMissHint = null   // 同 discardDraft()：按钮不是 view 上的 DOWN，残留的"没点中"提示要主动清
        updateHint()
        pushDraft()
    }

    /** 放弃当前草稿（不删除已提交的图形） */
    fun abortDraft() {
        pending.clear()
        liveScreen.clear()
        dragRaw.clear()
        dragFlushed = 0
        dragMissRun = 0
        boxAnchor = null
        boxLive = null
        ringCenter = null
        sampling = false
        overlay.setDraft(null)
    }

    /**
     * 「丢弃草稿」按钮专用：abortDraft() 只清数据不改文案，
     * 而内部工具切换路径（switchState/setMeasureType/setAnnotationType）都在调用后自己 updateHint()，
     * 于是按钮点完图形没了、提示却还停在「（6 点）」，用户以为没生效（本轮真机回归复现）。
     *
     * stickyMissHint 也要一起清：它是"上一次没点中"的残留提示，只在手指按下时才会被冲掉，
     * 而按钮不是 view 上的 DOWN，不清就会让页面继续显示"当前切面未拾取到点"。
     */
    fun discardDraft() {
        abortDraft()
        stickyMissHint = null
        updateHint()
        pushDraft()
    }

    /**
     * 「闭合路径」按钮：面积多边形原来只能靠点中起点闭合，容差是 2.0mm 世界距离，
     * 切面放大后连一个屏幕像素都不到，点不中就永远收不了尾。这里给一条不依赖命中起点的路径。
     * @return false 表示当前点数不足 3，草稿保持原样
     */
    fun closeAreaDraft(): Boolean {
        if (pending.size < 3) {
            setHint(if (pending.isEmpty()) "闭合无效：先在切面上点出至少 3 个点"
            else "闭合无效：还需 ${3 - pending.size} 个点（当前 ${pending.size} 点）")
            return false
        }
        val polygon = pending.toList()
        pending.clear()
        submitArea(polygon)
        pushDraft()
        return true
    }

    /** 提交后调用：刷新叠加层 + 通知列表 */
    fun notifyDataChanged() {
        syncOverlay()
        host.onDataChanged()
    }

    // =========================================================================
    // 触摸分发
    // =========================================================================

    /**
     * 本状态机是否要"吃掉"这次单指手势。
     * 浏览态、ROI 编辑态与双指手势返回 false，交给 CbctVtkView 旋转/缩放（AC-10）。
     */
    fun consumesSinglePointer(ev: MotionEvent): Boolean {
        if (ev.pointerCount > 1) return false
        if (host.sessionHandle() == 0L) return false
        return when (state) {
            ToolState.VIEW, ToolState.ROI_EDIT -> false
            else -> true
        }
    }

    /**
     * 处理单指手势；返回 true 表示已消费。
     * 相机姿态在每次事件里现取（平面/层位可能已被界面改过）。
     */
    fun onTouchEvent(ev: MotionEvent): Boolean {
        if (!consumesSinglePointer(ev)) return false
        if (ev.actionMasked == MotionEvent.ACTION_DOWN)
            Log.d(TAG, "consume single-pointer DOWN state=$state pending=${pending.size}")
        when (ev.actionMasked) {
            MotionEvent.ACTION_DOWN -> onDown(ev.x, ev.y)
            MotionEvent.ACTION_MOVE -> onMove(ev.x, ev.y)
            MotionEvent.ACTION_UP -> onUp(ev.x, ev.y)
            MotionEvent.ACTION_CANCEL -> {
                abortDraft()
                updateHint()
            }
            else -> return false
        }
        return true
    }

    private fun onDown(x: Float, y: Float) {
        stickyMissHint = null
        // 体积工具在 MPR 下不可用：单层切面上拖出的矩形法向跨度恒为 0，
        // 永远过不了 isBoxValid；与其让用户拖半天看到"区域太小"，不如在按下时就说清楚。
        mprUnsupportedHint()?.let {
            sampling = false
            gestureBlocked = true
            setHint(it)
            return
        }
        gestureBlocked = false
        liveScreen.clear()
        liveScreen.add(x); liveScreen.add(y)
        sampling = true
        when (state) {
            ToolState.MEASURE_VOLUME -> {
                // 拖拽定义裁剪盒：按下点先吸附成世界点作为 boxMin 锚
                val p = pickWorld(x, y, PickMode.SURFACE)
                boxAnchor = if (p.hit) p.point else null
                boxLive = if (p.hit) p.point else null
            }
            ToolState.ANNOTATE -> {
                if (annotationType == AnnotationType.RING) {
                    val p = pickWorld(x, y, PickMode.SURFACE)
                    if (p.hit) {
                        ringCenter = p.point
                        pending.clear(); pending.add(p.point)
                    }
                } else if (isDragTool(annotationType)) {
                    seedDragPoint(x, y)
                }
            }
            ToolState.MEASURE_DISTANCE, ToolState.MEASURE_ANGLE -> {
                if (isDragMeasure(measureType)) seedDragPoint(x, y)
            }
            else -> Unit     // 点选类工具在 UP 落点，避免"按下即误点"
        }
        updateHint()
        pushDraft()
    }

    /**
     * 拖动类工具（弧线长度 M-06、自由曲线标注 A-05）的第一个采样点必须在按下时落进 pending。
     * appendDragPoint() 以 pending.last() 为间距基准，只 clear 不播种的话后续每个 MOVE
     * 都会因为 lastOrNull()==null 直接 return —— 真机上表现为"拖半天一个点都没有"。
     */
    private fun seedDragPoint(x: Float, y: Float) {
        pending.clear()
        dragRaw.clear()
        dragFlushed = 0
        dragMissRun = 0
        val p = pickWorld(x, y, pickModeForCurrentTool())
        if (p.hit) pending.add(p.point)
    }

    private fun onMove(x: Float, y: Float) {
        if (!sampling) return
        val n = liveScreen.size
        if (n >= 2) {
            val lx = liveScreen[n - 2]; val ly = liveScreen[n - 1]
            if (hypot((x - lx).toDouble(), (y - ly).toDouble()) < MOVE_MIN_PX) return
        }
        liveScreen.add(x); liveScreen.add(y)
        when {
            state == ToolState.MEASURE_VOLUME -> {
                val p = pickWorld(x, y, PickMode.SURFACE)
                if (p.hit) boxLive = p.point
            }
            state == ToolState.ANNOTATE && isDragTool(annotationType) -> appendDragPoint(x, y)
            isArcDrag() -> appendDragPoint(x, y)
            else -> Unit
        }
        pushDraft()
    }

    private fun onUp(x: Float, y: Float) {
        sampling = false
        if (gestureBlocked) return      // 按下时已被 MPR 白名单拦下，别再用通用文案覆盖提示
        val hintBefore = hint
        when (state) {
            ToolState.MEASURE_VOLUME -> commitVolumeBox(x, y)
            ToolState.MEASURE_AREA -> commitAreaVertex(x, y)
            ToolState.NERVE_TRACE -> commitNervePoint(x, y)
            ToolState.IMPLANT_PLACE -> commitImplant(x, y)
            ToolState.ANNOTATE -> commitAnnotationGesture(x, y)
            ToolState.MEASURE_DISTANCE, ToolState.MEASURE_ANGLE -> commitMeasureGesture(x, y)
            else -> Unit
        }
        liveScreen.clear()
        // 提交路径给出了读数或失败原因时保留它（下一次按下才恢复通用引导文案）；
        // 否则按当前态刷新。不加这条会让"已取 0/2 点"盖掉"未拾取到点"，
        // 真机上表现为点了没任何反应（已踩过）。
        if (hint == hintBefore) updateHint() else stickyMissHint = hint
        pushDraft()
    }

    /** 弧线/自由曲线的逐点吸附：原始候选先进 dragRaw，中值滤波后才落进 pending */
    private fun appendDragPoint(x: Float, y: Float) {
        // 深度锁层：只在"最近几个采样深度中位数 ±窗口"内找骨面。
        // SURFACE 拾取取的是射线上第一个骨面，而躯干这类数据的骨面本身就是断续的：
        // 真机实测沿脊柱拖动时，相邻采样的相机距离会在 1302/1430/1516/1869mm 之间横跳
        // （射线从肋骨缝穿到其后几十~几百毫米的椎体）。不锁层时累积弦长被这些孤立尖峰
        // 放大到 2502mm；只按总长设上限（早期 20mm 版本）又几乎采不到点（16mm）。
        // 切面拾取（MPR 下描牙弓）没有"相机深度"可言，锁层会把每个采样都逼回骨面，
        // 所以 PLANE 模式直接跳过深度中位数滤波的中心值。
        val center = if (pickModeForCurrentTool() == PickMode.PLANE) null else dragDepthCenter()
        val p = if (center == null) pickWorld(x, y, pickModeForCurrentTool())
                else pickWorld(x, y, PickMode.SURFACE,
                        center - DRAG_DEPTH_WINDOW_MM, center + DRAG_DEPTH_WINDOW_MM)
        if (!p.hit) return
        if (dragRaw.size >= MAX_DRAG_CANDIDATES) return
        dragRaw.add(p.point to p.distanceMm)
        flushDragSamples(final = false)
    }

    /** 深度锁层的中心：最近 DEPTH_MEDIAN_TAP 个候选深度的中位数；无候选时返回 null */
    private fun dragDepthCenter(): Double? {
        if (dragRaw.isEmpty()) return null
        val tail = dragRaw.takeLast(DEPTH_MEDIAN_TAP * 2 + 1).map { it.second }.sorted()
        return tail[tail.size / 2]
    }

    /**
     * 深度中值滤波：把候选序列里"只有一两个采样跳到别的深度层"的孤立尖峰滤掉，
     * 保留连续的同层走行。取窗口深度中位数，再返回窗口里深度最接近中位数的那个
     * 采样点（同分时优先离中心近的），所以输出一定是真实命中过的骨面点，不会造出插值点。
     *
     * 右侧要等 DEPTH_MEDIAN_TAP 个前瞻样本才能凑满窗口，因此拖动过程中会滞后若干点；
     * 抬手时用 final=true 把尾巴补齐。
     */
    private fun flushDragSamples(final: Boolean) {
        val n = dragRaw.size
        val upto = if (final) n else n - DEPTH_MEDIAN_TAP
        while (dragFlushed < upto) {
            val i = dragFlushed
            val lo = maxOf(0, i - DEPTH_MEDIAN_TAP)
            val hi = minOf(n - 1, i + DEPTH_MEDIAN_TAP)
            val window = (lo..hi).map { dragRaw[it] }
            val medT = window.map { it.second }.sorted()[window.size / 2]
            var best = i
            var bestKey = Double.MAX_VALUE
            for (k in lo..hi) {
                val (pt, t) = dragRaw[k]
                // 主键 = 与中位深度的差；次键 = 与中心的索引距离（同差时取近者）
                val key = kotlin.math.abs(t - medT) * 1000.0 + kotlin.math.abs(k - i).toDouble()
                if (key < bestKey) { bestKey = key; best = k }
            }
            dragFlushed = i + 1
            acceptDragSample(dragRaw[best].first)
        }
    }

    /** 滤波后的采样点入列：仍要过最小/最大间距闸门，重合点与异常大跳都不要 */
    private fun acceptDragSample(pt: WorldPoint) {
        val last = pending.lastOrNull()
        // 按下点没命中骨面时（起笔落在软组织/背景上），从第一个命中的采样起笔，
        // 否则整条拖动都会因为"没有上一个点"而被丢掉，表现为拖动完全无反应
        if (last == null) {
            pending.add(pt)
            return
        }
        val d = distanceMm(last, pt)
        if (d < MIN_SEGMENT_MM) return
        if (d > MAX_DRAG_SEGMENT_MM) {
            Log.d(TAG, "drag reject d=%.1f pending=%d".format(d, pending.size))
            return
        }
        if (pending.size >= MAX_DRAG_POINTS) return
        pending.add(pt)
        Log.d(TAG, "drag accept #${pending.size} d=%.1f".format(d))
    }

    private fun isArcDrag(): Boolean =
        (state == ToolState.MEASURE_DISTANCE || state == ToolState.MEASURE_ANGLE) &&
                isDragMeasure(measureType)

    /**
     * 拖动描记式的测量类型：按住沿骨面/切面滑动采点，松开提交。
     * M-06 弧线长度与 S-07 牙弓弧线算法同源（累积弦长），共用一套采样链路，
     * 新增正畸量时不要再复制一遍 seedDragPoint / appendDragSample。
     */
    private fun isDragMeasure(type: Int): Boolean =
        type == MeasureType.ARC_LENGTH || type == MeasureType.ARCH_LENGTH

    // =========================================================================
    // 各态提交
    // =========================================================================

    /** M-01/M-02/M-03/M-06/M-07/M-08 + S-07~S-10：点够就走 addMeasure，数值由 core 计算 */
    private fun commitMeasureGesture(x: Float, y: Float) {
        if (isDragMeasure(measureType)) {
            flushDragSamples(final = true)   // 补齐中值滤波滞后的最后几个采样
            if (pending.size >= 2) submitMeasure(measureType, pending.toList()) else abortDraft()
            return
        }
        val p = pickWorld(x, y, pickModeForCurrentTool())
        if (!p.hit) {
            setHint("未拾取到点：请对准骨面或切面再点击")
            return
        }
        if (measureType == MeasureType.BONE_DENSITY) {
            submitDensityAt(p.point)
            return
        }
        pending.add(p.point)
        if (pending.size >= requiredPoints(measureType)) {
            val pts = pending.toList()
            pending.clear()
            submitMeasure(measureType, pts)
        }
    }

    /**
     * M-08 骨密度评估：PRD 5.1.2 规定输入是"ROI 内体素 HU 统计"，单点没有统计意义，
     * 所以这里按 R-04 球面 ROI 在落点建一个小球（默认 r=5mm、骨 HU 区间），
     * 再把 M-08 绑到该 ROI 上，由 core 出均值/标准差/最值。
     */
    private fun submitDensityAt(center: WorldPoint) {
        val roi = RoiItem(
            type = RoiType.SPHERE,
            name = "骨密度球 r=${DENSITY_SPHERE_RADIUS_MM}mm",
            color = penColor,
            sphereCenter = center,
            sphereRadius = DENSITY_SPHERE_RADIUS_MM,
        )
        val roiId = timed("addRoi 球面 r=${DENSITY_SPHERE_RADIUS_MM}mm") {
            RoiJni.addRoi(host.sessionHandle(), roi)
        }
        if (roiId <= 0) {
            setHint("骨密度评估需要球面 ROI，创建失败")
            return
        }
        submitMeasure(MeasureType.BONE_DENSITY, listOf(center), roiId)
    }

    /** M-04：拖出裁剪盒 -> 建 R-02 ROI -> 立即算体积 -> 生成测量项 */
    private fun commitVolumeBox(x: Float, y: Float) {
        val a = boxAnchor
        val picked = pickWorld(x, y, PickMode.SURFACE)
        val b = boxLive ?: if (picked.hit) picked.point else null
        boxAnchor = null
        boxLive = null
        if (a == null || b == null) {
            setHint("体积测量需要拖出一个有效区域")
            return
        }
        if (!isBoxValid(a, b)) {
            setHint("区域太小（每边需 >= ${MIN_BOX_EDGE_MM}mm），已取消")
            return
        }
        val roi = RoiItem(
            type = RoiType.BOX,
            name = "ROI 盒",
            color = penColor,
            boxMin = WorldPoint(minOf(a.x, b.x), minOf(a.y, b.y), minOf(a.z, b.z)),
            boxMax = WorldPoint(maxOf(a.x, b.x), maxOf(a.y, b.y), maxOf(a.z, b.z)),
        )
        val id = timed("addRoi 盒") { RoiJni.addRoi(host.sessionHandle(), roi) }
        if (id <= 0) {
            setHint("ROI 创建失败")
            return
        }
        submitMeasure(MeasureType.ROI_VOLUME, listOf(a, b), id)
    }

    /** M-05：MPR 切面上逐笔点出封闭多边形，点击起点附近闭合 */
    private fun commitAreaVertex(x: Float, y: Float) {
        val p = pickWorld(x, y, PickMode.PLANE)
        if (!p.hit) {
            setHint("当前切面未拾取到点")
            return
        }
        if (pending.size >= 3 && distanceMm(pending.first(), p.point) < CLOSE_TOLERANCE_MM) {
            val poly = pending.toList()
            pending.clear()
            submitArea(poly)
            return
        }
        pending.add(p.point)
        if (pending.size >= MAX_AREA_POINTS) {
            val poly = pending.toList()
            pending.clear()
            submitArea(poly)
        }
    }

    private fun submitArea(polygon: List<WorldPoint>) {
        if (polygon.size < 3) return
        val (plane, position) = currentPlane()
        val roi = RoiItem(
            type = RoiType.PLANE,
            name = "截面 ${polygon.size} 点",
            color = penColor,
            planeOrigin = polygon.first(),
            planeNormal = planeNormalOf(plane),
            polygon = polygon,
            plane = plane,
            planePosition = position,
        )
        val id = timed("addRoi 截面 ${polygon.size} 点") { RoiJni.addRoi(host.sessionHandle(), roi) }
        if (id <= 0) {
            setHint("截面 ROI 创建失败")
            return
        }
        submitMeasure(MeasureType.ROI_AREA, polygon, id)
    }

    /** S-01：点按骨面定入口点，参数用界面默认值，落点后立即整体重算 */
    private fun commitImplant(x: Float, y: Float) {
        val p = pickWorld(x, y, PickMode.SURFACE)
        if (!p.hit) {
            setHint("未拾取到骨面：请对准牙槽嵴点击")
            return
        }
        val handle = host.sessionHandle()
        val seq = SurgeryPlanJni.implantsOf(MeasureJni.dumpPlan(handle)).size + 1
        val name = "种植体 $seq"
        val id = SurgeryPlanJni.addImplant(
            handle,
            ImplantItem(
                name = name,
                entry = p.point,
                diaMm = DEFAULT_IMPLANT_DIA_MM,
                lengthMm = DEFAULT_IMPLANT_LEN_MM,
                depthMm = DEFAULT_IMPLANT_DEPTH_MM,
            )
        )
        if (id <= 0) {
            setHint("种植体创建失败")
            return
        }
        // S-06 的相邻间距依赖其它桩体位置，任何增删改都要整体重算
        SurgeryPlanJni.recomputePlan(handle)
        notifyDataChanged()
        setHint("已放置 $name，安全指标见方案列表")
    }

    /** A-xx 标注：区分点选类、拖拽类与像素类 */
    private fun commitAnnotationGesture(x: Float, y: Float) {
        when (annotationType) {
            AnnotationType.LINE, AnnotationType.ARROW -> {
                val p = pickWorld(x, y, PickMode.SURFACE)
                if (!p.hit) return
                pending.add(p.point)
                if (pending.size >= 2) {
                    val pts = pending.toList(); pending.clear()
                    askTextThenAdd(pts)
                }
            }
            AnnotationType.RING -> {
                val c = ringCenter
                val p = pickWorld(x, y, PickMode.SURFACE)
                ringCenter = null
                if (c == null || !p.hit) return
                val r = distanceMm(c, p.point)
                if (r < MIN_RING_RADIUS_MM) {
                    setHint("环形半径过小（<${MIN_RING_RADIUS_MM}mm）")
                    return
                }
                val (plane, position) = currentPlane()
                addAnnotation(
                    AnnotationItem(
                        type = AnnotationType.RING,
                        points = listOf(c),
                        radiusMm = r,
                        color = penColor,
                        plane = plane,
                        planePosition = position,
                    )
                )
            }
            AnnotationType.SCREENSHOT -> {
                // A-07 存渲染表面的像素坐标：不进三维叠加，只参与报告合成
                val pt = toDisplayPoint(x, y) ?: return
                addAnnotation(
                    AnnotationItem(
                        type = AnnotationType.SCREENSHOT,
                        points = listOf(WorldPoint(pt[0], pt[1], 0.0)),
                        radiusMm = SCREENSHOT_RADIUS_PX,
                        color = penColor,
                    )
                )
            }
            AnnotationType.FREE_CURVE -> {
                flushDragSamples(final = true)   // 补齐中值滤波滞后的最后几个采样
                if (pending.size >= 2) {
                    val pts = pending.toList(); pending.clear()
                    askTextThenAdd(pts)
                }
            }
            else -> {
                // A-01 文字标签 / A-06 截面标注：单点即提交
                val mode = if (annotationType == AnnotationType.MPR_SLICE) PickMode.PLANE else PickMode.SURFACE
                val p = pickWorld(x, y, mode)
                if (!p.hit) return
                val (plane, position) = currentPlane()
                if (annotationType == AnnotationType.MPR_SLICE) {
                    // A-06 允许在当前层连续画多段：起点在 DOWN 已清，这里把新点续上
                    pending.add(p.point)
                    val pts = pending.toList()
                    pending.clear()
                    askTextThenAdd(pts, plane, position)
                } else {
                    askTextThenAdd(listOf(p.point), plane, position)
                }
            }
        }
    }

    private fun askTextThenAdd(pts: List<WorldPoint>, plane: Int = -1, position: Int = 0) {
        val usePlane = if (plane >= 0) plane else currentPlane().first
        val type = annotationType
        host.promptAnnotationText(type, pts) { text ->
            addAnnotation(
                AnnotationItem(
                    type = type,
                    text = text,
                    points = pts,
                    color = penColor,
                    plane = usePlane,
                    planePosition = position,
                    radiusMm = if (type == AnnotationType.RING) 3.0 else 0.0,
                )
            )
        }
    }

    private fun addAnnotation(item: AnnotationItem) {
        val id = AnnotationJni.addAnnotation(host.sessionHandle(), item)
        if (id <= 0) setHint("标注创建失败") else notifyDataChanged()
    }

    /** NERVE_TRACE：当前层一个点，逐层描记（S-05 依据） */
    private fun commitNervePoint(x: Float, y: Float) {
        if (nervePathId <= 0) {
            setHint("请先新建神经管路径再描记")
            return
        }
        val p = pickWorld(x, y, PickMode.PLANE)
        if (!p.hit) {
            setHint("当前层未拾取到点")
            return
        }
        if (!SurgeryPlanJni.appendNervePoint(host.sessionHandle(), nervePathId, p.point)) {
            setHint("描记失败：路径不存在")
            return
        }
        notifyDataChanged()
    }

    /** 提交一条测量：数值、单位、明细全部来自 core */
    private fun submitMeasure(type: Int, points: List<WorldPoint>, roiId: Int = 0) {
        val handle = host.sessionHandle()
        val name = MeasureType.label(type)
        val id = timed("addMeasure $name") {
            MeasureJni.addMeasure(handle, type, MeasureJni.flatten(points), roiId, name, penColor, "")
        }
        if (id <= 0) {
            setHint("测量提交失败：$name")
            return
        }
        abortDraft()
        notifyDataChanged()
        val item = MeasureJni.records(handle).firstOrNull { it.id == id }
        setHint(if (item != null && !item.isFailed()) "$name ${item.displayValue()}" else "$name 待重算")
    }

    // =========================================================================
    // 拾取 / 相机
    // =========================================================================

    private fun pickModeForCurrentTool(): Int = when {
        state == ToolState.MEASURE_AREA -> PickMode.PLANE
        state == ToolState.NERVE_TRACE -> PickMode.PLANE
        state == ToolState.ANNOTATE && annotationType == AnnotationType.MPR_SLICE -> PickMode.PLANE
        // MPR 切面态：屏幕上是一张二维切面，"点一下"的语义就是"当前层位的这一像素"。
        // 三维射线拾取在这里根本不可用：MPR 相机视线恒为 -Z，ResetCameraClippingRange
        // 对退化的 2D Prop 给近一个巨大的裁剪窗，真机实测射线起点在 z=-1.69e5 且朝 -Z
        // 走，永远进不了体积（"ray misses volume"），沿射线找骨面/HU 区间全部 miss；
        // 冠状/矢状面的世界法线（Y/X）还与视线平行，平面求交也无解。
        // 所以 MPR 下一律走平面拾取（含 S-07~S-10 正畸量：PRD 5.3.3 要求牙弓在冠状面、
        // 覆合覆盖"需正侧位切面配合"）。VR 模式仍按骨面拾取。
        isMprMode() -> PickMode.PLANE
        measureType == MeasureType.HU_SAMPLE || measureType == MeasureType.BONE_DENSITY -> PickMode.SURFACE
        else -> PickMode.SURFACE
    }

    /** 当前是否处于 MPR 渲染模式（快照第 0 位是 CbctVtkJni 的模式索引） */
    private fun isMprMode(): Boolean {
        val snap = host.vtkView()?.renderSnapshot() ?: return false
        return snap.isNotEmpty() && snap[0].toInt() == MPR_MODE_INDEX
    }

    /**
     * MPR 下的工具黑名单：结构上必须在三维体里完成、单层切面上无解的工具，
     * 在按下时就把原因讲清楚，而不是等用户拖完再报"区域太小"。
     *
     * 体积（M-04）用拖动矩形定义三维裁剪盒；MPR 下拾取被锁在当前层位
     * （见 [pickModeForCurrentTool] 的说明），盒的法向跨度恒为 0，
     * [isBoxValid] 必然失败。神经管描记（S 系列）这里保留平面语义，不拦。
     */
    private fun mprUnsupportedHint(): String? =
        if (state == ToolState.MEASURE_VOLUME && isMprMode()) MPR_VOLUME_HINT else null

    /** PC-01 取证用：毫秒级耗时，与日志里的 cost= 字段同一口径 */
    private fun costMs(t0: Long): Double = (SystemClock.elapsedRealtime() - t0).toDouble()

    /**
     * PC-01 取证用：把一次 Native 计算包成"执行 + 打一行耗时"。
     * 体积/面积/骨密度这几条链路的重计算都发生在 JNI 调用里（体素统计在 core），
     * 所以只在这里计时，不去改 JNI 桥接层的声明式职责。
     */
    private inline fun <T> timed(tag: String, block: () -> T): T {
        val t0 = SystemClock.elapsedRealtime()
        val r = block()
        Log.d(TAG, "%s cost=%.1fms".format(tag, costMs(t0)))
        return r
    }

    /**
     * 屏幕像素 -> 世界射线 -> Native 拾取。
     * [tMinMm]/[tMaxMm] 只对骨面拾取生效：把搜索限制在沿射线的该深度区间（拖动描记锁层），
     * 默认 0/0 = 不限，与改动前的"第一个骨面"语义一致。
     */
    private fun pickWorld(
        x: Float, y: Float, modeIn: Int,
        tMinMm: Double = 0.0, tMaxMm: Double = 0.0,
    ): PickHit {
        // PC-01 取证：一次取点的交互耗时 = 渲染快照(与渲染线程同步) + 投影 + 核心拾取。
        // 只在这条已有日志上追加 cost，不新增日志行，避免拖动描记时刷屏。
        val t0 = SystemClock.elapsedRealtime()
        val vtk = host.vtkView() ?: return PickHit.MISS.also { Log.w(TAG, "pick: no vtk view") }
        val snap = vtk.renderSnapshot() ?: return PickHit.MISS.also { Log.w(TAG, "pick: no render snapshot") }
        val dx = toDisplayX(x, vtk, snap)
        val dy = toDisplayY(y, vtk, snap)
        var mode = modeIn
        // MPR 下一律收敛为平面拾取：SURFACE/THROUGH_BONE/HU_RANGE/ROI_SURFACE 都要沿
        // 射线找东西，而 MPR 的射线根本进不了体积（见 pickModeForCurrentTool 的实测数据），
        // 在切面上点一下的正确语义就是"当前层位的这一像素"。放在这里统一判断，
        // 调用点（种植体、标注、ROI 向导）就不必各自关心渲染模式。
        // NERVE 例外：它要的是神经管折线上的吸附点，锁到切面会得到错误语义，保留 VR 专用。
        if (snap[0].toInt() == MPR_MODE_INDEX && mode != PickMode.PLANE && mode != PickMode.NERVE) {
            mode = PickMode.PLANE
        }
        val ray: DoubleArray
        // MPR 平面拾取直算：显示点真正对应的是"切面图像的行列"，由渲染器给出该行列的
        // 世界毫米（冠状/矢状面只有这一条路可走，见 pickModeForCurrentTool 的注释）。
        // 拿到点后再用一条沿该面法线的 SLICE_PICK_NUDGE_MM 短射线交给核心：
        // pickPlane 求交会落在同一位置（法向坐标由核心按层位重算，面内坐标原样保留），
        // 于是体素索引与 HU 仍然由核心统一给出，不在 Kotlin 里另算一套。
        if (snap[0].toInt() == MPR_MODE_INDEX && mode == PickMode.PLANE) {
            val w = vtk.displayToSliceWorld(dx, dy)
            if (w == null || w.size < 3) {
                Log.d(TAG, "pick disp=($dx,$dy) mode=PLANE plane=${snap[1].toInt()}/${snap[2].toInt()} " +
                        "-> hit=false outside current slice cost=%.1fms".format(costMs(t0)))
                return PickHit.MISS
            }
            val n = planeNormalOf(snap[1].toInt())
            ray = doubleArrayOf(
                w[0] - n.x * SLICE_PICK_NUDGE_MM, w[1] - n.y * SLICE_PICK_NUDGE_MM,
                w[2] - n.z * SLICE_PICK_NUDGE_MM, n.x, n.y, n.z
            )
        } else {
            ray = vtk.displayToRay(dx, dy)
                ?: return PickHit.MISS.also { Log.w(TAG, "pick: displayToRay null at view($x,$y)") }
        }
        val (plane, position) = currentPlane()
        val req = JSONObject()
        try {
            val arr = JSONArray()
            for (v in ray) arr.put(v)
            req.put("ray", arr)
            req.put("mode", mode)
            req.put("plane", plane)
            req.put("position", position)
            req.put("huThreshold", BONE_HU_THRESHOLD)
            req.put("skip", if (mode == PickMode.THROUGH_BONE) 1 else 0)
            req.put("toleranceMm", NERVE_TOLERANCE_MM)
            if (tMinMm > 0.0) req.put("tMin", tMinMm)
            if (tMaxMm > 0.0) req.put("tMax", tMaxMm)
        } catch (e: Exception) {
            return PickHit.MISS
        }
        return try {
            val hit = PickHit.from(JSONObject(MeasureJni.pick(host.sessionHandle(), req.toString())))
            // 真机回归时"点了没反应"必须能一眼分清是投影、拾取还是提交哪一段出的问题。
            // 一次拖动会打几十条，所以只留紧凑字段（射线本身可由 disp + 快照复算）
            Log.d(TAG, "pick disp=($dx,$dy) mode=$mode plane=$plane/$position " +
                    "win=${if (tMinMm > 0.0) "%.0f~%.0f".format(tMinMm, tMaxMm) else "-"} " +
                    "-> hit=${hit.hit} ${hit.describe}" +
                    if (hit.hit) " t=%.1f".format(hit.distanceMm) else "" +
                    " cost=%.1fms".format(costMs(t0)))
            hit
        } catch (e: Exception) {
            Log.w(TAG, "pick failed", e)
            PickHit.MISS
        }
    }

    /** [plane, position]：MPR 态取渲染快照，VR 态按横断面兜底 */
    private fun currentPlane(): Pair<Int, Int> {
        val snap = host.vtkView()?.renderSnapshot() ?: return MeasurePlane.AXIAL to 0
        return if (snap[0].toInt() == MPR_MODE_INDEX) snap[1].toInt() to snap[2].toInt()
        else MeasurePlane.AXIAL to 0
    }

    private fun planeNormalOf(plane: Int): WorldPoint = when (plane) {
        MeasurePlane.CORONAL -> WorldPoint(0.0, 1.0, 0.0)
        MeasurePlane.SAGITTAL -> WorldPoint(1.0, 0.0, 0.0)
        else -> WorldPoint(0.0, 0.0, 1.0)
    }

    private fun toDisplayX(x: Float, view: CbctVtkView, snap: DoubleArray): Double =
        if (snap.size > 3 && snap[3] > 0 && view.width > 0) x.toDouble() * snap[3] / view.width
        else x.toDouble()

    private fun toDisplayY(y: Float, view: CbctVtkView, snap: DoubleArray): Double =
        if (snap.size > 4 && snap[4] > 0 && view.height > 0) y.toDouble() * snap[4] / view.height
        else y.toDouble()

    /** 视图像素 -> 渲染表面像素（A-07 截图像素标注用） */
    private fun toDisplayPoint(x: Float, y: Float): DoubleArray? {
        val vtk = host.vtkView() ?: return null
        val snap = vtk.renderSnapshot() ?: return null
        return doubleArrayOf(toDisplayX(x, vtk, snap), toDisplayY(y, vtk, snap))
    }

    // =========================================================================
    // 叠加层同步
    // =========================================================================

    /**
     * 拉取叠加层并投影。
     *
     * 调用顺序有讲究：先 overlayJson()（Native 内部按 dirty/参数决定是否重建并递增版本号），
     * 再 overlayVersion()，最后 overlayPoints()。反过来先读版本号会拿到"重建前"的旧值，
     * 导致提交新测量后界面不刷新。
     */
    fun syncOverlay() {
        val handle = host.sessionHandle()
        val vtk = host.vtkView()
        if (handle == 0L || vtk == null || !vtk.isReady) {
            overlay.reproject()
            return
        }
        val snap = vtk.renderSnapshot()
        if (snap != null && snap.size > 4) overlay.setSurfaceSize(snap[3].toInt(), snap[4].toInt())
        val (plane, position) = currentPlane()
        val primsJson = MeasureJni.overlayJson(handle, plane, position, false)
        val version = MeasureJni.overlayVersion(handle)
        if (version != overlaySerial) {
            overlaySerial = version
            val (prims, _) = OverlayPrim.parse(primsJson)
            overlay.setData(prims, MeasureJni.overlayPoints(handle))
        } else {
            overlay.reproject()
        }
    }

    /** 相机变化（旋转/缩放/切层）时只需重投影，不必回 Native 取数据 */
    fun reprojectOnly() {
        overlay.reproject()
    }

    private fun pushDraft() {
        overlay.setDraft(buildDraft())
    }

    /** 把当前草稿换成叠加层能画的数据；无可画内容时返回 null */
    private fun buildDraft(): MeasureOverlayView.Draft? {
        val live = FloatArray(liveScreen.size)
        for (i in liveScreen.indices) live[i] = liveScreen[i]
        return when {
            state == ToolState.MEASURE_VOLUME -> {
                val a = boxAnchor ?: return if (pending.isEmpty()) null else draftFromPending(live)
                MeasureOverlayView.Draft(
                    worldPoints = boxCorners(a, boxLive ?: a),
                    livePath = live,
                    color = penColor,
                    kind = OverlayKind.BOX,
                )
            }
            state == ToolState.ANNOTATE && annotationType == AnnotationType.RING && ringCenter != null ->
                MeasureOverlayView.Draft(
                    worldPoints = flatten(listOf(ringCenter!!)),
                    livePath = live,
                    color = penColor,
                    kind = OverlayKind.RING,
                )
            pending.isNotEmpty() -> draftFromPending(live)
            else -> null
        }
    }

    private fun draftFromPending(live: FloatArray): MeasureOverlayView.Draft =
        MeasureOverlayView.Draft(
            worldPoints = flatten(pending),
            livePath = live,
            color = penColor,
            kind = when {
                state == ToolState.MEASURE_AREA -> OverlayKind.POLYGON
                state == ToolState.ANNOTATE && isDragTool(annotationType) -> OverlayKind.POLYLINE
                isArcDrag() -> OverlayKind.POLYLINE
                else -> OverlayKind.POINT
            },
        )

    private fun flatten(points: List<WorldPoint>): DoubleArray = MeasureJni.flatten(points)

    /** 8 顶点按 xyz 位掩码排列，与 OverlayKind.BOX_EDGES 的棱表约定一致 */
    private fun boxCorners(a: WorldPoint, b: WorldPoint): DoubleArray {
        val lo = WorldPoint(minOf(a.x, b.x), minOf(a.y, b.y), minOf(a.z, b.z))
        val hi = WorldPoint(maxOf(a.x, b.x), maxOf(a.y, b.y), maxOf(a.z, b.z))
        val out = DoubleArray(24)
        for (n in 0 until 8) {
            out[n * 3] = if (n and 1 != 0) hi.x else lo.x
            out[n * 3 + 1] = if (n and 2 != 0) hi.y else lo.y
            out[n * 3 + 2] = if (n and 4 != 0) hi.z else lo.z
        }
        return out
    }

    /** 各测量类型需要的点选数（0 表示不是点选式） */
    fun requiredPoints(type: Int): Int = when (type) {
        MeasureType.DISTANCE -> 2
        MeasureType.ANGLE -> 3
        MeasureType.POINT_TO_LINE -> 3
        MeasureType.HU_SAMPLE -> 1
        MeasureType.BONE_DENSITY -> 1
        MeasureType.OVERBITE -> 2                     // S-10 上、下切牙切缘
        MeasureType.TOOTH_ANGULATION -> 4             // S-08 两颗牙长轴各 2 点
        MeasureType.MIDLINE_OFFSET -> 4               // S-09 上下中线各 2 点
        else -> 0                                     // 体积/面积/弧线等为拖动或 ROI 式
    }

    // =========================================================================
    // 提示文案
    // =========================================================================

    private fun updateHint() {
        // 拾取失败的提示要留到下一次按下，否则 onUp 末尾这次刷新会把它冲掉，
        // 用户只看到"已取 0/2"却不知道刚才那一下没命中
        if (stickyMissHint != null) return
        val need = requiredPoints(measureType)
        val text = when (state) {
            ToolState.VIEW -> "浏览：单指旋转，双指缩放/平移"
            ToolState.MEASURE_DISTANCE -> when (measureType) {
                MeasureType.ARC_LENGTH -> "按住并沿骨面拖动描记弧线，松开提交"
                MeasureType.ARCH_LENGTH -> "S-07 牙弓弧线：在冠状面沿牙弓拖动描记，松开提交"
                MeasureType.HU_SAMPLE -> "点击待测点取 HU 值"
                MeasureType.BONE_DENSITY -> "点击 ROI 内的骨小梁区域取骨密度"
                MeasureType.POINT_TO_LINE -> "已取 ${pending.size}/$need 点：依次点线的两端，再点到线的点"
                MeasureType.TOOTH_ANGULATION ->
                    "S-08 排列角度：先点第一颗牙长轴两点，再点第二颗牙长轴两点（已取 ${pending.size}/$need）"
                MeasureType.MIDLINE_OFFSET ->
                    "S-09 中线偏移：依次点上颌中线 2 点、下颌中线 2 点（已取 ${pending.size}/$need）"
                MeasureType.OVERBITE ->
                    "S-10 覆合/覆盖：点上切牙切缘、再点下切牙切缘（已取 ${pending.size}/$need）"
                else -> "已取 ${pending.size}/$need 点：" +
                        if (pending.size < need) "继续点击" else "点数已满，正在提交"
            }
            ToolState.MEASURE_ANGLE -> "依次点击 A / B(顶点) / C 三点（已取 ${pending.size}/3）"
            ToolState.MEASURE_VOLUME ->
                if (isMprMode()) MPR_VOLUME_HINT else "按住拖动定义裁剪盒，松开自动计算体积"
            ToolState.MEASURE_AREA ->
                // 起点 2.0mm 容差在放大视图里不到一个屏幕像素，所以把「闭合路径」按钮写进提示，
                // 否则用户只能反复点起点试错（本轮真机回归反馈）
                "在切面上逐笔点出封闭路径（${pending.size} 点），点击起点闭合，或按「闭合路径」直接提交"
            ToolState.ROI_EDIT -> "ROI 编辑：用参数面板调整，界面刷新叠加层"
            ToolState.IMPLANT_PLACE -> "点击牙槽嵴顶放置种植体入口点"
            ToolState.ANNOTATE -> when (annotationType) {
                AnnotationType.RING -> "点击圆心，再拖动到边缘确定半径"
                AnnotationType.SCREENSHOT -> "点击在截图上打一个圈注标记（像素坐标）"
                AnnotationType.FREE_CURVE -> "按住拖动绘制自由曲线"
                AnnotationType.MPR_SLICE -> "在当前切面点击放置截面标注"
                AnnotationType.LINE, AnnotationType.ARROW -> "点击${AnnotationType.label(annotationType)}的两个端点"
                else -> "点击放置${AnnotationType.label(annotationType)}"
            }
            ToolState.NERVE_TRACE -> "在当前层点击描记神经管，切换层位继续"
            else -> hint
        }
        setHint(text)
    }

    private fun setHint(text: String) {
        hint = text
        overlay.setHint(text)
        host.onHintChanged(text)
    }

    private fun isDragTool(type: Int): Boolean = type == AnnotationType.FREE_CURVE

    /** 裁剪盒有效性：三轴各自的跨度都要过最小边长，否则统计没有意义 */
    private fun isBoxValid(a: WorldPoint, b: WorldPoint): Boolean =
        kotlin.math.abs(a.x - b.x) >= MIN_BOX_EDGE_MM &&
                kotlin.math.abs(a.y - b.y) >= MIN_BOX_EDGE_MM &&
                kotlin.math.abs(a.z - b.z) >= MIN_BOX_EDGE_MM

    private fun distanceMm(a: WorldPoint, b: WorldPoint): Double =
        hypot(hypot(a.x - b.x, a.y - b.y), a.z - b.z)

    companion object {
        /** 状态机诊断 TAG：真机回归时区分"事件没到状态机"和"拾取没命中" */
        const val TAG = "CbctMeasureCtrl"

        /** MPR 渲染模式索引，与 CbctVtkJni.MODE_MPR 一致 */
        const val MPR_MODE_INDEX = 1

        /** 骨面拾取的 HU 门限，与 core 的 PickRequest.huThreshold 默认值同源 */
        const val BONE_HU_THRESHOLD = 200.0

        /** 相邻采样点最小间距（世界 mm）：太小会产生一堆重合点，射线也白算 */
        const val MIN_SEGMENT_MM = 0.8

        /** 拖拽描记的顶点上限：再多拾取耗时和叠加顶点数都不划算 */
        const val MAX_DRAG_POINTS = 80
        const val MAX_AREA_POINTS = 120

        /**
         * 拖动采样的单段跨度上限（世界 mm）：只用来挡真正的异常值。
         * 锁层之后相邻命中点仍可能隔着一段"采不到骨面"的空档（弦要跨过它才连得上），
         * 早期取 60mm 会让轨迹在空档后彻底停住，真机实测弧长只剩 31mm。
         */
        const val MAX_DRAG_SEGMENT_MM = 250.0

        /**
         * 拖动描记的深度锁层窗口（mm）：新采样只在"最近若干候选深度中位数 ±该值"内找骨面。
         * 依据真机实测：同一张骨面的相邻采样深度差 5~10mm，肋骨缝穿到其后椎体差 100~500mm，
         * 取 45mm 既不会砍掉正常走行，又能挡住大部分跨层跳变（剩下的由中值滤波收尾）。
         */
        const val DRAG_DEPTH_WINDOW_MM = 45.0

        /** 连续未命中时深度窗口的最大放宽倍数（窗口 = 基础值 × 该倍数） */
        const val DRAG_WINDOW_MAX_GROWTH = 4

        /** 深度中值滤波的半径（单侧采样个数），即 5 点窗口 */
        const val DEPTH_MEDIAN_TAP = 2

        /** 一次拖动最多保留的原始候选数（滤波后入列的顶点上限是 MAX_DRAG_POINTS） */
        const val MAX_DRAG_CANDIDATES = MAX_DRAG_POINTS * 6

        /** M-08 落点自动建的 R-04 球面 ROI 半径（mm）：种植体周围骨密度评估的常用取样体积 */
        const val DENSITY_SPHERE_RADIUS_MM = 5.0

        /** 体积裁剪盒每轴最小跨度（PRD 6 的体素级下限） */
        const val MIN_BOX_EDGE_MM = 1.0
        /** MPR 单层切面拖不出三维裁剪盒（见 [mprUnsupportedHint]） */
        const val MPR_VOLUME_HINT = "体积测量需三维框选：请切到 VR 体绘制模式（MPR 单层无法定义裁剪盒）"

        /** 面积路径闭合判定半径 */
        const val CLOSE_TOLERANCE_MM = 2.0

        /** 环形标注最小半径 */
        const val MIN_RING_RADIUS_MM = 0.5

        /** 神经管描记的容差（选点时吸附到折线附近） */
        const val NERVE_TOLERANCE_MM = 2.0

        /**
         * MPR 平面拾取把"渲染器算出的面内世界点"送回核心时用的法向起跳距离（mm）。
         * 核心 pickPlane 要求 t > 0，且它按自己的层位重算法向坐标，所以只要
         * 该距离大于两侧 spacing 的浮点误差即可；取 5mm 远大于任何 1-ULP 偏差，
         * 又远小于体数据尺度，不会跳到相邻结构。
         */
        const val SLICE_PICK_NUDGE_MM = 5.0

        /** A-07 截图标注的圈注半径（像素） */
        const val SCREENSHOT_RADIUS_PX = 24.0

        /** 手指移动的最小采样间隔（像素） */
        const val MOVE_MIN_PX = 8f

        /** 新测量/新标注的默认描边色（与项目深色背景搭配的青色） */
        const val DEFAULT_COLOR = 0xFF00E5FF.toInt()

        // S-01 的植入体默认规格：界面参数面板会覆盖这些值
        const val DEFAULT_IMPLANT_DIA_MM = 4.0
        const val DEFAULT_IMPLANT_LEN_MM = 11.0
        const val DEFAULT_IMPLANT_DEPTH_MM = 10.0
    }
}
