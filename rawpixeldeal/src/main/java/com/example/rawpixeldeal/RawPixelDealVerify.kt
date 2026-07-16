package com.example.rawpixeldeal

import android.util.Log
import com.example.rawpixeldeal.jni.RawPixelDealJni

/**
 * rawpixeldeal 模块对外的最小验证门面。
 *
 * 业务侧只需要调用一次 [verifyChain]：
 *  - 打印 OpenCV 版本（确认 [libopencv_java4.so] 已成功 dlopen）；
 *  - 构造一张 8x8 的灰度"棋盘"原始像素，走 native -> cv::Mat -> GaussianBlur，
 *    把处理结果回填并校验非 0。
 *
 * 整个调用串起来即：
 *   Kotlin ([verifyChain])
 *      -> JNI ([RawPixelDealJni.processRawGrayPixels])
 *      -> C++ ([native_processRawGrayPixels])
 *      -> OpenCV C++ ([cv::GaussianBlur])
 *      -> 回写 ByteArray -> Kotlin
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
}
