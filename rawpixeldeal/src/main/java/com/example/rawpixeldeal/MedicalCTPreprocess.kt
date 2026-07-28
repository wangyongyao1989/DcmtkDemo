package com.example.rawpixeldeal

import android.content.Context
import android.graphics.Bitmap
import android.util.Log
import com.example.rawpixeldeal.jni.RawPixelDealJni
import java.io.InputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * 医学图像预处理门面 (Requirement 2 & 4)
 */
object MedicalCTPreprocess {
    private const val TAG = "MedicalCTPreprocess"

    enum class Op(val id: Int, val displayName: String) {
        GAUSSIAN(1, "高斯滤波"),
        MEDIAN(2, "中值滤波"),
        BILATERAL(3, "双边滤波"),
        FFT(4, "频域FFT"),
        RESAMPLE_SIZE(5, "重采样(尺寸)"),
        RESAMPLE_SCALE(6, "重采样(比例)"),
        GLOBAL_EQUALIZE(7, "全局均衡化"),
        CLAHE(8, "CLAHE增强"),
        CONTRAST_STRETCH(9, "对比度拉伸"),
        HU_CONVERT(10, "HU校正"),
        TAILOR(11, "图片裁剪"),
        INVERT_LUT(12, "Invert LUTs"),
        FEATURE_SHARPEN(13, "特征锐化"),
        LOG_TRANSFORM(14, "Log变换")
    }

    data class PreprocessStep(
        val op: Op,
        val params: List<Double> = emptyList()
    )

    data class PreprocessResult(
        val bitmap: Bitmap,
        val outWidth: Int,
        val outHeight: Int,
        val minVal: Int,
        val maxVal: Int,
        /** P1-6: 浮点 HU 范围（保留小数）。`null` 表示未由 native 填充。 */
        val minValD: Double = minVal.toDouble(),
        val maxValD: Double = maxVal.toDouble()
    )

    /**
     * 执行预处理流水线
     *
     * @param windowMethod  调窗方法索引 (-1 表示 None，0-5 对应不同算法)
     * @param isUint16      raw buffer 是否为 16-bit 无符号。CT/HU 场景下探测器输出为
     *                      无符号 packed-in-16-bit，**必须传 true**；否则像素会被
     *                      解释为 CV_16SC1，高位全部变负，HU 校正后全黑。
     */
    fun process(
        context: Context,
        assetName: String,
        width: Int,
        height: Int,
        bitDepth: Int,
        bigEndian: Boolean = true,
        isUint16: Boolean = true,
        steps: List<PreprocessStep>,
        windowMethod: Int = -1
    ): PreprocessResult {
        val bytes = context.assets.open(assetName).use { it.readBytes() }
        return process(
            rawBuffer = bytes,
            width = width,
            height = height,
            bitDepth = bitDepth,
            bigEndian = bigEndian,
            isUint16 = isUint16,
            steps = steps,
            windowMethod = windowMethod
        )
    }

    /**
     * P1-9: 直接接收 [InputStream] 的 [process] 重载。
     *
     * 适用场景：
     *  - 文件不在 assets 中（如 [android.content.Context.getExternalFilesDir] 下的临时文件）
     *  - 网络下载 / 解压后的流（PACS 拉取的 DICOM PixelData）
     *  - 测试时用 [ClassLoader.getResourceAsStream] 加载非 assets 资源
     *
     * 调用方负责 [InputStream] 的生命周期；本方法内部会 read + close 它。
     */
    fun process(
        input: InputStream,
        width: Int,
        height: Int,
        bitDepth: Int,
        bigEndian: Boolean = true,
        isUint16: Boolean = true,
        steps: List<PreprocessStep>,
        windowMethod: Int = -1
    ): PreprocessResult {
        val bytes = input.use { it.readBytes() }
        return process(
            rawBuffer = bytes,
            width = width,
            height = height,
            bitDepth = bitDepth,
            bigEndian = bigEndian,
            isUint16 = isUint16,
            steps = steps,
            windowMethod = windowMethod
        )
    }

