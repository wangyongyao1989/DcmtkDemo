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
}
