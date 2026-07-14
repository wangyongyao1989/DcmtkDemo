package com.example.dcmtk.utils

import com.example.dcmtk.model.PixelData
import kotlin.math.roundToInt
import kotlin.math.sqrt

/**
 * 像素数据处理（dcm4che3 版 DicomFileUtils.kt 中引用，实现未给出）。
 * 此处提供一个与 native writeDcmFileFull 等价的 Kotlin 参考实现：
 * 扫描 16-bit 像素，按指定算法推导窗宽窗位/曝光/最大像素值。
 * live 代码走 native 路径，此类仅供编译完整性与对照参考。
 *
 * 支持的窗宽窗位计算方法参考论文《自适应调节医学CT序列图像窗宽窗位算法》：
 * - MIN_MAX:   基线方法，WW = max - min, WL = (max + min) / 2
 * - DEFAULT:   论文提到的默认窗 (127.5, 255)
 * - MEAN_STD:  统计自适应，WL = 均值, WW = k * 标准差
 * - PERCENTILE: 百分位法，去除离群值后求范围
 * - HISTOGRAM: 论文核心算法，直方图去噪(T0) + 相邻合并(T1) 后求窗宽窗位
 */
object ProcessPixelData {

    /** 窗宽窗位计算方法枚举 */
    enum class WindowCalcMethod(val displayName: String) {
        MIN_MAX("1. Min-Max(基线)"),
        DEFAULT("2. 默认窗(255/128)"),
        MEAN_STD("3. 均值-标准差"),
        PERCENTILE("4. 百分位法(P2-P98)"),
        HISTOGRAM("5. 直方图自适应(论文)")
    }

    /** 窗宽窗位计算结果，附带调试信息 */
    data class WindowResult(
        val winWidth: Int,
        val winCenter: Int,
        val detail: String
    )

    /**
     * 兼容旧接口：默认使用 MIN_MAX 方法。
     */
    fun process(raw: ByteArray, imageWidth: Int, imageHeight: Int): PixelData {
        return process(raw, imageWidth, imageHeight, WindowCalcMethod.MIN_MAX)
    }

    /**
     * 按指定算法计算窗宽窗位并构造 PixelData。
     * Raw 文件是大端序：第 1 字节为高位(Hi)，第 2 字节为低位(Lo)。
     */
    fun process(
        raw: ByteArray,
        imageWidth: Int,
        imageHeight: Int,
        method: WindowCalcMethod
    ): PixelData {
        val pixels = decodeRaw(raw)
        val numPixels = pixels.size

        var minVal = 65535
        var maxVal = 0
        for (v in pixels) {
            if (v < minVal) minVal = v
            if (v > maxVal) maxVal = v
        }
        if (numPixels == 0) {
            minVal = 0
            maxVal = 0
        }

        val result = calcWindow(pixels, minVal, maxVal, method)

        return PixelData(
            imageHeight,
            imageWidth,
            raw,
            maxVal,
            result.winCenter,
            result.winWidth,
            maxVal,
            0.0
        )
    }

    /**
     * 仅计算窗宽窗位（供外部直接调用，不构造 PixelData）。
     * 返回包含 WW/WL 和调试信息的结果。
     */
    fun calcWindow(
        pixels: IntArray,
        minVal: Int,
        maxVal: Int,
        method: WindowCalcMethod
    ): WindowResult {
        if (pixels.isEmpty()) {
            return WindowResult(1, 0, "空像素")
        }
        return when (method) {
            WindowCalcMethod.MIN_MAX -> calcMinMax(minVal, maxVal)
            WindowCalcMethod.DEFAULT -> calcDefault()
            WindowCalcMethod.MEAN_STD -> calcMeanStd(pixels)
            WindowCalcMethod.PERCENTILE -> calcPercentile(pixels, minVal, maxVal)
            WindowCalcMethod.HISTOGRAM -> calcHistogram(pixels, minVal, maxVal)
        }
    }

    // ---- 方法 1: Min-Max（基线，论文对比对象）----
    private fun calcMinMax(minVal: Int, maxVal: Int): WindowResult {
        var width = (maxVal - minVal).toDouble()
        if (width < 1.0) width = 1.0
        val center = minVal + width / 2.0
        return WindowResult(
            width.roundToInt(),
            center.roundToInt(),
            "Min-Max: min=$minVal, max=$maxVal"
        )
    }

    // ---- 方法 2: 默认窗（论文提到的默认窗 127.5/255）----
    private fun calcDefault(): WindowResult {
        return WindowResult(255, 128, "默认窗: WW=255, WL=128(论文默认127.5/255)")
    }

    // ---- 方法 3: 均值-标准差（统计自适应）----
    // WL = 均值, WW = 4 * 标准差（因子 4 为常用经验值）
    private fun calcMeanStd(pixels: IntArray): WindowResult {
        val n = pixels.size
        var sum = 0.0
        for (v in pixels) sum += v
        val mean = sum / n
        var variance = 0.0
        for (v in pixels) {
            val d = v - mean
            variance += d * d
        }
        variance /= n
        val std = sqrt(variance)
        var width = (4.0 * std)
        if (width < 1.0) width = 1.0
        return WindowResult(
            width.roundToInt(),
            mean.roundToInt(),
            "Mean-Std: mean=${mean.roundToInt()}, std=${std.roundToInt()}, WW=4*std"
        )
    }