    /**
     * [process] 的核心实现：接收原始字节数组，避免重复打开流的样板代码。
     */
    fun process(
        rawBuffer: ByteArray,
        width: Int,
        height: Int,
        bitDepth: Int,
        bigEndian: Boolean = true,
        isUint16: Boolean = true,
        steps: List<PreprocessStep>,
        windowMethod: Int = -1
    ): PreprocessResult {
        // 1. 转换 Steps 到 JNI 格式
        val opIds = steps.map { it.op.id }.toIntArray()
        val paramList = mutableListOf<Double>()
        steps.forEach { step ->
            paramList.addAll(step.params)
        }
        val params = paramList.toDoubleArray()

        // 3. 调 JNI
        val outInfo = IntArray(4)
        // P1-6: 浮点 outHuRange —— 保留 HU 小数精度
        val outHuRange = DoubleArray(2)
        val rgba = RawPixelDealJni.processMedicalCT(
            rawBuffer = rawBuffer,
            width = width,
            height = height,
            bitDepth = bitDepth,
            bigEndian = bigEndian,
            isUint16 = isUint16,
            ops = opIds,
            params = params,
            windowMethod = windowMethod,
            outInfo = outInfo,
            outHuRange = outHuRange
        ) ?: throw IllegalStateException("native processMedicalCT returned null")

        val outW = outInfo[0]
        val outH = outInfo[1]

        // 4. 转 Bitmap
        val bmp = Bitmap.createBitmap(outW, outH, Bitmap.Config.ARGB_8888)
        val buf = ByteBuffer.wrap(rgba).order(ByteOrder.nativeOrder())
        bmp.copyPixelsFromBuffer(buf)

        return PreprocessResult(
            bitmap = bmp,
            outWidth = outW,
            outHeight = outH,
            minVal = outInfo[2],
            maxVal = outInfo[3],
            minValD = outHuRange[0],
            maxValD = outHuRange[1]
        )
    }

    /**
     * 执行完整预处理流水线（标准流程）
     *
     * @param isUint16  raw buffer 是否为 16-bit 无符号。CT/HU 场景下默认 true。
     */
    fun processFullPipeline(
        context: Context,
        assetName: String,
        width: Int,
        height: Int,
        tarW: Int,
        tarH: Int,
        slope: Float,
        intercept: Float,
        bigEndian: Boolean = true,
        isUint16: Boolean = true
    ): PreprocessResult {
        val bytes = context.assets.open(assetName).use { it.readBytes() }
        return processFullPipeline(
            rawBuffer = bytes,
            width = width, height = height,
            tarW = tarW, tarH = tarH,
            slope = slope, intercept = intercept,
            bigEndian = bigEndian, isUint16 = isUint16
        )
    }

    /**
     * P1-9: [processFullPipeline] 的 [InputStream] 重载。
     */
    fun processFullPipeline(
        input: InputStream,
        width: Int,
        height: Int,
        tarW: Int,
        tarH: Int,
        slope: Float,
        intercept: Float,
        bigEndian: Boolean = true,
        isUint16: Boolean = true
    ): PreprocessResult {
        val bytes = input.use { it.readBytes() }
        return processFullPipeline(
            rawBuffer = bytes,
            width = width, height = height,
            tarW = tarW, tarH = tarH,
            slope = slope, intercept = intercept,
            bigEndian = bigEndian, isUint16 = isUint16
        )
    }

