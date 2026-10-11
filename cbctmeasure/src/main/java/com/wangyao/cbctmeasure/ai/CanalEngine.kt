package com.wangyao.cbctmeasure.ai

import android.content.Context
import android.graphics.Color
import android.net.Uri
import android.util.Log
import com.wangyao.cbctmeasure.jni.AiJni
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream

/**
 * AI-02 下颌管（升级建议 P-01）的 Kotlin 侧编排。
 *
 * 与 [AiEngine]（牙齿）的三点区别，都来自本轮的决策口径：
 *
 * 1) **权重不进 APK**（决策 2：AI 资产只在主机侧训练，交付走外置通道）。
 *    牙齿模型是随 assets 发布的（一期已这么做，20KB）；下颌管模型的权重来自
 *    DentVoxel 专家标注训练，许可与再分发口径还没落纸（见
 *    doc/AI_LICENSE_AND_REDISTRIBUTION.md），所以这里**只从外部路径找**：
 *      a) app 专属外部目录 files/ai_models/canal_cnn.onnx
 *         —— `adb push` 免 root 可写，真机取证与试用都走这条路；
 *      b) 内部 filesDir/ai_models —— 用户在页面上用系统文件选择器挑完之后
 *         落到这里，卸载即清；
 *      c) 页面直接选中的 content:// —— 复制一份到 b) 再装载，因为 native
 *         的 dlopen/fopen 只认路径，不认 Uri。
 *    找不到就是"未就绪"，按钮不可点，牙齿链路与测量功能完全不受影响。
 *
 * 2) **阈值不自己定**（决策 4）。真机用的阈值来自随权重一起交付的
 *    canal_cnn.json（主机侧在验证集上标定）。代码里的 0.5 不是"判定阈值"，
 *    只是 2 类 softmax 的平票线，用于规格文件缺失时的兜底，并在状态栏里
 *    明写"标定值缺失"——不能让人误以为 0.5 是有依据的临床判据。
 *
 * 3) **模型质量门禁没过的东西不能进临床判级**。本轮主机准入门禁是
 *    MISSED（见 parity_ref/metrics_canal.json），所以自动管道的默认身份是
 *    "只预览"：照样绘制、照样进归档与报告，但不进 S-05/AI-03 的判级输入
 *    （[setFeedGrading]，默认关）。医生确认过影像后可以显式打开；
 *    一键清除（[clear]）只撤自动管道，手描神经管一条都不动。
 */
object CanalEngine {

    private const val TAG = "CbctMeasureAi"

    const val CANAL_MODEL_NAME = "canal_cnn.onnx"
    const val CANAL_SPEC_NAME = "canal_cnn.json"

    /** 规格缺失时的兜底 = 2 类 softmax 平票线，与 C++ AiConst::TOOTH_THRESHOLD 同值 */
    const val FALLBACK_THRESHOLD = 0.5

    /**
     * 准入门禁的两条展示用下限（状态栏把"过没过"直接写给医生看）。
     *
     * 注意它们的身份：**PRD 里没有 AI-02 的验收数字**，这条门槛是
     * doc/CBCT_APPLICATION_UPGRADE_PROPOSALS.md 的 P-01 验收段提出的
     * "必须先定"的值（Dice 沿用 PRD 给 AI-01 的 0.85，HD95 按同一量级提 1.2mm），
     * 等 PRD 定值之后应改由 PRD 引用。
     *
     * 之所以现在写死也不会误导：留出集实测 Dice 0.146 / HD95 42.5mm，
     * 连主机脚本里那条更松的工程线（0.7 / 3.0mm）都差着一个数量级，
     * 换成 PRD 将来定的任何合理数字，结论都是"未通过"。
     */
    const val GATE_DICE_MIN = 0.85
    const val GATE_HD95_MAX_MM = 1.2

    /** 与牙齿共用的线程预算：平板 8 核留一半给渲染（PC-02） */
    const val DEFAULT_THREADS = 4

    /** 外置模型的相对目录名（与 AiEngine 的 ai_models 同名，同一份目录两种模型） */
    const val MODEL_SUB_DIR = "ai_models"

