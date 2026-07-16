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
}
