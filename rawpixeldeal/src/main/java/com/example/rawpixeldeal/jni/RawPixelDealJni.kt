package com.example.rawpixeldeal.jni

import android.util.Log

/**
 * rawpixeldeal 模块的 JNI 入口。
 *
 * 加载顺序：
 *  1) [loadLibrary] 先加载 OpenCV 的 `libopencv_java4.so`（交叉编译产物，位于 libs/<abi>/），
 *     否则 native 侧 `cv::*` 符号会在运行时报 `UnsatisfiedLinkError`。
 *  2) 再加载本模块自身编译出的 `librawpixeldeal_native.so`，里面完成了
 *     `JNI_OnLoad` 中的 [RegisterNatives]。
 */
object RawPixelDealJni {

    private const val TAG = "RawPixelDealJni"

    init {
        try {
            // OpenCV 预编译库
            System.loadLibrary("opencv_java4")
        } catch (t: Throwable) {
            Log.e(TAG, "Failed to load opencv_java4", t)
            throw t
        }
        try {
            // 业务 native 库
            System.loadLibrary("rawpixeldeal_native")
        } catch (t: Throwable) {
            Log.e(TAG, "Failed to load rawpixeldeal_native", t)
            throw t
        }
    }

    /**
     * 探针：仅打印 JNI 通路，不依赖 OpenCV 业务符号。
     */
    @JvmStatic
    external fun stringFromJNI(): String

    /**
     * 返回 [cv::getVersionString]，用于校验 native 与 OpenCV 链接是否正确。
     */
    @JvmStatic
    external fun getOpenCVVersion(): String

    /**
     * 验证全链路：
     *  1) Kotlin 端把任意 `width * height` 的灰度原始像素（[src]）传给 native；
     *  2) native 用 [cv::Mat] + [cv::GaussianBlur] 处理；
     *  3) 把处理后的像素**回写**到 [src]（in-place），同时返回 5 个 [Int]：
     *     `[p00, p01, p02, p10, sum]` 供 Kotlin 侧做"非全 0" / "非 0 变化"校验。
     *
     * 返回 null 表示参数错误或 native 调用失败。
     */
    @JvmStatic
    external fun processRawGrayPixels(
        width: Int,
        height: Int,
        src: ByteArray
    ): IntArray?

    /**
     * 把 assets 中"原始像素数据缓冲"（如 .bin / .raw 文件）经 OpenCV
     * 处理后输出为可直接填入 [android.graphics.Bitmap.ARGB_8888] 的 RGBA 字节。
     *
     * 流程：
     *  - 16-bit 时先做 min/max 线性归一化到 0~255；
     *  - 可选 [cv::CLAHE] 增强局部对比；
     *  - 可选按四周像素数做矩形裁剪（用于去掉传感器空白边）；
     *  - [cv::cvtColor] 到 RGBA8888，内存布局与 Bitmap.copyPixelsFromBuffer 兼容。
     *
     * @param width   原图宽
     * @param height  原图高
     * @param bitDepth  8 或 16
     * @param src    little-endian 原始字节
     * @param cropLeft/Top/Right/Bottom  四周要裁掉的像素数（>=0），0 表示不裁
     * @param enableClahe  是否启用 CLAHE
     * @param clipLimit   CLAHE clipLimit，<=0 走默认 2.0
     * @param tileSize    CLAHE tile 边长，<=0 走默认 8
     * @param head        out 参数，长度 4，回传 [srcMin, srcMax, outW, outH]；可为 null
     * @return            长度 = outW * outH * 4 的 RGBA 字节；失败返回 null
     */
    @JvmStatic
    external fun processRawToRgba(
        width: Int,
        height: Int,
        bitDepth: Int,
        src: ByteArray,
        cropLeft: Int,
        cropTop: Int,
        cropRight: Int,
        cropBottom: Int,
        enableClahe: Int,
        clipLimit: Double,
        tileSize: Int,
        head: IntArray?
    ): ByteArray?

