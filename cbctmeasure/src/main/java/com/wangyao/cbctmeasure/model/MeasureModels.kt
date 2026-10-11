package com.wangyao.cbctmeasure.model

import org.json.JSONArray
import org.json.JSONObject
import java.util.Locale

/**
 * 测量数据的 Kotlin 侧镜像。
 *
 * 为什么用 org.json 而不是 Gson/kotlinx.serialization：
 * PRD 4.1 要求 Phase 1 不引入新的第三方依赖，而 android.json 是 framework 自带的，
 * 零新增 APK 体积；字段名与 Native 侧 MeasurementManager / AnnotationStore 的
 * public static 映射一一对应（跨语言只认字段名，不认顺序）。
 *
 * 数值一律按 Native 回传为准：本层只做展示格式化，不做任何二次计算，
 * 避免"界面上的数值"和"报告/SR 里的数值"来自两处。
 */

/** 世界坐标点（毫米，原点为体数据首体素中心，见 core/MeasureTypes.h 的坐标约定） */
data class WorldPoint(val x: Double, val y: Double, val z: Double) {

    fun toJsonArray(): JSONArray = JSONArray().put(x).put(y).put(z)

    /**
     * 是否恰为原点。原生侧把"零向量"当作"没有这个量"（冠中心、冠长轴都这样表达），
     * UI 过滤候选对象时必须用同一个判据，否则会出现"列表里有 ROI #3、原生却解析不出参考"。
     */
    fun isZero(): Boolean = x == 0.0 && y == 0.0 && z == 0.0

    companion object {
        val ZERO = WorldPoint(0.0, 0.0, 0.0)

        fun from(a: JSONArray?): WorldPoint =
            if (a == null || a.length() < 3) ZERO
            else WorldPoint(a.optDouble(0, 0.0), a.optDouble(1, 0.0), a.optDouble(2, 0.0))

        fun list(a: JSONArray?): List<WorldPoint> {
            if (a == null) return emptyList()
            val out = ArrayList<WorldPoint>(a.length())
            for (i in 0 until a.length()) {
                val p = a.optJSONArray(i) ?: continue   // 半截数组直接跳过，不让整条记录作废
                out.add(from(p))
            }
            return out
        }

        fun jsonArray(points: List<WorldPoint>): JSONArray =
            JSONArray().apply { points.forEach { put(it.toJsonArray()) } }
    }
}

/** 一条测量记录（M-01 ~ M-08） */
data class MeasureItem(
    val id: Int,
    val type: Int,
    val name: String,
    val note: String,
    val color: Int,
    val points: List<WorldPoint>,
    val value: Double,
    val unit: String,
    val roiId: Int,
    val visible: Boolean,
    val createdAt: Long,
    /** 派生明细（骨密度的均值/标准差、失败原因等）；原样保留，UI 按需取字段 */
    val detail: JSONObject?,
) {
    /** 结果文本：按 PRD 6 的精度指标决定小数位 */
    fun displayValue(): String = String.format("%.${MeasureType.decimals(type)}f %s", value, unit)

    /** detailJson.error 非空表示"待重算"（体数据未绑定或点数不足） */
    fun errorText(): String = detail?.optString("error").orEmpty()

    fun isFailed(): Boolean = errorText().isNotEmpty()

    companion object {
        fun from(j: JSONObject): MeasureItem = MeasureItem(
            id = j.optInt("id"),
            type = j.optInt("type", MeasureType.DISTANCE),
            name = j.optString("name"),
            note = j.optString("note"),
            color = j.optLong("color", 0xFF00E5FFL).toInt(),
            points = WorldPoint.list(j.optJSONArray("points")),
            value = j.optDouble("value", 0.0),
            unit = j.optString("unit", "mm"),
            roiId = j.optInt("roiId"),
            visible = j.optBoolean("visible", true),
            createdAt = j.optLong("createdAt"),
            detail = j.optJSONObject("detail"),
        )

        fun list(root: JSONObject?): List<MeasureItem> {
            val arr = root?.optJSONArray("records") ?: return emptyList()
            return (0 until arr.length()).map { from(arr.getJSONObject(it)) }
        }
    }
}

