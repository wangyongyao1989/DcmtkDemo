package com.example.rawpixeldeal.xray

import android.util.Log

/**
 * X-ray 管线专用日志工具。
 *
 * 设计缘由（详见 ProcessPixelData-readme.md §6）：
 *  - 老 ProcessPixelData.java 中调用 `LogUtil.d(...)` 但模块内无 LogUtil
 *  - 在 rawpixeldeal 内独立放一个轻量包装，便于和老代码兼容
 *  - TAG 前缀 `Xray/`，便于 logcat 过滤：`adb logcat -s XrayPipeline:V XrayJni:V`
 */
object LogUtil {
    private const val PREFIX = "Xray"

    fun d(tag: String, msg: String) {
        Log.d("$PREFIX/$tag", msg)
    }

    fun i(tag: String, msg: String) {
        Log.i("$PREFIX/$tag", msg)
    }

    fun w(tag: String, msg: String) {
        Log.w("$PREFIX/$tag", msg)
    }

    fun e(tag: String, msg: String, t: Throwable? = null) {
        if (t != null) Log.e("$PREFIX/$tag", msg, t) else Log.e("$PREFIX/$tag", msg)
    }
}
