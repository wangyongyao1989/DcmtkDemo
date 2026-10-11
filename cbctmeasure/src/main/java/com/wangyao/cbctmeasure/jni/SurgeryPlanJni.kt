package com.wangyao.cbctmeasure.jni

import com.wangyao.cbctmeasure.model.ImplantItem
import com.wangyao.cbctmeasure.model.NervePathItem
import com.wangyao.cbctmeasure.model.NerveRiskItem
import com.wangyao.cbctmeasure.model.WorldPoint
import org.json.JSONObject

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

    // ---- P-09 植体型号库（进程内共享，与会话无关）----

    /** 用资产内容覆盖型号库；返回载入条数，-1 = 解析失败（原库保持不变） */
    @JvmStatic
    external fun loadModelLibrary(modelsJson: String): Int

    /** {kind, generic, count, models:[{sku,vendor,series,connection,diaMm,lengthMm,platformMm,abutmentMm,generic}]} */
    @JvmStatic
    external fun dumpModelLibrary(): String

    /** 回到内置通用阶梯（资产缺失时的兜底），返回条数 */
    @JvmStatic
    external fun useBuiltinModelLibrary(): Int

    /** 骨量包络内的可选最长型号；found=false 表示"这个骨量放不进任何已知型号" */
    @JvmStatic
    external fun fitImplantModel(maxDiaMm: Double, maxLenMm: Double): String

    // ---- P-10 第三磨牙 ↔ 下牙槽神经风险度量（会话级，不进判级）----

    /** 计算一条并入库（同 (实例, 神经管) 组合覆盖旧条目）；返回条目 JSON */
    @JvmStatic
    external fun assessNerveRisk(handle: Long, instanceId: Int, nervePathId: Int): String

    /** 一键评估：下颌弓全部实例 × 会话内全部神经管；返回入库条数 */
    @JvmStatic
    external fun assessAllNerveRisks(handle: Long): Int

    /** {count, gradingCaveat, risks:[...]} */
    @JvmStatic
    external fun dumpNerveRisks(handle: Long): String

    /** {summaries:[一行结论...], gradingCaveat} —— Toast/日志直接用，不再拼第二套措辞 */
    @JvmStatic
    external fun nerveRiskSummary(handle: Long): String

    @JvmStatic
    external fun removeNerveRisk(handle: Long, id: Int): Boolean

    @JvmStatic
    external fun clearNerveRisks(handle: Long)

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

    // ---- P-09 型号库包装 ----

    /** 一个可选型号（platform/abutment 为 0 表示资产未提供，不能当成"没有平台切换"） */
    data class ImplantModel(
        val sku: String,
        val vendor: String,
        val series: String,
        val connection: String,
        val diaMm: Double,
        val lengthMm: Double,
        val platformMm: Double,
        val abutmentMm: Double,
        val generic: Boolean,
    ) {
        fun label(): String = "$sku · Ø${"%.1f".format(diaMm)}×${"%.1f".format(lengthMm)}mm" +
                (if (platformMm > 0.0) " · 平台 ${"%.1f".format(platformMm)}" else "") +
                (if (vendor.isEmpty()) "（通用库）" else " · $vendor")
    }

    /** 当前库内容（每次现取，不缓存：资产可能在会话中被替换） */
    fun models(): List<ImplantModel> = try {
        val root = org.json.JSONObject(dumpModelLibrary())
        val arr = root.optJSONArray("models") ?: org.json.JSONArray()
        (0 until arr.length()).map { i ->
            val m = arr.getJSONObject(i)
            ImplantModel(
                sku = m.optString("sku"),
                vendor = m.optString("vendor"),
                series = m.optString("series"),
                connection = m.optString("connection"),
                diaMm = m.optDouble("diaMm", 0.0),
                lengthMm = m.optDouble("lengthMm", 0.0),
                platformMm = m.optDouble("platformMm", 0.0),
                abutmentMm = m.optDouble("abutmentMm", 0.0),
                generic = m.optBoolean("generic", true),
            )
        }
    } catch (e: Exception) {
        android.util.Log.e("SurgeryPlanJni", "models() 解析失败", e)
        emptyList()
    }

    /**
     * 当前库是不是"通用名义尺寸"（Native 由数据派生：所有型号 vendor 为空）。
     *
     * 读它而不是读"是不是内置表"：随包的 implant_models.json 也全是通用名义尺寸，
     * 报告若按来源判断就会把它印成"厂商目录"，凭空给读者一个不存在的采购依据。
     * 见 ImplantLibrary.h 的 isGeneric()。
     */
    fun libraryIsGeneric(): Boolean = try {
        org.json.JSONObject(dumpModelLibrary()).optBoolean("generic", true)
    } catch (e: Exception) {
        true
    }

    fun fitModel(maxDiaMm: Double, maxLenMm: Double): ImplantModel? {
        // 块体而不是表达式体：下面要在"没匹配到"时提前退出，表达式体里不许 return
        val o = try {
            org.json.JSONObject(fitImplantModel(maxDiaMm, maxLenMm))
        } catch (e: Exception) {
            return null
        }
        if (!o.optBoolean("found", false)) return null
        // 厂商字段一并取出：写死成空串会让厂商目录的匹配结果在 UI 上看起来像通用名义尺寸
        return ImplantModel(
            sku = o.optString("sku"),
            vendor = o.optString("vendor"),
            series = o.optString("series"),
            connection = o.optString("connection"),
            diaMm = o.optDouble("diaMm", 0.0),
            lengthMm = o.optDouble("lengthMm", 0.0),
            platformMm = o.optDouble("platformMm", 0.0),
            abutmentMm = o.optDouble("abutmentMm", 0.0),
            generic = o.optBoolean("generic", true),
        )
    }

    /** 解析 dumpPlan() 文本，取种植体列表 */
    fun implantsOf(planJson: String?): List<ImplantItem> = parsePlan(planJson).first

    /** 解析 dumpPlan() 文本，取神经管路径列表 */
    fun nervesOf(planJson: String?): List<NervePathItem> = parsePlan(planJson).second

    /** 解析 dumpPlan() 文本，取 P-10 风险条目（旧归档没有该键 -> 空表） */
    fun nerveRisksOf(planJson: String?): List<NerveRiskItem> = parsePlan(planJson).third

    // ---- P-10 便捷包装 ----

    /** 会话内当前风险条目（每次现取，与 Native 单向下一致） */
    fun risksOf(handle: Long): List<NerveRiskItem> = try {
        NerveRiskItem.list(JSONObject(dumpNerveRisks(handle)))
    } catch (e: Exception) {
        android.util.Log.e("SurgeryPlanJni", "risksOf 解析失败", e)
        emptyList()
    }

    /**
     * 判读口径（"这些数字没有阈值"那句话）。
     * 取不到时回退到写死的一句同样的话：报告与 UI 不能因为一次解析失败就
     * 把没有判级口径的数字印成看起来有判级的样子。
     */
    fun gradingCaveat(handle: Long): String = try {
        JSONObject(dumpNerveRisks(handle)).optString("gradingCaveat").ifEmpty { FALLBACK_CAVEAT }
    } catch (e: Exception) {
        FALLBACK_CAVEAT
    }

    /** assessNerveRisk 的返回值：条目 + 是否入库（失败时 measured=false + error） */
    data class RiskOutcome(val item: NerveRiskItem, val stored: Boolean)

    fun assessRisk(handle: Long, instanceId: Int, nervePathId: Int): RiskOutcome = try {
        val o = JSONObject(assessNerveRisk(handle, instanceId, nervePathId))
        RiskOutcome(NerveRiskItem.from(o), o.optBoolean("stored", false))
    } catch (e: Exception) {
        android.util.Log.e("SurgeryPlanJni", "assessRisk 解析失败", e)
        RiskOutcome(NerveRiskItem(error = "JNI 结果解析失败：${e.message}"), false)
    }

    /** 一键评估 + 取回结论行（Toast/日志用 Native 的原文措辞） */
    fun assessAllRisks(handle: Long): Pair<Int, List<String>> {
        val n = assessAllNerveRisks(handle)
        val list = try {
            val arr = JSONObject(nerveRiskSummary(handle)).optJSONArray("summaries")
                ?: org.json.JSONArray()
            (0 until arr.length()).map { arr.getString(it) }
        } catch (e: Exception) {
            emptyList()
        }
        return n to list
    }

    private fun parsePlan(
        planJson: String?,
    ): Triple<List<ImplantItem>, List<NervePathItem>, List<NerveRiskItem>> = try {
        val root = org.json.JSONObject(planJson.orEmpty())
        val im = root.optJSONArray("implants") ?: org.json.JSONArray()
        val nv = root.optJSONArray("nervePaths") ?: org.json.JSONArray()
        Triple(
            (0 until im.length()).map { ImplantItem.from(im.getJSONObject(it)) },
            (0 until nv.length()).map { NervePathItem.from(nv.getJSONObject(it)) },
            NerveRiskItem.list(root),
        )
    } catch (e: Exception) {
        Triple(emptyList(), emptyList(), emptyList())
    }

    private const val FALLBACK_CAVEAT =
        "P-10 只给几何事实（距离/接触/管壁区间骨取样），判级阈值待 PRD 定值，不参与 S-02 安全色，也不改写 S-05。"
}