    /**
     * CT 序列级处理管线（ct-opencv-raw-buffer-windowing-prd）v2：
     *
     *   raw buffer -> HU 标准化 ->（可选先裁剪后优化）-> 自动裁剪 ROI
     *              -> 百分位 Gmin/Gmax -> 序列级直方图 -> 论文算法自适应调窗
     *              -> 可选预设窗兜底 -> 8-bit 窗映射 -> 可选显示 CLAHE -> RGBA8888
     *
     * @param rawBuffers        每片一个 jbyteArray，包装成 `Array<ByteArray>` 传入
     * @param width/height      像素几何
     * @param bitsAllocated     16（或 8）
     * @param pixelSigned       0=无符号，1=有符号（对应 DICOM PixelRepresentation）
     * @param rescaleSlope      DICOM RescaleSlope
     * @param rescaleIntercept  DICOM RescaleIntercept
     * @param photometric       0=MONOCHROME2，1=MONOCHROME1
     * @param bodyThreshold     HU > threshold 视为"非空气"用于自动裁剪
     * @param cropMargin        自动裁剪外扩像素
     * @param bodyMorphSize     形态学核大小
     * @param minBodyAreaPx     最小主体面积，过滤小噪点
     * @param nBins             直方图 bin 数（论文用 256）
     * @param n0                阈值剔除比例（论文范围 [0.0005, 0.0025]）
     * @param n1                相邻组"差值"合并比例
     * @param histSampleStride  直方图采样步长（1=全采样）
     * @param enableBilateral   1=启用双边滤波
     * @param bilateralD        双边 d
     * @param bilateralSigmaColor  双边 sigmaColor
     * @param bilateralSigmaSpace  双边 sigmaSpace
     * @param clipLowHu/clipHighHu HU 极值截断区间
     * @param enableAutoPixelSign  1=自动检测：signed 全负时 LOGW 提示
     * @param gminPercentile       百分位 Gmin（0..100；=0 表示用绝对 min）
     * @param gmaxPercentile       百分位 Gmax（0..100；=100 表示用绝对 max）
     * @param enableHistFallback   1=直方图退化时使用预设常用窗
     * @param fallbackWindowCenter 预设窗位（默认 40，软组织）
     * @param fallbackWindowWidth  预设窗宽（默认 400）
     * @param enableDisplayClahe   1=对最终 8-bit 图做 CLAHE
     * @param displayClaheClip     CLAHE clipLimit
     * @param displayClaheTile     CLAHE tile 边长
     * @param cropFirst            1=先裁剪后优化（推荐），0=旧顺序
     * @param outDisplays       out，每个 slice 一张 RGBA8888（jbyteArray）
     * @param outWindowStats    out，长度 11：
     *   [0]c, [1]w, [2]Gmin, [3]Gmax, [4]H_bins, [5]T0, [6]T1, [7]B,
     *   [8]srcMin(SV), [9]srcMax(SV), [10]N0*1e6
     * @param outCropAndOut     out，长度 10：
     *   [0]cropL, [1]cropT, [2]cropW, [3]cropH,
     *   [4]outW, [5]outH, [6]sliceCount, [7]N0*1e6, [8]N1*1e6, [9]okFlag
     * @param outHistogram      out，长度 nBins：原始 ROI 直方图
     * @param outUsedFlags      out，长度 1：[0]=1 表示成功
     * @return true 表示成功；false 表示参数或处理失败
     */
    @JvmStatic
    external fun processCtSeries(
        rawBuffers: Array<ByteArray>,
        width: Int,
        height: Int,
        bitsAllocated: Int,
        pixelSigned: Int,
        rescaleSlope: Double,
        rescaleIntercept: Double,
        photometric: Int,
        bodyThreshold: Float,
        cropMargin: Int,
        bodyMorphSize: Int,
        minBodyAreaPx: Int,
        nBins: Int,
        n0: Double,
        n1: Double,
        histSampleStride: Int,
        enableBilateral: Int,
        bilateralD: Int,
        bilateralSigmaColor: Double,
        bilateralSigmaSpace: Double,
        clipLowHu: Float,
        clipHighHu: Float,
        // ---- v2 新增 ----
        enableAutoPixelSign: Int,
        gminPercentile: Float,
        gmaxPercentile: Float,
        enableHistFallback: Int,
        fallbackWindowCenter: Double,
        fallbackWindowWidth: Double,
        enableDisplayClahe: Int,
        displayClaheClip: Double,
        displayClaheTile: Int,
        cropFirst: Int,
        // ---- 输出 ----
        outDisplays: Array<ByteArray?>,
        outWindowStats: DoubleArray,
        outCropAndOut: IntArray,
        outHistogram: IntArray,
        outUsedFlags: IntArray
    ): Boolean

