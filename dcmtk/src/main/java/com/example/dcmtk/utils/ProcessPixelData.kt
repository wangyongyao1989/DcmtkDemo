package com.example.dcmtk.utils

import com.example.dcmtk.model.PixelDataNew
import kotlin.math.abs
import kotlin.math.log10
import kotlin.math.roundToInt

/**
 * 像素数据处理与窗宽窗位计算。
 *
 * 依据论文《自适应调节医学CT序列图像窗宽窗位算法》及需求文档 ct_window_requirements.html 实现。
 * 支持以下窗宽窗位计算方法：
 * - DEFAULT:           默认固定窗宽窗位法（论文基线，参数化预设）
 * - CUMULATIVE_72:     文献[7] 72%累计面积法（窗宽固定508）
 * - BIMODAL_PEAK:      文献[6] 双峰直方图法（依据摘要近似实现）
 * - ADAPTIVE_HISTOGRAM: 本文自适应序列直方图法（论文核心，完整实现）
 * - HISTOGRAM_TYPE:    文献[8] 直方图分类法（仅预留接口，调用时抛异常）
 * - MIN_MAX:           工程基线（非论文方法，供对比参考）
 *
 * 窗变换公式（论文）：
 *   若 x < c - w/2，则 y = 0
 *   若 c - w/2 ≤ x ≤ c + w/2，则 y = 255/w × (x - c + w/2)
 *   若 x > c + w/2，则 y = 255
 *
 * HU 转换：H = raw × S + I（S=slope, I=intercept，对应 DICOM 0028,1053 / 0028,1052）
 */
object ProcessPixelData {

    // ==================== 公共类型 ====================

    /** 窗宽窗位计算方法枚举 */
    enum class WindowCalcMethod(val displayName: String) {
        DEFAULT("1. 默认固定窗(127.5/255)"),
        CUMULATIVE_72("2. 72%累计面积法(文献[7])"),
        BIMODAL_PEAK("3. 双峰直方图法(文献[6]近似)"),
        ADAPTIVE_HISTOGRAM("4. 自适应序列直方图法(本文)"),
        HISTOGRAM_TYPE("5. 直方图分类法(文献[8]预留)"),
        MIN_MAX("6. Min-Max(工程基线)")
    }

    /** T1 合并模式 */
    enum class MergeMode {
        /** 按频数阈值合并：|freq差| < T1 = T × N1 */
        COUNT_THRESHOLD,
        /** 按比率阈值合并：|freq差| < N1（论文字面版） */
        RATIO_THRESHOLD
    }

    /** 算法参数配置 */
    data class WindowConfig(
        val nbins: Int = 256,
        val n0: Double = 0.0015,
        val n1: Double = 0.0015,
        val slope: Double = 1.0,
        val intercept: Double = 0.0,
        val roundHbins: Boolean = true,
        val mergeMode: MergeMode = MergeMode.COUNT_THRESHOLD,
        // 默认窗预设
        val defaultWindowWidth: Double = 255.0,
        val defaultWindowCenter: Double = 127.5,
        // 72%累计面积法参数
        val cumulativeRatio: Double = 0.72,
        val cumulativeWindowWidth: Double = 508.0
    )

    /** 窗宽窗位计算结果 */
    data class WindowResult(
        val windowWidth: Double,
        val windowCenter: Double,
        val detail: String,
        val debugInfo: Map<String, Any?> = emptyMap(),
        val implementationLevel: String = "full"
    )

    /** 评价指标结果 */
    data class EvalResult(
        val mse: Double,
        val psnr: Double,
        val snr: Double
    )

    // ==================== 公共 API ====================

    /** 兼容旧接口：默认使用 MIN_MAX 方法 */
    fun process(raw: ByteArray, imageWidth: Int, imageHeight: Int): PixelDataNew {
        return process(raw, imageWidth, imageHeight, WindowCalcMethod.MIN_MAX)
    }