    // ---- 方法 4: 百分位法（去除离群值）----
    // 取 P2~P98 百分位范围作为窗宽，中心为范围中点
    private fun calcPercentile(pixels: IntArray, minVal: Int, maxVal: Int): WindowResult {
        val sorted = pixels.sortedArray()
        val n = sorted.size
        // P2 和 P98 索引
        val loIdx = ((n - 1) * 0.02).toInt().coerceIn(0, n - 1)
        val hiIdx = ((n - 1) * 0.98).toInt().coerceIn(0, n - 1)
        val loVal = sorted[loIdx]
        val hiVal = sorted[hiIdx]
        var width = (hiVal - loVal).toDouble()
        if (width < 1.0) width = 1.0
        val center = loVal + width / 2.0
        return WindowResult(
            width.roundToInt(),
            center.roundToInt(),
            "Percentile(P2-P98): lo=$loVal, hi=$hiVal"
        )
    }

    // ---- 方法 5: 直方图自适应（论文核心算法）----
    // 步骤: ①绘制直方图 ②去除频数<T0的bin  ③合并相邻频数差值<T1的bin  ④求窗宽窗位
    private fun calcHistogram(pixels: IntArray, minVal: Int, maxVal: Int): WindowResult {
        val numBins = 256
        val range = (maxVal - minVal).coerceAtLeast(1)
        val binSize = range.toDouble() / numBins

        // ① 绘制直方图
        val hist = IntArray(numBins)
        for (v in pixels) {
            val bin = (((v - minVal) / binSize).toInt()).coerceIn(0, numBins - 1)
            hist[bin]++
        }

        val totalPixels = pixels.size
        // T0: 频数阈值，低于此值的 bin 视为噪声去除（取总像素的 0.1%）
        val t0 = (totalPixels * 0.001).toInt().coerceAtLeast(1)
        // T1: 相邻 bin 频数差值阈值，小于此值则合并（取最大频数的 5%）
        val maxFreq = hist.maxOrNull() ?: 1
        val t1 = (maxFreq * 0.05).toInt().coerceAtLeast(1)

        // ② 去除频数 < T0 的 bin
        val filtered = IntArray(numBins) { if (hist[it] >= t0) hist[it] else 0 }

        // ③ 合并相邻频数差值 < T1 的 bin
        // 策略：遍历连续非零区间，在区间内若相邻 bin 频数差 < T1 则视为同一组
        // 最终取最大连续区间作为有效灰度范围
        var bestStart = 0
        var bestEnd = -1
        var bestCount = 0
        var curStart = -1
        for (i in 0 until numBins) {
            if (filtered[i] > 0) {
                if (curStart < 0) {
                    curStart = i
                }
                // 检查与前一个非零 bin 的频数差是否 < T1，若 >= T1 则断开当前区间
                if (i > 0 && filtered[i - 1] > 0 && kotlin.math.abs(filtered[i] - filtered[i - 1]) >= t1) {
                    // 频数跳变过大，结束当前区间
                    val curCount = countRange(filtered, curStart, i - 1)
                    if (curCount > bestCount) {
                        bestCount = curCount
                        bestStart = curStart
                        bestEnd = i - 1
                    }
                    curStart = i
                }
            } else {
                // 遇到零 bin，结束当前区间
                if (curStart >= 0) {
                    val curCount = countRange(filtered, curStart, i - 1)
                    if (curCount > bestCount) {
                        bestCount = curCount
                        bestStart = curStart
                        bestEnd = i - 1
                    }
                    curStart = -1
                }
            }
        }
        // 处理末尾区间
        if (curStart >= 0) {
            val curCount = countRange(filtered, curStart, numBins - 1)
            if (curCount > bestCount) {
                bestStart = curStart
                bestEnd = numBins - 1
            }
        }

        // ④ 根据有效区间计算窗宽窗位
        if (bestEnd < bestStart) {
            // 回退到 Min-Max
            return calcMinMax(minVal, maxVal).copy(
                detail = "Histogram回退Min-Max: 无有效区间(T0=$t0, T1=$t1)"
            )
        }

        val effMin = minVal + (bestStart * binSize).toInt()
        val effMax = minVal + ((bestEnd + 1) * binSize).toInt()
        var width = (effMax - effMin).toDouble()
        if (width < 1.0) width = 1.0
        val center = effMin + width / 2.0

        return WindowResult(
            width.roundToInt(),
            center.roundToInt(),
            "Histogram: bins=[$bestStart..$bestEnd], T0=$t0, T1=$t1" +
                    ", effMin=$effMin, effMax=$effMax, 像素数=$bestCount"
        )
    }

    /** 统计数组 [start, end] 区间内的频数总和 */
    private fun countRange(arr: IntArray, start: Int, end: Int): Int {
        var sum = 0
        for (i in start..end) sum += arr[i]
        return sum
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