    /**
     * X-ray tailorImage：实现 ProcessPixelData-readme.md §4.1 多阶段裁剪
     *
     *   raw 16-bit（大端）→ 16→8 降级 → OTSU 找前景 → minAreaRect 旋转
     *                      → 锐化 → Sobel x-y 差异 → OTSU → 闭运算
     *                      → boundingRect → 裁剪后 16-bit raw
     *
     * @param rawBuffer        大端 16-bit raw 字节
     * @param width/height     像素几何
     * @param bitsAllocated    16 / 8
     * @param pixelSigned      0=无符号，1=有符号
     * @param minAreaThreshold 抗噪面积阈值，< 此值视为裁剪失败回退全图
     * @param enableSobel      true=启用 Sobel + 闭运算；false=仅 OTSU
     * @param morphCross       闭运算核边长，0=跳过形态学
     * @param otsuThresholdLow 排除全黑背景的固定下界
     * @param outCroppedBytes  out，裁剪后的 16-bit raw（与返回值同步）
     * @param outInfo          out，长度 6：[cropL, cropT, cropW, cropH, rotateAngle*1000, okFlag]
     * @return 裁剪后的 16-bit raw 字节（与 outCroppedBytes 内容一致）
     */
    @JvmStatic
    external fun tailorImage(
        rawBuffer: ByteArray,
        width: Int,
        height: Int,
        bitsAllocated: Int,
        pixelSigned: Int,
        bigEndian: Boolean,
        minAreaThreshold: Int,
        enableSobel: Boolean,
        morphCross: Int,
        otsuThresholdLow: Double,
        outCroppedBytes: ByteArray,
        outInfo: IntArray,
    ): ByteArray?

    /**
     * 对 16-bit 原始像素执行颜色反转 (Invert LUTs)
     *
     * @param rawBuffer 原始像素字节
     * @param w 宽
     * @param h 高
     * @param bits 位深 (16/8)
     * @param sign 是否有符号 (0/1)
     * @return 反转后的原始像素字节
     */
    @JvmStatic
    external fun invertLut(
        rawBuffer: ByteArray,
        w: Int,
        h: Int,
        bits: Int,
        sign: Int,
        bigEndian: Boolean
    ): ByteArray?

    /**
     * 医学图像通用预处理接口（Kotlin -> Native -> C++）。
     *
     * @param rawBuffer     原始像素字节
     * @param width         原图宽
     * @param height        原图高
     * @param bitDepth      8 或 16
     * @param bigEndian     是否大端
     * @param isUint16      是否无符号 16 位
     * @param ops           预处理操作 ID 列表
     * @param params        每个操作对应的参数（扁平化数组）
     * @param windowMethod  调窗方法索引 (-1 表示 None，0-5 对应不同算法)
     * @param outInfo       out [outW, outH, srcMin(int), srcMax(int)] —— 向后兼容
     * @param outHuRange    out [srcMin(double), srcMax(double)] —— **P1-6: 保留 HU 浮点精度**
     *                      可为 null（不关心时省略）；outInfo 中 int 截断的精度可在此参数中找回。
     * @return              处理后的 RGBA8888 字节
     */
    @JvmStatic
    external fun processMedicalCT(
        rawBuffer: ByteArray,
        width: Int,
        height: Int,
        bitDepth: Int,
        bigEndian: Boolean,
        isUint16: Boolean,
        ops: IntArray,
        params: DoubleArray,
        windowMethod: Int,
        outInfo: IntArray,
        outHuRange: DoubleArray?
    ): ByteArray?