/** ROI 定义（R-01 ~ R-05）；字段按 type 取用，未用到的保持默认值即可 */
data class RoiItem(
    val id: Int = 0,
    val type: Int = RoiType.HU_THRESHOLD,
    val name: String = "",
    val color: Int = 0xFFFFC400.toInt(),
    val visible: Boolean = true,
    val opacity: Double = 0.35,
    val huMin: Double = 200.0,
    val huMax: Double = 3000.0,
    val boxMin: WorldPoint = WorldPoint.ZERO,
    val boxMax: WorldPoint = WorldPoint.ZERO,
    val planeOrigin: WorldPoint = WorldPoint.ZERO,
    val planeNormal: WorldPoint = WorldPoint(0.0, 0.0, 1.0),
    val sphereCenter: WorldPoint = WorldPoint.ZERO,
    val sphereRadius: Double = 5.0,
    val childA: Int = 0,
    val childB: Int = 0,
    val childC: Int = 0,
    val opAB: Int = CombineOp.INTERSECT,
    val opAC: Int = CombineOp.SUBTRACT,
    val polygon: List<WorldPoint> = emptyList(),
    val plane: Int = MeasurePlane.AXIAL,
    val planePosition: Int = 0,
    /**
     * R-06 AI 掩膜 ROI 引用的分割实例编号（0 = 未引用）。
     * 掩膜本体不在 ROI 里存副本，只在 Native 侧的 AiResult 里有一份，
     * 因此 ROI 可以持久化、可以进组合 ROI，但"重推理一次"会覆盖它。
     */
    val aiLabel: Int = 0,
) {
    fun toJson(): JSONObject = JSONObject().apply {
        put("id", id)
        put("type", type)
        put("name", name)
        put("color", color.toLong() and 0xFFFFFFFFL)
        put("visible", visible)
        put("opacity", opacity)
        put("huMin", huMin)
        put("huMax", huMax)
        put("boxMin", boxMin.toJsonArray())
        put("boxMax", boxMax.toJsonArray())
        put("planeOrigin", planeOrigin.toJsonArray())
        put("planeNormal", planeNormal.toJsonArray())
        put("sphereCenter", sphereCenter.toJsonArray())
        put("sphereRadius", sphereRadius)
        put("childA", childA)
        put("childB", childB)
        put("childC", childC)
        put("opAB", opAB)
        put("opAC", opAC)
        put("polygon", WorldPoint.jsonArray(polygon))
        put("plane", plane)
        put("planePosition", planePosition)
        put("aiLabel", aiLabel)
    }

    /** 只带需要修改的字段的浅合并请求（配合 RoiJni.roiPatch） */
    fun patchJson(modify: JSONObject.() -> Unit): JSONObject = JSONObject().apply(modify)

    companion object {
        fun from(j: JSONObject): RoiItem = RoiItem(
            id = j.optInt("id"),
            type = j.optInt("type", RoiType.HU_THRESHOLD),
            name = j.optString("name"),
            color = j.optLong("color", 0xFFFFC400L).toInt(),
            visible = j.optBoolean("visible", true),
            opacity = j.optDouble("opacity", 0.35),
            huMin = j.optDouble("huMin", 200.0),
            huMax = j.optDouble("huMax", 3000.0),
            boxMin = WorldPoint.from(j.optJSONArray("boxMin")),
            boxMax = WorldPoint.from(j.optJSONArray("boxMax")),
            planeOrigin = WorldPoint.from(j.optJSONArray("planeOrigin")),
            planeNormal = WorldPoint.from(j.optJSONArray("planeNormal")),
            sphereCenter = WorldPoint.from(j.optJSONArray("sphereCenter")),
            sphereRadius = j.optDouble("sphereRadius", 5.0),
            childA = j.optInt("childA"),
            childB = j.optInt("childB"),
            childC = j.optInt("childC"),
            opAB = j.optInt("opAB", CombineOp.INTERSECT),
            opAC = j.optInt("opAC", CombineOp.SUBTRACT),
            polygon = WorldPoint.list(j.optJSONArray("polygon")),
            plane = j.optInt("plane", MeasurePlane.AXIAL),
            planePosition = j.optInt("planePosition"),
            // 旧存档没有这个键 -> 0；0 号实例不存在，统计会给出"未引用有效实例"
            aiLabel = j.optInt("aiLabel"),
        )

        fun list(root: JSONObject?): List<RoiItem> {
            val arr = root?.optJSONArray("rois") ?: return emptyList()
            return (0 until arr.length()).map { from(arr.getJSONObject(it)) }
        }

        fun array(json: String?): List<RoiItem> {
            if (json.isNullOrEmpty()) return emptyList()
            return try {
                val arr = JSONArray(json)
                (0 until arr.length()).map { from(arr.getJSONObject(it)) }
            } catch (e: Exception) {
                emptyList()
            }
        }
    }
}

