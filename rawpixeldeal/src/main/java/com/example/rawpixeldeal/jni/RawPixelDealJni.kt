package com.example.rawpixeldeal.jni

import android.util.Log

/**
 * rawpixeldeal 模块的 JNI 入口 - Simplified Version
 */
object RawPixelDealJni {

    private const val TAG = "RawPixelDealJni"

    init {
        try {
            System.loadLibrary("opencv_java4")
            System.loadLibrary("rawpixeldeal_native")
        } catch (t: Throwable) {
            Log.e(TAG, "Failed to load libraries", t)
            throw t
        }
    }

    /**
     * 调窗对比：重负载（裁剪/HU/去噪/重采样/增强）只跑一次，
     * 8-bit 映射按 windowMethods 列表逐个执行。
     */
    @JvmStatic
    external fun processMedicalCTCompareWindows(
        rawBuffer: ByteArray,
        width: Int,
        height: Int,
        bitDepth: Int,
        bigEndian: Boolean,
        isUint16: Boolean,
        ops: IntArray,
        params: DoubleArray,
        windowMethods: IntArray,
        outDisplays: Array<ByteArray?>,
        outInfo: IntArray,
        outHuRange: DoubleArray?
    )

    /**
     * 获取经过算子链处理（如裁剪）后的 16-bit 原始像素数据。
     * 用于写入 DICOM 文件。
     */
    @JvmStatic
    external fun getProcessedRawPixels(
        rawBuffer: ByteArray,
        width: Int,
        height: Int,
        bitDepth: Int,
        bigEndian: Boolean,
        isUint16: Boolean,
        ops: IntArray,
        params: DoubleArray,
        windowMethod: Int,
        outInfo: IntArray
    ): ByteArray?
}
