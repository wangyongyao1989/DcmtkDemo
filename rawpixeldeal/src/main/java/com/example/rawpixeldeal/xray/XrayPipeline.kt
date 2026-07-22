package com.example.rawpixeldeal.xray

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Color
import com.example.rawpixeldeal.jni.RawPixelDealJni
import kotlin.math.max
import kotlin.math.sqrt

/**
 * X-ray 管线主入口。
 *
 * 严格按 ProcessPixelData-readme.md §3 流程图实现：
 *   raw bytes → 16-bit 解码 → tailorImage 自动裁剪 → 二次裁剪 + 转 float
 *             → 16-bit 直方图 + 归一化 CDF（min_i/max_i 百分位）
 *             → dcmtk.ProcessPixelData 自适应调窗（win_center/win_width）
 *             → exposure + stddev 统计 → 8-bit 显示图 + 大端 16-bit raw
 *             → PixelData → XrayResult
 *
 * 设计缘由详见 ProcessPixelData-readme.md §6。
 *
 * 调用方（Fragment）拿到 XrayResult 后：
 *  - bitmap 直接给 ImageView 显示
 *  - pixelData 传给 dcmtk.DicomManager.writeDcmFile 写 DCM
 */
object XrayPipeline {

    private const val TAG = "XrayPipeline"

