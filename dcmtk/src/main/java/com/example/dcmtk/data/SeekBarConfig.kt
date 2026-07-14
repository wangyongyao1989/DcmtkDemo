package com.example.dcmtk.data

/**
 * SeekBar配置信息，包含范围、默认值和转换方法
 * @param minValue 最小值（WW/WL的实际最小值）
 * @param maxValue 最大值（WW/WL的实际最大值）
 * @param defaultValue 默认值（文件中读取的默认值）
 * @param seekBarMax SeekBar的最大进度值（固定1000，精度足够）
 */
data class SeekBarConfig(
    val minValue: Double,
    val maxValue: Double,
    val defaultValue: Double,
    val seekBarMax: Int = 1000
) {
    /**
     * 将实际值转换为SeekBar进度
     */
    fun valueToProgress(value: Double): Int {
        if (minValue >= maxValue) return 0
        val clampedValue = value.coerceIn(minValue, maxValue)
        val ratio = (clampedValue - minValue) / (maxValue - minValue)
        return (ratio * seekBarMax).toInt()
    }

    /**
     * 将SeekBar进度转换为实际值
     */
    fun progressToValue(progress: Int): Double {
        if (minValue >= maxValue) return minValue
        val clampedProgress = progress.coerceIn(0, seekBarMax)
        val ratio = clampedProgress.toDouble() / seekBarMax
        return minValue + ratio * (maxValue - minValue)
    }

    /**
     * 获取默认进度
     */
    val defaultProgress: Int get() = valueToProgress(defaultValue)
}

/**
 * 窗宽窗位SeekBar配置集合
 */
data class WindowSeekBarConfigs(
    val windowWidthConfig: SeekBarConfig,
    val windowCenterConfig: SeekBarConfig
)

/**
 * 根据 DICOM 窗宽窗位信息生成适合的 SeekBar 配置。
 * 针对牙科影像优化。
 */
fun DicomWindowSettings.createSeekBarConfigs(): WindowSeekBarConfigs {
    // 窗宽(WW)范围设计：
    // - 最小值：尝试设为 100，但不能大于最大值
    // - 最大值：整个像素范围（至少为 100，避免太小）
    val pixelRange = (largestPixelValue - smallestPixelValue).toDouble().coerceAtLeast(0.0)
    val wwMax = maxOf(100.0, pixelRange)
    val wwMin = minOf(100.0, wwMax)
    val wwDefault = firstAvailableWindow.width

    // 窗位(WL)范围设计：
    // - 最小值：像素最小值
    // - 最大值：像素最大值
    // 如果两者相等，则增加一点范围避免空区间
    val wlMin = smallestPixelValue.toDouble()
    var wlMax = largestPixelValue.toDouble()
    if (wlMin >= wlMax) {
        wlMax = wlMin + 1.0
    }
    val wlDefault = firstAvailableWindow.center

    return WindowSeekBarConfigs(
        windowWidthConfig = SeekBarConfig(
            minValue = wwMin,
            maxValue = wwMax,
            defaultValue = wwDefault
        ),
        windowCenterConfig = SeekBarConfig(
            minValue = wlMin,
            maxValue = wlMax,
            defaultValue = wlDefault
        )
    )
}