    /**
     * [processFullPipeline] 的核心实现：直接接收字节数组。
     */
    fun processFullPipeline(
        rawBuffer: ByteArray,
        width: Int,
        height: Int,
        tarW: Int,
        tarH: Int,
        slope: Float,
        intercept: Float,
        bigEndian: Boolean = true,
        isUint16: Boolean = true
    ): PreprocessResult {
        val outInfo = IntArray(4)
        // P1-6: 浮点 HU 范围
        val outHuRange = DoubleArray(2)
        val rgba = RawPixelDealJni.processCTFullPipeline(
            rawBuffer = rawBuffer,
            width = width,
            height = height,
            tarW = tarW,
            tarH = tarH,
            slope = slope,
            intercept = intercept,
            bigEndian = bigEndian,
            isUint16 = isUint16,
            outInfo = outInfo,
            outHuRange = outHuRange
        ) ?: throw IllegalStateException("native processCTFullPipeline returned null")

        val bmp = Bitmap.createBitmap(outInfo[0], outInfo[1], Bitmap.Config.ARGB_8888)
        val buf = ByteBuffer.wrap(rgba).order(ByteOrder.nativeOrder())
        bmp.copyPixelsFromBuffer(buf)

        return PreprocessResult(
            bitmap = bmp,
            outWidth = outInfo[0],
            outHeight = outInfo[1],
            minVal = outInfo[2],
            maxVal = outInfo[3],
            minValD = outHuRange[0],
            maxValD = outHuRange[1]
        )
    }

    /**
     * 调窗对比：重负载（裁剪/HU/去噪/重采样/增强）只跑一次，
     * 8-bit 映射按 [windowMethods] 列表逐个执行。
     *
     * 用于"调窗前后对比"场景，避免对同一份预处理数据重复执行双边滤波等 O(N) 操作。
     *
     * @param windowMethods 调窗方法列表。-1 表示 min-max 归一化；0..5 对应各算法。
     *                      第一个元素通常为 -1（"调窗前"），后续为对比方法。
     * @return 与 windowMethods 一一对应的 Bitmap 列表
     */
    fun processCompareWindows(
        context: Context,
        assetName: String,
        width: Int,
        height: Int,
        bitDepth: Int,
        bigEndian: Boolean = true,
        isUint16: Boolean = true,
        steps: List<PreprocessStep>,
        windowMethods: List<Int>
    ): List<PreprocessResult> {
        val bytes = context.assets.open(assetName).use { it.readBytes() }
        return processCompareWindows(
            rawBuffer = bytes,
            width = width, height = height, bitDepth = bitDepth,
            bigEndian = bigEndian, isUint16 = isUint16,
            steps = steps, windowMethods = windowMethods
        )
    }

    /**
     * P1-9: [processCompareWindows] 的 [InputStream] 重载。
     */
    fun processCompareWindows(
        input: InputStream,
        width: Int,
        height: Int,
        bitDepth: Int,
        bigEndian: Boolean = true,
        isUint16: Boolean = true,
        steps: List<PreprocessStep>,
        windowMethods: List<Int>
    ): List<PreprocessResult> {
        val bytes = input.use { it.readBytes() }
        return processCompareWindows(
            rawBuffer = bytes,
            width = width, height = height, bitDepth = bitDepth,
            bigEndian = bigEndian, isUint16 = isUint16,
            steps = steps, windowMethods = windowMethods
        )
    }