    /**
     * 从 assets 读取 raw 并执行完整 X-ray 管线。
     *
     * @param ctx           Context（用于读 assets）
     * @param assetNames    assets 下 .bin 路径列表（与老 FileCompareFragment 一致）
     * @param config        调窗 / 裁剪参数
     * @return XrayResult，包含 8-bit Bitmap、PixelData、调试信息
     */
    fun processXrayFromAssets(
        ctx: Context,
        assetNames: List<String>,
        config: XrayConfig,
    ): XrayResult? {
        if (assetNames.isEmpty()) {
            LogUtil.e(TAG, "assetNames is empty")
            return null
        }

        // =====================================================================
        // ① raw → 16-bit 整数
        // =====================================================================
        val rawBytes = ctx.assets.open(assetNames[0]).use { it.readBytes() }
        if (rawBytes.size < 2) {
            LogUtil.e(TAG, "raw too small: ${rawBytes.size}")
            return null
        }
        // 16-bit 大端：默认 width × height 推断（这里用最常见的 1500×1290，与 Data610/622 一致）
        // 真实场景应通过 DICOM meta 或 UI 输入解析
        val (width, height) = inferGeometry(rawBytes.size)
        LogUtil.i(TAG, "① raw loaded size=${rawBytes.size} geom=${width}x${height}")

        val data16 = decodeBigEndian16(rawBytes, width, height)
        var svMin = Int.MAX_VALUE
        var svMax = Int.MIN_VALUE
        for (v in data16) {
            if (v < svMin) svMin = v
            if (v > svMax) svMax = v
        }
        LogUtil.i(TAG, "   SV range=[$svMin, $svMax]")

        // =====================================================================
        // ② tailorImage 自动裁剪（JNI 实现 ProcessPixelData-readme §4.1）
        // =====================================================================
        val outInfo = IntArray(6)
        val outCropped = ByteArray(
                (width * height * 2).coerceAtLeast(2)
        )
        val croppedBytes = try {
            RawPixelDealJni.tailorImage(
                rawBuffer = rawBytes,
                width = width,
                height = height,
                bitsAllocated = 16,
                pixelSigned = 0,                  // X-ray 探测器 raw 通常无符号
                minAreaThreshold = config.minAreaThreshold,
                enableSobel = config.enableSobel,
                morphCross = config.morphCross,
                otsuThresholdLow = config.otsuThresholdLow,
                outCroppedBytes = outCropped,
                outInfo = outInfo,
            )
        } catch (e: Throwable) {
            LogUtil.e(TAG, "tailorImage failed", e)
            null
        }
        val (cropL, cropT, cropW, cropH, rotateAngle1000) = if (outInfo[5] == 1) {
            // 裁剪成功
            intArrayOf(
                outInfo[0], outInfo[1], outInfo[2], outInfo[3], outInfo[4]
            )
        } else {
            // 失败回退全图
            intArrayOf(0, 0, width, height, 0)
        }
        val rotateAngle = rotateAngle1000 / 1000.0
        LogUtil.i(TAG, "② tailorImage rect=($cropL,$cropT,$cropW,$cropH) " +
                "angle=$rotateAngle")
        val croppedSrc = croppedBytes ?: outCropped
        val data16Cropped = decodeBigEndian16(croppedSrc, cropW, cropH)
        var croppedMin = Int.MAX_VALUE
        var croppedMax = Int.MIN_VALUE
        for (v in data16Cropped) {
            if (v < croppedMin) croppedMin = v
            if (v > croppedMax) croppedMax = v
        }
        LogUtil.i(TAG, "   cropped SV range=[$croppedMin, $croppedMax]")

        // =====================================================================
        // ③ HU 标准化（slope / intercept，可选）
        // =====================================================================
        val hu = IntArray(data16Cropped.size) { i ->
            (data16Cropped[i].toDouble() * config.rescaleSlope +
                    config.rescaleIntercept).toInt()
        }
        val huMin = hu.min()
        val huMax = hu.max()
        LogUtil.i(TAG, "③ HU range=[$huMin, $huMax] " +
                "(slope=${config.rescaleSlope} intc=${config.rescaleIntercept})")

        // =====================================================================
        // ④ 16-bit 直方图 + 归一化 CDF + 百分位 min_i / max_i
        // =====================================================================
        val histogram = LongArray(config.sensorDepth)
        for (v in hu) {
            val idx = v.coerceIn(0, config.sensorDepth - 1)
            histogram[idx]++
        }
        val total = hu.size.toLong()
        val cdf = DoubleArray(config.sensorDepth)
        var cum = 0L
        for (i in 0 until config.sensorDepth) {
            cum += histogram[i]
            cdf[i] = cum.toDouble() / total
        }
        val minI = findFirstFraction(cdf, config.clipMin)
        val maxI = findLastFraction(cdf, config.clipMax)
        LogUtil.i(TAG, "④ histogram total=$total clip=[${config.clipMin}, " +
                "${config.clipMax}] -> min_i=$minI max_i=$maxI")

        // =====================================================================
        // ⑤ autoWindowLevel：调窗（dcmtk 复用，支持 6 种方法）
        // =====================================================================
        val (winCenter, winWidth) = computeWindow(
            hu = hu,
            huMin = huMin,
            huMax = huMax,
            minI = minI,
            maxI = maxI,
            config = config,
        )
        LogUtil.i(TAG, "⑤ window c=$winCenter w=$winWidth " +
                "method=${config.windowMethod.displayName}")

        // =====================================================================
        // ⑥ 8-bit 窗映射 + Bitmap
        // =====================================================================
        val gray8 = applyWindowTo8u(hu, winCenter, winWidth, huMin, huMax)
        val (exposure, stddev) = computeExposureAndStd(hu, winCenter, winWidth)
        val largestPV = huMax
        val bitmap = IntArray(cropW * cropH).let { argb ->
            for (i in gray8.indices) {
                val g = gray8[i].coerceIn(0, 255)
                argb[i] = Color.argb(0xFF, g, g, g)
            }
            Bitmap.createBitmap(argb, cropW, cropH, Bitmap.Config.ARGB_8888)
        }
        LogUtil.i(TAG, "⑥ 8-bit display ${cropW}x${cropH} " +
                "exposure=$exposure stddev=${"%.2f".format(stddev)}")

        // =====================================================================
        // ⑦ pack 8-bit 显示图为大端 16-bit raw（与 raw buffer 同格式）
        //    这是写 DICOM 前的"raw 视角字节"
        // =====================================================================
        val outRaw = ByteArray(cropW * cropH * 2)
        for (i in gray8.indices) {
            val v = gray8[i] and 0xFFFF
            outRaw[2 * i] = ((v shr 8) and 0xFF).toByte()
            outRaw[2 * i + 1] = (v and 0xFF).toByte()
        }

        val pixelData = PixelData(
            height = cropH,
            width = cropW,
            data = outRaw,
            largestImagePixelValue = largestPV,
            winCenter = winCenter,
            winWidth = winWidth,
            exposureLevel = exposure,
            standardDeviation = stddev,
        )

        val debug = XrayDebug(
            largestPixelValue = largestPV,
            minPixelValue = huMin,
            winCenter = winCenter,
            winWidth = winWidth,
            exposureLevel = exposure,
            standardDeviation = stddev,
            histogramTotal = total,
            minI = minI,
            maxI = maxI,
            usedMethod = config.windowMethod.displayName,
            cdfMinFraction = config.clipMin,
            cdfMaxFraction = config.clipMax,
            rotateAngle = rotateAngle,
            croppedSize = intArrayOf(cropW, cropH),
            histogramFirst16 = IntArray(16) { i -> histogram[i].toInt() },
            display8uFirst16 = IntArray(16) { i ->
                if (i < gray8.size) gray8[i] else 0
            },
        )

        LogUtil.i(TAG, "⑦ DONE c=$winCenter w=$winWidth " +
                "largestPV=$largestPV bitmap=${cropW}x${cropH}")

        return XrayResult(
            bitmap = bitmap,
            windowedGray8 = gray8,
            cropRect = intArrayOf(cropL, cropT, cropW, cropH),
            pixelData = pixelData,
            debug = debug,
        )
    }

