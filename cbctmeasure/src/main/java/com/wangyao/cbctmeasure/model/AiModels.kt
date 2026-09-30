package com.wangyao.cbctmeasure.model

import org.json.JSONArray
import org.json.JSONObject

/**
 * AI-01 / AI-03 的数据镜像（PRD 5.6）。
 *
 * 字段名单点来自 Native：core/AiTypes.h 的 AiResult / AiInstance 与
 * core/AiPlanner.h 的 AiCandidate，序列化在
 * MeasureJniHelper::aiResultToJson 与 AiPlanner::candidatesJson。
 * 这里只读不写 —— 掩膜和推理结果的生命周期完全在 C++ 会话里，
 * Kotlin 拿到的是"某一次推理的回执"，不能反过来改它（要改就重新推理）。
 *
 * 所有数值都是 C++ 已经算好的显示值，本页与报告不再重新计算，
 * 这样"屏幕上的体积"与"PDF/SR 里的体积"必然一致（PRD 6 精度项的前提）。
 */
object AiArch {
    const val UNKNOWN = -1
    const val MAXILLA = 0        // 上颌
    const val MANDIBLE = 1       // 下颌

    fun label(arch: Int): String = when (arch) {
        MAXILLA -> "上颌"
        MANDIBLE -> "下颌"
        else -> "未判定"
    }
}

/** 一颗被分割出来的牙（连通域） */
data class AiInstanceInfo(
    val id: Int = 0,
    val voxels: Long = 0,
    val volumeCm3: Double = 0.0,
    val centroid: WorldPoint = WorldPoint.ZERO,
    val axis: WorldPoint = WorldPoint.ZERO,
    val meanHu: Double = 0.0,
    val minHu: Double = 0.0,
    val maxHu: Double = 0.0,
    val sdHu: Double = 0.0,
    val arch: Int = AiArch.UNKNOWN,
    val toothIndex: Int = 0,
    val archAngle: Double = 0.0,
    val crownZ: Double = 0.0,
    val rootZ: Double = 0.0,
    /** [minX,minY,minZ,maxX,maxY,maxZ]，世界 mm */
    val bbox: DoubleArray = DoubleArray(6),
    val roiId: Int = 0,
    val measureId: Int = 0,
) {
    /** 列表一行足够用的名字："上颌 3 号 · 1.24 cm³" */
    fun displayName(): String =
        "${AiArch.label(arch)} ${if (toothIndex > 0) "${toothIndex}号" else "#$id"}"

    fun bboxMin(): WorldPoint = WorldPoint(bbox.getOrElse(0) { 0.0 },
                                           bbox.getOrElse(1) { 0.0 },
                                           bbox.getOrElse(2) { 0.0 })

    fun bboxMax(): WorldPoint = WorldPoint(bbox.getOrElse(3) { 0.0 },
                                           bbox.getOrElse(4) { 0.0 },
                                           bbox.getOrElse(5) { 0.0 })

    companion object {
        fun from(o: JSONObject): AiInstanceInfo = AiInstanceInfo(
            id = o.optInt("id"),
            voxels = o.optLong("voxels"),
            volumeCm3 = o.optDouble("volumeCm3", 0.0),
            centroid = WorldPoint.from(o.optJSONArray("centroid")),
            axis = WorldPoint.from(o.optJSONArray("axis")),
            meanHu = o.optDouble("meanHu", 0.0),
            minHu = o.optDouble("minHu", 0.0),
            maxHu = o.optDouble("maxHu", 0.0),
            sdHu = o.optDouble("sdHu", 0.0),
            arch = o.optInt("arch", AiArch.UNKNOWN),
            toothIndex = o.optInt("toothIndex"),
            archAngle = o.optDouble("archAngle", 0.0),
            crownZ = o.optDouble("crownZ", 0.0),
            rootZ = o.optDouble("rootZ", 0.0),
            bbox = doubleArray(o.optJSONArray("bbox"), 6),
            roiId = o.optInt("roiId"),
            measureId = o.optInt("measureId"),
        )

        fun list(root: JSONObject?): List<AiInstanceInfo> {
            val arr = root?.optJSONArray("instances") ?: return emptyList()
            return (0 until arr.length()).map { from(arr.getJSONObject(it)) }
        }

        /** 定长扁平数组（缺元素补 0，不让一条记录作废） */
        fun doubleArray(a: JSONArray?, n: Int): DoubleArray {
            val out = DoubleArray(n)
            if (a == null) return out
            for (i in 0 until minOf(n, a.length())) out[i] = a.optDouble(i, 0.0)
            return out
        }
    }
}

