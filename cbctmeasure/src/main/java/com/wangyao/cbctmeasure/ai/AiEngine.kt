package com.wangyao.cbctmeasure.ai

import android.content.Context
import android.os.Build
import android.util.Log
import com.wangyao.cbctmeasure.jni.AiJni
import com.wangyao.cbctmeasure.model.AiResultInfo
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.io.IOException

/**
 * AI-01 / AI-03 的 Kotlin 侧编排（PRD 5.6 的"AI 层"入口，不含任何算法）。
 *
 * 这里只负责四件"必须依赖 Android 环境"的事：
 *   1) 找到 libonnxruntime.so 的真实可 dlopen 路径；
 *   2) 把 assets/models 里的 ONNX 模型与标定参数释放到私有目录；
 *   3) 把耗时的整次推理放到 Default 线程，别让渲染线程掉帧（PC-02）；
 *   4) AC-08 取证：把真机的 feat/prob/label/inst 落到可被 adb run-as 拉走的路径。
 *
 * 阈值不在这里发明：它来自随模型一起交付的 teeth_cnn.json（主机侧在验证集上
 * 标定得到），代码里的 DEFAULT_THRESHOLD 只是"规格文件缺失时的保守回退"，
 * 且必须与 cpp/include/AiTypes.h 的 AiConst::TOOTH_THRESHOLD 保持一致。
 */
object AiEngine {

    private const val TAG = "CbctMeasureAi"

    const val MODEL_ASSET_DIR = "models"
    const val MODEL_ASSET_NAME = "teeth_cnn.onnx"
    const val SPEC_ASSET_NAME = "teeth_cnn.json"

    /** 与 C++ AiConst::TOOTH_THRESHOLD 同步的回退值（0.5 = 2 类 softmax 的平票线） */
    const val DEFAULT_THRESHOLD = 0.5

    /** ORT 的线程数：平板 8 核，留一半给渲染与手势（PC-02 30FPS 预算） */
    const val DEFAULT_THREADS = 4

    /** 装载结果（UI 直接把这些字段显示在状态栏，报告也引用同一份） */
    data class LoadResult(
        val ok: Boolean,
        val error: String = "",
        val runtimeInfo: String = "",
        val soPath: String = "",
        val usedSoPath: String = "",
        val modelPath: String = "",
        val threshold: Double = DEFAULT_THRESHOLD,
        val inChannels: Int = 0,
        val outChannels: Int = 0,
        val expectChannels: Int = 0,
        val modelDim: IntArray = IntArray(3),
    ) {
        /** 特征通道数必须与 core 的构造一致，否则掩膜没有意义 */
        fun channelsMatch(): Boolean = inChannels == expectChannels

        fun summary(): String = if (ok) {
            "运行时 $runtimeInfo（so=$usedSoPath）；模型 $modelPath 已装载；阈值 $threshold；输入 ${modelDim.joinToString("x")}"
        } else {
            "AI 不可用：$error"
        }
    }

    /** 推理 + 自动测量的整次回执 */
    data class SegmentResult(
        val info: AiResultInfo,
        val autoMeasureCount: Int = 0,
    )

    // =========================================================================
    // 1) libonnxruntime.so 的路径
    // =========================================================================

    /**
     * 返回一个"值得尝试 dlopen"的绝对路径。
     *
     * 为什么要列候选而不是直接拼 nativeLibraryDir：AAR 交付的 .so 是否被安装期
     * 解到磁盘，取决于 AGP 的 jniLibs.useLegacyPackaging（本工程没有显式设置，
     * 走的是默认 false -> .so 以不压缩方式留在 APK 内原位映射）。这种情况下
     * linker 认的是 "<apk>!/lib/<abi>/libxxx.so" 这种写法，而不是
     * nativeLibraryDir 下的普通文件路径。两种形态都试，并把真正用的那条记进日志，
     * 报告里"运行时怎么加载的"就有据可查。
     */
    fun ortSoCandidates(ctx: Context): List<String> {
        val out = ArrayList<String>(4)
        val abi = try {
            // 不用 ApplicationInfo.primaryAbiList（API 34 才有；本工程 minSdk 24），
            // 本模块只编 arm64-v8a，SUPPORTED_ABIS 的首项在真机上即为它。
            Build.SUPPORTED_ABIS.firstOrNull()?.takeIf { it.isNotEmpty() } ?: "arm64-v8a"
        } catch (e: Exception) {
            "arm64-v8a"
        }
        val nativeDir = ctx.applicationInfo.nativeLibraryDir
        if (!nativeDir.isNullOrEmpty()) {
            out.add(File(nativeDir, LIB_ORT).absolutePath)
        }
        val apk = ctx.applicationInfo.sourceDir
        if (!apk.isNullOrEmpty()) {
            out.add("$apk!/lib/$abi/$LIB_ORT")
        }
        ctx.applicationInfo.splitSourceDirs?.forEach { sp ->
            out.add("$sp!/lib/$abi/$LIB_ORT")
        }
        return out
    }

