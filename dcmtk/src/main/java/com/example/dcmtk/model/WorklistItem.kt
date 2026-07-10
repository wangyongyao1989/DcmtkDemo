package com.example.dcmtk.model

import java.io.Serializable

/**
 * MWL C-FIND 查询返回的完整 Worklist Item 数据类。
 *
 * 对照 C# mwl_query 中从 DicomDataset 提取的全部字段，覆盖患者模块与检查模块，
 * 并附带由生日推算的 [calculatedAge]，供 DB 落库时在 PatientAge 为空时回退使用。
 *
 * 字段来源映射（DICOM Tag → 属性）:
 * - (0010,0010) PatientName            → [patientName]
 * - (0010,0020) PatientID              → [patientID]
 * - (0010,0040) PatientSex             → [patientSex]          ("M"/"F"/"O")
 * - (0010,1010) PatientAge             → [patientAge]          (原始字符串)
 * - (0010,0030) PatientBirthDate       → [patientBirthDate]    (yyyyMMdd)
 * - (0010,21C0) PregnancyStatus        → [pregnancyStatus]
 * - (0018,0015) BodyPartExamined       → [bodyPartExamined]
 * - (0008,0050) AccessionNumber        → [accessionNumber]
 * - (0008,0060) Modality               → [modality]
 * - (0020,000D) StudyInstanceUID       → [studyInstanceUID]
 * - (0008,0090) ReferringPhysicianName → [referringPhysicianName]
 * - (0008,1050) PerformingPhysicianName→ [performingPhysicianName]
 * - (0008,1030) StudyDescription       → [studyDescription]
 */
data class WorklistItem(

    /* ===================== 患者模块 ===================== */
    /** 患者姓名 (PN)，C# 端曾做 GB18030 解码，Kotlin 端 JNI 已解码为 UTF-8 */
    val patientName: String,
    /** 患者 ID (LO)，作为患者表主键 */
    val patientID: String,
    /** 患者性别 (CS)："M" / "F" / "O" */
    val patientSex: String,
    /** 患者年龄原始串 (AS)，可能含非数字字符如 "035Y" */
    val patientAge: String,
    /** 患者出生日期 (DA)，格式 yyyyMMdd */
    val patientBirthDate: String,
    /** 妊娠状态 (US) */
    val pregnancyStatus: String,
    /** 检查部位 (CS)，C# 端仅保留数字与逗号 */
    val bodyPartExamined: String,

    /* ===================== 检查模块 ===================== */
    /** 检查号 (SH) */
    val accessionNumber: String,
    /** 模态 (CS)："CR" / "DR" 等 */
    val modality: String,
    /** 检查实例 UID (UI)，检查表唯一标识之一 */
    val studyInstanceUID: String,
    /** 申请医师 (PN) → 映射为检查的 ExamineDoctorID */
    val referringPhysicianName: String,
    /** 执行医师 (PN) → 映射为检查的 ReviewDoctorID */
    val performingPhysicianName: String,
    /** 检查描述 (LO) */
    val studyDescription: String,

    /* ===================== 派生字段 ===================== */
    /** 由 [patientBirthDate] 推算的年龄；当 [patientAge] 为空时使用 */
    val calculatedAge: Int,

    /* ===================== UI 辅助 ===================== */
    var isSelected: Boolean = false,
    var isDownloaded: Boolean = false

) : Serializable {

    /**
     * 获取最终落库用的年龄字符串：
     * 优先使用清洗后的 [patientAge]，为空则回退到 [calculatedAge]。
     */
    fun resolveAge(): String {
        val cleaned = patientAge.replace(Regex("[^0-9]+"), "")
        return if (cleaned.isNotEmpty()) cleaned else calculatedAge.toString()
    }

    /**
     * 获取最终落库用的检查部位：
     * 仅保留数字与逗号（与 C# 逻辑一致）。
     */
    fun resolveBodyPart(): String {
        return bodyPartExamined.replace(Regex("[^0-9,]+"), "")
    }

    /**
     * 性别映射：M → "1"，其余 → "2"（与 C# 逻辑一致）。
     */
    fun resolveSexCode(): String {
        return if (patientSex.equals("M", ignoreCase = true)) "1" else "2"
    }

    /**
     * 妊娠状态映射：空 → "1"，非空 → "2"（与 C# 逻辑一致）。
     */
    fun resolvePregnancyStatusCode(): String {
        return if (pregnancyStatus.isEmpty()) "1" else "2"
    }

    /**
     * 模态映射：CR 或空 → "1"，其余 → "2"（与 C# 逻辑一致）。
     */
    fun resolveModalityCode(): String {
        return if (modality.equals("CR", ignoreCase = true) || modality.isEmpty()) "1" else "2"
    }

    /**
     * 是否通过模态过滤：当开启模态检查时仅允许 CR/cr/DR/dr。
     */
    fun isModalityAllowed(): Boolean {
        return modality.equals("CR", ignoreCase = true) ||
                modality.equals("DR", ignoreCase = true)
    }

    override fun toString(): String {
        return "$patientName ($patientID)" +
                if (accessionNumber.isEmpty()) "" else " Acc:$accessionNumber"
    }
}