/** 一次推理的回执（PC-05 的延迟/内存取证就引用这里的前四个时间） */
data class AiResultInfo(
    val ok: Boolean = false,
    val error: String = "",
    val modelName: String = "",
    val runtimeInfo: String = "",
    /** 实际生效的判定阈值；报告必须引用它，否则掩膜不可复现 */
    val threshold: Double = 0.0,
    val prepMs: Double = 0.0,
    val inferMs: Double = 0.0,
    val postMs: Double = 0.0,
    val totalMs: Double = 0.0,
    val allocBytes: Long = 0,
    val toothVoxels: Long = 0,
    val overlayVisible: Boolean = true,
    val modelDim: IntArray = IntArray(3),
    val factor: IntArray = IntArray(3),
    val redDim: IntArray = IntArray(3),
    val modelSpacing: DoubleArray = DoubleArray(3),
    val redVoxelMm3: Double = 0.0,
    val runtimeReady: Boolean = false,
    val instances: List<AiInstanceInfo> = emptyList(),
    val summary: String = "",
) {
    /** "推理占比"：ONNX Runtime 本身占整条链路的多少（PC-05 归因用） */
    fun inferSharePercent(): Double =
        if (totalMs > 1e-9) inferMs / totalMs * 100.0 else 0.0

    fun pc05Within5s(): Boolean = ok && totalMs <= 5000.0

    companion object {
        val EMPTY = AiResultInfo()

        /** JNI 返回的 JSON -> 回执；null / 解析失败 -> EMPTY（UI 只显示"无结果"） */
        fun parse(json: String?): AiResultInfo {
            if (json.isNullOrEmpty()) return EMPTY
            val o = try {
                JSONObject(json)
            } catch (e: org.json.JSONException) {
                return EMPTY.copy(error = "JSON 解析失败: ${e.message}")
            }
            return from(o)
        }

        fun from(o: JSONObject): AiResultInfo = AiResultInfo(
            ok = o.optBoolean("ok"),
            error = o.optString("error"),
            modelName = o.optString("modelName"),
            runtimeInfo = o.optString("runtimeInfo"),
            threshold = o.optDouble("threshold", 0.0),
            prepMs = o.optDouble("prepMs", 0.0),
            inferMs = o.optDouble("inferMs", 0.0),
            postMs = o.optDouble("postMs", 0.0),
            totalMs = o.optDouble("totalMs", 0.0),
            allocBytes = o.optLong("allocBytes"),
            toothVoxels = o.optLong("toothVoxels"),
            overlayVisible = o.optBoolean("overlayVisible", true),
            modelDim = intArray(o.optJSONArray("modelDim"), 3),
            factor = intArray(o.optJSONArray("factor"), 3),
            redDim = intArray(o.optJSONArray("redDim"), 3),
            modelSpacing = AiInstanceInfo.doubleArray(o.optJSONArray("modelSpacing"), 3),
            redVoxelMm3 = o.optDouble("redVoxelMm3", 0.0),
            runtimeReady = o.optBoolean("runtimeReady"),
            instances = AiInstanceInfo.list(o),
            summary = o.optString("summary"),
        )

        private fun intArray(a: JSONArray?, n: Int): IntArray {
            val out = IntArray(n)
            if (a == null) return out
            for (i in 0 until minOf(n, a.length())) out[i] = a.optInt(i)
            return out
        }
    }
}

/** AI-03 的一个候选种植位（缺牙间隙） */
data class AiCandidateInfo(
    val valid: Boolean = false,
    val arch: Int = AiArch.UNKNOWN,
    val beforeId: Int = 0,
    val afterId: Int = 0,
    val gapMm: Double = 0.0,
    val entry: WorldPoint = WorldPoint.ZERO,
    val axis: WorldPoint = WorldPoint.ZERO,
    val pitchDeg: Double = 0.0,
    val yawDeg: Double = 0.0,
    val diaMm: Double = 0.0,
    val lengthMm: Double = 0.0,
    val depthMm: Double = 0.0,
    val meanHu: Double = 0.0,
    val score: Double = 0.0,
    val boneHeightMm: Double = 0.0,
    val boneWidthMm: Double = 0.0,
    val nerveDistMm: Double = -1.0,
    val level: Int = SafetyLevel.GREEN,
    /** 已经落库成种植体时回填（0 = 只是候选） */
    val implantId: Int = 0,
    val reason: String = "",
) {
    fun title(): String =
        "${AiArch.label(arch)} ${beforeId}-${afterId} 间隙 " +
                String.format("%.1f", gapMm) + "mm"

    companion object {
        fun from(o: JSONObject): AiCandidateInfo = AiCandidateInfo(
            valid = o.optBoolean("valid"),
            arch = o.optInt("arch", AiArch.UNKNOWN),
            beforeId = o.optInt("beforeId"),
            afterId = o.optInt("afterId"),
            gapMm = o.optDouble("gap", 0.0),
            entry = WorldPoint.from(o.optJSONArray("entry")),
            axis = WorldPoint.from(o.optJSONArray("axis")),
            pitchDeg = o.optDouble("pitch", 0.0),
            yawDeg = o.optDouble("yaw", 0.0),
            diaMm = o.optDouble("dia", 0.0),
            lengthMm = o.optDouble("length", 0.0),
            depthMm = o.optDouble("depth", 0.0),
            meanHu = o.optDouble("meanHu", 0.0),
            score = o.optDouble("score", 0.0),
            boneHeightMm = o.optDouble("boneHeight", 0.0),
            boneWidthMm = o.optDouble("boneWidth", 0.0),
            nerveDistMm = o.optDouble("nerveDist", -1.0),
            level = o.optInt("level", SafetyLevel.GREEN),
            implantId = o.optInt("implantId"),
            reason = o.optString("reason"),
        )

        fun parse(json: String?): Pair<List<AiCandidateInfo>, String> {
            if (json.isNullOrEmpty()) return emptyList<AiCandidateInfo>() to ""
            val o = try {
                JSONObject(json)
            } catch (e: org.json.JSONException) {
                return emptyList<AiCandidateInfo>() to "JSON 解析失败: ${e.message}"
            }
            val arr = o.optJSONArray("candidates")
            val list = if (arr == null) emptyList()
            else (0 until arr.length()).map { from(arr.getJSONObject(it)) }
            return list to o.optString("summary")
        }
    }
}