    // =========================================================================
    // 1) 权重在外部的哪里
    // =========================================================================

    /** 候选目录：外部专属目录优先（adb 可直接 push），其次内部 filesDir */
    fun modelDirs(ctx: Context): List<File> {
        val out = ArrayList<File>(2)
        ctx.getExternalFilesDir(null)?.let { out.add(File(it, MODEL_SUB_DIR)) }
        out.add(File(ctx.filesDir, MODEL_SUB_DIR))
        return out
    }

    /** 已就位的模型文件（都没有时返回 null，并把找过的位置留成可读文本） */
    fun findModel(ctx: Context): File? = modelDirs(ctx)
        .map { File(it, CANAL_MODEL_NAME) }
        .firstOrNull { it.isFile && it.length() > 0 }

    /** 状态栏/报告用的一句"从哪里找的"，避免用户以为模型在 APK 里 */
    fun searchPathsText(ctx: Context): String =
        modelDirs(ctx).joinToString(" 或 ") { File(it, CANAL_MODEL_NAME).absolutePath }

    /**
     * 把用户选中的 content:// 复制进 filesDir/ai_models。
     *
     * 为什么要复制而不是直接读 Uri：native 侧 OrtEngine::load 走的是
     * fopen/dlopen（只认绝对路径）；而 content:// 的授权是 Activity 级的，
     * 会话跨线程读它并不可靠。复制一份的代价是 20KB 量级。
     */
    suspend fun importFromUri(ctx: Context, uri: Uri): File = withContext(Dispatchers.IO) {
        val dir = File(ctx.filesDir, MODEL_SUB_DIR)
        dir.mkdirs()
        val target = File(dir, CANAL_MODEL_NAME)
        ctx.contentResolver.openInputStream(uri)?.use { input ->
            FileOutputStream(target).use { output -> input.copyTo(output, 8192) }
        } ?: throw IllegalStateException("无法打开所选文件")
        // 规格文件与模型同名不同后缀，选中 .onnx 时顺手把同目录的 json 也带上
        Log.d(TAG, "已导入下颌管模型 ${target.absolutePath} (${target.length()} B)")
        copySiblingSpec(ctx, uri, dir)
        target
    }

    /** content:// 模型旁边的 canal_cnn.json（同一父目录，名字固定） */
    private fun copySiblingSpec(ctx: Context, modelUri: Uri, dir: File) {
        try {
            val doc = android.provider.DocumentsContract.getDocumentId(modelUri)
            val idx = doc.lastIndexOf('/')
            if (idx <= 0) return
            val rawParent = doc.substring(0, idx).removePrefix("raw:")
            val spec = File(rawParent, CANAL_SPEC_NAME)
            if (spec.isFile) {
                spec.copyTo(File(dir, CANAL_SPEC_NAME), overwrite = true)
                Log.d(TAG, "已复制配套规格 ${spec.absolutePath}")
            } else {
                // 选的是 SAF 虚拟文档（没有 raw: 前缀）时拿不到兄弟文件：
                // 阈值就走兜底，状态栏会明写"缺少 canal_cnn.json"，不假装标定过。
                Log.w(TAG, "同级没有 $CANAL_SPEC_NAME，阈值用兜底值")
            }
        } catch (e: Exception) {
            Log.w(TAG, "配套规格文件复制失败（阈值退兜底）: ${e.message}")
        }
    }

