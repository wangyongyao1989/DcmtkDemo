package com.example.rawpixeldeal.xray

/**
 * 8 参构造的像素数据类（与老 ProcessPixelData.java 字段一一对应）。
 *
 * 设计缘由（详见 ProcessPixelData-readme.md §1/§4）：
 *  - 老 Java 版 `new PixelData(img_cutted_height, img_cutted_width, out_pixelData,
 *    largestImagePixelValue, win_center, win_width, exposure_level, standardDeviation)`
 *  - 与 dcmtk.PixelDataNew（7 参，少 stddev）字段差异：这里多一个 standardDeviation
 *  - 该 standardDeviation 用于"探测器均匀性"QA
 *
 * 字段说明：
 *  - [height]/[width]        裁剪后图像尺寸
 *  - [data]                  大端 16-bit 像素字节（裁剪后的 width × height × 2）
 *  - [largestImagePixelValue] 整图最大像素值（写 DICOM `(0028,0107)` 字段）
 *  - [winCenter] / [winWidth]  调窗窗位 / 窗宽（HU）
 *  - [exposureLevel]         全图平均像素（用于剂量评估）
 *  - [standardDeviation]     全图标准差（用于对比度 / 均匀性评估）
 */
data class PixelData(
    val height: Int,
    val width: Int,
    val data: ByteArray?,
    val largestImagePixelValue: Int,
    val winCenter: Int,
    val winWidth: Int,
    val exposureLevel: Int,
    val standardDeviation: Double,
) {
    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (javaClass != other?.javaClass) return false

        other as PixelData

        if (height != other.height) return false
        if (width != other.width) return false
        if (!((data ?: byteArrayOf()).contentEquals(other.data ?: byteArrayOf()))) return false
        if (largestImagePixelValue != other.largestImagePixelValue) return false
        if (winCenter != other.winCenter) return false
        if (winWidth != other.winWidth) return false
        if (exposureLevel != other.exposureLevel) return false
        return standardDeviation == other.standardDeviation
    }

    override fun hashCode(): Int {
        var result = height
        result = 31 * result + width
        result = 31 * result + (data?.contentHashCode() ?: 0)
        result = 31 * result + largestImagePixelValue
        result = 31 * result + winCenter
        result = 31 * result + winWidth
        result = 31 * result + exposureLevel
        result = 31 * result + standardDeviation.hashCode()
        return result
    }

    companion object {
        /**
         * 把 dcmtk 7 参 PixelDataNew 升级为 8 参 PixelData
         * （新增 standardDeviation 参数）。
         */
        fun fromNew(
            height: Int,
            width: Int,
            data: ByteArray?,
            largestImagePixelValue: Int,
            winCenter: Int,
            winWidth: Int,
            standardDeviation: Double,
        ): PixelData = PixelData(
            height = height,
            width = width,
            data = data,
            largestImagePixelValue = largestImagePixelValue,
            winCenter = winCenter,
            winWidth = winWidth,
            // dcmtk.ProcessPixelData 内部把 maxVal 同时写到 maxVal 和
            // exposure_leve 字段；这里沿用这个语义，避免再算一次。
            exposureLevel = largestImagePixelValue,
            standardDeviation = standardDeviation,
        )
    }
}