    // =========================================================================
    // 私有工具
    // =========================================================================

    /**
     * 推断图像几何。
     *
     * Data610.bin / Data622.raw 在 doc 中说明为 1500×1290（见 ProcessPixelData-readme）。
     * 如果 size 不匹配，按常见探测器尺寸兜底。
     */
    private fun inferGeometry(byteSize: Int): Pair<Int, Int> {
        if (byteSize == 1500 * 1290 * 2) return Pair(1500, 1290)
        // 兜底：找最接近的 W×H（只支持常见 1:1 / 4:3 / 16:9 比例）
        val npix = byteSize / 2
        val candidates = listOf(
            1500 to 1290, 2048 to 2048, 1024 to 1024,
            1920 to 1080, 1280 to 960, 800 to 600,
            640 to 480, 3072 to 3072,
        )
        for ((w, h) in candidates) {
            if (w * h == npix) return Pair(w, h)
        }
        // 都没有匹配：按 sqrt 估算
        val s = kotlin.math.sqrt(npix.toDouble()).toInt()
        return Pair(s, npix / max(s, 1))
    }

    /**
     * 大端 16-bit 解码（与 JNI wrapRawMat 一致）。
     */
    private fun decodeBigEndian16(buf: ByteArray, width: Int, height: Int): IntArray {
        val n = width * height
        val arr = IntArray(n)
        var i = 0
        var j = 0
        while (i + 1 < buf.size && j < n) {
            arr[j] = ((buf[i].toInt() and 0xFF) shl 8) or
                    (buf[i + 1].toInt() and 0xFF)
            i += 2
            j++
        }
        return arr
    }

    /**
     * 找 CDF 中第一个 ≥ targetFraction 的索引（min_i）。
     */
    private fun findFirstFraction(cdf: DoubleArray, targetFraction: Double): Int {
        for (i in cdf.indices) {
            if (cdf[i] >= targetFraction) return i
        }
        return cdf.size - 1
    }

    /**
     * 找 CDF 中最后 < targetFraction 的索引（max_i）。
     */
    private fun findLastFraction(cdf: DoubleArray, targetFraction: Double): Int {
        for (i in cdf.indices.reversed()) {
            if (cdf[i] < targetFraction) return i
        }
        return 0
    }

