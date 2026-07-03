package com.example.dcmtk.model

import java.io.File

/**
 * 用于 DICOM 文件信息传递。
 * 包含从 DICOM 标签解析出的关键字段，以及源 dcm 与转换后 jpg 的绝对路径。
 */
data class DicomImageRecord(
    val name: String,
    val id: String,
    val sex: String,
    val studyDate: String,
    val studyDesc: String,
    val dcmPath: String,
    val jpgPath: String,
    var isSelected: Boolean = false
) {
    /** 转换后的 jpg 是否已存在 */
    fun hasJpg(): Boolean {
        return jpgPath != null && File(jpgPath).exists()
    }
}