/** 组合 ROI 的布尔算子（R-05） */
object CombineOp {
    const val NONE = 0
    const val INTERSECT = 1
    const val UNION = 2
    const val SUBTRACT = 3

    fun label(op: Int): String = when (op) {
        UNION -> "并"
        SUBTRACT -> "差"
        INTERSECT -> "交"
        else -> "无"
    }
}

/** ROI 统计结果（R-01 ~ R-05 的量化输出，同时供 M-04 / M-05 / M-08 显示） */
data class RoiStatsResult(
    val ok: Boolean,
    val error: String,
    val coverageVoxels: Double,
    val fullVoxels: Long,
    val scannedVoxels: Long,
    val elapsedMs: Long,
    val volumeMm3: Double,
    val volumeCm3: Double,
    val meanHu: Double,
    val sdHu: Double,
    val minHu: Double,
    val maxHu: Double,
    val hasHu: Boolean,
    val areaMm2: Double,
    val areaCm2: Double,
) {
    fun summaryText(): String = buildString {
        if (!ok) {
            append(error.ifEmpty { "统计失败" })
            return@buildString
        }
        if (volumeCm3 > 0.0) append(String.format("体积 %.3f cm³  ", volumeCm3))
        if (areaCm2 > 0.0) append(String.format("面积 %.3f cm²  ", areaCm2))
        if (hasHu) append(String.format("均值 %.0f ± %.0f HU（%.0f ~ %.0f）", meanHu, sdHu, minHu, maxHu))
        append(String.format("  遍历 %,d 体素 / %,d ms", scannedVoxels, elapsedMs))
    }

    companion object {
        val EMPTY = RoiStatsResult(false, "", 0.0, 0L, 0L, 0L, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, false, 0.0, 0.0)

        fun from(j: JSONObject?): RoiStatsResult {
            if (j == null) return EMPTY
            return RoiStatsResult(
                ok = j.optBoolean("ok"),
                error = j.optString("error"),
                coverageVoxels = j.optDouble("coverageVoxels", 0.0),
                fullVoxels = j.optLong("fullVoxels"),
                scannedVoxels = j.optLong("scannedVoxels"),
                elapsedMs = j.optLong("elapsedMs"),
                volumeMm3 = j.optDouble("volumeMm3", 0.0),
                volumeCm3 = j.optDouble("volumeCm3", 0.0),
                meanHu = j.optDouble("meanHu", 0.0),
                sdHu = j.optDouble("sdHu", 0.0),
                minHu = j.optDouble("minHu", 0.0),
                maxHu = j.optDouble("maxHu", 0.0),
                hasHu = j.optBoolean("hasHu"),
                areaMm2 = j.optDouble("areaMm2", 0.0),
                areaCm2 = j.optDouble("areaCm2", 0.0),
            )
        }
    }
}

