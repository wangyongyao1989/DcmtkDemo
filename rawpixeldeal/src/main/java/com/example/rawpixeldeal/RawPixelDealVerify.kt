package com.example.rawpixeldeal

import android.content.Context
import android.graphics.Bitmap
import android.util.Log
import com.example.rawpixeldeal.jni.RawPixelDealJni
import java.io.InputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * rawpixeldeal 模块对外的业务门面：
 *  - [verifyChain]：最小链路验证（Kotlin -> JNI -> OpenCV -> 校验非 0）。
 *  - [processAssetFromAssets]：把 assets 下的"原始像素数据缓冲"
 *    （如 Data610.bin / Data622.bin）经 OpenCV 裁剪/归一化/CLAHE 后
 *    输出 [Bitmap]，用于在 [RawPixelDealFragment] 做人工筛查。
 *  - [processCtSeriesFromAssets]：实现 PRD ct-opencv-raw-buffer-windowing-prd
 *    要求的"CT 序列级处理管线"：raw buffer -> HU 标准化 -> OpenCV 优化
 *    -> 自动裁剪 -> 序列级自适应窗宽窗位 -> 8-bit 显示。
 */
object RawPixelDealVerify {

    private const val TAG = "RawPixelDealVerify"

    data class Report(
        val opencvVersion: String,
        val nativeMessage: String,
        val width: Int,
        val height: Int,
        val p00: Int,
        val p01: Int,
        val p02: Int,
        val p10: Int,
        val pixelSum: Int,
        val rawBeforeHead: IntArray,
        val rawAfterHead: IntArray,
        val changedAnyPixel: Boolean,
    )

    /** processAssetFromAssets 的处理结果，附带诊断信息便于 UI 文字展示 */
    data class ImageResult(
        val bitmap: Bitmap,
        val srcMin: Int,
        val srcMax: Int,
        val outWidth: Int,
        val outHeight: Int,
        val assetName: String,
        val srcWidth: Int,
        val srcHeight: Int,
        val bitDepth: Int,
        val cropLeft: Int,
        val cropTop: Int,
        val cropRight: Int,
        val cropBottom: Int,
        val enableClahe: Boolean,
        val clipLimit: Double,
        val tileSize: Int,
    )

    /**
     * DICOM 像素元数据（与 PRD 中 PixelMeta 对齐）。
     *  - rows/cols                  = Rows / Columns
     *  - bitsAllocated               = BitsAllocated（16 / 8）
     *  - bitsStored                  = BitsStored（如 12）；不在 native 路径使用，保留
     *  - pixelSigned                 = PixelRepresentation（0/1）
     *  - rescaleSlope / rescaleIntercept = RescaleSlope / RescaleIntercept
     *  - photometric                 = 0 = MONOCHROME2，1 = MONOCHROME1
     *  - littleEndian                = true = little-endian（本 native 路径固定 LE）
     */
    data class PixelMeta(
        val rows: Int,
        val cols: Int,
        val bitsAllocated: Int,
        val bitsStored: Int,
        val pixelSigned: Int,
        val rescaleSlope: Double,
        val rescaleIntercept: Double,
        val photometric: Int,
        val littleEndian: Boolean = true,
    )

    /**
     * CT 序列级处理结果（与 PRD 中 WindowResult 对齐 + 扩展 debug）。
     */
    data class SeriesWindowResult(
        /** 窗位 c（HU） */
        val windowCenter: Double,
        /** 窗宽 w（HU） */
        val windowWidth: Double,
        /** 自动裁剪 ROI（原图坐标） */
        val cropLeft: Int,
        val cropTop: Int,
        val cropWidth: Int,
        val cropHeight: Int,
        /** 每片一张 RGBA8888 Bitmap（已按 cropRect 裁剪） */
        val bitmaps: List<Bitmap>,
        /** 调试信息：Gmin/Gmax, H_bins, T0/T1, B, srcMin/srcMax（SV） */
        val debug: SeriesWindowDebug,
        /** 原始 ROI 直方图（已用于调窗） */
        val histogram: IntArray,
    )

    data class SeriesWindowDebug(
        val gMin: Double,
        val gMax: Double,
        val hBins: Double,
        val t0: Double,
        val t1: Double,
        val b: Int,
        val srcMin: Int,
        val srcMax: Int,
        val n0: Double,
        val n1: Double,
        val sliceCount: Int,
    )

