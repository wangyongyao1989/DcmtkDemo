package com.example.dcmtk.utils

import android.util.Log

/**
 * 简易日志工具，对应 dcm4che3 版 DicomFileUtils.kt 中引用的 LogUtil。
 */
object LogUtil {
    private const val TAG = "DicomFileUtils"

    fun d(msg: String) = Log.d(TAG, msg)
    fun w(msg: String) = Log.w(TAG, msg)
    fun e(msg: String) = Log.e(TAG, msg)
    fun i(msg: String) = Log.i(TAG, msg)
}