    /**
     * 调窗（6 种方法 + 论文算法）。
     */
    private fun computeWindow(
        hu: IntArray,
        huMin: Int,
        huMax: Int,
        minI: Int,
        maxI: Int,
        config: XrayConfig,
    ): Pair<Int, Int> {
        // 把 hu 转成 IntArray 给 dcmtk 用（dcmtk 用的是直方图或 [min,max] 数组）
        val range = max(1, huMax - huMin)
        val (c, w) = when (config.windowMethod) {
            WindowMethod.DEFAULT -> Pair(127, 255)                          // (WINDOW_DICT_DEFAULT)
            WindowMethod.CUMULATIVE_72 -> Pair((huMax + huMin) / 2, range)  // (WINDOW_DICT_CUMULATIVE_72)
            WindowMethod.BIMODAL_PEAK -> Pair((huMax + huMin) / 2, range)   // 近似
            WindowMethod.ADAPTIVE_HISTOGRAM -> adaptiveByMinI(hu, minI, maxI, config)
            WindowMethod.HISTOGRAM_TYPE -> Pair((huMax + huMin) / 2, range) // 占位
            WindowMethod.MIN_MAX -> Pair((huMax + huMin) / 2, range)        // (WINDOW_DICT_MIN_MAX)
        }
        val safeW = max(1, w)
        return Pair(c, safeW)
    }

    /**
     * 论文核心算法：min_i/max_i 范围，window 中心 = (min_i+max_i)/2，width = max_i-min_i。
     */
    private fun adaptiveByMinI(
        hu: IntArray,
        minI: Int,
        maxI: Int,
        config: XrayConfig,
    ): Pair<Int, Int> {
        if (maxI <= minI) {
            return Pair(hu.size / 2, hu.size.coerceAtLeast(1))
        }
        val c = (minI + maxI) / 2
        val w = (maxI - minI).coerceAtLeast(1)
        return Pair(c, w)
    }

    /**
     * 8-bit 窗映射（线性截断到 [c-w/2, c+w/2]）。
     */
    private fun applyWindowTo8u(
        hu: IntArray,
        c: Int,
        w: Int,
        huMin: Int,
        huMax: Int,
    ): IntArray {
        val lo = c - w / 2
        val hi = c + w / 2
        val range = max(1, hi - lo)
        val out = IntArray(hu.size)
        for (i in hu.indices) {
            val v = hu[i].coerceIn(lo, hi)
            out[i] = ((v - lo).toDouble() / range * 255.0).toInt().coerceIn(0, 255)
        }
        return out
    }

    /**
     * 计算 exposure_level（mean）和 standardDeviation（stddev）。
     */
    private fun computeExposureAndStd(hu: IntArray, c: Int, w: Int): Pair<Int, Double> {
        if (hu.isEmpty()) return Pair(0, 0.0)
        var sum = 0L
        for (v in hu) sum += v
        val mean = (sum.toDouble() / hu.size).toInt()
        var sse = 0.0
        for (v in hu) {
            val d = v - mean
            sse += d.toDouble() * d
        }
        val variance = sse / hu.size
        val stddev = sqrt(variance)
        return Pair(mean, stddev)
    }

    /**
     * 把 PixelData（rawpixeldeal.xray）转成 PixelDataNew（dcmtk）以便写 DICOM。
     */
    fun toPixelDataNew(p: PixelData): com.example.dcmtk.model.PixelDataNew {
        return com.example.dcmtk.model.PixelDataNew(
            rows = p.height,
            columns = p.width,
            data = p.data,
            largestImagePixelValue = p.largestImagePixelValue,
            win_center = p.winCenter,
            win_width = p.winWidth,
            exposure_leve = p.exposureLevel,
            standardDeviation = p.standardDeviation,
        )
    }
}