    /**
     * 跑一次最小验证，返回 [Report]；任何一步失败均会抛 [IllegalStateException] 便于上层捕获。
     */
    fun verifyChain(): Report {
        // 1) JNI + OpenCV 探针
        val msg = RawPixelDealJni.stringFromJNI()
        Log.i(TAG, "native -> $msg")

        val ver = RawPixelDealJni.getOpenCVVersion()
        Log.i(TAG, "OpenCV -> $ver")

        // 2) 8x8 棋盘：纯白(255) + 纯黑(0) 交错
        val w = 8
        val h = 8
        val src = ByteArray(w * h) { i ->
            val x = i % w
            val y = i / w
            if (((x + y) and 1) == 0) 0.toByte() else 255.toByte()
        }
        val beforeHead = intArrayOf(
            src[0].toInt() and 0xFF,
            src[1].toInt() and 0xFF,
            src[2].toInt() and 0xFF,
            src[w].toInt() and 0xFF,
        )
        Log.i(TAG, "raw before -> ${beforeHead.toList()}")

        // 3) 调 native
        val ret = RawPixelDealJni.processRawGrayPixels(w, h, src)
            ?: throw IllegalStateException("native returned null")

        require(ret.size >= 5) { "native returned array size=${ret.size}" }
        val p00 = ret[0]
        val p01 = ret[1]
        val p02 = ret[2]
        val p10 = ret[3]
        val sum = ret[4]
        val afterHead = intArrayOf(
            src[0].toInt() and 0xFF,
            src[1].toInt() and 0xFF,
            src[2].toInt() and 0xFF,
            src[w].toInt() and 0xFF,
        )
        Log.i(TAG, "native head -> [$p00, $p01, $p02, $p10], sum=$sum")
        Log.i(TAG, "raw after  -> ${afterHead.toList()}")

        // 4) 棋盘经 3x3 高斯后，边缘像素必然被"模糊"为非 0/非 255
        val changed = (0 until src.size).any { (src[it].toInt() and 0xFF) != beforeHead.let { _ ->
            val x = it % w
            val y = it / w
            if (((x + y) and 1) == 0) 0 else 255
        } }
        if (!changed) {
            throw IllegalStateException("OpenCV blur did not change any pixel, chain broken")
        }
        if (sum == 0) {
            throw IllegalStateException("OpenCV blur result sum=0, chain broken")
        }

        return Report(
            opencvVersion = ver,
            nativeMessage = msg,
            width = w,
            height = h,
            p00 = p00,
            p01 = p01,
            p02 = p02,
            p10 = p10,
            pixelSum = sum,
            rawBeforeHead = beforeHead,
            rawAfterHead = afterHead,
            changedAnyPixel = changed,
        )
    }