    /** 按指定算法计算窗宽窗位并构造 PixelData（默认参数） */
    fun process(
        raw: ByteArray,
        imageWidth: Int,
        imageHeight: Int,
        method: WindowCalcMethod
    ): PixelDataNew {
        return process(raw, imageWidth, imageHeight, method, WindowConfig())
    }

    /** 按指定算法和参数计算窗宽窗位并构造 PixelData */
    fun process(
        raw: ByteArray,
        imageWidth: Int,
        imageHeight: Int,
        method: WindowCalcMethod,
        config: WindowConfig
    ): PixelDataNew {
        val pixels = decodeRaw(raw)
        val numPixels = pixels.size

        var minVal = Int.MAX_VALUE
        var maxVal = Int.MIN_VALUE
        for (v in pixels) {
            if (v < minVal) minVal = v
            if (v > maxVal) maxVal = v
        }
        if (numPixels == 0) {
            minVal = 0
            maxVal = 0
        }

        val result = calcWindow(pixels, minVal, maxVal, method, config)

        return PixelDataNew(
            imageHeight,
            imageWidth,
            raw,
            maxVal,
            result.windowCenter.roundToInt(),
            result.windowWidth.roundToInt(),
            maxVal,
            0.0
        )
    }

    /** 仅计算窗宽窗位（默认参数） */
    fun calcWindow(
        pixels: IntArray,
        minVal: Int,
        maxVal: Int,
        method: WindowCalcMethod
    ): WindowResult {
        return calcWindow(pixels, minVal, maxVal, method, WindowConfig())
    }

    /**
     * 从 raw 字节数组直接计算窗宽窗位（含中间数据），无需外部解码。
     * 供 UI 层调用获取调试信息与评价指标。
     */
    fun calcWindowFromRaw(
        raw: ByteArray,
        method: WindowCalcMethod,
        config: WindowConfig = WindowConfig()
    ): WindowResult {
        val pixels = decodeRaw(raw)
        var minVal = Int.MAX_VALUE
        var maxVal = Int.MIN_VALUE
        for (v in pixels) {
            if (v < minVal) minVal = v
            if (v > maxVal) maxVal = v
        }
        if (pixels.isEmpty()) { minVal = 0; maxVal = 0 }
        return calcWindow(pixels, minVal, maxVal, method, config)
    }

    /**
     * 从 raw 字节数组计算评价指标 MSE/PSNR/SNR。
     * 参考图为直接映射 8-bit，调窗图按指定窗宽窗位映射。
     */
    fun evaluateFromRaw(
        raw: ByteArray,
        windowCenter: Double,
        windowWidth: Double
    ): EvalResult {
        val pixels = decodeRaw(raw)
        if (pixels.isEmpty()) return EvalResult(0.0, 0.0, 0.0)
        var minVal = Int.MAX_VALUE
        var maxVal = Int.MIN_VALUE
        for (v in pixels) {
            if (v < minVal) minVal = v
            if (v > maxVal) maxVal = v
        }
        if (pixels.isEmpty()) { minVal = 0; maxVal = 0 }
        val reference = directMap(pixels, minVal, maxVal)
        val windowed = applyWindow(pixels, windowCenter, windowWidth)
        return evaluate(reference, windowed)
    }

    /** 仅计算窗宽窗位（完整参数），返回包含中间数据的结果 */
    fun calcWindow(
        pixels: IntArray,
        minVal: Int,
        maxVal: Int,
        method: WindowCalcMethod,
        config: WindowConfig
    ): WindowResult {
        if (pixels.isEmpty()) {
            return WindowResult(1.0, 0.0, "空像素")
        }
        return when (method) {
            WindowCalcMethod.DEFAULT -> calcDefault(config)
            WindowCalcMethod.CUMULATIVE_72 -> calcCumulative72(pixels, minVal, maxVal, config)
            WindowCalcMethod.BIMODAL_PEAK -> calcBimodalPeak(pixels, minVal, maxVal, config)
            WindowCalcMethod.ADAPTIVE_HISTOGRAM -> calcAdaptiveHistogram(pixels, minVal, maxVal, config)
            WindowCalcMethod.HISTOGRAM_TYPE -> calcHistogramType()
            WindowCalcMethod.MIN_MAX -> calcMinMax(minVal, maxVal)
        }
    }

