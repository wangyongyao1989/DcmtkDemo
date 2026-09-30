package com.wangyao.cbctmeasure.jni

import com.wangyao.cbctmeasure.model.ImplantItem
import com.wangyao.cbctmeasure.model.NervePathItem
import com.wangyao.cbctmeasure.model.WorldPoint

/**
 * 种植体 / 正畸方案 JNI（PRD 附录 B 的 SurgeryPlanJni，功能项 S-01 ~ S-06）。
 *
 * 数值的唯一计算入口在 core/ImplantPlanner：本层只提交参数、取回结果。
 * 任何"参数改动"都会触发整体重算，因为 S-06 的相邻间距依赖其它桩体位置，
 * 单颗重算会得到过期结果。
 */
object SurgeryPlanJni {

    init {
        MeasureNative.ensureLoaded()
    }

    /** 新建种植体（S-01），返回 id（<=0 失败） */
    @JvmStatic
    external fun addImplant(handle: Long, implantJson: String): Int

    @JvmStatic
    external fun updateImplant(handle: Long, implantJson: String): Boolean

    @JvmStatic
    external fun removeImplant(handle: Long, id: Int): Boolean

    @JvmStatic
    external fun setImplantVisible(handle: Long, id: Int, visible: Boolean): Boolean

    /** 批量改参数后一次性重算安全指标 */
    @JvmStatic
    external fun recomputePlan(handle: Long)

    /** 新建神经管路径（S-05 依据），返回 id */
    @JvmStatic
    external fun addNervePath(handle: Long, name: String, color: Int, radiusMm: Double): Int

    /** 逐层描记：向路径追加一个世界坐标点（ST_NERVE_TRACE 手势的落点） */
    @JvmStatic
    external fun appendNervePoint(handle: Long, id: Int, x: Double, y: Double, z: Double): Boolean

    /** 描记回退：删除第 index 个点（撤销上一笔） */
    @JvmStatic
    external fun removeNervePoint(handle: Long, id: Int, index: Int): Boolean

    @JvmStatic
    external fun removeNervePath(handle: Long, id: Int): Boolean

    @JvmStatic
    external fun setNerveVisible(handle: Long, id: Int, visible: Boolean): Boolean

    @JvmStatic
    external fun clearPlan(handle: Long)

    /**
     * 安全阈值 [骨高下限, 骨宽下限, 神经距离下限, 间距下限, 临界系数]。
     * UI 文案与滑杆边界从这里取，避免与 Native 判级阈值两处写死。
     */
    @JvmStatic
    external fun safetyThresholds(): DoubleArray

    // ---- 便捷包装 ----

    fun addImplant(handle: Long, implant: ImplantItem): Int =
        addImplant(handle, implant.toJson().toString())

    fun updateImplant(handle: Long, implant: ImplantItem): Boolean =
        updateImplant(handle, implant.toJson().toString())

    fun appendNervePoint(handle: Long, id: Int, p: WorldPoint): Boolean =
        appendNervePoint(handle, id, p.x, p.y, p.z)

    /** 方案阈值的名义值（便于日志与 UI 断言） */
    data class Limits(
        val minBoneHeightMm: Double,
        val minBoneWidthMm: Double,
        val minNerveDistMm: Double,
        val minSpacingMm: Double,
        val marginalFactor: Double,
    )

    fun limits(): Limits {
        val t = safetyThresholds()
        return if (t.size < 5) Limits(10.0, 6.0, 2.0, 3.0, 1.2) else Limits(t[0], t[1], t[2], t[3], t[4])
    }

    /** 解析 dumpPlan() 文本，取种植体列表 */
    fun implantsOf(planJson: String?): List<ImplantItem> = parsePlan(planJson).first

    /** 解析 dumpPlan() 文本，取神经管路径列表 */
    fun nervesOf(planJson: String?): List<NervePathItem> = parsePlan(planJson).second

    private fun parsePlan(planJson: String?): Pair<List<ImplantItem>, List<NervePathItem>> = try {
        val root = org.json.JSONObject(planJson.orEmpty())
        val im = root.optJSONArray("implants") ?: org.json.JSONArray()
        val nv = root.optJSONArray("nervePaths") ?: org.json.JSONArray()
        (0 until im.length()).map { ImplantItem.from(im.getJSONObject(it)) } to
                (0 until nv.length()).map { NervePathItem.from(nv.getJSONObject(it)) }
    } catch (e: Exception) {
        emptyList<ImplantItem>() to emptyList()
    }
}