/** 种植体（S-01 参数 + S-03 ~ S-06 结果；结果字段由 Native 计算回传） */
data class ImplantItem(
    val id: Int = 0,
    val name: String = "",
    val color: Int = SafetyLevel.COLOR_GREEN,
    val visible: Boolean = true,
    val entry: WorldPoint = WorldPoint.ZERO,
    val pitchDeg: Double = 0.0,
    val yawDeg: Double = 0.0,
    val depthMm: Double = 10.0,
    val diaMm: Double = 4.0,
    val lengthMm: Double = 11.0,
    val boneHeightMm: Double = 0.0,
    val boneWidthMm: Double = 0.0,
    val nerveDistMm: Double = 0.0,
    val minSpacingMm: Double = 0.0,
    // D-10（第 8 轮）：S-05/S-06 是否真的测到了值。修复后 minSpacingMm 可以合法为负
    // （负 = 两植体表面重叠，判级直接红），所以"-1.0"只代表"没有邻居/没有描记"，
    // UI 与报告必须读这两个布尔位，不能再用符号猜。
    val nerveMeasured: Boolean = false,
    val spacingMeasured: Boolean = false,
    // ---- P-09 型号库 + 冠方参考（crown-down）----
    /** 型号库编号；空 = 自由参数，不是目录里可采购的型号 */
    val sku: String = "",
    /** 冠方参考来源：0 无 / 1 R-04 球面 ROI / 2 A-* 标注 / 3 手工坐标（CrownRefKind） */
    val crownRefKind: Int = CrownRefKind.NONE,
    val crownRefId: Int = 0,
    /** Native 解析出的冠中心（只读回显；写入只有 kind=3 才用得上） */
    val crownCenter: WorldPoint = WorldPoint.ZERO,
    val crownOffsetMm: Double = 0.0,
    val crownAngleDeg: Double = 0.0,
    val crownAlongAxisMm: Double = 0.0,
    val crownMeasured: Boolean = false,
    val crownAxisFromReference: Boolean = false,
    val level: Int = SafetyLevel.GREEN,
    val warnText: String = "",
) {
    fun toJson(): JSONObject = JSONObject().apply {
        put("id", id)
        put("name", name)
        put("color", color.toLong() and 0xFFFFFFFFL)
        put("visible", visible)
        put("entry", entry.toJsonArray())
        put("pitchDeg", pitchDeg)
        put("yawDeg", yawDeg)
        put("depthMm", depthMm)
        put("diaMm", diaMm)
        put("lengthMm", lengthMm)
        // 结果字段一并回传：Native 侧 updateImplant 会忽略它们并重算，
        // 但带上可以让 UI 在编辑弹窗里显示"改动前的值"
        put("boneHeightMm", boneHeightMm)
        put("boneWidthMm", boneWidthMm)
        put("nerveDistMm", nerveDistMm)
        put("minSpacingMm", minSpacingMm)
        put("nerveMeasured", nerveMeasured)
        put("spacingMeasured", spacingMeasured)
        put("sku", sku)
        put("crownRefKind", crownRefKind)
        put("crownRefId", crownRefId)
        put("crownCenter", crownCenter.toJsonArray())
        put("level", level)
        put("warnText", warnText)
    }

    /**
     * D-10（第 8 轮）：S-05/S-06 的显示口径。
     * 未测量必须是文字，不能再印成 "-1.0"——那会被读成一个真实的负距离；
     * 而真正测到的负间距（表面重叠）要照实显示负值，它是判红依据。
     */
    fun nerveDistText(): String =
        if (nerveMeasured) String.format(Locale.US, "%.1f", nerveDistMm) else "未测"

    fun spacingText(): String =
        if (spacingMeasured) String.format(Locale.US, "%.1f", minSpacingMm) else "无邻"

    /**
     * P-09：S-11/S-12 的显示口径与 D-10 同构 —— 没有冠方参考时必须写"未设参考"，
     * 绝不能印 "0.0mm / 0.0°"，那看起来像"零偏差 = 完美"，实际上什么都没测。
     */
    fun crownText(): String = if (!crownMeasured) "S-11/S-12 未设冠方参考" else
        String.format(
            Locale.US,
            "S-11 %.1fmm / S-12 %.1f°%s",
            crownOffsetMm, crownAngleDeg,
            if (crownAxisFromReference) "" else "（成角按入口→冠中心）"
        )

    /** 冠方参考来源的文字（弹窗与报告同一措辞） */
    fun crownRefText(): String = CrownRefKind.label(crownRefKind, crownRefId)

    companion object {
        fun from(j: JSONObject): ImplantItem = ImplantItem(
            id = j.optInt("id"),
            name = j.optString("name"),
            color = j.optLong("color", SafetyLevel.COLOR_GREEN.toLong() and 0xFFFFFFFFL).toInt(),
            visible = j.optBoolean("visible", true),
            entry = WorldPoint.from(j.optJSONArray("entry")),
            pitchDeg = j.optDouble("pitchDeg", 0.0),
            yawDeg = j.optDouble("yawDeg", 0.0),
            depthMm = j.optDouble("depthMm", 10.0),
            diaMm = j.optDouble("diaMm", 4.0),
            lengthMm = j.optDouble("lengthMm", 11.0),
            boneHeightMm = j.optDouble("boneHeightMm", 0.0),
            boneWidthMm = j.optDouble("boneWidthMm", 0.0),
            nerveDistMm = j.optDouble("nerveDistMm", 0.0),
            minSpacingMm = j.optDouble("minSpacingMm", 0.0),
            // 缺字段 = 第 7 轮及以前的归档/JSON，当时"未测量"只能用负哨兵表达，
            // 所以兼容默认值按符号推断；新数据一律以 Native 写出的布尔位为准。
            nerveMeasured = j.optBoolean("nerveMeasured", j.optDouble("nerveDistMm", 0.0) >= 0.0),
            spacingMeasured = j.optBoolean("spacingMeasured",
                j.optDouble("minSpacingMm", 0.0) >= 0.0),
            sku = j.optString("sku"),
            crownRefKind = j.optInt("crownRefKind", CrownRefKind.NONE),
            crownRefId = j.optInt("crownRefId", 0),
            crownCenter = WorldPoint.from(j.optJSONArray("crownCenter")),
            crownOffsetMm = j.optDouble("crownOffsetMm", 0.0),
            crownAngleDeg = j.optDouble("crownAngleDeg", 0.0),
            crownAlongAxisMm = j.optDouble("crownAlongAxisMm", 0.0),
            crownMeasured = j.optBoolean("crownMeasured", false),
            crownAxisFromReference = j.optBoolean("crownAxisFromReference", false),
            level = j.optInt("level", SafetyLevel.GREEN),
            warnText = j.optString("warnText"),
        )

        fun list(root: JSONObject?): List<ImplantItem> {
            val arr = root?.optJSONArray("implants") ?: return emptyList()
            return (0 until arr.length()).map { from(arr.getJSONObject(it)) }
        }
    }
}

