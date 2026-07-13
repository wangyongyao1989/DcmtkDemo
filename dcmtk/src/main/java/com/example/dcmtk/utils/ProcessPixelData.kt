package com.example.dcmtk.utils

import com.example.dcmtk.model.PixelData

/**
 * 像素数据处理（dcm4che3 版 DicomFileUtils.kt 中引用，实现未给出）。
 * 此处提供一个与 native writeDcmFileFull 等价的 Kotlin 参考实现：
 * 扫描 16-bit 像素求 min/max，推导窗宽窗位/曝光/最大像素值。
 * live 代码走 native 路径，此类仅供编译完整性与对照参考。
 */
object ProcessPixelData {
    fun process(raw: ByteArray, imageWidth: Int, imageHeight: Int): PixelData {
        val rows = imageHeight
        val columns = imageWidth
        var minVal = 65535
        var maxVal = 0
        val numPixels = raw.size / 2
        for (i in 0 until numPixels) {
            val lo = raw[i * 2].toInt() and 0xFF
            val hi = raw[i * 2 + 1].toInt() and 0xFF
            val v = (hi shl 8) or lo
            if (v < minVal) minVal = v
            if (v > maxVal) maxVal = v
        }
        if (numPixels == 0) {
            minVal = 0
            maxVal = 0
        }
        var width = (maxVal - minVal).toDouble()
        if (width < 1.0) width = 1.0
        val center = minVal + width / 2.0
        return PixelData(
            rows = rows,
            columns = columns,
            data = raw,
            win_width = width,
            win_center = center,
            exposure_leve = maxVal,
            largestImagePixelValue = maxVal
        )
    }
}
