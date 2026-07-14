package com.example.dcmtk.model

/**
 * 检查记录，作为 writeDcmFile 写入 DICOM 的元数据来源。
 * 对应 dcm4che3 版 DicomFileUtils.kt 中 writeDcmFile 的入参类型。
 */
data class ScanRecord(
    val examineNo: Int,
    val patientName: String,
    val patientAge: String,
    val patientSex: String,
    val toothPosition: String
)