/**
 * 冠方参考来源（P-09；与 core 的 CrownRefKind 一一对应，跨语言只传 int）。
 * 可选来源只有模块里已存在的临床对象：R-04 球面 ROI 与 A-* 标注 ——
 * 不新增第三种"专用取点工具"，因此不需要动 9 态手势机（AC-10）。
 */
object CrownRefKind {
    const val NONE = 0
    const val SPHERE_ROI = 1
    const val ANNOTATION = 2
    const val MANUAL = 3

    fun label(kind: Int, refId: Int): String = when (kind) {
        SPHERE_ROI -> "R-04 球面 ROI #$refId"
        ANNOTATION -> "A-* 标注 #$refId"
        MANUAL -> "手工坐标"
        else -> "未设置"
    }
}

/** 神经管路径（S-05 依据） */
data class NervePathItem(
    val id: Int = 0,
    val name: String = "",
    val color: Int = 0xFFFF1744.toInt(),
    val visible: Boolean = true,
    val radiusMm: Double = 1.5,
    val points: List<WorldPoint> = emptyList(),
) {
    companion object {
        fun from(j: JSONObject): NervePathItem = NervePathItem(
            id = j.optInt("id"),
            name = j.optString("name"),
            color = j.optLong("color", 0xFFFF1744L).toInt(),
            visible = j.optBoolean("visible", true),
            radiusMm = j.optDouble("radiusMm", 1.5),
            points = WorldPoint.list(j.optJSONArray("points")),
        )

        fun list(root: JSONObject?): List<NervePathItem> {
            val arr = root?.optJSONArray("nervePaths") ?: return emptyList()
            return (0 until arr.length()).map { from(arr.getJSONObject(it)) }
        }
    }
}

/**
 * 第三磨牙 ↔ 下牙槽神经风险条目（P-10；对应 core/NerveRisk.h 的 ThirdMolarRisk）。
 *
 * 刻意**没有** level / 判级字段：PRD 未给阻生牙的风险阈值（决策点 4「不自定数字」），
 * 这里只承载几何事实 + 口径说明。判读口径字符串随每条一起回传，
 * UI/报告/SR 三处共用同一句，避免任何一处的排版把"这只是距离"读成"这是风险等级"。
 */