    // ==================== 方法 1: 默认固定窗宽窗位法 ====================
    // 论文基线方法，参数化预设。论文出现 (127.5,255) 与 (127.5,225) 两种写法。
    private fun calcDefault(config: WindowConfig): WindowResult {
        return WindowResult(
            config.defaultWindowWidth,
            config.defaultWindowCenter,
            "默认窗: WW=${config.defaultWindowWidth}, WL=${config.defaultWindowCenter}",
            debugInfo = mapOf(
                "method" to "default",
                "preset_ww" to config.defaultWindowWidth,
                "preset_wl" to config.defaultWindowCenter
            )
        )
    }

    // ==================== 方法 2: 文献[7] 72%累计面积法 ====================
    // 从灰度最小端开始累计直方图面积，累计达总面积 72% 时的灰度点为窗位，窗宽固定 508。
    private fun calcCumulative72(
        pixels: IntArray,
        minVal: Int,
        maxVal: Int,
        config: WindowConfig
    ): WindowResult {
        val nbins = config.nbins
        val range = (maxVal - minVal).coerceAtLeast(1)
        var hbins = range.toDouble() / nbins
        if (config.roundHbins) hbins = hbins.roundToInt().toDouble().coerceAtLeast(1.0)

        val hist = buildHistogram(pixels, minVal, maxVal, nbins, hbins)
        val total = pixels.size
        val threshold = (total * config.cumulativeRatio).toLong()

        var cumulative = 0L
        var targetBin = 0
        for (i in 0 until nbins) {
            cumulative += hist[i]
            if (cumulative >= threshold) {
                targetBin = i
                break
            }
        }

        val windowCenter = minVal + (targetBin + 0.5) * hbins
        val windowWidth = config.cumulativeWindowWidth

        return WindowResult(
            windowWidth,
            windowCenter,
            "72%累计面积法: 累计比=${config.cumulativeRatio}, 目标bin=$targetBin, " +
                    "累计=$cumulative/$total, WL=${windowCenter.roundToInt()}, WW=${windowWidth.roundToInt()}",
            debugInfo = mapOf(
                "method" to "cumulative_72",
                "Gmin" to minVal,
                "Gmax" to maxVal,
                "Hbins" to hbins,
                "nbins" to nbins,
                "T" to total,
                "cumulative_ratio" to config.cumulativeRatio,
                "cumulative_threshold" to threshold,
                "target_bin" to targetBin,
                "cumulative_at_target" to cumulative,
                "window_width" to windowWidth
            ),
            implementationLevel = "full"
        )
    }