    /**
     * 把候选列表拼成 C++ 侧约定的 ';' 分隔串（OrtEngine::load 会逐条试 dlopen，
     * 并额外补一条裸 SONAME 兜底）。
     *
     * 不在 Kotlin 侧提前"选一条"：nativeLibraryDir 下的文件是否存在取决于打包方式
     * （AGP 默认 useLegacyPackaging=false，真机实测确实没有落盘文件，见
     * doc/AI_ONNX_TEST_REPORT.md 的 dlopen 取证），而 `!/` 形态又不是 File，
     * exists() 恒为 false —— 只有 linker 能给出结论，所以整条列表交下去。
     */
    fun resolveOrtSoPath(ctx: Context): String {
        val cs = ortSoCandidates(ctx)
        val disk = cs.firstOrNull { !it.contains("!/") && File(it).isFile }
        Log.d(TAG, "ORT so 候选（${cs.size} 条，磁盘形态${if (disk == null) "无" else "有 $disk"}）：" +
                cs.joinToString(" | ")
        )
        return cs.joinToString(";")
    }

    // =========================================================================
    // 2) assets -> 私有目录
    // =========================================================================

    /** 把 assets 的 models 目录内容释放到 filesDir/ai_models（按字节数判断是否需要重拷） */
    fun releaseModelAssets(ctx: Context): File {
        val dir = File(ctx.filesDir, "ai_models")
        dir.mkdirs()
        val target = File(dir, MODEL_ASSET_NAME)
        copyIfChanged(ctx, "$MODEL_ASSET_DIR/$MODEL_ASSET_NAME", target)
        // 规格文件可选（缺了就用 DEFAULT_THRESHOLD），所以这里允许安静失败
        if (!copySilently(ctx, "$MODEL_ASSET_DIR/$SPEC_ASSET_NAME", File(dir, SPEC_ASSET_NAME))) {
            Log.w(TAG, "缺少 $SPEC_ASSET_NAME，阈值回退 $DEFAULT_THRESHOLD")
        }
        return dir
    }

    private fun copyIfChanged(ctx: Context, assetPath: String, target: File) {
        val want = assetSize(ctx, assetPath)
        if (target.isFile && (want <= 0 || target.length() == want)) {
            Log.d(TAG, "复用已释放的 ${target.name} ($want B)")
            return
        }
        ctx.assets.open(assetPath).use { input ->
            FileOutputStream(target).use { output -> input.copyTo(output, 8192) }
        }
        Log.d(TAG, "已释放 ${target.name} -> ${target.length()} B")
    }

    /** 可选文件：不存在就安静返回 false，不抛 */
    private fun copySilently(ctx: Context, assetPath: String, target: File): Boolean = try {
        copyIfChanged(ctx, assetPath, target)
        true
    } catch (e: IOException) {
        false
    }

    private fun assetSize(ctx: Context, assetPath: String): Long = try {
        val fd = ctx.assets.openFd(assetPath)
        val len = fd.declaredLength
        fd.close()
        if (len > 0) len else 0L
    } catch (e: Exception) {
        // 压缩过的 asset 没有 declaredLength：退化成"每次重拷"，代价可接受（模型 <20KB）
        0L
    }

    /** 读随模型交付的标定阈值；缺失或解析失败 -> DEFAULT_THRESHOLD */
    fun readThreshold(ctx: Context): Double {
        val spec = File(ctx.filesDir, "ai_models/$SPEC_ASSET_NAME")
        if (!spec.isFile) return DEFAULT_THRESHOLD
        return try {
            val j = JSONObject(spec.readText())
            val t = j.optDouble("threshold", Double.NaN)
            if (t.isNaN() || t <= 0.0 || t >= 1.0) DEFAULT_THRESHOLD else t
        } catch (e: Exception) {
            Log.w(TAG, "阈值规格解析失败: ${e.message}")
            DEFAULT_THRESHOLD
        }
    }