data class NerveRiskItem(
    val id: Int = 0,
    val instanceId: Int = 0,
    val nervePathId: Int = 0,
    val toothName: String = "",
    val nerveName: String = "",
    val nerveSource: String = "",
    val createdAt: Long = 0L,
    val measured: Boolean = false,
    val minDistMm: Double = -1.0,
    val contact: Boolean = false,
    val withinCanalRadius: Boolean = false,
    val nearestRoot: WorldPoint = WorldPoint(0.0, 0.0, 0.0),
    val nearestCanal: WorldPoint = WorldPoint(0.0, 0.0, 0.0),
    val rootVoxels: Long = 0L,
    val canalRadiusMm: Double = 0.0,
    val cortexMeasured: Boolean = false,
    val cortexPresent: Boolean = false,
    val cortexGapMm: Double = 0.0,
    val cortexBoneThresholdHu: Double = 200.0,
    val fovXMm: Double = 0.0,
    val fovYMm: Double = 0.0,
    val fovZMm: Double = 0.0,
    val spacingX: Double = 0.0,
    val spacingY: Double = 0.0,
    val spacingZ: Double = 0.0,
    val doseText: String = "",
    val seriesUid: String = "",
    val manufacturer: String = "",
    val error: String = "",
    val gradingCaveat: String = "",
) {
    /** 一行结论（与 Native summaryText 同口径，Native 已给原文时优先显示原文） */
    fun summary(): String {
        if (!measured) return "未测量：${error.ifEmpty { "输入不可用" }}"
        val tail = when {
            contact -> "（接触）"
            withinCanalRadius -> "（进入管腔半径 ${"%.1f".format(canalRadiusMm)}mm 内）"
            else -> ""
        }
        val cortex = if (!cortexMeasured) "；管壁区间无可取样段"
        else "；管壁${if (cortexPresent) "采到骨" else "未采到骨"}，最长非骨段 " +
                "${"%.1f".format(cortexGapMm)}mm"
        return "${toothName.ifEmpty { "牙 #$instanceId" }} ↔ " +
                "${nerveName.ifEmpty { "神经管 #$nervePathId" }}：" +
                "根面(体素球心)最近距离 ${"%.1f".format(minDistMm)}mm$tail$cortex"
    }

    /** 距离显示：未测量既不是 -1 也不是 0（0 只属于"确实接触"） */
    fun distText(): String = if (measured) "%.2f".format(minDistMm) else "未测"

    fun cortexText(): String =
        if (!cortexMeasured) "无区间"
        else (if (cortexPresent) "有骨" else "无骨") + " ${"%.1f".format(cortexGapMm)}mm"

    /** 来源一行（FOV / 间距 / 剂量口径）：报告与 UI 都显示，便于回查原始序列 */
    fun provenanceText(): String = buildString {
        append("FOV %.0f×%.0f×%.0fmm".format(fovXMm, fovYMm, fovZMm))
        append(" · 体素 %.2f/%.2f/%.2fmm".format(spacingX, spacingY, spacingZ))
        if (manufacturer.isNotEmpty()) append(" · $manufacturer")
        if (seriesUid.isNotEmpty()) append(" · Series $seriesUid")
        if (doseText.isNotEmpty()) append(" · $doseText")
    }

    companion object {
        private fun pt(j: JSONObject, key: String): WorldPoint {
            val a = j.optJSONArray(key) ?: return WorldPoint(0.0, 0.0, 0.0)
            return WorldPoint(a.optDouble(0, 0.0), a.optDouble(1, 0.0), a.optDouble(2, 0.0))
        }

        fun from(j: JSONObject): NerveRiskItem = NerveRiskItem(
            id = j.optInt("id"),
            instanceId = j.optInt("instanceId"),
            nervePathId = j.optInt("nervePathId"),
            toothName = j.optString("toothName"),
            nerveName = j.optString("nerveName"),
            nerveSource = j.optString("nerveSource"),
            createdAt = j.optLong("createdAt"),
            measured = j.optBoolean("measured", false),
            minDistMm = j.optDouble("minDistMm", -1.0),
            contact = j.optBoolean("contact", false),
            withinCanalRadius = j.optBoolean("withinCanalRadius", false),
            nearestRoot = pt(j, "nearestRoot"),
            nearestCanal = pt(j, "nearestCanal"),
            rootVoxels = j.optLong("rootVoxels"),
            canalRadiusMm = j.optDouble("canalRadiusMm", 0.0),
            cortexMeasured = j.optBoolean("cortexMeasured", false),
            cortexPresent = j.optBoolean("cortexPresent", false),
            cortexGapMm = j.optDouble("cortexGapMm", 0.0),
            cortexBoneThresholdHu = j.optDouble("cortexBoneThresholdHu", 200.0),
            fovXMm = j.optDouble("fovXMm", 0.0),
            fovYMm = j.optDouble("fovYMm", 0.0),
            fovZMm = j.optDouble("fovZMm", 0.0),
            spacingX = j.optDouble("spacingX", 0.0),
            spacingY = j.optDouble("spacingY", 0.0),
            spacingZ = j.optDouble("spacingZ", 0.0),
            doseText = j.optString("doseText"),
            seriesUid = j.optString("seriesUid"),
            manufacturer = j.optString("manufacturer"),
            error = j.optString("error"),
            gradingCaveat = j.optString("gradingCaveat"),
        )

        /** 键名两种都认：dumpNerveRisks 用 risks，plan.json 用 nerveRisks */
        fun list(root: JSONObject?): List<NerveRiskItem> {
            val arr = root?.optJSONArray("risks") ?: root?.optJSONArray("nerveRisks")
                ?: return emptyList()
            return (0 until arr.length()).map { from(arr.getJSONObject(it)) }
        }
    }
}