    // ==================== 方法 3: 文献[6] 双峰直方图法（近似） ====================
    // 依据论文摘要近似实现：去掉最左侧高峰，寻找剩余直方图极小值点为窗底，
    // 最大值点为窗位，再推算窗宽。标注为 approximate_from_summary。
    private fun calcBimodalPeak(
        pixels: IntArray,
        minVal: Int,
        maxVal: Int,
        config: WindowConfig
    ): WindowResult {
        val nbins = config.nbins
        val range = (maxVal - minVal).coerceAtLeast(1)
        var hbins = range.toDouble() / nbins
        if (config.roundHbins) hbins = hbins.roundToInt().toDouble().coerceAtLeast(1.0)

        val hist = buildHistogram(pixels, minVal, maxVal, nbins, hbins)

        // 找到最左侧高峰（通常是背景/空气）
        var leftPeakIdx = 0
        var leftPeakFreq = 0
        for (i in 0 until nbins) {
            if (hist[i] > leftPeakFreq) {
                leftPeakFreq = hist[i]
                leftPeakIdx = i
            }
        }

        // 去掉最左侧高峰：将高峰及其邻域置零
        val suppressed = hist.copyOf()
        val suppressRadius = (nbins * 0.05).toInt().coerceAtLeast(1)
        for (i in (leftPeakIdx - suppressRadius).coerceAtLeast(0)..(leftPeakIdx + suppressRadius).coerceAtMost(nbins - 1)) {
            if (i < nbins) suppressed[i] = 0
        }

        // 在剩余直方图中寻找极小值点（窗底）和最大值点（窗位）
        var valleyIdx = -1
        var valleyFreq = Int.MAX_VALUE
        var peakIdx = -1
        var peakFreq = 0
        for (i in 0 until nbins) {
            if (suppressed[i] > 0) {
                if (suppressed[i] < valleyFreq) {
                    valleyFreq = suppressed[i]
                    valleyIdx = i
                }
                if (suppressed[i] > peakFreq) {
                    peakFreq = suppressed[i]
                    peakIdx = i
                }
            }
        }

        if (peakIdx < 0 || valleyIdx < 0) {
            // 回退到 Min-Max
            return calcMinMax(minVal, maxVal).copy(
                detail = "双峰法回退Min-Max: 未找到有效峰值",
                implementationLevel = "approximate_from_summary"
            )
        }

        val windowCenter = minVal + (peakIdx + 0.5) * hbins
        val windowBottom = minVal + (valleyIdx + 0.5) * hbins
        // 推算窗宽：2 × (窗位 - 窗底)
        var windowWidth = 2.0 * (windowCenter - windowBottom)
        if (windowWidth < 1.0) windowWidth = 1.0

        return WindowResult(
            windowWidth,
            windowCenter,
            "双峰直方图法(近似): 左峰bin=$leftPeakIdx(freq=$leftPeakFreq), " +
                    "窗底bin=$valleyIdx, 窗位bin=$peakIdx, WL=${windowCenter.roundToInt()}, WW=${windowWidth.roundToInt()}",
            debugInfo = mapOf(
                "method" to "bimodal_peak",
                "Gmin" to minVal,
                "Gmax" to maxVal,
                "Hbins" to hbins,
                "left_peak_bin" to leftPeakIdx,
                "left_peak_freq" to leftPeakFreq,
                "valley_bin" to valleyIdx,
                "peak_bin" to peakIdx,
                "window_bottom" to windowBottom,
                "implementation_note" to "依据论文摘要近似实现，不等同于原文精确复现"
            ),
            implementationLevel = "approximate_from_summary"
        )
    }