    /** 读随权重交付的标定阈值；缺失/非法 -> FALLBACK_THRESHOLD，并把来源说清 */
    fun readSpec(ctx: Context): Pair<Double, String> {
        val model = findModel(ctx) ?: return FALLBACK_THRESHOLD to "模型未就位"
        val spec = File(model.parentFile, CANAL_SPEC_NAME)
        if (!spec.isFile) return FALLBACK_THRESHOLD to "缺少 $CANAL_SPEC_NAME"
        return try {
            val j = JSONObject(spec.readText())
            val t = j.optDouble("threshold", Double.NaN)
            if (t.isNaN() || t <= 0.0 || t >= 1.0) return FALLBACK_THRESHOLD to "规格里的阈值非法"
            // 留出集指标决定"这个模型能不能参与判级"，所以它必须和阈值出现在同一行。
            // 早先这里读的是一个规格里根本没有的键（test_dice），医生看到的是
            // "留出集 Dice ?" —— 问号会让人以为在加载，其实门禁结论就写在同一段 JSON 里。
            val m = j.optJSONObject("metrics_test")
            val dice = m?.optDouble("dice_red", Double.NaN) ?: Double.NaN
            val hd95 = m?.optDouble("hd95_mm", Double.NaN) ?: Double.NaN
            val gate = when {
                dice.isNaN() || hd95.isNaN() ->
                    "留出集指标缺失（规格没有 metrics_test）→ 按未通过处理"

                dice >= GATE_DICE_MIN && hd95 <= GATE_HD95_MAX_MM ->
                    "留出集 Dice ${"%.3f".format(dice)} / HD95 ${"%.1f".format(hd95)}mm → 准入门禁通过"

                else ->
                    "留出集 Dice ${"%.3f".format(dice)} / HD95 ${"%.1f".format(hd95)}mm" +
                            " → 准入门禁未过（门槛 Dice>=$GATE_DICE_MIN、HD95<=$GATE_HD95_MAX_MM mm）" +
                            "，默认只预览不判级"
            }
            val chosen = j.optString("threshold_chosen_by")
            t to "标定阈值 $t" + if (chosen.isEmpty()) "" else "（$chosen）" + "；$gate"
        } catch (e: Exception) {
            Log.w(TAG, "canal 规格解析失败: ${e.message}")
            FALLBACK_THRESHOLD to "规格解析失败"
        }
    }

    // =========================================================================
    // 2) 装载 / 推理 / 取证
    // =========================================================================

    data class LoadResult(
        val ok: Boolean,
        val error: String = "",
        val runtimeInfo: String = "",
        val modelPath: String = "",
        val threshold: Double = FALLBACK_THRESHOLD,
        val thresholdSource: String = "",
        val inChannels: Int = 0,
        val outChannels: Int = 0,
        val expectChannels: Int = 0,
        val modelDim: IntArray = IntArray(3),
        /** 装载失败时把"找过哪些外置路径"原样带出去，免得用户以为模型在 APK 里 */
        val searchedPaths: String = "",
    ) {
        fun channelsMatch(): Boolean = inChannels == expectChannels
        fun summary(): String = if (ok) {
            "下颌管模型已装载：$modelPath；$thresholdSource"
        } else "下颌管模型不可用：$error；已找 $searchedPaths"
    }

    /** 一次追踪的结果（tracks 里的点就是主机侧要逐点比对的折线） */
    data class CanalTrack(
        val voxels: Int,
        val traceAxis: Int,
        val radiusMm: Double,
        val groups: Int,
        val minLinearIndex: Long,
        val points: List<DoubleArray>,
    ) {
        /** 折线长度（mm）：叠加层与报告里"这条管道追了多长"的唯一可见量 */
        fun lengthMm(): Double {
            var s = 0.0
            for (i in 1 until points.size) {
                val a = points[i - 1]; val b = points[i]
                s += Math.hypot(Math.hypot(b[0] - a[0], b[1] - a[1]), b[2] - a[2])
            }
            return s
        }
    }