    /**
     * [processCompareWindows] 的核心实现：直接接收字节数组。
     */
    fun processCompareWindows(
        rawBuffer: ByteArray,
        width: Int,
        height: Int,
        bitDepth: Int,
        bigEndian: Boolean = true,
        isUint16: Boolean = true,
        steps: List<PreprocessStep>,
        windowMethods: List<Int>
    ): List<PreprocessResult> {
        require(windowMethods.isNotEmpty()) { "windowMethods must not be empty" }

        val opIds = steps.map { it.op.id }.toIntArray()
        val paramList = mutableListOf<Double>()
        steps.forEach { step -> paramList.addAll(step.params) }
        val params = paramList.toDoubleArray()
        val methodIds = windowMethods.toIntArray()

        val outDisplays: Array<ByteArray?> = arrayOfNulls(methodIds.size)
        val outInfo = IntArray(2)
        // P1-6: 浮点 HU 范围
        val outHuRange = DoubleArray(2)
        RawPixelDealJni.processMedicalCTCompareWindows(
            rawBuffer = rawBuffer,
            width = width,
            height = height,
            bitDepth = bitDepth,
            bigEndian = bigEndian,
            isUint16 = isUint16,
            ops = opIds,
            params = params,
            windowMethods = methodIds,
            outDisplays = outDisplays,
            outInfo = outInfo,
            outHuRange = outHuRange
        )

        val outW = outInfo[0]
        val outH = outInfo[1]
        // 共享同一组 outW/outH / huRange；所有调窗图共享同一份预处理数据
        val sharedMinD = outHuRange[0]
        val sharedMaxD = outHuRange[1]
        val sharedMinI = if (sharedMinD.isFinite()) sharedMinD.toInt() else 0
        val sharedMaxI = if (sharedMaxD.isFinite()) sharedMaxD.toInt() else 0
        return outDisplays.mapIndexed { i, rgba ->
            if (rgba == null) {
                throw IllegalStateException("native processMedicalCTCompareWindows returned null at idx=$i")
            }
            val bmp = Bitmap.createBitmap(outW, outH, Bitmap.Config.ARGB_8888)
            val buf = ByteBuffer.wrap(rgba).order(ByteOrder.nativeOrder())
            bmp.copyPixelsFromBuffer(buf)
            PreprocessResult(
                bitmap = bmp,
                outWidth = outW,
                outHeight = outH,
                minVal = sharedMinI,
                maxVal = sharedMaxI,
                minValD = sharedMinD,
                maxValD = sharedMaxD
            )
        }
    }

    /**
     * 动态调窗：用户直接指定窗位 C 和窗宽 W。
     *
     * 核心原理（参考 CSDN 博客 u013598963/121023205）：
     *  - 逐像素线性映射：(C-W/2)→0, (C+W/2)→255
     *  - 超出范围的值用 saturate_cast 截断
     *
     * 与 [process] 的区别：用显式 (C, W) 替代 windowMethod 算法选择，
     * 适用于用户通过 SeekBar 实时拖动调节窗宽窗位的交互场景。
     *
     * @param windowCenter  窗位 C（HU 域）
     * @param windowWidth   窗宽 W（HU 域）
     * @return 处理后的 [PreprocessResult]
     */
    fun processWithCustomWindow(
        rawBuffer: ByteArray,
        width: Int,
        height: Int,
        bitDepth: Int,
        bigEndian: Boolean = true,
        isUint16: Boolean = true,
        steps: List<PreprocessStep>,
        windowCenter: Double,
        windowWidth: Double
    ): PreprocessResult {
        val opIds = steps.map { it.op.id }.toIntArray()
        val paramList = mutableListOf<Double>()
        steps.forEach { step -> paramList.addAll(step.params) }
        val params = paramList.toDoubleArray()

        val outInfo = IntArray(4)
        val outHuRange = DoubleArray(2)
        val rgba = RawPixelDealJni.processMedicalCTCustomWindow(
            rawBuffer = rawBuffer,
            width = width,
            height = height,
            bitDepth = bitDepth,
            bigEndian = bigEndian,
            isUint16 = isUint16,
            ops = opIds,
            params = params,
            windowCenter = windowCenter,
            windowWidth = windowWidth,
            outInfo = outInfo,
            outHuRange = outHuRange
        ) ?: throw IllegalStateException("native processMedicalCTCustomWindow returned null")

        val outW = outInfo[0]
        val outH = outInfo[1]
        val bmp = Bitmap.createBitmap(outW, outH, Bitmap.Config.ARGB_8888)
        val buf = ByteBuffer.wrap(rgba).order(ByteOrder.nativeOrder())
        bmp.copyPixelsFromBuffer(buf)

        return PreprocessResult(
            bitmap = bmp,
            outWidth = outW,
            outHeight = outH,
            minVal = outInfo[2],
            maxVal = outInfo[3],
            minValD = outHuRange[0],
            maxValD = outHuRange[1]
        )
    }