    // ==================== 方法 4: 本文自适应序列直方图法（核心） ====================
    // 论文完整步骤：
    //   1. 遍历得 Gmax、Gmin
    //   2. nbins=256, Hbins=(Gmax-Gmin)/nbins
    //   3. 构造全局直方图
    //   4. T=总频数, T0=T×N0, T1=T×N1 (N0,N1 ∈ [0.0005,0.0025], 默认0.0015)
    //   5. 删除频数<T0 的分组
    //   6. 合并相邻频数差值<T1 的分组
    //   7. 统计剩余分组数 B
    //   8. c = B × Hbins × 0.125,  w = B × Hbins + c
    private fun calcAdaptiveHistogram(
        pixels: IntArray,
        minVal: Int,
        maxVal: Int,
        config: WindowConfig
    ): WindowResult {
        val nbins = config.nbins
        val range = (maxVal - minVal).coerceAtLeast(1)
        var hbins = range.toDouble() / nbins
        if (config.roundHbins) hbins = hbins.roundToInt().toDouble().coerceAtLeast(1.0)

        // ③ 构造直方图
        val hist = buildHistogram(pixels, minVal, maxVal, nbins, hbins)
        val histBefore = hist.copyOf()

        // ④ 计算阈值
        val T = pixels.size
        val t0 = (T * config.n0).toInt().coerceAtLeast(1)
        val t1: Double = when (config.mergeMode) {
            MergeMode.COUNT_THRESHOLD -> (T * config.n1).coerceAtLeast(1.0)
            MergeMode.RATIO_THRESHOLD -> config.n1
        }

        // ⑤ 删除频数 < T0 的分组
        val filtered = IntArray(nbins) { if (hist[it] >= t0) hist[it] else 0 }

        // ⑥ 合并相邻频数差值 < T1 的分组
        // 策略：遍历 filtered，相邻非零 bin 若频数差 < T1 则归为同一组，否则另起一组
        val groups = mutableListOf<IntArray>() // 每组存 [startBin, endBin]
        var groupStart = -1
        var prevFreq = 0
        for (i in 0 until nbins) {
            if (filtered[i] > 0) {
                if (groupStart < 0) {
                    // 新组开始
                    groupStart = i
                    prevFreq = filtered[i]
                } else {
                    // 检查是否与前一非零 bin 频数差 < T1
                    if (abs(filtered[i] - prevFreq) < t1) {
                        // 同组，继续
                        prevFreq = filtered[i]
                    } else {
                        // 频数跳变 ≥ T1，关闭当前组，开启新组
                        groups.add(intArrayOf(groupStart, i - 1))
                        groupStart = i
                        prevFreq = filtered[i]
                    }
                }
            } else {
                // 遇到零 bin，关闭当前组
                if (groupStart >= 0) {
                    groups.add(intArrayOf(groupStart, i - 1))
                    groupStart = -1
                }
            }
        }
        // 处理末尾
        if (groupStart >= 0) {
            groups.add(intArrayOf(groupStart, nbins - 1))
        }

        // ⑦ 统计剩余分组数 B
        val B = groups.size
        val histAfter = filtered.copyOf()

        // ⑧ 按论文公式计算窗位和窗宽
        // c = B × Hbins × 0.125
        // w = B × Hbins + c
        val cFormula = B * hbins * 0.125
        val wFormula = B * hbins + cFormula
        // 窗位相对于 Gmin，实际窗位 = Gmin + c
        val windowCenter = minVal + cFormula
        val windowWidth = wFormula.coerceAtLeast(1.0)

        val groupsStr = groups.joinToString(",") { "[${it[0]}..${it[1]}]" }

        return WindowResult(
            windowWidth,
            windowCenter,
            "自适应直方图法: B=$B, Hbins=${hbins.roundToInt()}, T0=$t0, T1=$t1, " +
                    "c=${cFormula.roundToInt()}, w=${wFormula.roundToInt()}, " +
                    "WL=${windowCenter.roundToInt()}, WW=${windowWidth.roundToInt()}",
            debugInfo = mapOf(
                "method" to "adaptive_histogram",
                "Gmin" to minVal,
                "Gmax" to maxVal,
                "Hbins" to hbins,
                "nbins" to nbins,
                "T" to T,
                "N0" to config.n0,
                "N1" to config.n1,
                "T0" to t0,
                "T1" to t1,
                "merge_mode" to config.mergeMode.name,
                "B" to B,
                "c_formula" to cFormula,
                "w_formula" to wFormula,
                "groups" to groupsStr,
                "histogram_before" to histBefore,
                "histogram_after" to histAfter
            ),
            implementationLevel = "full"
        )
    }

    // ==================== 方法 5: 文献[8] 直方图分类法（预留接口） ====================
    // 论文仅给出四种直方图类型名称，无分类规则和计算公式，仅预留接口。
    private fun calcHistogramType(): WindowResult {
        throw UnsupportedOperationException(
            "文献[8]直方图分类法仅预留接口，未提供分类规则和计算公式，无法实现。" +
                    "类型枚举：普通单峰、普通双峰、灰度集中单峰、灰度靠右双峰。"
        )
    }