    data class CanalResult(
        val ok: Boolean = false,
        val error: String = "",
        val thresholdUsed: Double = 0.0,
        val canalVoxels: Int = 0,
        val totalMs: Double = 0.0,
        val prepMs: Double = 0.0,
        val inferMs: Double = 0.0,
        val postMs: Double = 0.0,
        val allocBytes: Long = 0,
        val modelDim: IntArray = IntArray(3),
        val redVoxelMm3: Double = 0.0,
        val runtimeReady: Boolean = false,
        val runtimeInfo: String = "",
        val adopted: Int = 0,
        val autoNerveCount: Int = 0,
        val manualNerveCount: Int = 0,
        /** 本次回执里 native 报告的判级开关状态（UI 回显，别让文案和颜色矛盾） */
        val feedsGrading: Boolean = false,
        /** 判级实际使用的神经管条数（开关关闭时 = 手描条数） */
        val gradingNerveCount: Int = 0,
        val trackSummary: String = "",
        val tracks: List<CanalTrack> = emptyList(),
    ) {
        /** 一行状态（AI 页状态栏与第 8 轮测试报告引用同一份措辞） */
        fun line(): String = if (!ok) "下颌管：$error" else {
            "下颌管：掩膜 ${canalVoxels} 体素 → 追出 ${tracks.size} 条" +
                    "（入库 $adopted，自动共 $autoNerveCount 条，手描 $manualNerveCount 条）；" +
                    "阈值 $thresholdUsed；耗时 ${"%.0f".format(totalMs)}ms"
        }

        companion object {
            fun parse(text: String): CanalResult {
                val j = try { JSONObject(text) } catch (e: Exception) {
                    return CanalResult(error = "返回值不是 JSON")
                }
                val arr = j.optJSONArray("tracks") ?: JSONArray()
                val tracks = ArrayList<CanalTrack>(arr.length())
                for (i in 0 until arr.length()) {
                    val o = arr.optJSONObject(i) ?: continue
                    val pts = o.optJSONArray("pointsMm") ?: JSONArray()
                    val pl = ArrayList<DoubleArray>(pts.length())
                    for (q in 0 until pts.length()) {
                        val p = pts.optJSONArray(q) ?: continue
                        if (p.length() >= 3) {
                            pl.add(doubleArrayOf(p.optDouble(0, 0.0), p.optDouble(1, 0.0),
                                p.optDouble(2, 0.0)))
                        }
                    }
                    tracks.add(CanalTrack(
                        voxels = o.optInt("voxels"),
                        traceAxis = o.optInt("traceAxis"),
                        radiusMm = o.optDouble("radiusMm", 0.0),
                        groups = o.optInt("groups"),
                        minLinearIndex = o.optLong("minLinearIndex"),
                        points = pl,
                    ))
                }
                val dims = j.optJSONArray("modelDim")
                return CanalResult(
                    ok = j.optBoolean("ok"),
                    error = j.optString("error"),
                    thresholdUsed = j.optDouble("thresholdUsed", j.optDouble("threshold", 0.0)),
                    // 字段名沿用牙齿通道的 toothVoxels（同一个 AiResult 结构），
                    // 在下颌管通道它就是"管腔体素数"
                    canalVoxels = j.optInt("toothVoxels"),
                    totalMs = j.optDouble("totalMs", 0.0),
                    prepMs = j.optDouble("prepMs", 0.0),
                    inferMs = j.optDouble("inferMs", 0.0),
                    postMs = j.optDouble("postMs", 0.0),
                    allocBytes = j.optLong("allocBytes"),
                    modelDim = IntArray(3) { i -> dims?.optInt(i, 0) ?: 0 },
                    redVoxelMm3 = j.optDouble("redVoxelMm3", 0.0),
                    runtimeReady = j.optBoolean("runtimeReady"),
                    runtimeInfo = j.optString("runtimeInfo"),
                    adopted = j.optInt("adopted"),
                    autoNerveCount = j.optInt("autoNerveCount"),
                    manualNerveCount = j.optInt("manualNerveCount"),
                    feedsGrading = j.optBoolean("feedsGrading"),
                    // 旧回执里没有 gradingNerveCount 时按"自动 + 手描"估：那对应
                    // 开关打开的情形。缺字段不会静默显示 0，状态行仍写得出来。
                    gradingNerveCount = if (j.has("gradingNerveCount")) {
                        j.optInt("gradingNerveCount")
                    } else {
                        j.optInt("autoNerveCount") + j.optInt("manualNerveCount")
                    },
                    trackSummary = j.optString("trackSummary"),
                    tracks = tracks,
                )
            }
        }
    }

