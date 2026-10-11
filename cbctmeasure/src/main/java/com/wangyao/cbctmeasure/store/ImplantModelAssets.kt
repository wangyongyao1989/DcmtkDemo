package com.wangyao.cbctmeasure.store

import android.content.Context
import android.util.Log
import com.wangyao.cbctmeasure.jni.SurgeryPlanJni

/**
 * 植体型号库的资产装载（升级建议 P-09）。
 *
 * 为什么单独一个对象：型号目录是**进程级**的（与具体体数据/会话无关），
 * 而资产读取需要 Context。把它放在 store/ 里和 MeasurementStore 并列，
 * Fragment 只需要在 onViewCreated 调一次 ensureLoaded()。
 *
 * 与 AI 资产的口径不同（决策点 2）：AI 权重"不进 APK、只在主机侧/外置目录"，
 * 因为它们是按 MB 计的模型文件；型号库是一份几 KB 的**产品目录配置**，
 * 进 APK 不会影响体积预算，也让"离线开箱就能推荐可采购型号"成立。
 * 换成真实厂商目录时仍然只需要替换 `assets/implant_models.json` 这一个文件。
 */
object ImplantModelAssets {

    private const val TAG = "CbctMeasureStore"
    private const val ASSET = "implant_models.json"

    @Volatile
    private var loaded = false

    /** 最近一次装载结果的一行说明（进 UI 状态栏与报告，避免"用的哪版库"不可知） */
    @Volatile
    var statusText: String = "未装载"
        private set

    /**
     * 幂等装载。
     * @return 型号条数（>0）。资产缺失/解析失败时自动退回内置通用阶梯，
     *         并在 statusText 里写明是兜底 —— 不做"静默空库"，
     *         因为空库会让 AI-03 退化成连续长度（正是 P-09 要修掉的行为）。
     */
    @Synchronized
    fun ensureLoaded(ctx: Context): Int {
        if (loaded) return currentCount()
        val text = try {
            ctx.assets.open(ASSET).use { it.readBytes().toString(Charsets.UTF_8) }
        } catch (e: Exception) {
            Log.w(TAG, "读取 $ASSET 失败，退回内置通用型号库", e)
            null
        }
        val n = if (text.isNullOrEmpty()) -1 else SurgeryPlanJni.loadModelLibrary(text)
        if (n > 0) {
            loaded = true
            statusText = "型号库：资产 $ASSET，${n} 个型号（generic=${SurgeryPlanJni.libraryIsGeneric()}）"
            Log.d(TAG, "loadModelLibrary ok: $n models, generic=${SurgeryPlanJni.libraryIsGeneric()}")
            return n
        }
        val fallback = SurgeryPlanJni.useBuiltinModelLibrary()
        loaded = true
        statusText = "型号库：内置通用阶梯 ${fallback} 个（资产未载入：$ASSET 缺失或解析失败）"
        Log.w(TAG, "型号库退回内置通用阶梯：$fallback 个")
        return fallback
    }

    fun currentCount(): Int = try {
        SurgeryPlanJni.models().size
    } catch (e: Exception) {
        0
    }
}
