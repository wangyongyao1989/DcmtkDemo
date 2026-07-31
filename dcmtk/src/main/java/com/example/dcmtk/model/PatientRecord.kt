package com.example.dcmtk.model

import com.example.dcmtk.utils.DicomTag
import java.io.Serializable

data class PatientRecord(
    var name: String = "",
    var id: String = "",
    var sex: String = "",
    var birthDate: String = "",
    var accessionNumber: String = "",
    var modality: String = "",
    var isSelected: Boolean = false,
    var isDownloaded: Boolean = false,

    // 以下字段保持与数据库或 JNI 逻辑的兼容性
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

    /** 检查号 CR+日期+随机数 */
    var examNumber: String = "",

    var dcmPath: String = "",
    var imagePath: String = "",
    var modifiedImagePath: String = "",

    var deviceName: String = "",
    var deviceIp: String = "",
    var checked: Int = 0,
) : Serializable {

    companion object {
        /**
         * 直接从 PACS 查询结果 Map 映射为 PatientRecord。
         */
        fun fromMap(map: Map<String, String>): PatientRecord {
            val nameVal = map[DicomTag.PatientName.formattedTag].orEmpty().ifEmpty { "N/A" }
            val idVal = map[DicomTag.PatientID.formattedTag].orEmpty().ifEmpty { "N/A" }
            val sexVal = map[DicomTag.PatientSex.formattedTag].orEmpty().ifEmpty { "N/A" }
            val birthVal = map[DicomTag.PatientBirthDate.formattedTag].orEmpty().ifEmpty { "N/A" }
            val accVal = map[DicomTag.AccessionNumber.formattedTag].orEmpty()
            val modVal = map[DicomTag.Modality.formattedTag].orEmpty()

            return PatientRecord(
                // UI 核心字段
                name = nameVal,
                id = idVal,
                sex = sexVal,
                birthDate = birthVal,
                accessionNumber = accVal,
                modality = modVal,

                // 数据库/业务兼容字段
                examineNo = idVal.filter { it.isDigit() }.toLongOrNull() ?: 0L,
                patientName = nameVal,
                patientSex = sexVal,
                patientAge = map[DicomTag.PatientAge.formattedTag].orEmpty(),
                patientHeight = map[DicomTag.PatientSize.formattedTag].orEmpty(),
                patientWeight = map[DicomTag.PatientWeight.formattedTag].orEmpty(),
                patientNote = map[DicomTag.PatientComments.formattedTag].orEmpty(),
                patientTelephoneNumbers = map["(0010,2154)"].orEmpty(), // Patient's Telephone Numbers

                sendDoctorName = map[DicomTag.ReferringPhysicianName.formattedTag].orEmpty(),
                sendCheckDate = map[DicomTag.StudyDate.formattedTag].orEmpty(),

                checkDoctorName = map[DicomTag.PerformingPhysicianName.formattedTag].orEmpty(),
                checkDate = map[DicomTag.StudyDate.formattedTag].orEmpty(),

                examNumber = accVal.ifEmpty { idVal },
                dcmPath = "",
                imagePath = "",
                modifiedImagePath = ""
            )
        }
    }
}