    /** 叠加层描边色（与 C++ AiConst::CANAL_COLOR 同值；Int 就是 ARGB） */
    fun overlayColor(): Int = Color.parseColor("#B388FF")

    /** 装载（同步；调用方负责放 IO 线程）。模型缺失时不去碰 ORT，直接返回原因 */
    fun load(ctx: Context, handle: Long, threads: Int = DEFAULT_THREADS): LoadResult {
        if (handle == 0L) return LoadResult(false, "会话未创建（先解析序列）")
        val searched = searchPathsText(ctx)
        val model = findModel(ctx)
            ?: return LoadResult(false, "外置目录里没有 $CANAL_MODEL_NAME",
                searchedPaths = searched)
        val (thr, src) = readSpec(ctx)
        val so = AiEngine.resolveOrtSoPath(ctx)
        if (so.isEmpty()) return LoadResult(false, "找不到 libonnxruntime.so",
            searchedPaths = searched)
        val raw = AiJni.nativeLoadCanalModel(handle, so, model.absolutePath, CANAL_MODEL_NAME,
            threads, thr)
        val j = try {
            JSONObject(raw)
        } catch (e: Exception) {
            return LoadResult(false, "返回值不是 JSON: $raw", modelPath = model.absolutePath,
                threshold = thr, thresholdSource = src, searchedPaths = searched)
        }
        val dims = j.optJSONArray("modelDim")
        return LoadResult(
            ok = j.optBoolean("ok"),
            error = j.optString("error"),
            runtimeInfo = j.optString("runtimeInfo"),
            modelPath = model.absolutePath,
            threshold = if (j.has("threshold")) j.optDouble("threshold", thr) else thr,
            thresholdSource = src,
            inChannels = j.optInt("inChannels"),
            outChannels = j.optInt("outChannels"),
            expectChannels = j.optInt("expectChannels"),
            modelDim = IntArray(3) { i -> dims?.optInt(i, 0) ?: 0 },
            searchedPaths = searched,
        )
    }

    fun release(handle: Long) = AiJni.nativeReleaseCanalModel(handle)

    fun ready(handle: Long): Boolean = AiJni.nativeCanalReady(handle)

    /** 推理 + 追踪（CPU 密集，几十 MB 临时缓冲）：必须在 Default 线程 */
    suspend fun run(
        handle: Long,
        threshold: Double,
        keepParity: Boolean = false,
    ): CanalResult = withContext(Dispatchers.Default) {
        CanalResult.parse(AiJni.nativeRunCanal(handle, threshold, keepParity))
    }

    fun status(handle: Long): CanalResult = CanalResult.parse(AiJni.nativeCanalStatus(handle))

    /**
     * 自动下颌管是否进 S-05 / AI-03 判级。
     *
     * 关闭（默认）时自动管道仍然会绘制、归档、进报告，只是不改种植体颜色；
     * 打开是医生的主动动作，UI 必须在同一屏把主机实测误差说清楚
     * （16 条探针轴上平均偏 7.81mm、最大 18.16mm，而红档判据只有 2.0mm）。
     */
    fun setFeedGrading(handle: Long, feed: Boolean) = AiJni.nativeCanalFeedGrading(handle, feed)

    fun feedsGrading(handle: Long): Boolean = AiJni.nativeCanalFeedsGrading(handle)

    /** @return Pair(移除条数, 剩余神经管条数) */
    fun clear(handle: Long): Pair<Int, Int> {
        return try {
            val j = JSONObject(AiJni.nativeCanalClear(handle))
            j.optInt("removed") to j.optInt("remainingNerves")
        } catch (e: Exception) {
            0 to -1
        }
    }

    /** AC-08 取证：下颌管通道的 feat/prob/label/inst 落盘（与牙齿分开两个文件） */
    fun dumpParity(ctx: Context, handle: Long,
                   name: String = "canal_device.parity.bin"): String {
        val dir = File(ctx.filesDir, "ai_parity")
        dir.mkdirs()
        return AiJni.nativeCanalDumpParity(handle, File(dir, name).absolutePath)
    }
}
