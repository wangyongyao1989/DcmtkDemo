package com.example.rawpixeldeal.xray

import android.graphics.Bitmap

/**
 * X-ray 管线处理结果。
 *
 * 字段说明：
 *  - [bitmap]        8-bit ARGB_8888 显示位图（已应用窗位窗宽）
 *  - [windowedGray8] 8-bit 灰度数据（用于直接写 DICOM 单色帧）
 *  - [cropRect]      裁剪矩形 [L, T, W, H]
 *  - [pixelData]     8 参像素数据对象（用于写 DICOM 文件）
 *  - [debug]         调试统计
 */
data class XrayResult(
    val bitmap: Bitmap,
    val windowedGray8: IntArray,
    val cropRect: IntArray,           // [L, T, W, H]
    val pixelData: PixelData,
    val debug: XrayDebug,
)

/**
 * X-ray 管线调试信息（用于 UI 显示 / logcat）。
 */
data class XrayDebug(
    /** 整图最大像素值 */
    val largestPixelValue: Int,
    /** 整图最小像素值 */
    val minPixelValue: Int,
    /** 调窗后的窗位 */
    val winCenter: Int,
    /** 调窗后的窗宽 */
    val winWidth: Int,
    /** 全图平均像素（exposure） */
    val exposureLevel: Int,
    /** 全图标准差（detector uniformity） */
    val standardDeviation: Double,
    /** 直方图总像素数 */
    val histogramTotal: Long,
    /** 5% 分位像素值（min_i） */
    val minI: Int,
    /** 95% 分位像素值（max_i） */
    val maxI: Int,
    /** 实际使用的调窗方法 */
    val usedMethod: String,
    /** 实际 CDF 截断比例（min 端） */
    val cdfMinFraction: Double,
    /** 实际 CDF 截断比例（max 端） */
    val cdfMaxFraction: Double,
    /** 旋转校正角度（0 表示未旋转） */
    val rotateAngle: Double,
    /** 裁剪后图像尺寸 [W, H] */
    val croppedSize: IntArray,
    /** 16-bit 直方图前 16 bin（调试用） */
    val histogramFirst16: IntArray,
    /** 输出 8-bit 直方图前 16 bin（调试用） */
    val display8uFirst16: IntArray,
)
