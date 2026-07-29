package com.example.rawpixeldeal

import android.graphics.Bitmap
import com.example.rawpixeldeal.jni.RawPixelDealJni
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * 医学图像预处理门面 - Simplified Version
 * Only keeps operations and logic used by Optimal Adjustment.
 */
object MedicalCTPreprocess {
    private const val TAG = "MedicalCTPreprocess"

    enum class Op(val id: Int, val displayName: String) {
        BILATERAL(3, "双边滤波"),
        CLAHE(8, "CLAHE增强"),
        HU_CONVERT(10, "HU校正"),
        TAILOR(11, "图片裁剪"),
        INVERT_LUT(12, "Invert LUTs"),
        FEATURE_SHARPEN(13, "特征锐化")
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
        val minValD: Double = minVal.toDouble(),
        val maxValD: Double = maxVal.toDouble()
    )

    /**
     * 调窗对比：用于 "最优调节" 流水线。
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
        val params = steps.flatMap { it.params }.toDoubleArray()
        val methodIds = windowMethods.toIntArray()

        val outDisplays: Array<ByteArray?> = arrayOfNulls(methodIds.size)
        val outInfo = IntArray(2)
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
        val sharedMinD = outHuRange[0]
        val sharedMaxD = outHuRange[1]
        
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
                minVal = if (sharedMinD.isFinite()) sharedMinD.toInt() else 0,
                maxVal = if (sharedMaxD.isFinite()) sharedMaxD.toInt() else 0,
                minValD = sharedMinD,
                maxValD = sharedMaxD
            )
        }
    }
}