/** 标注（A-01 ~ A-07） */
data class AnnotationItem(
    val id: Int = 0,
    val type: Int = AnnotationType.TEXT_LABEL,
    val text: String = "",
    val color: Int = 0xFFFFFFFF.toInt(),
    val visible: Boolean = true,
    val measureId: Int = 0,
    val points: List<WorldPoint> = emptyList(),
    val radiusMm: Double = 3.0,
    val plane: Int = MeasurePlane.AXIAL,
    val planePosition: Int = 0,
) {
    fun toJson(): JSONObject = JSONObject().apply {
        put("id", id)
        put("type", type)
        put("text", text)
        put("color", color.toLong() and 0xFFFFFFFFL)
        put("visible", visible)
        put("measureId", measureId)
        put("points", WorldPoint.jsonArray(points))
        put("radiusMm", radiusMm)
        put("plane", plane)
        put("planePosition", planePosition)
    }

    /** A-07 用图像像素坐标，不参与三维叠加（Native 侧同样按此约定过滤） */
    fun isPixelSpace(): Boolean = type == AnnotationType.SCREENSHOT

    companion object {
        fun from(j: JSONObject): AnnotationItem = AnnotationItem(
            id = j.optInt("id"),
            type = j.optInt("type", AnnotationType.TEXT_LABEL),
            text = j.optString("text"),
            color = j.optLong("color", 0xFFFFFFFFL).toInt(),
            visible = j.optBoolean("visible", true),
            measureId = j.optInt("measureId"),
            points = WorldPoint.list(j.optJSONArray("points")),
            radiusMm = j.optDouble("radiusMm", 3.0),
            plane = j.optInt("plane", MeasurePlane.AXIAL),
            planePosition = j.optInt("planePosition"),
        )

        fun fromRoot(root: JSONObject?): List<AnnotationItem> {
            val arr = root?.optJSONArray("annotations") ?: return emptyList()
            return (0 until arr.length()).map { from(arr.getJSONObject(it)) }
        }
    }
}

/** 一次拾取的结果（core/MeasurePicker.h 的 PickResult） */
data class PickHit(
    val hit: Boolean,
    val point: WorldPoint,
    val voxelCenter: WorldPoint,
    val index: IntArray,
    val hu: Double,
    val distanceMm: Double,
    val describe: String,
) {
    companion object {
        val MISS = PickHit(false, WorldPoint.ZERO, WorldPoint.ZERO, intArrayOf(-1, -1, -1), 0.0, 0.0, "未命中")

        fun from(j: JSONObject?): PickHit {
            if (j == null) return MISS
            val idx = j.optJSONArray("index")
            return PickHit(
                hit = j.optBoolean("hit"),
                point = WorldPoint.from(j.optJSONArray("point")),
                voxelCenter = WorldPoint.from(j.optJSONArray("voxelCenter")),
                index = IntArray(3) { i -> idx?.optInt(i, -1) ?: -1 },
                hu = j.optDouble("hu", 0.0),
                distanceMm = j.optDouble("distanceMm", 0.0),
                describe = j.optString("describe"),
            )
        }
    }
}

/**
 * 叠加图元描述（Native 产出，MeasureOverlayView 消费）。
 * from/count 是 MeasureJni.overlayPoints() 扁平数组里的"点"下标区间，
 * 即第 from 点到第 from+count-1 点，每点占 3 个 double。
 */