    /**
     * 完整预处理流水线（原文标准流程）。
     *
     * @param rawBuffer   原始像素字节
     * @param width       宽
     * @param height      高
     * @param tarW        目标宽
     * @param tarH        目标高
     * @param slope       Slope
     * @param intercept   Intercept
     * @param isUint16    raw buffer 是否为 16-bit 无符号
     * @param outInfo     out [outW, outH, srcMin(int), srcMax(int)] —— 向后兼容
     * @param outHuRange  out [srcMin(double), srcMax(double)] —— P1-6: 保留 HU 浮点精度
     * @return            处理后的 RGBA8888 字节
     */
    @JvmStatic
    external fun processCTFullPipeline(
        rawBuffer: ByteArray,
        width: Int,
        height: Int,
        tarW: Int,
        tarH: Int,
        slope: Float,
        intercept: Float,
        bigEndian: Boolean,
        isUint16: Boolean,
        outInfo: IntArray,
        outHuRange: DoubleArray?
    ): ByteArray?

    /**
     * RAW裁剪后的标准流程 -> Invert LUTs -> 调窗 (Requirement 1 & 2)
     *
     * 步骤：
     * 1. 自动裁剪 (tailorImage)
     * 2. 颜色反转 (invertLut)
     * 3. 标准流水线 (processCTFullPipeline 逻辑) + 自定义调窗方法
     */
    @JvmStatic
    external fun processCTTailorInvertWindowPipeline(
        rawBuffer: ByteArray,
        width: Int,
        height: Int,
        slope: Float,
        intercept: Float,
        bigEndian: Boolean,
        windowMethod: Int,
        outInfo: IntArray
    ): ByteArray?

    /**
     * 调窗对比：重负载（裁剪/HU/去噪/重采样/增强）只跑一次，
     * 8-bit 映射按 windowMethods 列表逐个执行，输出多张 RGBA8888 字节。
     *
     * 适用场景：CTPreprocessFragment 的 "调窗前后对比"，避免对同一份预处理数据
     * 调用两次 native_processMedicalCT（重复执行双边滤波等 O(N) 操作）。
     *
     * @param windowMethods   调窗方法列表；-1 表示 min-max 归一化；0..5 见 WindowMethod
     * @param outDisplays     out，长度 = windowMethods.size；每项一张 RGBA8888
     * @param outInfo         out，长度 >= 2，前 2 元素 [outW, outH]
     * @param outHuRange      out [srcMin(double), srcMax(double)] —— P1-6: 保留 HU 浮点精度
     */
    @JvmStatic
    external fun processMedicalCTCompareWindows(
        rawBuffer: ByteArray,
        width: Int,
        height: Int,
        bitDepth: Int,
        bigEndian: Boolean,
        isUint16: Boolean,
        ops: IntArray,
        params: DoubleArray,
        windowMethods: IntArray,
        outDisplays: Array<ByteArray?>,
        outInfo: IntArray,
        outHuRange: DoubleArray?
    )

    /**
     * 动态调窗：用户直接指定窗位 C 和窗宽 W，不使用算法索引。
     *
     * 核心原理（参考 CSDN 博客 u013598963/121023205）：
     *  - 逐像素线性映射：(L-W/2)→0, (L+W/2)→255
     *  - 超出范围的值用 saturate_cast 截断
     *
     * 与 [processMedicalCT] 的区别：用显式 (C, W) 替代 windowMethod 算法选择。
     *
     * @param windowCenter  窗位 C（HU 域）
     * @param windowWidth   窗宽 W（HU 域）
     * @param outInfo       out [outW, outH, srcMin(int), srcMax(int)]
     * @param outHuRange    out [srcMin(double), srcMax(double)]
     * @return RGBA8888 字节
     */
    @JvmStatic
    external fun processMedicalCTCustomWindow(
        rawBuffer: ByteArray,
        width: Int,
        height: Int,
        bitDepth: Int,
        bigEndian: Boolean,
        isUint16: Boolean,
        ops: IntArray,
        params: DoubleArray,
        windowCenter: Double,
        windowWidth: Double,
        outInfo: IntArray,
        outHuRange: DoubleArray?
    ): ByteArray?

    /**
     * 获取经过算子链处理（如裁剪）后的 16-bit 原始像素数据。
     * 用于写入 DICOM 文件。
     */
    external fun getProcessedRawPixels(
        rawBuffer: ByteArray,
        width: Int,
        height: Int,
        bitDepth: Int,
        bigEndian: Boolean,
        isUint16: Boolean,
        ops: IntArray,
        params: DoubleArray,
        outInfo: IntArray
    ): ByteArray?
}
