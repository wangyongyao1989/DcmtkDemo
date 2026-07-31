package com.example.dcmtk.model

/**
 * 检查记录，作为 writeDcmFile 写入 DICOM 的元数据来源。
 * 对应 dcm4che3 版 DicomFileUtils.kt 中 writeDcmFile 的入参类型。
 */
data class ScanRecord(
    var examineNo: Long = 0,

    var patientName: String = "",
    var patientSex: String = "",
    var patientAge: String = "",
    var patientHeight: String = "",
    var patientWeight: String = "",
    var patientTelephoneNumbers: String = "",
    var patientNote: String = "",

    var sendDoctorNo: String = "",
    var sendDoctorName: String = "",
    var sendCheckDate: String = "",
    var toothPosition: Int = -1,

    var checkDoctorNo: String = "",
    var checkDoctorName: String = "",
    var checkDate: String = "",

    /** 出生日期（非 Room 字段，由原始 SQL 管理） */
     var birthDate: String = "",
    /** 检查号 CR+日期+随机数（非 Room 字段，由原始 SQL 管理） */
     var examNumber: String = "",

    var dcmPath: String = "",
    var imagePath: String = "",
    var modifiedImagePath: String = "",


    var deviceName: String = "",
    var deviceIp: String = "",
    var checked: Int= 0,
    var pacsUploaded: Boolean = false,
) {

    override fun hashCode(): Int {
        return patientName.hashCode() +
                patientSex.hashCode() +
                patientAge.hashCode() +
                patientTelephoneNumbers.hashCode() +
                patientNote.hashCode()
    }
}
