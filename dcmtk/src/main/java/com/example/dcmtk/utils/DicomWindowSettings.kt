package com.example.dcmtk.utils

/**
 * DICOM 窗宽窗位及像素范围信息。
 * 对应 dcm4che3 版 DicomFileUtils.kt 中 readDicomWindowSettings 的返回类型。
 */
data class DicomWindowSettings(
    val smallestPixelValue: Int,
    val largestPixelValue: Int,
    val windows: List<DicomWindow>,
    val autoCalculatedWindow: DicomWindow
) {
    data class DicomWindow(
        val center: Double,
        val width: Double,
        val description: String? = null
    )

    /** 第一个可用的窗（预设优先，否则取自动计算） */
    val firstAvailableWindow: DicomWindow
        get() = windows.firstOrNull() ?: autoCalculatedWindow
}
