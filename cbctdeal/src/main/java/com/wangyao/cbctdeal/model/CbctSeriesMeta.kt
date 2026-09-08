package com.wangyao.cbctdeal.model

import com.wangyao.cbctdeal.jni.CbctJni

/**
 * CBCT 序列解析结果元数据（对应 Native 侧 CbctVolume）。
 */
data class CbctSeriesMeta(
    val patientName: String,
    val patientID: String,
    val patientSex: String,
    val patientBirthDate: String,
    val studyDate: String,
    val modality: String,
    val manufacturer: String,
    val width: Int,               // x 方向像素数
    val height: Int,              // y 方向像素数
    val depth: Int,               // z 方向切片数（含补全空白片）
    val sliceCount: Int,          // 有效切片帧数
    val skippedFiles: Int,        // 过滤的非 DICOM / 损坏文件数
    val elapsedMs: Long,          // 解析耗时
    val spacingX: Double,         // mm，各向同性时三者相等
    val spacingY: Double,
    val spacingZ: Double,
    val slope: Double,            // HU = pixel * slope + intercept
    val intercept: Double,
    val pixelRepresentation: Int, // 0=unsigned 1=signed
    val windowWidth: Double,      // 默认窗宽（HU）
    val windowCenter: Double,     // 默认窗位（HU）
    val zMin: Double,
    val zMax: Double,
) {
    companion object {
        fun fromMap(m: Map<String, String>): CbctSeriesMeta = CbctSeriesMeta(
            patientName = m["patientName"].orEmpty(),
            patientID = m["patientID"].orEmpty(),
            patientSex = m["patientSex"].orEmpty(),
            patientBirthDate = m["patientBirthDate"].orEmpty(),
            studyDate = m["studyDate"].orEmpty(),
            modality = m["modality"].orEmpty(),
            manufacturer = m["manufacturer"].orEmpty(),
            width = m["width"]?.toIntOrNull() ?: 0,
            height = m["height"]?.toIntOrNull() ?: 0,
            depth = m["depth"]?.toIntOrNull() ?: 0,
            sliceCount = m["sliceCount"]?.toIntOrNull() ?: 0,
            skippedFiles = m["skippedFiles"]?.toIntOrNull() ?: 0,
            elapsedMs = m["elapsedMs"]?.toLongOrNull() ?: 0L,
            spacingX = m["spacingX"]?.toDoubleOrNull() ?: 1.0,
            spacingY = m["spacingY"]?.toDoubleOrNull() ?: 1.0,
            spacingZ = m["spacingZ"]?.toDoubleOrNull() ?: 1.0,
            slope = m["slope"]?.toDoubleOrNull() ?: 1.0,
            intercept = m["intercept"]?.toDoubleOrNull() ?: 0.0,
            pixelRepresentation = m["pixelRepresentation"]?.toIntOrNull() ?: 0,
            windowWidth = m["windowWidth"]?.toDoubleOrNull() ?: 4000.0,
            windowCenter = m["windowCenter"]?.toDoubleOrNull() ?: 600.0,
            zMin = m["zMin"]?.toDoubleOrNull() ?: 0.0,
            zMax = m["zMax"]?.toDoubleOrNull() ?: 0.0,
        )
    }
}

/**
 * Volume 句柄：持有 Native 指针 + 元数据。
 * 使用完毕必须调用 [release] 释放 Native 内存（建议在 UI 组件销毁时调用）。
 */
class CbctVolumeHandle(val ptr: Long, val meta: CbctSeriesMeta) {

    @Volatile
    private var released = ptr == 0L

    val isReleased: Boolean
        get() = released

    /** 提取横断面（固定 Z） */
    fun extractAxial(sliceIndex: Int, ww: Double, wc: Double): android.graphics.Bitmap? =
        if (released) null else CbctJni.extractAxialSlice(ptr, sliceIndex, ww, wc)

    /** 提取 MPR 切面 [CbctJni.PLANE_CORONAL] / [CbctJni.PLANE_SAGITTAL] */
    fun extractMpr(plane: Int, position: Int, ww: Double, wc: Double): android.graphics.Bitmap? =
        if (released) null else CbctJni.extractMpr(ptr, plane, position, ww, wc)

    /** 释放 Native 内存（幂等安全，可多次调用） */
    fun release() {
        if (!released) {
            released = true
            if (ptr != 0L) {
                CbctJni.releaseVolume(ptr)
            }
        }
    }
}