data class OverlayPrim(
    val kind: Int,
    val color: Int,
    val widthPx: Double,
    val fill: Boolean,
    val arrowHead: Boolean,
    val labelAnchored: Boolean,
    val text: String,
    val from: Int,
    val count: Int,
    /** 归属对象类型/编号（OverlayOwner）：列表选中 -> 图元高亮的精确匹配依据 */
    val ownerKind: Int,
    val ownerId: Int,
) {
    companion object {
        fun from(j: JSONObject): OverlayPrim = OverlayPrim(
            kind = j.optInt("kind"),
            color = j.optLong("color", 0xFF00E5FFL).toInt(),
            widthPx = j.optDouble("width", 2.0),
            fill = j.optBoolean("fill"),
            arrowHead = j.optInt("arrow") != 0,
            labelAnchored = j.optInt("label") != 0,
            text = j.optString("text"),
            from = j.optInt("from"),
            count = j.optInt("count"),
            ownerKind = j.optInt("owner"),
            ownerId = j.optInt("ownerId"),
        )

        fun parse(json: String?): Pair<List<OverlayPrim>, Int> {
            if (json.isNullOrEmpty()) return emptyList<OverlayPrim>() to 0
            return try {
                val root = JSONObject(json)
                val arr = root.optJSONArray("prims") ?: JSONArray()
                val prims = (0 until arr.length()).map { from(arr.getJSONObject(it)) }
                prims to root.optInt("pointCount")
            } catch (e: Exception) {
                emptyList<OverlayPrim>() to 0
            }
        }
    }
}

/** 体数据概况（状态栏 / 报告页眉 / SR 患者信息核对） */
data class VolumeInfo(
    val valid: Boolean,
    val width: Int,
    val height: Int,
    val depth: Int,
    val spacingX: Double,
    val spacingY: Double,
    val spacingZ: Double,
    val maxDiagonalMm: Double,
    val studyInstanceUID: String,
    val seriesInstanceUID: String,
    val patientName: String,
    val patientID: String,
    val patientSex: String,
    val patientBirthDate: String,
    val studyDate: String,
    val seriesDescription: String,
    val manufacturer: String,
    val firstSlicePath: String,
) {
    /** 归档文件名前缀：{StudyUID}_{SeriesUID}（PRD 8.4） */
    fun archiveKey(): String {
        val s = studyInstanceUID.ifEmpty { "unknownStudy" }
        val sri = seriesInstanceUID.ifEmpty { "unknownSeries" }
        return "${s}_$sri"
    }

    fun voxelSizeText(): String =
        String.format("%.3f × %.3f × %.3f mm", spacingX, spacingY, spacingZ)

    companion object {
        val INVALID = VolumeInfo(false, 0, 0, 0, 1.0, 1.0, 1.0, 0.0,
            "", "", "", "", "", "", "", "", "", "")

        fun from(j: JSONObject?): VolumeInfo {
            if (j == null || !j.optBoolean("valid")) return INVALID
            return VolumeInfo(
                valid = true,
                width = j.optInt("width"),
                height = j.optInt("height"),
                depth = j.optInt("depth"),
                spacingX = j.optDouble("spacingX", 1.0),
                spacingY = j.optDouble("spacingY", 1.0),
                spacingZ = j.optDouble("spacingZ", 1.0),
                maxDiagonalMm = j.optDouble("maxDiagonalMm", 0.0),
                studyInstanceUID = j.optString("studyInstanceUID"),
                seriesInstanceUID = j.optString("seriesInstanceUID"),
                patientName = j.optString("patientName"),
                patientID = j.optString("patientID"),
                patientSex = j.optString("patientSex"),
                patientBirthDate = j.optString("patientBirthDate"),
                studyDate = j.optString("studyDate"),
                seriesDescription = j.optString("seriesDescription"),
                manufacturer = j.optString("manufacturer"),
                firstSlicePath = j.optString("firstSlicePath"),
            )
        }
    }
}

/** SR 导出 / 自校验的结果回执 */
data class SrResult(
    val ok: Boolean,
    val sopUid: String,
    val docType: String,
    val summary: String,
    val error: String,
) {
    companion object {
        fun from(json: String?): SrResult {
            if (json.isNullOrEmpty()) return SrResult(false, "", "", "", "空响应")
            return try {
                val j = JSONObject(json)
                SrResult(
                    ok = j.optBoolean("ok"),
                    sopUid = j.optString("sopUid"),
                    docType = j.optString("docType"),
                    summary = j.optString("summary"),
                    error = j.optString("error"),
                )
            } catch (e: Exception) {
                SrResult(false, "", "", "", "响应解析失败：${e.message}")
            }
        }
    }
}
