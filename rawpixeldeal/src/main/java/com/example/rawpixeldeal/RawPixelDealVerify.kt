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
}