    /**
     * 从 assets 读取 [assetName]，按 [srcWidth] x [srcHeight] x [bitDepth] 解析为灰度
     * 原始像素，再走 native 做归一化/CLAHE/裁剪，输出可直接显示的 [Bitmap]。
     *
     * 任何失败都会抛 [IllegalStateException]，由调用方在协程中捕获并 Toast 展示。
     */
    fun processAssetFromAssets(
        context: Context,
        assetName: String,
        srcWidth: Int,
        srcHeight: Int,
        bitDepth: Int,
        cropLeft: Int = 0,
        cropTop: Int = 0,
        cropRight: Int = 0,
        cropBottom: Int = 0,
        enableClahe: Boolean = true,
        clipLimit: Double = 2.0,
        tileSize: Int = 8,
    ): ImageResult {
        require(srcWidth > 0 && srcHeight > 0) { "invalid size: ${srcWidth}x$srcHeight" }
        require(bitDepth == 8 || bitDepth == 16) { "unsupported bitDepth=$bitDepth" }

        // 1) 从 assets 读全部字节
        val bytes: ByteArray = context.assets.open(assetName).use { ins: InputStream ->
            ins.readBytes()
        }
        val expectedBytes = srcWidth * srcHeight * (if (bitDepth == 16) 2 else 1)
        require(bytes.size >= expectedBytes) {
            "asset $assetName size=${bytes.size} < expected=$expectedBytes " +
                    "(${srcWidth}x${srcHeight}@${bitDepth}bit)"
        }
        Log.i(TAG, "processAsset: $assetName size=${bytes.size} expected=$expectedBytes")

        // 2) 调 native 处理
        val head = IntArray(4)
        val rgba = RawPixelDealJni.processRawToRgba(
            width = srcWidth,
            height = srcHeight,
            bitDepth = bitDepth,
            src = bytes,
            cropLeft = cropLeft,
            cropTop = cropTop,
            cropRight = cropRight,
            cropBottom = cropBottom,
            enableClahe = if (enableClahe) 1 else 0,
            clipLimit = clipLimit,
            tileSize = tileSize,
            head = head,
        ) ?: throw IllegalStateException("native processRawToRgba returned null")

        val outW = head[2]
        val outH = head[3]
        require(rgba.size >= outW * outH * 4) {
            "rgba size=${rgba.size} < expected=${outW * outH * 4}"
        }

        // 3) 构造 ARGB_8888 Bitmap。COLOR_GRAY2RGBA 内存布局为 R,G,B,A，
        //    与 Android Bitmap ARGB_8888（内存按 R,G,B,A 排列）一致，
        //    因此可以直接 copyPixelsFromBuffer，无需额外通道交换。
        val bmp = Bitmap.createBitmap(outW, outH, Bitmap.Config.ARGB_8888)
        val buf = ByteBuffer.wrap(rgba).order(ByteOrder.nativeOrder())
        bmp.copyPixelsFromBuffer(buf)

        return ImageResult(
            bitmap = bmp,
            srcMin = head[0],
            srcMax = head[1],
            outWidth = outW,
            outHeight = outH,
            assetName = assetName,
            srcWidth = srcWidth,
            srcHeight = srcHeight,
            bitDepth = bitDepth,
            cropLeft = cropLeft,
            cropTop = cropTop,
            cropRight = cropRight,
            cropBottom = cropBottom,
            enableClahe = enableClahe,
            clipLimit = clipLimit,
            tileSize = tileSize,
        )
    }

    // ------------------------------------------------------------------------
    // CT 序列级处理管线（PRD ct-opencv-raw-buffer-windowing-prd）
    // ------------------------------------------------------------------------

    /**
     * CT 序列级处理配置（覆盖 PRD 全部可调参数 + 工程化约束默认值）。
     *
     *  - nBins 论文建议 256
     *  - n0/n1 论文范围 [0.0005, 0.0025]，这里默认 0.0015
     *  - bodyThreshold 默认 -600 HU（"非空气"）
     *  - clipLowHu/clipHighHu 默认 -1200 / 3000（极值温和裁剪）
     */
    data class CtSeriesConfig(
        val bodyThreshold: Float = -600f,
        val cropMargin: Int = 16,
        val bodyMorphSize: Int = 5,
        val minBodyAreaPx: Int = 1000,
        val nBins: Int = 256,
        val n0: Double = 0.0015,
        val n1: Double = 0.0015,
        val histSampleStride: Int = 1,
        val enableBilateral: Boolean = true,
        val bilateralD: Int = 5,
        val bilateralSigmaColor: Double = 50.0,
        val bilateralSigmaSpace: Double = 50.0,
        val clipLowHu: Float = -1200f,
        val clipHighHu: Float = 3000f,
    )

