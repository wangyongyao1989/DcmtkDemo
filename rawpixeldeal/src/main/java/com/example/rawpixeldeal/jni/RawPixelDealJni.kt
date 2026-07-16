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
     * 把 assets 中"原始像素数据缓冲"（如 Data610.bin / Data622.bin）经 OpenCV
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
     * CT 序列级处理管线（ct-opencv-raw-buffer-windowing-prd）：
     *
     *   raw buffer -> HU 标准化 -> OpenCV 优化（双边 + 极值截断）
     *              -> 自动裁剪（HU 阈值 + 形态学 + 最大连通域）
     *              -> 序列级直方图（ROI 内聚合，256 bin）
     *              -> 论文算法自适应调窗 (T0/T1 阈值剔除 + 合并 + B->(c,w))
     *              -> 8-bit 窗映射（按 cropRect 裁剪后） -> RGBA8888
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
        outDisplays: Array<ByteArray?>,
        outWindowStats: DoubleArray,
        outCropAndOut: IntArray,
        outHistogram: IntArray,
        outUsedFlags: IntArray
    ): Boolean
}