    /**
     * 获取处理后的 16-bit 原始像素数据（大端序字节流），主要用于写 DCM 文件。
     * 包含裁剪、HU 转换等 dispatchOps 中的所有操作。
     */
    fun getProcessedRawPixels(
        rawBuffer: ByteArray,
        width: Int, height: Int, bitDepth: Int,
        bigEndian: Boolean, isUint16: Boolean,
        steps: List<PreprocessStep>
    ): Pair<ByteArray, IntArray> {
        val opIds = steps.map { it.op.id }.toIntArray()
        val params = steps.flatMap { it.params }.toDoubleArray()
        val outMeta = IntArray(4)
        val data = RawPixelDealJni.getProcessedRawPixels(
            rawBuffer, width, height, bitDepth, bigEndian, isUint16,
            opIds, params, outMeta
        ) ?: throw Exception("Failed to get processed raw pixels")
        return Pair(data, intArrayOf(outMeta[0], outMeta[1]))
    }

    /**
     * Requirement: Export statistics files (pixel_array.txt, histogram.txt, smoothed_data.txt)
     * based on the raw buffer.
     *
     * @return Status message or error description.
     */
    fun exportStatistics(
        context: Context,
        rawBuffer: ByteArray,
        width: Int,
        height: Int,
        bitDepth: Int,
        bigEndian: Boolean
    ): String {
        val pixels = try {
            if (bitDepth == 16) {
                val buf = ByteBuffer.wrap(rawBuffer)
                    .order(if (bigEndian) ByteOrder.BIG_ENDIAN else ByteOrder.LITTLE_ENDIAN)
                val shortBuf = buf.asShortBuffer()
                IntArray(shortBuf.remaining()) { shortBuf.get().toInt() and 0xFFFF }
            } else {
                IntArray(rawBuffer.size) { rawBuffer[it].toInt() and 0xFF }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to parse raw buffer", e)
            return "Parse failed: ${e.message}"
        }

        // 改进：同时写入内部存储和外部私有存储，确保无论用户通过何种方式（Device Explorer 或 USB）都能查找到
        val internalDir = context.filesDir
        val externalDir = context.getExternalFilesDir(null)

        val dirs = mutableListOf<java.io.File>()
        dirs.add(internalDir)
        externalDir?.let { dirs.add(it) }

        try {
            for (dir in dirs) {
                if (!dir.exists()) dir.mkdirs()
                val pixelArrayFile = java.io.File(dir, "pixel_array.txt")
                val histogramFile = java.io.File(dir, "histogram.txt")
                val smoothedFile = java.io.File(dir, "smoothed_data.txt")

                // 1. pixel_array.txt
                pixelArrayFile.printWriter().use { out ->
                    for (y in 0 until height) {
                        val start = y * width
                        val end = minOf(start + width, pixels.size)
                        for (i in start until end) {
                            out.print(pixels[i])
                            if (i < end - 1) out.print(" ")
                        }
                        out.println()
                    }
                }

                // 2. histogram.txt
                val maxVal = pixels.maxOrNull() ?: 0
                val histogram = IntArray(maxVal + 1)
                for (p in pixels) histogram[p]++
                histogramFile.printWriter().use { out ->
                    out.println("Value: Count")
                    for (i in histogram.indices) {
                        if (histogram[i] > 0) out.println("$i: ${histogram[i]}")
                    }
                }

                // 3. smoothed_data.txt
                smoothedFile.printWriter().use { out ->
                    val windowSize = 5
                    val halfWindow = windowSize / 2
                    for (y in 0 until height) {
                        val start = y * width
                        val end = minOf(start + width, pixels.size)
                        for (i in start until end) {
                            var sum = 0.0
                            var count = 0
                            for (j in (i - halfWindow)..(i + halfWindow)) {
                                if (j >= start && j < end) {
                                    sum += pixels[j]
                                    count++
                                }
                            }
                            out.print("%.2f".format(sum / count))
                            if (i < end - 1) out.print(" ")
                        }
                        out.println()
                    }
                }
            }
            return "Statistics exported successfully.\nInternal: ${internalDir.absolutePath}\nExternal: ${externalDir?.absolutePath ?: "N/A"}"
        } catch (e: Exception) {
            Log.e(TAG, "Failed to write statistics files", e)
            return "Export failed: ${e.message}"
        }
    }
}
