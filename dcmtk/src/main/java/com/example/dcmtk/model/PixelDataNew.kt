package com.example.dcmtk.model

data class PixelDataNew(
    var rows: Int,
    var columns: Int,
    var data: ByteArray?,
    var largestImagePixelValue: Int,
    var win_center: Int,
    var win_width: Int,
    val exposure_leve: Int,
    val standardDeviation: Double,
) {
    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (javaClass != other?.javaClass) return false

        other as PixelDataNew

        if (rows != other.rows) return false
        if (columns != other.columns) return false
        if (!((data ?: byteArrayOf()).contentEquals(other.data ?: byteArrayOf()))) return false
        if (largestImagePixelValue != other.largestImagePixelValue) return false
        if (win_center != other.win_center) return false
        if (win_width != other.win_width) return false
        if (exposure_leve != other.exposure_leve) return false
        return standardDeviation == other.standardDeviation
    }

    override fun hashCode(): Int {
        var result = rows
        result = 31 * result + columns
        result = 31 * result + (data?.contentHashCode() ?: 0)
        result = 31 * result + largestImagePixelValue
        result = 31 * result + win_center
        result = 31 * result + win_width
        result = 31 * result + exposure_leve
        result = 31 * result + standardDeviation.hashCode()
        return result
    }
}
