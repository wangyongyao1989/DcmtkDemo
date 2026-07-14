package com.example.dcmtk.model

/**
 * 像素数据处理结果。
 * 字段顺序即 JNI 构造器签名顺序：(I I [B D D I I)，native 构造 PixelData 时须严格匹配。
 */
data class PixelData(
    val rows: Int,
    val columns: Int,
    val data: ByteArray,
    val win_width: Double,
    val win_center: Double,
    val exposure_leve: Int,
    val largestImagePixelValue: Int
) {
    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (other !is PixelData) return false
        return rows == other.rows && columns == other.columns &&
                win_width == other.win_width && win_center == other.win_center &&
                exposure_leve == other.exposure_leve &&
                largestImagePixelValue == other.largestImagePixelValue &&
                data.contentEquals(other.data)
    }

    override fun hashCode(): Int {
        var r = rows
        r = 31 * r + columns
        r = 31 * r + data.contentHashCode()
        r = 31 * r + win_width.hashCode()
        r = 31 * r + win_center.hashCode()
        r = 31 * r + exposure_leve
        r = 31 * r + largestImagePixelValue
        return r
    }
}
