package com.example.rawpixeldeal

import android.graphics.Bitmap
import com.example.rawpixeldeal.MedicalCTPreprocess.Op
import com.example.rawpixeldeal.jni.RawPixelDealJni
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * 医学图像预处理门面 - Optimized Version
 * 
 * 职责：
 * 1. 提供 Kotlin 友好接口，封装复杂的 JNI 调用。
 * 2. 定义预处理算子 (Op) 和流水线步骤 (PreprocessStep)。
 * 3. 统一管理处理结果的包装 (PreprocessResult/ProcessedRawResult)。
 */
object MedicalCTPreprocess {
    private const val TAG = "MedicalCTPreprocess"

    /**
     * 内部缓存最后一次执行成功的预处理步骤。
     * 供 getProcessedRawPixels 导出时使用，实现参数闭环。
     */
    @Volatile
    private var lastAppliedSteps: List<PreprocessStep> = emptyList()

    fun getLastAppliedSteps(): List<PreprocessStep> = lastAppliedSteps

    /**
     * 预处理算子枚举
     * 每个算子对应 Native 层 dispatchOps 中的一个逻辑分支。
     */
    enum class Op(val id: Int, val displayName: String) {
        BILATERAL(3, "双边滤波"),
        CLAHE(8, "CLAHE增强"),
        HU_CONVERT(10, "HU校正"),
        TAILOR(11, "图片裁剪"),
        INVERT_LUT(12, "Invert LUTs"),
        FEATURE_SHARPEN(13, "特征锐化")
    }

    /**
     * 预处理步骤：包含算子类型及对应的控制参数。
     */
    data class PreprocessStep(
        val op: Op,
        val params: List<Double> = emptyList()
    )

    /**
     * 针对给定的算子枚举，生成携带默认参数的步骤对象。
     */
    fun generateStepWithDefaultParams(op: Op): PreprocessStep {
        return when (op) {
            Op.HU_CONVERT -> PreprocessStep(op, listOf(1.0, -1024.0))
            Op.BILATERAL -> PreprocessStep(op, listOf(5.0, 75.0, 75.0))
            Op.CLAHE -> PreprocessStep(op, listOf(3.0, 8.0, 8.0))
            Op.TAILOR -> PreprocessStep(op, listOf(40000.0, 1.0, 25.0, 10.0))
            Op.FEATURE_SHARPEN -> PreprocessStep(op, listOf(1.5, 6.0))
            Op.INVERT_LUT -> PreprocessStep(op)
        }
    }

    /**
     * 强制排序与验证：解耦 APP 层的逻辑，确保流水线顺序符合物理/算法逻辑。
     * 规则示例：HU 校正应在反色之前。
     */
    fun validateAndSortSteps(steps: MutableList<PreprocessStep>) {
        val huIdx = steps.indexOfFirst { it.op == Op.HU_CONVERT }
        val invertIdx = steps.indexOfFirst { it.op == Op.INVERT_LUT }

        if (huIdx >= 0 && invertIdx >= 0 && huIdx > invertIdx) {
            val huStep = steps.removeAt(huIdx)
            val newInvertIdx = steps.indexOfFirst { it.op == Op.INVERT_LUT }
            steps.add(newInvertIdx, huStep)
        }
    }

    /**
     * 默认预处理流程接口：
     * 仅传入算子列表，内部使用默认参数并执行对比调窗。
     */
    fun processWithDefaultOptions(
        rawBuffer: ByteArray,
        width: Int,
        height: Int,
        bitDepth: Int,
        bigEndian: Boolean = true,
        isUint16: Boolean = true,
        ops: List<Op>,
        windowMethods: List<Int> = listOf(-1, 6)
    ): List<PreprocessResult> {
        val steps = ops.map { generateStepWithDefaultParams(it) }.toMutableList()
        validateAndSortSteps(steps)
        return processCompareWindows(rawBuffer, width, height, bitDepth, bigEndian, isUint16, steps, windowMethods)
    }

    /**
     * 自动执行全套最优预处理流水线
     * 包含：HU校正、智能裁剪、双边滤波、CLAHE增强、特征锐化。
     * 内部使用默认参数，并生成对比调窗结果（线性 vs Peak Area Auto）。
     */
    fun runOptimalPipeline(
        rawBuffer: ByteArray,
        width: Int,
        height: Int,
        bitDepth: Int,
        bigEndian: Boolean = true,
        isUint16: Boolean = true
    ): List<PreprocessResult> {
        val ops = listOf(
            Op.HU_CONVERT,
            Op.TAILOR,
            Op.BILATERAL,
            Op.CLAHE,
            Op.FEATURE_SHARPEN,
            Op.INVERT_LUT
        )
        // 使用默认调窗方法 [-1, 6]
        return processWithDefaultOptions(
            rawBuffer, width, height, bitDepth, bigEndian, isUint16, ops
        )
    }

    /**
     * 可视化处理结果：包含缩放后的 Bitmap 以及原始 HU 范围。
     */
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
     * 原始像素处理结果：包含处理后的 16-bit 字节数组及自动计算的调窗参数。
     * 主要用于写入 DICOM 文件。
     */
    data class ProcessedRawResult(
        val data: ByteArray,
        val width: Int,
        val height: Int,
        val maxVal: Int,
        val windowCenter: Double,
        val windowWidth: Double
    )

    /**
     * 调窗对比接口：
     * 用于在同一份预处理数据上，快速对比不同调窗算法的效果。
     * 重负载操作（如双边滤波、裁剪）仅执行一次，提高响应速度。
     * 
     * @param windowMethods 需要对比的方法列表，-1 代表线性映射，6 为 Peak Area Auto。
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

        // 成功执行后，缓存步骤
        lastAppliedSteps = steps.toList()

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

    /**
     * 导出专用接口：
     * 获取经过算子链处理（如裁剪、校正）后的 16-bit 原始像素（大端序）。
     * 返回结果中包含自动计算出的最佳窗位窗宽。
     */
    fun getProcessedRawPixels(
        rawBuffer: ByteArray,
        width: Int,
        height: Int,
        bitDepth: Int,
        bigEndian: Boolean,
        isUint16: Boolean,
        windowMethod: Int = 6
    ): ProcessedRawResult {
        val steps = lastAppliedSteps
        val opIds = steps.map { it.op.id }.toIntArray()
        val params = steps.flatMap { it.params }.toDoubleArray()
        val outInfo = IntArray(5)

        val processedData = RawPixelDealJni.getProcessedRawPixels(
            rawBuffer, width, height, bitDepth, bigEndian, isUint16,
            opIds, params, windowMethod, outInfo
        ) ?: throw IllegalStateException("native getProcessedRawPixels returned null")

        return ProcessedRawResult(
            data = processedData,
            width = outInfo[0],
            height = outInfo[1],
            maxVal = outInfo[2],
            // 内部回传时为了保留精度乘了10，此处还原
            windowCenter = outInfo[3] / 10.0,
            windowWidth = outInfo[4] / 10.0
        )
    }
}