    /**
     * 从 assets 加载 [assetNames]（多张原始像素文件视作一个"序列"），
     * 按 PRD ct-opencv-raw-buffer-windowing-prd 走完整管线，输出
     * [SeriesWindowResult]。
     *
     * @param context   Android Context（用于读 assets）
     * @param assetNames 要处理的文件名列表（如 ["Data610.bin"] 单张，或
     *                   多张同尺寸 raw）。注意所有文件必须同 width/height/bitDepth
     * @param meta      DICOM 像素元数据；可只填 width/height/bitDepth/slope/intercept
     * @param config    调窗/裁剪/优化参数；默认使用 [CtSeriesConfig] 默认值
     */
    fun processCtSeriesFromAssets(
        context: Context,
        assetNames: List<String>,
        meta: PixelMeta,
        config: CtSeriesConfig = CtSeriesConfig(),
    ): SeriesWindowResult {
        require(assetNames.isNotEmpty()) { "assetNames must not be empty" }
        require(meta.cols > 0 && meta.rows > 0) { "invalid meta size" }
        require(meta.bitsAllocated == 16 || meta.bitsAllocated == 8) {
            "unsupported bitsAllocated=${meta.bitsAllocated}"
        }
        require(config.nBins > 0 && config.n0 > 0.0 && config.n1 > 0.0) {
            "invalid windowing config"
        }

        Log.i(TAG, "processCtSeriesFromAssets: nSlices=${assetNames.size} meta=$meta cfg=$config")

        // 1) 从 assets 读所有 slice
        val rawBuffers: Array<ByteArray> = assetNames.map { name ->
            context.assets.open(name).use { it.readBytes() }
        }.toTypedArray()

        val sliceCount = rawBuffers.size
        val expectedBytes =
            meta.cols * meta.rows * (if (meta.bitsAllocated == 16) 2 else 1)
        for ((idx, b) in rawBuffers.withIndex()) {
            require(b.size >= expectedBytes) {
                "slice[$idx] size=${b.size} < expected=$expectedBytes"
            }
        }

        // 2) 准备 out 缓冲
        val outDisplays: Array<ByteArray?> = arrayOfNulls(sliceCount)
        val outWindowStats = DoubleArray(11)
        val outCropAndOut = IntArray(10)
        val outHistogram = IntArray(config.nBins)
        val outUsedFlags = IntArray(1)

        // 3) 调 native
        val ok = RawPixelDealJni.processCtSeries(
            rawBuffers = rawBuffers,
            width = meta.cols,
            height = meta.rows,
            bitsAllocated = meta.bitsAllocated,
            pixelSigned = meta.pixelSigned,
            rescaleSlope = meta.rescaleSlope,
            rescaleIntercept = meta.rescaleIntercept,
            photometric = meta.photometric,
            bodyThreshold = config.bodyThreshold,
            cropMargin = config.cropMargin,
            bodyMorphSize = config.bodyMorphSize,
            minBodyAreaPx = config.minBodyAreaPx,
            nBins = config.nBins,
            n0 = config.n0,
            n1 = config.n1,
            histSampleStride = config.histSampleStride,
            enableBilateral = if (config.enableBilateral) 1 else 0,
            bilateralD = config.bilateralD,
            bilateralSigmaColor = config.bilateralSigmaColor,
            bilateralSigmaSpace = config.bilateralSigmaSpace,
            clipLowHu = config.clipLowHu,
            clipHighHu = config.clipHighHu,
            outDisplays = outDisplays,
            outWindowStats = outWindowStats,
            outCropAndOut = outCropAndOut,
            outHistogram = outHistogram,
            outUsedFlags = outUsedFlags,
        )
        if (!ok) {
            throw IllegalStateException("native processCtSeries returned false")
        }
        Log.i(TAG, "processCtSeriesFromAssets: c=${outWindowStats[0]} w=${outWindowStats[1]} " +
                "Gmin=${outWindowStats[2]} Gmax=${outWindowStats[3]} B=${outWindowStats[7].toInt()}")

        // 4) 把每片 RGBA bytes 包成 Bitmap
        val outW = outCropAndOut[4]
        val outH = outCropAndOut[5]
        val bitmaps: List<Bitmap> = outDisplays.map { rgba ->
            requireNotNull(rgba) { "native returned null for a slice" }
            require(rgba.size >= outW * outH * 4) {
                "rgba size=${rgba.size} < expected=${outW * outH * 4}"
            }
            val bmp = Bitmap.createBitmap(outW, outH, Bitmap.Config.ARGB_8888)
            val buf = ByteBuffer.wrap(rgba).order(ByteOrder.nativeOrder())
            bmp.copyPixelsFromBuffer(buf)
            bmp
        }

        val debug = SeriesWindowDebug(
            gMin = outWindowStats[2],
            gMax = outWindowStats[3],
            hBins = outWindowStats[4],
            t0 = outWindowStats[5],
            t1 = outWindowStats[6],
            b = outWindowStats[7].toInt(),
            srcMin = outWindowStats[8].toInt(),
            srcMax = outWindowStats[9].toInt(),
            n0 = outWindowStats[10] / 1e6,
            n1 = config.n1,
            sliceCount = sliceCount,
        )

        return SeriesWindowResult(
            windowCenter = outWindowStats[0],
            windowWidth = outWindowStats[1],
            cropLeft = outCropAndOut[0],
            cropTop = outCropAndOut[1],
            cropWidth = outCropAndOut[2],
            cropHeight = outCropAndOut[3],
            bitmaps = bitmaps,
            debug = debug,
            histogram = outHistogram,
        )
    }
}