    // ==================== 工程基线: Min-Max ====================
    private fun calcMinMax(minVal: Int, maxVal: Int): WindowResult {
        var width = (maxVal - minVal).toDouble()
        if (width < 1.0) width = 1.0
        val center = minVal + width / 2.0
        return WindowResult(
            width,
            center,
            "Min-Max: min=$minVal, max=$maxVal",
            debugInfo = mapOf(
                "method" to "min_max",
                "Gmin" to minVal,
                "Gmax" to maxVal
            )
        )
    }

    // ==================== 窗变换与评价 ====================

    /**
     * 应用窗变换：将 16-bit 像素按窗宽窗位线性映射到 8-bit (0~255)。
     */
    fun applyWindow(pixels: IntArray, center: Double, width: Double): IntArray {
        val result = IntArray(pixels.size)
        val lower = center - width / 2.0
        val upper = center + width / 2.0
        val w = width.coerceAtLeast(1.0)
        for (i in pixels.indices) {
            result[i] = when {
                pixels[i] <= lower -> 0
                pixels[i] >= upper -> 255
                else -> ((pixels[i] - lower) / w * 255.0).roundToInt().coerceIn(0, 255)
            }
        }
        return result
    }

    /**
     * 直接映射（无调窗）：将 16-bit 线性映射到 8-bit，作为评价参考图。
     */
    fun directMap(pixels: IntArray, minVal: Int, maxVal: Int): IntArray {
        val range = (maxVal - minVal).coerceAtLeast(1).toDouble()
        return IntArray(pixels.size) {
            ((pixels[it] - minVal) / range * 255.0).roundToInt().coerceIn(0, 255)
        }
    }

    /**
     * 计算评价指标 MSE / PSNR / SNR。
     * - reference: 直接映射的 8-bit 图（不调窗）
     * - windowed:  调窗后的 8-bit 图
     */
    fun evaluate(reference: IntArray, windowed: IntArray): EvalResult {
        val n = reference.size
        if (n == 0 || windowed.size != n) {
            return EvalResult(0.0, 0.0, 0.0)
        }
        var sumSqDiff = 0.0
        var sumSqRef = 0.0
        for (i in 0 until n) {
            val diff = (reference[i] - windowed[i]).toDouble()
            sumSqDiff += diff * diff
            sumSqRef += reference[i].toDouble() * reference[i].toDouble()
        }
        val mse = sumSqDiff / n
        val psnr = if (mse > 0) 10.0 * log10(255.0 * 255.0 / mse) else 100.0
        // SNR = 10 * log10( sum(k^2) / (MSE * M * N) ) = 10 * log10( sum(k^2) / sumSqDiff )
        val snr = if (sumSqDiff > 0) 10.0 * log10(sumSqRef / sumSqDiff) else 100.0
        return EvalResult(mse, psnr, snr)
    }

    // ==================== 辅助方法 ====================

    /** 构造直方图 */
    private fun buildHistogram(
        pixels: IntArray,
        minVal: Int,
        maxVal: Int,
        nbins: Int,
        hbins: Double
    ): IntArray {
        val hist = IntArray(nbins)
        for (v in pixels) {
            val bin = ((v - minVal) / hbins).toInt().coerceIn(0, nbins - 1)
            hist[bin]++
        }
        return hist
    }

    /** 将大端序 16-bit raw 字节数组解码为 IntArray */
    private fun decodeRaw(raw: ByteArray): IntArray {
        val numPixels = raw.size / 2
        val pixels = IntArray(numPixels)
        for (i in 0 until numPixels) {
            val hi = raw[i * 2].toInt() and 0xFF
            val lo = raw[i * 2 + 1].toInt() and 0xFF
            pixels[i] = (hi shl 8) or lo
        }
        return pixels
    }
}