    // =========================================================================
    // 3) 装载 / 推理 / 取证
    // =========================================================================

    /**
     * 装载运行时与模型（同步方法；调用方负责放到 IO/Default 线程）。
     *
     * 失败不抛异常，只把 error 带回来 —— 上层据此禁用 AI 按钮，
     * 而测量、ROI、规划、报告这些一期功能继续正常工作。
     */
    fun loadModel(ctx: Context, handle: Long, threads: Int = DEFAULT_THREADS): LoadResult {
        if (handle == 0L) return LoadResult(false, "会话未创建（先解析序列）")
        val dir = try {
            releaseModelAssets(ctx)
        } catch (e: Exception) {
            Log.e(TAG, "releaseModelAssets failed", e)
            return LoadResult(false, "模型释放失败: ${e.message}")
        }
        val model = File(dir, MODEL_ASSET_NAME)
        if (!model.isFile) return LoadResult(false, "找不到模型文件 ${model.absolutePath}")
        val so = resolveOrtSoPath(ctx)
        if (so.isEmpty()) return LoadResult(false, "找不到 libonnxruntime.so")

        val raw = AiJni.nativeLoadModel(handle, so, model.absolutePath, MODEL_ASSET_NAME, threads)
        val j = try {
            JSONObject(raw)
        } catch (e: Exception) {
            return LoadResult(false, "返回值不是 JSON: $raw", soPath = so, modelPath = model.path)
        }
        val dims = j.optJSONArray("modelDim")
        val modelDim = IntArray(3) { i -> dims?.optInt(i, 0) ?: 0 }
        return LoadResult(
            ok = j.optBoolean("ok"),
            error = j.optString("error"),
            runtimeInfo = j.optString("runtimeInfo"),
            soPath = so,
            usedSoPath = j.optString("runtimeSo"),
            modelPath = model.absolutePath,
            threshold = readThreshold(ctx),
            inChannels = j.optInt("inChannels"),
            outChannels = j.optInt("outChannels"),
            expectChannels = j.optInt("expectChannels"),
            modelDim = modelDim,
        )
    }

    fun release(handle: Long) {
        if (handle != 0L) AiJni.nativeReleaseModel(handle)
    }

    /** 推理（CPU 密集，几十 MB 临时缓冲）：必须在 Default 线程 */
    suspend fun segment(
        handle: Long,
        threshold: Double,
        keepParity: Boolean = false,
        autoMeasure: Boolean = true,
        boneDensity: Boolean = true,
    ): SegmentResult = withContext(Dispatchers.Default) {
        val info = AiJni.runSegment(handle, threshold, keepParity)
        val n = if (info.ok && autoMeasure) AiJni.nativeAiAutoMeasure(handle, boneDensity) else 0
        SegmentResult(info, n)
    }

    /** 只查回执，不重算（状态栏刷新用） */
    fun status(handle: Long): AiResultInfo = AiJni.status(handle)

    fun setOverlayVisible(handle: Long, visible: Boolean) =
        AiJni.nativeAiOverlayVisible(handle, visible)

    /** AI-03 候选位点（推理已经算完全弓/咬合平面/骨量，这里只排序） */
    suspend fun recommend(
        handle: Long,
        minGapMm: Double = 5.0,
        maxOut: Int = 12,
    ): Pair<List<com.wangyao.cbctmeasure.model.AiCandidateInfo>, String> =
        withContext(Dispatchers.Default) { AiJni.recommend(handle, minGapMm, maxOut) }

    /**
     * AC-08 取证：把真机 feat/prob/label/inst 落盘。
     *
     * 落在 filesDir 下（debug 包可用 adb run-as 拉走），文件名固定，
     * 主机脚本 parity_ref/check_device_dump.py 按同一份格式读。
     */
    fun dumpParity(ctx: Context, handle: Long, name: String = "device_0101.parity.bin"): String {
        val dir = File(ctx.filesDir, "ai_parity")
        dir.mkdirs()
        val target = File(dir, name)
        return AiJni.nativeAiDumpParity(handle, target.absolutePath)
    }

    private const val LIB_ORT = "libonnxruntime.so"
}
