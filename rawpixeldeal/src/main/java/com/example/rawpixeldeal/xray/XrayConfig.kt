package com.example.rawpixeldeal.xray

/**
 * X-ray 调窗方法枚举。
 *
 * 与 dcmtk.ProcessPixelData.WindowCalcMethod 一一对应；这里复刻是为了让 rawpixeldeal
 * 模块的 UI/Fragment 层不直接依赖 dcmtk 的枚举（防止循环引用、便于独立演进）。
 */
enum class WindowMethod(val displayName: String) {
    /** 默认固定窗宽窗位法（论文基线，WW=255, WL=127.5） */
    DEFAULT("1. 默认固定窗"),

    /** 文献[7] 72% 累计面积法（窗宽固定 508） */
    CUMULATIVE_72("2. 72% 累计面积法"),

    /** 文献[6] 双峰直方图法（依据摘要近似） */
    BIMODAL_PEAK("3. 双峰直方图法"),

    /** 论文核心：自适应序列直方图法 */
    ADAPTIVE_HISTOGRAM("4. 自适应直方图法"),

    /** 文献[8] 直方图分类法（仅预留接口） */
    HISTOGRAM_TYPE("5. 直方图分类法"),

    /** 工程基线：min/max 线性窗 */
    MIN_MAX("6. Min-Max"),

    /** 自定义波峰面积法 (基于 image_processing_jni.c) */
    PEAK_AREA_AUTO("7. 波峰面积法 (Custom)"),
}

/**
 * X-ray 管线配置。
 *
 * 设计缘由（详见 ProcessPixelData-readme.md §5）：
 *  - SENSOR_DEPTH = 65536（16-bit 直方图）
 *  - clip_min / clip_max = 0.05（CDF 头尾 5% 鲁棒截断）
 *  - minAreaThreshold = 40000（tailor_img 抗噪面积阈值）
 *  - morphCross = 25（25×25 MORPH_CROSS 闭运算）
 *  - otsuThresholdLow = 10（OTSU 前先固定 10 排除全黑背景）
 */
data class XrayConfig(
    /** 16-bit 直方图 bin 边界（最大像素值 + 1） */
    val sensorDepth: Int = 65536,
    /** CDF 累积分布的下分位数（剔除最暗 5% 像素） */
    val clipMin: Double = 0.05,
    /** CDF 累积分布的上分位数（剔除最亮 5% 像素） */
    val clipMax: Double = 0.95,
    /** tailor_img 裁剪框面积下限（< 此值视为裁剪失败，回退原图） */
    val minAreaThreshold: Int = 40000,
    /** 形态学闭运算核大小（MORPH_CROSS），0 表示跳过形态学 */
    val morphCross: Int = 25,
    /** Sobel/sharpen 是否启用（false 时只用 OTSU 裁剪） */
    val enableSobel: Boolean = true,
    /** OTSU 前的固定下界阈值（排除全黑背景对 OTSU 的干扰） */
    val otsuThresholdLow: Double = 10.0,
    /** 调窗方法（默认走论文核心算法） */
    val windowMethod: WindowMethod = WindowMethod.ADAPTIVE_HISTOGRAM,
    /** 直方图 bin 数（论文默认 256） */
    val nBins: Int = 256,
    /** 论文阈值剔除比例 N0（默认 0.0015，范围 [0.0005, 0.0025]） */
    val n0: Double = 0.0015,
    /** 论文相邻组合并比例 N1 */
    val n1: Double = 0.0015,
    /** 16-bit 大端 raw 像素转 HU 时的 slope（DICOM RescaleSlope） */
    val rescaleSlope: Double = 1.0,
    /** 16-bit 大端 raw 像素转 HU 时的 intercept（DICOM RescaleIntercept） */
    val rescaleIntercept: Double = 0.0,
    /** 8-bit 显示图是否启用 CLAHE 局部增强 */
    val enableDisplayClahe: Boolean = false,
    val displayClaheClip: Double = 2.0,
    val displayClaheTile: Int = 8,
)
