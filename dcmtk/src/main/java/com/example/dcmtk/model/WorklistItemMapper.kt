package com.example.dcmtk.model

import android.util.Log
import com.example.dcmtk.utils.DicomTag
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * 将 MWL C-FIND 返回的 `HashMap<String, String>`（key = DICOM Tag 的 formattedTag）
 * 转换为完整的 [WorklistItem]。
 *
 * 对照 C# mwl_query 中逐字段 `DataSet.GetSingleValueOrDefault` 的提取逻辑，
 * 并实现：
 * 1. 字段缺失时回退为空串（而非 "N/A"），保持与 C# `string.Empty` 一致
 * 2. 由 [DicomTag.PatientBirthDate] 推算 [WorklistItem.calculatedAge]
 */
object WorklistItemMapper {

    private const val TAG = "WorklistItemMapper"
    private val DATE_FMT = SimpleDateFormat("yyyyMMdd", Locale.US)

    /**
     * 从单个 HashMap 映射出 [WorklistItem]。
     *
     * @param map JNI cFindMWL 返回的单条结果，key 形如 "(0010,0010)"
     */
    fun fromMap(map: HashMap<String, String>): WorklistItem {
        val patientName = map[DicomTag.PatientName.formattedTag].orEmpty()
        val patientID = map[DicomTag.PatientID.formattedTag].orEmpty()
        val patientSex = map[DicomTag.PatientSex.formattedTag].orEmpty()
        val patientAge = map[DicomTag.PatientAge.formattedTag].orEmpty()
        val patientBirthDate = map[DicomTag.PatientBirthDate.formattedTag].orEmpty()
        val pregnancyStatus = map[DicomTag.PregnancyStatus.formattedTag].orEmpty()
        val bodyPartExamined = map[DicomTag.BodyPartExamined.formattedTag].orEmpty()

        val accessionNumber = map[DicomTag.AccessionNumber.formattedTag].orEmpty()
        val modality = map[DicomTag.Modality.formattedTag].orEmpty()
        val studyInstanceUID = map[DicomTag.StudyInstanceUID.formattedTag].orEmpty()
        val referringPhysicianName = map[DicomTag.ReferringPhysicianName.formattedTag].orEmpty()
        val performingPhysicianName = map[DicomTag.PerformingPhysicianName.formattedTag].orEmpty()
        val studyDescription = map[DicomTag.StudyDescription.formattedTag].orEmpty()

        val calculatedAge = calculateAge(patientBirthDate)

        Log.d(TAG, "Mapped WorklistItem: patientID=$patientID, studyUID=$studyInstanceUID, " +
                "acc=$accessionNumber, modality=$modality, age=${patientAge.ifEmpty { calculatedAge }}")

        return WorklistItem(
            patientName = patientName,
            patientID = patientID,
            patientSex = patientSex,
            patientAge = patientAge,
            patientBirthDate = patientBirthDate,
            pregnancyStatus = pregnancyStatus,
            bodyPartExamined = bodyPartExamined,
            accessionNumber = accessionNumber,
            modality = modality,
            studyInstanceUID = studyInstanceUID,
            referringPhysicianName = referringPhysicianName,
            performingPhysicianName = performingPhysicianName,
            studyDescription = studyDescription,
            calculatedAge = calculatedAge
        )
    }

    /**
     * 批量映射。
     */
    fun fromMapList(list: Array<HashMap<String, String>>): List<WorklistItem> {
        return list.map { fromMap(it) }
    }

    /**
     * 由出生日期(yyyyMMdd)推算周岁年龄。
     * 与 C# 逻辑一致：TimeSpan 秒数 / (24*60*60*365) 后四舍五入。
     * 解析失败时返回 0。
     */
    private fun calculateAge(birthDateStr: String): Int {
        if (birthDateStr.isEmpty()) return 0
        return try {
            val birthDate: Date = DATE_FMT.parse(birthDateStr) ?: return 0
            val now = Date()
            val diffMillis = now.time - birthDate.time
            val ageDouble = diffMillis / 1000.0 / (24 * 60 * 60 * 365)
            Math.round(ageDouble).toInt()
        } catch (e: Exception) {
            Log.w(TAG, "Failed to parse birthDate '$birthDateStr', age=0", e)
            0
        }
    }
}
