package com.example.dcmtk.data

/**
 * DICOM图像窗宽窗位及像素范围信息
 * @param smallestPixelValue 最小像素值(0028,0106)
 * @param largestPixelValue 最大像素值(0028,0107)
 * @param windows 窗宽窗位预设列表（支持多个）
 * @param autoCalculatedWindow 自动计算的默认窗宽窗位（当文件无预设时使用）
 */
data class DicomWindowSettings(
    val smallestPixelValue: Int,
    val largestPixelValue: Int,
    val windows: List<DicomWindow>,
    val autoCalculatedWindow: DicomWindow
) {
    /**
     * 单个窗宽窗位预设
     * @param center 窗位(WL/Window Center)
     * @param width 窗宽(WW/Window Width)
     * @param description 预设描述（如"肺窗"、"骨窗"）
     */
    data class DicomWindow(
        val center: Double,
        val width: Double,
        val description: String? = null
    )

    /**
     * 获取第一个可用的窗宽窗位（优先使用文件存储的预设，无则用自动计算的）
     */
    val firstAvailableWindow: DicomWindow
        get() = windows.firstOrNull() ?: autoCalculatedWindow
}
