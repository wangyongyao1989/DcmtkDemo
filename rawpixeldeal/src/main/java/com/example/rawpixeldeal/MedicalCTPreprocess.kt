package com.example.rawpixeldeal

import android.content.Context
import android.graphics.Bitmap
import android.util.Log
import com.example.rawpixeldeal.jni.RawPixelDealJni
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
        HU_CONVERT(10, "HU校正")
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
        val maxVal: Int
    )

    /**
     * 执行预处理流水线
     */
    fun process(
        context: Context,
        assetName: String,
        width: Int,
        height: Int,
        bitDepth: Int,
        bigEndian: Boolean = true,
        isUint16: Boolean = false,
        steps: List<PreprocessStep>
    ): PreprocessResult {
        // 1. 读取 Asset
        val bytes = context.assets.open(assetName).use { it.readBytes() }
        
        // 2. 转换 Steps 到 JNI 格式
        val opIds = steps.map { it.op.id }.toIntArray()
        val paramList = mutableListOf<Double>()
        steps.forEach { step ->
            paramList.addAll(step.params)
        }
        val params = paramList.toDoubleArray()
        
        // 3. 调 JNI
        val outInfo = IntArray(4)
        val rgba = RawPixelDealJni.processMedicalCT(
            rawBuffer = bytes,
            width = width,
            height = height,
            bitDepth = bitDepth,
            bigEndian = bigEndian,
            isUint16 = isUint16,
            ops = opIds,
            params = params,
            outInfo = outInfo
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
            maxVal = outInfo[3]
        )
    }

    /**
     * 执行完整预处理流水线（标准流程）
     */
    fun processFullPipeline(
        context: Context,
        assetName: String,
        width: Int,
        height: Int,
        tarW: Int,
        tarH: Int,
        slope: Float,
        intercept: Float
    ): PreprocessResult {
        val bytes = context.assets.open(assetName).use { it.readBytes() }
        val outInfo = IntArray(4)
        val rgba = RawPixelDealJni.processCTFullPipeline(
            rawBuffer = bytes,
            width = width,
            height = height,
            tarW = tarW,
            tarH = tarH,
            slope = slope,
            intercept = intercept,
            outInfo = outInfo
        ) ?: throw IllegalStateException("native processCTFullPipeline returned null")

        val bmp = Bitmap.createBitmap(outInfo[0], outInfo[1], Bitmap.Config.ARGB_8888)
        val buf = ByteBuffer.wrap(rgba).order(ByteOrder.nativeOrder())
        bmp.copyPixelsFromBuffer(buf)

        return PreprocessResult(
            bitmap = bmp,
            outWidth = outInfo[0],
            outHeight = outInfo[1],
            minVal = outInfo[2],
            maxVal = outInfo[3]
        )
    }
}
