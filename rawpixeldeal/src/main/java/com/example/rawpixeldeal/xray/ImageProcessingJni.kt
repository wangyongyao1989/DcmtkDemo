package com.example.rawpixeldeal.xray

/**
 * 老 ProcessPixelData.java 中 `ImageProcessingJni.autoWindowLevel(int[] data, int length)` 的
 * Kotlin 实现（replacement），保留 JNI 风格 API 以便 Java/Kotlin 双向调用。
 *
 * 设计缘由（详见 ProcessPixelData-readme.md §6.5）：
 *  - 原 Java 版调用 `ImageProcessingJni.autoWindowLevel(data_cal, arrayLength)`
 *  - 返回 `[largestPixelValue, win_center, win_width]`
 *  - 这里用纯 Kotlin 实现（不需要 native JNI 跳转）
 *  - 算法：min/max 线性窗 + 5%/95% 鲁棒截断（与 dcmtk.ProcessPixelData.MIN_MAX 等价）
 */
object ImageProcessingJni {

    private const val TAG = "ImageProcessingJni"

    /**
     * 等价于 JNI `autoWindowLevel(int[] data, int length) → int[3]`
     *
     * @param data   16-bit 像素数组
     * @param length 数组有效长度（< data.size 时取前 length 个）
     * @return [largestPixelValue, win_center, win_width]
     */
    @JvmStatic
    fun autoWindowLevel(data: IntArray, length: Int): IntArray {
        val n = minOf(length, data.size)
        if (n <= 0) {
            LogUtil.w(TAG, "autoWindowLevel: empty input")
            return intArrayOf(0, 0, 1)
        }
        var minV = Int.MAX_VALUE
        var maxV = Int.MIN_VALUE
        for (i in 0 until n) {
            val v = data[i]
            if (v < minV) minV = v
            if (v > maxV) maxV = v
        }
        // 5% / 95% 百分位鲁棒截断
        val sorted = data.copyOf(n).also { it.sort() }
        val idxLow = (n * 0.05).toInt().coerceIn(0, n - 1)
        val idxHigh = (n * 0.95).toInt().coerceIn(0, n - 1)
        val lo = sorted[idxLow]
        val hi = sorted[idxHigh]
        val c = (lo + hi) / 2
        val w = maxOf(1, hi - lo)
        LogUtil.d(TAG, "autoWindowLevel: range=[$minV, $maxV] " +
                "p5=$lo p95=$hi -> c=$c w=$w")
        return intArrayOf(maxV, c, w)
    }

    /**
     * 与老 Java 版完全兼容的便捷重载（不带 length）
     */
    @JvmStatic
    fun autoWindowLevel(data: IntArray): IntArray = autoWindowLevel(data, data.size)
}
